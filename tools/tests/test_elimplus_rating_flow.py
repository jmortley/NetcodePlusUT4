"""ElimPlus rating: the production code itself, run end to end.

ElimPlusRatingSystem.cpp is compiled WHOLE, unmodified, with the vendored TeamGlicko2 library, against stand-ins
for the engine types (elimplus_rating_adapter.h), and driven through its public API in the order AElimPlusGame
calls it (elimplus_rating_cases.cpp). Mods.db is a real sqlite database owned by this test: every statement the
server process issues comes back here, is run with the engine wrapper's row format reproduced, and can be made to
fail. So what is checked is what the shipped code does: loads, rounds through the real rating library, the match-end
save, the rank query, the upload payload and the log lines. LifecycleFlowTests covers what happens to a player
around the match: joining late, reconnecting, leaving, a drawn round, a failed load, a table without the newer
columns, a server without its stats actor, and a round nobody opposed.

Not covered: UBT, replication and anything in AElimPlusGame itself. That still needs the dedicated-server playtest.
The stand-ins also differ from the engine in ways listed at the top of elimplus_rating_adapter.h.
No sibling plugin or third-party Python module is required.
"""

import json
import math
import os
from pathlib import Path
import random
import sqlite3
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler
import test_elimplus_recentre as model
from test_elimplus_recentre import NOW, add_player, baseline_row, database, stored, sunk_hub, wrapper_rows


SEPARATOR = "\x1f"
SHIM_HEADERS = (
    "CoreMinimal.h", "NCEloUploader.h", "UnrealTournament.h", "UTGameInstance.h", "UTGameState.h",
    "ElimPlusStatsReplicator.h", "Engine/World.h", "Misc/DateTime.h", "Policies/CondensedJsonPrintPolicy.h",
    "Serialization/JsonSerializer.h", "Serialization/JsonWriter.h")
BUILD = {}
BASELINE_READ = "SELECT COUNT(*), MAX(Shift)"
BASELINE_SET = "INSERT OR IGNORE INTO NCRatingElimPlusBaseline"
# The whole statement LoadPlayerFromDB reads a player's row with, so a failure injected here hits nothing else.
PLAYER_READ = ("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames, TotalPoints, RoundsPlayed FROM NCRatingElimPlus "
               "WHERE UniqueId='%s';")
PLAYER_SAVE = "INSERT OR REPLACE INTO NCRatingElimPlus"
# The table as it was before the TotalDamage and PlayerName columns (schema "v2").
V2_TABLE = ("CREATE TABLE NCRatingElimPlus (UniqueId TEXT PRIMARY KEY NOT NULL, Rating REAL NOT NULL DEFAULT 1400.0, "
            "RD REAL NOT NULL DEFAULT 350.0, Sigma REAL NOT NULL DEFAULT 0.03, PerfIndexEMA REAL NOT NULL DEFAULT 0.0, "
            "PerfGames INTEGER NOT NULL DEFAULT 0, LastSeenUtc INTEGER NOT NULL DEFAULT 0, "
            "TotalPoints REAL NOT NULL DEFAULT 0.0, RoundsPlayed INTEGER NOT NULL DEFAULT 0, "
            "SchemaVersion INTEGER NOT NULL DEFAULT 2)")


def setUpModule():
    compiler, environment, msvc = find_compiler()                 # SkipTest when there is no compiler
    temporary = tempfile.TemporaryDirectory(prefix="ncp-elimplus-flow-")
    unittest.addModuleCleanup(temporary.cleanup)
    directory = Path(temporary.name)
    shim = directory / "shim"
    for header in SHIM_HEADERS:                                   # every engine include resolves to the stand-ins
        path = shim / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "elimplus_rating_adapter.h"\n', encoding="utf-8")
    sources = ([PLUGIN / "Source/Private/ElimPlusRatingSystem.cpp"]
               + sorted((PLUGIN / "Source/Private/Glicko2").glob("*.cpp"))
               + [PLUGIN / "tools/tests/elimplus_rating_cases.cpp"])
    includes = [shim, PLUGIN / "tools/tests", PLUGIN / "Source/Public", PLUGIN / "Source/Public/Glicko2"]
    executable = directory / ("flow.exe" if os.name == "nt" else "flow")
    if msvc:
        command = ([compiler, "/nologo", "/EHsc", "/std:c++14", "/utf-8"] + [f"/I{path}" for path in includes]
                   + [str(path) for path in sources] + [f"/Fe{executable}", f"/Fo{directory}{os.sep}"])
    else:
        command = ([compiler, "-std=c++14"] + [f"-I{path}" for path in includes]
                   + [str(path) for path in sources] + ["-o", str(executable)])
    built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=600)
    if built.returncode:
        raise AssertionError(f"The rating system did not compile against the stand-ins:\n{built.stdout}\n{built.stderr}")
    BUILD.update(executable=executable, environment=environment)


class Server(object):
    """One game-server process running the production rating system against `connection`."""

    def __init__(self, test, connection, guard=1, now=NOW):
        self.connection = connection
        self.logs, self.statements, self.failures = [], [], []
        self.elo, self.ranks = {}, {}
        self.process = subprocess.Popen(
            [str(BUILD["executable"])], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
            bufsize=1, env=BUILD["environment"])
        test.addCleanup(self.close)
        self.call("now", now)
        self.call("cvar", "ncp.ElimPlusRecentre", guard)

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=15)
        self.process.stdout.close()

    def fail_next(self, *fragments):
        """The next statement containing each fragment fails without running, as a locked database would."""
        self.failures.extend(fragments)

    def answer(self, sql):
        self.statements.append(sql)
        for fragment in self.failures:
            if fragment in sql:
                self.failures.remove(fragment)
                return ["FAIL"]
        try:
            rows = wrapper_rows(self.connection, sql)
        except sqlite3.Error:
            return ["FAIL"]
        return [SEPARATOR.join(["ROW"] + row) for row in rows] + ["OK"]

    def call(self, *fields):
        self.process.stdin.write(SEPARATOR.join(str(field) for field in fields) + "\n")
        self.process.stdin.flush()
        out = []
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise AssertionError("The server process stopped during: " + " ".join(str(field) for field in fields))
            kind, *rest = line.rstrip("\n").split(SEPARATOR)
            if kind == "DONE":
                return out
            if kind == "SQL":
                self.process.stdin.write("\n".join(self.answer(rest[0])) + "\n")
                self.process.stdin.flush()
            elif kind == "LOG":
                self.logs.append((rest[1], rest[2]))
            elif kind == "ELO":
                self.elo[rest[0]] = (int(rest[1]), int(rest[2]))
            elif kind == "RANK":
                self.ranks[rest[0]] = int(rest[1])
            else:
                out.append(rest[0] if rest else "")

    # ---- the calls AElimPlusGame makes, in its order ----
    def map_load(self):
        self.call("new")
        (ok,) = self.call("init")
        return ok == "1"

    def login(self, *players):
        for player in players:
            self.call("load", player, player)

    def start_match(self):
        self.call("snapshot")

    def round(self, winners, losers, draw=False):
        def side(entries):
            return ";".join("%s:%d:%d:%s:%d" % entry for entry in entries)
        self.call("round", int(draw), side(winners), side(losers))

    def end_match(self):
        self.call("flush")

    def replicator(self, present):
        """Whether the game mode has its stats actor. Without one it never counts a round for anyone."""
        self.call("replicator", int(present))

    def upload(self, roster, winner=0):
        """BuildResultPayload for `roster` ([(id, team)]), parsed, with players keyed by id."""
        (text,) = self.call("payload", winner, 1, 0, ";".join("%s:%s:%d:0:0:0" % (name, name, team) for name, team in roster))
        if not text:
            return None
        payload = json.loads(text)
        payload["players"] = {player["id"]: player for player in payload["players"]}
        return payload

    def live(self, *players):
        """Each player's cached rating exactly as the server holds it (the upload's `post`)."""
        payload = self.upload([(player, 0) for player in players])
        return {player: payload["players"][player]["post"] for player in players}

    def warnings(self):
        return [message for verbosity, message in self.logs if verbosity == "Warning"]

    def logged(self, fragment):
        return [message for _, message in self.logs if fragment in message]


RED = ["active0", "active1", "active2", "active3"]
BLUE = ["active4", "active5", "active6", "active7"]


def four_v_four(server, rounds=6, seed=1, red=RED, blue=BLUE, leaver=None):
    """A real match through the rating library: seeded kills and damage, sides alternating wins. `leaver` plays
    only the first round."""
    rng = random.Random(seed)
    for index in range(rounds):
        sides = []
        for team, names in ((0, red), (1, blue)):
            sides.append([(name, rng.randint(0, 3), rng.randint(0, 1), "%.1f" % rng.uniform(40, 420), team)
                          for name in names if not (name == leaver and index > 0)])
        winner = index % 2
        server.round(sides[winner], sides[1 - winner])
    return [(name, 0) for name in red] + [(name, 1) for name in blue]


def row(connection, name):
    """(Rating, RoundsPlayed) exactly as Mods.db holds them."""
    return connection.execute("SELECT Rating, RoundsPlayed FROM NCRatingElimPlus WHERE UniqueId = ?", (name,)).fetchone()


def rounded(rating):
    """FMath::RoundToInt, which is how a rating becomes the whole number a player sees."""
    return int(math.floor(rating + 0.5))


class FlowCase(unittest.TestCase):
    def setUp(self):
        self.connection = database()
        self.addCleanup(self.connection.close)

    def server(self, **options):
        return Server(self, self.connection, **options)

    def loaded_server(self, players=RED + BLUE, **options):
        server = self.server(**options)
        self.assertTrue(server.map_load())
        server.login(*players)
        return server

    def hub_below_the_seed(self, shortfall):
        """A measured hub whose active pool has since slipped `shortfall` points under the seed."""
        sunk_hub(self.connection)
        self.server().map_load()
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating - ? WHERE UniqueId LIKE 'active%'",
                                (shortfall + 0.5,))


class BaselineFlowTests(FlowCase):
    def test_first_map_load_measures_the_hub_and_leaves_the_table_alone(self):
        sunk_hub(self.connection)
        before = stored(self.connection)
        server = self.loaded_server(RED + ["login-only", "stranded2"])
        self.assertEqual(baseline_row(self.connection), [(1, 362, 14, 1038.5, NOW)])
        self.assertEqual(stored(self.connection), before)
        (line,) = server.logged("Rating baseline set for this hub")
        self.assertTrue(line.startswith("Rating baseline set for this hub: +362, measured on 14 established players "
                                        "averaging 1038.5. The game now shows each stored rating +362;"), line)
        self.assertTrue(line.endswith("recentre guard ON"), line)
        self.assertEqual(server.warnings(), [])
        live = server.live(*RED, "login-only", "stranded2")
        for name in RED + ["stranded2"]:
            self.assertAlmostEqual(live[name], before[name] + 362, places=3, msg=name)
        self.assertEqual(live["login-only"], 1400.0)                        # never played: the untouched seed
        self.assertEqual(server.upload([("active0", 0)])["baseline_shift"], 362)
        # A later map load reads the same baseline back and says so.
        again = self.server()
        again.map_load()
        self.assertEqual(again.logged("Rating baseline"),
                         ["Rating baseline +362 (measured on 14 established players averaging 1038.5); recentre guard ON"])

    def test_match_is_saved_as_stored_ratings_and_read_back_unchanged(self):
        sunk_hub(self.connection)
        before = stored(self.connection)
        first = self.loaded_server()
        at_login = first.live(*RED, *BLUE)
        first.start_match()
        roster = four_v_four(first)
        first.end_match()
        after_match = first.upload(roster)
        table = stored(self.connection)
        for name in RED + BLUE:
            post = after_match["players"][name]["post"]
            self.assertNotAlmostEqual(post, at_login[name], places=2, msg=name)           # the match moved everyone
            self.assertAlmostEqual(table[name], post - 362, places=5, msg=name)         # saved WITHOUT the baseline
            self.assertAlmostEqual(table[name] - before[name], post - at_login[name], places=3, msg=name)
        for name in before:
            if name not in RED + BLUE:
                self.assertEqual(table[name], before[name], name)                        # nobody else was written
        # Three more map loads, no matches: the rating neither creeps nor jumps.
        for _ in range(3):
            later = self.loaded_server()
            live = later.live(*RED, *BLUE)
            for name in RED + BLUE:
                self.assertAlmostEqual(live[name], after_match["players"][name]["post"], places=3, msg=name)
        self.assertEqual(stored(self.connection), table)

    def test_consecutive_matches_start_where_the_last_one_ended(self):
        sunk_hub(self.connection)
        previous = None
        for match in range(4):
            server = self.loaded_server()
            server.start_match()
            roster = four_v_four(server, seed=10 + match)
            server.end_match()
            payload = server.upload(roster)
            for name in RED + BLUE:
                player = payload["players"][name]
                if previous:
                    self.assertAlmostEqual(player["pre"], round(previous[name]), delta=1.0, msg=name)
                self.assertAlmostEqual(player["post"], player["pre"] + player["delta"], delta=1.0, msg=name)
            previous = {name: payload["players"][name]["post"] for name in RED + BLUE}
        self.assertLess(abs(sum(previous.values()) / 8 - (980 + 9 * 3.5 + 362)), 40)      # still around 1373, not +362 a match

    def test_new_and_never_played_players_are_seeds_until_they_play(self):
        sunk_hub(self.connection)
        server = self.loaded_server(RED + BLUE + ["newcomer", "login-only", "watcher"])
        self.assertEqual(server.live("newcomer", "login-only", "watcher"),
                         {"newcomer": 1400.0, "login-only": 1400.0, "watcher": 1400.0})
        server.start_match()
        roster = four_v_four(server, red=RED[:3] + ["newcomer"], blue=BLUE[:3] + ["login-only"])
        server.end_match()
        payload = server.upload(roster + [("watcher", 0)])
        table = stored(self.connection)
        for name in ("newcomer", "login-only"):                             # their first played row: baseline comes off
            self.assertEqual(payload["players"][name]["pre"], 1400.0)
            self.assertEqual(payload["players"][name]["rounds_played"], 6)
            self.assertAlmostEqual(table[name], payload["players"][name]["post"] - 362, places=5, msg=name)
        watcher = payload["players"]["watcher"]                             # on the roster, never spawned
        self.assertEqual((watcher["pre"], watcher["post"], watcher["rounds_played"]), (1400.0, 1400.0, 0))
        self.assertEqual(table["watcher"], 1400.0)
        later = self.loaded_server(["newcomer", "login-only", "watcher"])
        live = later.live("newcomer", "login-only", "watcher")
        self.assertAlmostEqual(live["newcomer"], payload["players"]["newcomer"]["post"], places=3)
        self.assertAlmostEqual(live["login-only"], payload["players"]["login-only"]["post"], places=3)
        self.assertEqual(live["watcher"], 1400.0)

    def test_rank_compares_live_ratings(self):
        add_player(self.connection, "seed-row", 1400.0, 0, 0)               # logged in once, never played
        for index in range(12):
            add_player(self.connection, f"regular{index}", 1100.0, 400, 1)
        add_player(self.connection, "top", 1500.0, 400, 1)
        add_player(self.connection, "high", 1200.0, 400, 1)
        server = self.loaded_server(["top", "high", "seed-row", "regular0"])
        self.assertEqual(baseline_row(self.connection)[0][1], 264)          # the 14 established average 1135.7

        def rank(name):
            (value,) = server.call("rank", name)
            return int(value)

        # Live: top 1764, high 1464, the never-played seed row 1400, regulars 1364. Compared as stored, the seed
        # row (1400) would sit above "high" (1200) and the two answers below would be 3 and 2.
        self.assertEqual([rank(name) for name in ("top", "high", "seed-row", "regular0", "nobody")], [1, 2, 3, 4, 0])
        server.start_match()
        for _ in range(3):
            server.round([("high", 3, 0, "400.0", 0)], [("BOT:Kilo", 0, 1, "30.0", 1)])
        server.end_match()
        self.assertEqual(server.ranks, {"high": 2})                         # what the scoreboard is sent at match end


class StepFlowTests(FlowCase):
    def test_step_is_per_round_played_and_reported_apart(self):
        self.hub_below_the_seed(9.0)                                        # live pool mean 1391 -> +0.27 a round
        server = self.loaded_server()
        server.start_match()
        roster = four_v_four(server, rounds=5, leaver="active7")
        server.end_match()
        payload = server.upload(roster)
        self.assertAlmostEqual(payload["recentre_per_round"], 0.27, places=6)
        self.assertEqual(server.logged("Recentre: pool mean"),
                         ["Recentre: pool mean 1391.0 over 14 established players seen in the last 30 days -> +0.270 per round"])
        for name in RED + BLUE:
            rounds = 1 if name == "active7" else 5
            self.assertAlmostEqual(payload["players"][name]["recentre"], 0.27 * rounds, places=6, msg=name)
        # The step is inside post and inside the saved row.
        table = stored(self.connection)
        for name in RED + BLUE:
            self.assertAlmostEqual(table[name], payload["players"][name]["post"] - 362, places=5, msg=name)

    def test_step_is_exactly_the_difference_from_the_same_match_without_it(self):
        self.hub_below_the_seed(9.0)
        twin_connection = sqlite3.connect(":memory:", isolation_level=None)
        self.addCleanup(twin_connection.close)
        self.connection.backup(twin_connection)                             # the same database, byte for byte
        posts = []
        for connection, switch_off in ((self.connection, False), (twin_connection, True)):
            server = Server(self, connection)
            server.map_load()
            server.login(*RED, *BLUE)
            server.start_match()
            roster = four_v_four(server, rounds=5, leaver="active7")
            if switch_off:
                server.call("cvar", "ncp.ElimPlusRecentre", 0)              # read again at match end
            server.end_match()
            posts.append(server.upload(roster))
        guarded, plain = posts
        self.assertEqual(plain["recentre_per_round"], 0.0)
        for name in RED + BLUE:
            self.assertEqual(plain["players"][name]["recentre"], 0.0)
            self.assertAlmostEqual(guarded["players"][name]["post"] - plain["players"][name]["post"],
                                   guarded["players"][name]["recentre"], places=9, msg=name)
        self.assertEqual(plain["baseline_shift"], 362)                      # the baseline is not the guard's to switch

    def test_step_is_capped(self):
        self.hub_below_the_seed(60.0)
        server = self.loaded_server()
        server.start_match()
        roster = four_v_four(server, rounds=3)
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual(payload["recentre_per_round"], 0.3)
        self.assertAlmostEqual(payload["players"]["active0"]["recentre"], 0.9, places=9)

    def test_player_who_only_faced_bots_gets_no_step_and_a_capped_change(self):
        self.hub_below_the_seed(9.0)
        server = self.loaded_server(["active0"])
        server.start_match()
        for _ in range(6):
            server.round([("active0", 4, 0, "600.0", 0)], [("BOT:Kilo", 0, 1, "20.0", 1), ("BOT:Lima", 0, 1, "10.0", 1)])
        server.end_match()
        payload = server.upload([("active0", 0)])
        player = payload["players"]["active0"]
        self.assertAlmostEqual(payload["recentre_per_round"], 0.27, places=6)   # the hub has a pool...
        self.assertEqual(player["recentre"], 0.0)                               # ...but this player gets nothing
        self.assertFalse(player["faced_humans"])
        self.assertLessEqual(abs(player["delta"]), 5)
        self.assertEqual(server.elo["active0"], (int(player["pre"]) + player["delta"], player["delta"]))

    def test_small_hub_gets_a_zero_baseline_and_no_step(self):
        for index in range(9):
            add_player(self.connection, f"few{index}", 1100.0, 800, 1)
        server = self.loaded_server([f"few{index}" for index in range(8)])
        self.assertEqual(baseline_row(self.connection), [(1, 0, 9, 1100.0, NOW)])
        self.assertEqual(server.logged("Rating baseline"),
                         ["Rating baseline +0 (set just now, when the hub had 9 established players, too few to measure one); recentre guard ON"])
        server.start_match()
        names = [f"few{index}" for index in range(8)]
        roster = four_v_four(server, red=names[:4], blue=names[4:])
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual((payload["baseline_shift"], payload["recentre_per_round"]), (0, 0.0))
        self.assertAlmostEqual(stored(self.connection)["few0"], payload["players"]["few0"]["post"], places=5)

    def test_second_save_of_the_same_match_adds_nothing(self):
        self.hub_below_the_seed(9.0)
        server = self.loaded_server()
        server.start_match()
        roster = four_v_four(server, rounds=4)
        server.end_match()
        first, table = server.upload(roster), stored(self.connection)
        server.end_match()                                                  # the engine can route match end twice
        second = server.upload(roster)
        self.assertEqual(stored(self.connection), table)
        for name in RED + BLUE:
            self.assertEqual(second["players"][name]["post"], first["players"][name]["post"], name)
            self.assertAlmostEqual(second["players"][name]["recentre"], 0.27 * 4, places=6, msg=name)   # still reported
        self.assertEqual(second["recentre_per_round"], first["recentre_per_round"])


class SwitchFlowTests(FlowCase):
    def test_guard_off_changes_nothing_and_sets_no_baseline(self):
        sunk_hub(self.connection)
        before = stored(self.connection)
        server = self.loaded_server(guard=0)
        self.assertEqual(baseline_row(self.connection), [])
        self.assertEqual(server.logged("Rating baseline"), [])
        self.assertEqual(server.logged("No rating baseline"),
                         ["No rating baseline for this hub; recentre guard OFF (ncp.ElimPlusRecentre 0)"])
        live = server.live(*RED)
        for name in RED:
            self.assertAlmostEqual(live[name], before[name], places=3)      # ratings exactly as stored
        server.start_match()
        roster = four_v_four(server)
        server.call("cvar", "ncp.ElimPlusRecentre", 1)                      # switched on in the middle of the map...
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual((payload["baseline_shift"], payload["recentre_per_round"]), (0, 0.0))   # ...waits for a map load
        self.assertEqual(server.logged("Recentre: pool mean"), [])
        self.assertEqual(baseline_row(self.connection), [])
        self.assertAlmostEqual(stored(self.connection)["active0"], payload["players"]["active0"]["post"], places=5)
        next_map = self.server()
        next_map.map_load()
        self.assertEqual(len(baseline_row(self.connection)), 1)

    def test_guard_switched_off_later_keeps_the_baseline_and_stops_the_step(self):
        sunk_hub(self.connection)
        self.server().map_load()
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating - 9.5 WHERE UniqueId LIKE 'active%'")
        server = self.loaded_server(guard=0)                                # off at map load, baseline already there
        self.assertEqual(server.logged("Rating baseline"),
                         ["Rating baseline +362 (measured on 14 established players averaging 1038.5); "
                          "recentre guard OFF (ncp.ElimPlusRecentre 0)"])
        self.assertAlmostEqual(server.live("active0")["active0"], 980.0 - 9.5 + 362, places=3)
        server.start_match()
        roster = four_v_four(server)
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual((payload["baseline_shift"], payload["recentre_per_round"]), (362, 0.0))
        self.assertAlmostEqual(stored(self.connection)["active0"], payload["players"]["active0"]["post"] - 362, places=5)


class FailureFlowTests(FlowCase):
    def measured_hub(self):
        sunk_hub(self.connection)
        self.server().map_load()
        return stored(self.connection)

    def test_first_login_retries_a_baseline_the_map_load_could_not_read(self):
        before = self.measured_hub()
        server = self.server()
        server.fail_next(BASELINE_READ)
        server.map_load()
        self.assertEqual(server.warnings(),
                         ["Rating baseline unreadable (db busy?): this map runs on the stored ratings as they are, recentre guard off"])
        server.login("active0", "active1")
        self.assertEqual(len(server.logged("Rating baseline +362")), 1)
        live = server.live("active0", "active1")
        self.assertAlmostEqual(live["active0"], before["active0"] + 362, places=3)
        self.assertAlmostEqual(live["active1"], before["active1"] + 362, places=3)
        self.assertEqual(sum(BASELINE_READ in sql for sql in server.statements), 2)        # the failed read, then the retry

    def test_unreadable_baseline_runs_the_map_on_stored_ratings_and_loses_nothing(self):
        before = self.measured_hub()
        server = self.server()
        server.fail_next(BASELINE_READ, BASELINE_READ)                      # at map load and at the first login
        server.map_load()
        server.login(*RED, *BLUE)                                           # later logins must not try again
        self.assertEqual(len(server.warnings()), 2)
        self.assertEqual(sum(BASELINE_READ in sql for sql in server.statements), 2)
        live = server.live(*RED, *BLUE)
        for name in RED + BLUE:
            self.assertAlmostEqual(live[name], before[name], places=3, msg=name)          # every player on one scale
        server.start_match()
        roster = four_v_four(server)
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual((payload["baseline_shift"], payload["recentre_per_round"]), (0, 0.0))
        table = stored(self.connection)
        for name in RED + BLUE:
            self.assertAlmostEqual(table[name], payload["players"][name]["post"], places=5, msg=name)
            self.assertAlmostEqual(table[name] - before[name],
                                   payload["players"][name]["post"] - live[name], places=3, msg=name)
        healthy = self.loaded_server()                                      # the next map is back on live ratings
        again = healthy.live(*RED)
        for name in RED:
            self.assertAlmostEqual(again[name], table[name] + 362, places=3, msg=name)

    def test_failed_first_measurement_is_not_mistaken_for_a_hub_without_one(self):
        sunk_hub(self.connection)
        before = stored(self.connection)
        server = self.server()
        server.fail_next(BASELINE_SET, BASELINE_SET)
        server.map_load()
        server.login(*RED, *BLUE)
        self.assertEqual(baseline_row(self.connection), [])
        self.assertEqual(len(server.warnings()), 2)
        server.start_match()
        roster = four_v_four(server)
        server.end_match()
        payload = server.upload(roster)
        self.assertEqual(payload["recentre_per_round"], 0.0)                # no step toward 1400 from a sunk pool
        self.assertEqual(server.logged("Recentre: pool mean"), [])
        self.assertAlmostEqual(stored(self.connection)["active0"], payload["players"]["active0"]["post"], places=5)
        self.assertAlmostEqual(payload["players"]["active0"]["pre"], before["active0"], delta=0.5)
        retry = self.server()
        retry.map_load()
        self.assertEqual(len(baseline_row(self.connection)), 1)

    def test_two_servers_starting_together_measure_once(self):
        sunk_hub(self.connection)
        one, two = self.server(), self.server(now=NOW + 3)
        self.assertTrue(one.map_load())
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating + 50")       # whatever happens in between
        self.assertTrue(two.map_load())
        self.assertEqual(baseline_row(self.connection), [(1, 362, 14, 1038.5, NOW)])
        self.assertEqual(two.upload([("x", 0)]), None)                      # nobody loaded: nothing to upload
        two.login("active0")
        self.assertEqual(two.upload([("active0", 0)])["baseline_shift"], 362)


class OlderBuildFlowTests(FlowCase):
    """A build from before the baseline, sharing the database with this one. The older build is its own SQL: the
    load SELECT and the save INSERT, which this commit left textually unchanged, with no shift."""

    def old_load(self, player):
        (row,) = wrapper_rows(self.connection, model.render(model.LOAD_TEMPLATE, [player]))
        return float(row[0]), int(row[6])

    def old_save(self, player, rating, rounds):
        self.connection.execute(model.render(model.FLUSH_TEMPLATE, [
            player, rating, 45.0, 0.03, 0.0, 0, NOW, rounds * 2.5, rounds, rounds * 250.0, player]))

    def test_older_build_match_in_flight_when_the_baseline_is_set(self):
        sunk_hub(self.connection)
        old_rating, old_rounds = self.old_load("active0")                   # its match started before the update
        new = self.loaded_server(BLUE)                                      # first map load of this build
        self.assertEqual(new.upload([("active4", 1)])["baseline_shift"], 362)
        new.start_match()
        four_v_four(new, red=["active8", "active9", "active10", "active11"], blue=BLUE)
        new.end_match()
        self.old_save("active0", old_rating + 6.5, old_rounds + 14)         # saved after the baseline was set
        after = self.loaded_server(["active0"])
        self.assertAlmostEqual(after.live("active0")["active0"], 980.0 + 6.5 + 362, places=3)   # not 362 low

    def test_older_build_that_keeps_running_and_a_rollback(self):
        sunk_hub(self.connection)
        first = self.loaded_server(RED + BLUE)
        first.start_match()
        roster = four_v_four(first)
        first.end_match()
        post = first.upload(roster)["players"]["active5"]["post"]
        rating, rounds = self.old_load("active5")                           # loads AFTER the baseline was set
        self.assertAlmostEqual(rating, post - 362, places=3)                # the older build shows the stored value
        self.old_save("active5", rating + 10.0, rounds + 13)
        later = self.loaded_server(["active5"])
        self.assertAlmostEqual(later.live("active5")["active5"], post + 10.0, places=3)   # not shifted twice


class LifecycleFlowTests(FlowCase):
    """What happens to a player around the match rather than in it: joining late, reconnecting, leaving, a drawn
    round, a failed load, an older table, a server without its stats actor, a round nobody opposed."""

    def test_player_who_joins_mid_match_starts_from_the_rating_they_joined_with(self):
        sunk_hub(self.connection)
        server = self.loaded_server(RED + BLUE[:3])
        server.start_match()
        four_v_four(server, rounds=2, seed=3, blue=BLUE[:3])
        server.login("active7", "newcomer")                                 # PostLogin in the middle of the match
        joined = server.live("active7", "newcomer")
        self.assertAlmostEqual(joined["active7"], 1043.0 + 362, places=3)
        self.assertEqual(joined["newcomer"], 1400.0)
        roster = four_v_four(server, rounds=4, seed=4, red=RED + ["newcomer"])
        server.end_match()
        payload = server.upload(roster)
        for name, start, lifetime in (("active7", 1405, 904), ("newcomer", 1400, 4)):
            player = payload["players"][name]
            self.assertEqual(player["pre"], float(start), name)             # not 0, which the site reads as "+post"
            self.assertEqual(player["delta"], rounded(player["post"]) - start, name)
            self.assertEqual(server.elo[name], (rounded(player["post"]), player["delta"]), name)   # the scoreboard
            self.assertEqual(player["rounds_played"], lifetime, name)
            self.assertAlmostEqual(row(self.connection, name)[0], player["post"] - 362, places=5, msg=name)
        self.assertNotEqual(payload["players"]["newcomer"]["delta"], 0)
        early = payload["players"]["active0"]                               # there from the start: measured from there
        self.assertEqual(early["pre"], 980.0 + 362)

    def test_reconnect_mid_match_keeps_the_rating_and_rounds_earned_so_far(self):
        sunk_hub(self.connection)
        server = self.loaded_server()
        start = server.live("active2")["active2"]
        server.start_match()
        four_v_four(server, rounds=3, seed=5)
        earned = server.live("active2")["active2"]
        self.assertNotAlmostEqual(earned, start, places=2)
        issued = len(server.statements)
        server.login("active2")                                             # the same player's PostLogin again
        self.assertEqual(server.statements[issued:], [])                    # nothing is read from Mods.db again
        self.assertEqual(server.live("active2")["active2"], earned)
        roster = four_v_four(server, rounds=3, seed=6)
        server.end_match()
        player = server.upload(roster)["players"]["active2"]
        self.assertEqual(player["pre"], float(rounded(start)))              # still measured from the match start
        self.assertEqual(player["rounds_played"], 906)
        self.assertEqual(row(self.connection, "active2")[1], 906)
        self.assertAlmostEqual(row(self.connection, "active2")[0], player["post"] - 362, places=5)

    def test_player_who_left_is_still_uploaded_and_saved_with_their_step(self):
        self.hub_below_the_seed(9.0)                                        # +0.27 a round
        server = self.loaded_server()
        server.start_match()
        roster = four_v_four(server, rounds=5, leaver="active7")            # plays round one, then disconnects
        server.end_match()
        present = [entry for entry in roster if entry[0] != "active7"]      # the player list at match end
        payload = server.upload(present)
        gone = payload["players"]["active7"]
        self.assertIs(gone["left"], True)
        self.assertEqual((gone["team"], gone["result"]), (1, "loss"))       # the team comes from the round log
        self.assertAlmostEqual(gone["recentre"], 0.27, places=6)            # one round's step, and it is in post
        self.assertEqual((gone["pre"], gone["rounds_played"]), (1396.0, 901))
        self.assertEqual(gone["delta"], rounded(gone["post"]) - 1396)
        (their_round,) = [line for line in payload["rounds"][0]["players"] if line["id"] == "active7"]
        self.assertEqual((gone["kills"], gone["deaths"], gone["damage"]),
                         (their_round["kills"], their_round["deaths"], their_round["damage"]))
        self.assertEqual(sum(1 for logged in payload["rounds"] for line in logged["players"] if line["id"] == "active7"), 1)
        self.assertAlmostEqual(row(self.connection, "active7")[0], gone["post"] - 362, places=5)
        self.assertEqual(row(self.connection, "active7")[1], 901)
        for name, _ in present:
            self.assertNotIn("left", payload["players"][name], name)
            self.assertAlmostEqual(payload["players"][name]["recentre"], 0.27 * 5, places=6, msg=name)
        self.assertEqual(len(server.logged("included 1 mid-match leaver(s) alongside 7 rostered player(s)")), 1)

    def test_drawn_round_is_rated_as_a_draw(self):
        names = [f"even{index}" for index in range(8)]
        for name in names:
            add_player(self.connection, name, 1250.0, 400, 1)
        lines = [(2, 0, "310.0"), (1, 1, "150.0"), (0, 1, "80.0"), (1, 0, "220.0")]       # the same four on each side
        red = [(name,) + line + (0,) for name, line in zip(names[:4], lines)]
        blue = [(name,) + line + (1,) for name, line in zip(names[4:], lines)]
        twin = sqlite3.connect(":memory:", isolation_level=None)
        self.addCleanup(twin.close)
        self.connection.backup(twin)
        drawn, won = Server(self, self.connection), Server(self, twin)
        for server, draw in ((drawn, True), (won, False)):
            server.map_load()
            server.login(*names)
            server.start_match()
            server.round(red, blue, draw=draw)
        after_draw, after_win = drawn.live(*names), won.live(*names)
        for mine, theirs in zip(names[:4], names[4:]):
            self.assertAlmostEqual(after_draw[mine], after_draw[theirs], places=6, msg=mine)      # mirror images end level
            self.assertGreater(after_win[mine] - after_win[theirs], 1.0, mine)                   # a win separates them
        payload = drawn.upload([(name, 0) for name in names[:4]] + [(name, 1) for name in names[4:]], winner=-1)
        (logged,) = payload["rounds"]
        self.assertEqual((logged["draw"], logged["winner_team"]), (True, -1))
        self.assertEqual({line["result"] for line in logged["players"]}, {"draw"})
        self.assertEqual({player["result"] for player in payload["players"].values()}, {"draw"})
        (decided,) = won.upload([(name, 0) for name in names[:4]] + [(name, 1) for name in names[4:]])["rounds"]
        self.assertEqual((decided["draw"], decided["winner_team"]), (False, 0))

    def test_player_whose_load_failed_plays_on_defaults_and_is_never_saved(self):
        sunk_hub(self.connection)
        self.server().map_load()
        before = stored(self.connection)
        server = self.server()
        server.map_load()
        server.fail_next(PLAYER_READ % "active3")                           # a locked database, for this one read
        server.login(*RED, *BLUE)
        self.assertEqual(server.failures, [])
        self.assertEqual(len(server.logged("Load guard for active3 (db busy/unavailable)")), 1)
        self.assertEqual(server.live("active3")["active3"], 1400.0)         # defaults, not their real 1369
        server.start_match()
        roster = four_v_four(server)
        server.end_match()
        payload = server.upload(roster)
        self.assertIs(payload["players"]["active3"]["provisional"], True)   # the site applies nothing for them
        self.assertNotIn("provisional", payload["players"]["active0"])
        self.assertEqual(row(self.connection, "active3"), (before["active3"], 900))      # their real row is untouched
        self.assertEqual(len(server.logged("Flush skip active3")), 1)
        self.assertNotIn("active3", server.elo)
        self.assertAlmostEqual(row(self.connection, "active0")[0], payload["players"]["active0"]["post"] - 362, places=5)
        later = self.loaded_server(["active3"])
        self.assertAlmostEqual(later.live("active3")["active3"], before["active3"] + 362, places=3)

    def test_table_without_the_newer_columns_saves_stored_ratings_too(self):
        connection = sqlite3.connect(":memory:", isolation_level=None)
        self.addCleanup(connection.close)
        connection.execute(V2_TABLE)
        for index in range(14):
            connection.execute(
                "INSERT INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, LastSeenUtc, TotalPoints, RoundsPlayed) "
                "VALUES (?, ?, 45.0, 0.03, ?, 2250.0, 900)", (f"active{index}", 980.0 + 9 * index, NOW - index * 86400))
        players = RED + BLUE + ["newcomer"]

        def map_load():
            server = Server(self, connection)
            server.fail_next("ADD COLUMN TotalDamage", "ADD COLUMN PlayerName")       # locked: the migration does not land
            server.map_load()
            self.assertEqual(server.failures, [])
            self.assertEqual(len(server.logged("v2-shape writes this session")), 1)
            server.login(*players)
            return server

        server = map_load()
        self.assertEqual(connection.execute("SELECT Shift FROM NCRatingElimPlusBaseline").fetchall(), [(362,)])
        at_login = server.live(*players)
        self.assertAlmostEqual(at_login["active0"], 980.0 + 362, places=3)
        self.assertEqual(at_login["newcomer"], 1400.0)
        server.start_match()
        roster = four_v_four(server, red=RED + ["newcomer"])
        server.end_match()
        payload = server.upload(roster)
        saves = [sql for sql in server.statements if sql.startswith(PLAYER_SAVE)]
        self.assertEqual(len(saves), 9)
        for sql in saves:                                                   # the shorter statement, marked version 2
            self.assertNotIn("TotalDamage", sql)
            self.assertTrue(sql.endswith(", 2);"), sql)
        self.assertFalse(any("CREATE VIEW" in sql for sql in server.statements))
        table = dict(connection.execute("SELECT UniqueId, Rating FROM NCRatingElimPlus"))
        for name in players:
            self.assertAlmostEqual(table[name], payload["players"][name]["post"] - 362, places=5, msg=name)
        live = map_load().live(*players)
        for name in players:
            self.assertAlmostEqual(live[name], payload["players"][name]["post"], places=3, msg=name)

    def test_without_the_stats_actor_ratings_still_survive_a_save_and_a_reload(self):
        sunk_hub(self.connection)
        server = self.loaded_server(RED + BLUE[:3] + ["newcomer"])
        server.replicator(False)                                            # the actor failed to spawn
        server.start_match()
        roster = four_v_four(server, blue=BLUE[:3] + ["newcomer"])
        server.end_match()
        self.assertEqual((server.elo, server.ranks), ({}, {}))              # nobody to send a scoreboard to
        payload = server.upload(roster)
        regular, new = payload["players"]["active0"], payload["players"]["newcomer"]
        self.assertEqual((regular["rounds_played"], new["rounds_played"]), (900, 0))       # no round was counted...
        self.assertGreater(abs(new["post"] - 1400.0), 1.0)                                  # ...but everyone was rated
        self.assertAlmostEqual(row(self.connection, "active0")[0], regular["post"] - 362, places=5)
        # A rated row with no lifetime rounds takes no baseline, on the way out or on the way back in.
        self.assertEqual(row(self.connection, "newcomer")[1], 0)
        self.assertAlmostEqual(row(self.connection, "newcomer")[0], new["post"], places=5)
        live = self.loaded_server(["active0", "newcomer"]).live("active0", "newcomer")
        self.assertAlmostEqual(live["newcomer"], new["post"], places=3)
        self.assertAlmostEqual(live["active0"], regular["post"], places=3)

    def test_rated_row_from_before_rounds_were_counted_keeps_its_rating(self):
        sunk_hub(self.connection)
        add_player(self.connection, "veteran", 1450.0, 0, 90)               # a real rating, RoundsPlayed defaulted to 0
        server = self.loaded_server(RED + BLUE[:3] + ["veteran"])
        self.assertEqual(server.live("veteran")["veteran"], 1450.0)         # no rounds, so no baseline: shown as stored
        server.start_match()
        roster = four_v_four(server, blue=BLUE[:3] + ["veteran"])
        server.end_match()
        post = server.upload(roster)["players"]["veteran"]["post"]
        self.assertEqual(row(self.connection, "veteran")[1], 6)
        self.assertAlmostEqual(row(self.connection, "veteran")[0], post - 362, places=5)
        self.assertAlmostEqual(self.loaded_server(["veteran"]).live("veteran")["veteran"], post, places=3)

    def test_round_nobody_opposed_is_counted_for_the_player_but_not_rated_or_logged(self):
        """The upload this produces is the fixture of ut4stats' drift-report regression
        (browse/test_elimplus_drift.py, PLUGIN_SOLO_ROUND_UPLOAD): keep the two in step."""
        sunk_hub(self.connection)
        server = self.loaded_server(["p", "q", "s"])                        # three players new to this hub
        server.start_match()
        server.round([("p", 0, 0, "0.0", 0), ("s", 0, 0, "0.0", 0)], [])    # nobody on the other team yet
        self.assertEqual(server.live("p", "s"), {"p": 1400.0, "s": 1400.0})  # the rating system rejected that round
        server.round([("p", 2, 0, "260.0", 0)], [("q", 0, 1, "90.0", 1)])   # q arrives; s sits this one out
        server.end_match()
        payload = server.upload([("p", 0), ("q", 1), ("s", 0)])
        self.assertEqual(payload["baseline_shift"], 362)
        self.assertEqual(len(payload["rounds"]), 1)                         # only the rated round is in the log
        p, q, s = (payload["players"][name] for name in "pqs")
        self.assertEqual((p["pre"], p["rounds_played"]), (1400.0, 2))       # lifetime rounds run ahead of logged ones
        self.assertEqual((q["pre"], q["rounds_played"]), (1400.0, 1))
        self.assertEqual((s["pre"], s["post"], s["rounds_played"]), (1400.0, 1400.0, 1))
        self.assertEqual(row(self.connection, "p")[1], 2)
        self.assertAlmostEqual(row(self.connection, "p")[0], p["post"] - 362, places=5)
        self.assertEqual(row(self.connection, "s"), (1400.0, 0))            # never rated: not saved, its round dropped
        later = self.loaded_server(["p", "s"])
        self.assertAlmostEqual(later.live("p")["p"], p["post"], places=3)
        self.assertEqual(later.live("s")["s"], 1400.0)


class ModelAgreementTests(FlowCase):
    def test_python_model_and_production_code_write_the_same_rows(self):
        """test_elimplus_recentre.Instance replays the same statements in Python. Feed it the movements the real
        rating library produced and it must leave Rating, RoundsPlayed and the baseline row as the production code
        did. One successful match only: it does not compare the other columns or any failure path, so it is a
        check on the model, not proof that the model equals the production code."""
        sunk_hub(self.connection)
        twin = database()
        self.addCleanup(twin.close)
        sunk_hub(twin)
        players = RED + BLUE + ["newcomer", "login-only"]
        server = self.loaded_server(players)
        instance = model.Instance(twin).init_database()
        self.assertEqual((instance.known, instance.shift), (True, 362))
        at_login = server.live(*players)
        for name in players:
            self.assertAlmostEqual(instance.load(name), at_login[name], places=9, msg=name)
        server.start_match()
        roster = four_v_four(server, red=RED[:3] + ["newcomer"], blue=BLUE[:3] + ["login-only"])
        server.end_match()
        payload = server.upload(roster)
        for name, _ in roster:
            instance.play(name, payload["players"][name]["post"] - at_login[name], 6)
        instance.cache = {name: state for name, state in instance.cache.items() if name in dict(roster)}
        instance.flush()
        production = dict((row[0], row[1:]) for row in self.connection.execute(
            "SELECT UniqueId, Rating, RoundsPlayed FROM NCRatingElimPlus"))
        modelled = dict((row[0], row[1:]) for row in twin.execute("SELECT UniqueId, Rating, RoundsPlayed FROM NCRatingElimPlus"))
        self.assertEqual(sorted(production), sorted(modelled))
        for name in production:
            self.assertAlmostEqual(production[name][0], modelled[name][0], places=5, msg=name)
            self.assertEqual(production[name][1], modelled[name][1], name)
        self.assertEqual(baseline_row(self.connection), baseline_row(twin))


if __name__ == "__main__":
    unittest.main()
