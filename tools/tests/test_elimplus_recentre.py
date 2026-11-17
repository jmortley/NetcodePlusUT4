"""ElimPlus recentre guard and hub baseline: the maths, the SQL the hub runs, and the admin guide.

The pure helpers are compiled from the production source against the real TeamGlicko2Config.h. The SQL statements
are lifted out of ElimPlusRatingSystem.cpp and run through sqlite on tables created by the production CREATE
statements, with the engine wrapper's row format reproduced (NULL columns dropped, REAL through a 32-bit float).
`Instance` then replays what a hub instance does with those statements, so that several instances, including
ones from before the baseline existed, can share one database in any order.

`Instance` is a Python MODEL of the C++ control flow: its conversions are its own, so these tests alone would
keep passing if the production load or save went wrong. What the shipped code does is checked in
test_elimplus_rating_flow.py, which compiles ElimPlusRatingSystem.cpp whole and runs it against the same
database helpers. That file also compares this model with the production code, but only for one successful
match and only the Rating and RoundsPlayed columns and the baseline row: a check on the model, not proof that
the two are equivalent. The structure tests here only pin the shape of the source. No sibling plugin or
third-party Python module is required.
"""

import ctypes
import os
from pathlib import Path
import re
import sqlite3
import struct
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


SOURCE = (PLUGIN / "Source/Private/ElimPlusRatingSystem.cpp").read_text(encoding="utf-8-sig")
ADMIN_GUIDE = (PLUGIN / "SERVER-ADMINS.md").read_text(encoding="utf-8-sig")
NOW = 1_795_000_000                      # a fixed "now" for LastSeenUtc arithmetic
DAY = 86400

# What the rule is, written out independently of the C++ so a silent change of either is caught.
GAIN, CAP, MIN_POOL, ESTABLISHED, ACTIVE_DAYS = 0.03, 0.3, 10, 20, 30
SEED = 1400.0


def live_from_stored(stored, rounds, shift):
    return stored + shift if rounds > 0 else stored


def stored_from_live(live, rounds, shift):
    return live - shift if rounds > 0 else live


def call_arguments(source, anchor, opener="FString::Printf("):
    """Top-level arguments of the first `opener` call after `anchor`, as raw text."""
    start = source.index(opener, source.index(anchor)) + len(opener)
    arguments, current, depth, position = [], [], 0, start
    while True:
        character = source[position]
        if character == '"':                                   # skip a string literal whole
            end = position + 1
            while source[end] != '"':
                end += 2 if source[end] == "\\" else 1
            current.append(source[position:end + 1])
            position = end + 1
            continue
        if character == "(":
            depth += 1
        elif character == ")":
            if depth == 0:
                arguments.append("".join(current).strip())
                return arguments
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append("".join(current).strip())
            current = []
            position += 1
            continue
        current.append(character)
        position += 1


def text_literals(fragment):
    """Adjacent TEXT("...") pieces joined into the string they spell."""
    pieces = re.findall(r'TEXT\("((?:[^"\\]|\\.)*)"\)', fragment)
    if not pieces:
        raise AssertionError("No TEXT literal in: " + fragment[:80])
    return "".join(pieces).replace('\\"', '"').replace("\\\\", "\\")


SPECIFIER = re.compile(r"%(?:lld|[-+]?(?:\.\d+)?[dfs])")


def printf_statement(anchor, source=SOURCE):
    """(format string, argument expressions) of the FString::Printf that follows `anchor` in `source`."""
    arguments = call_arguments(source, anchor)
    return text_literals(arguments[0]), arguments[1:]


def literal_statement(anchor, source=SOURCE):
    """A statement spelled as plain TEXT pieces: everything from `anchor` to the end of that C++ statement."""
    start = source.index(anchor)
    return text_literals(source[start:source.index(";\n", start)])


def render(template, values):
    """printf-style substitution, enough for the specifiers these statements use."""
    values = list(values)

    def substitute(match):
        value = values.pop(0)
        spec = match.group(0)
        if spec.endswith("f"):
            return ("%" + spec[1:]) % float(value)
        if spec.endswith("d"):
            return ("%" + spec[1:].replace("ll", "")) % int(value)
        return str(value)

    text = SPECIFIER.sub(substitute, template)
    if values:
        raise AssertionError("Unused values for: " + template)
    return text


def wrapper_rows(connection, sql):
    """Run one statement the way UUTGameInstance::ExecDatabaseCommand hands rows back: text as it is, INTEGER
    through a 32-bit int, REAL through a 32-bit float printed with %f, and NULL columns simply left out."""
    rows = []
    for row in connection.execute(sql):
        columns = []
        for value in row:
            if value is None:
                continue
            if isinstance(value, float):
                columns.append("%f" % struct.unpack("f", struct.pack("f", value))[0])
            elif isinstance(value, int):
                columns.append(str(ctypes.c_int32(value).value))
            else:
                columns.append(str(value))
        rows.append(columns)
    return rows


RATING_TABLE_SQL = literal_statement('TEXT("CREATE TABLE IF NOT EXISTS NCRatingElimPlus (")')
BASELINE_TABLE_SQL = literal_statement('TEXT("CREATE TABLE IF NOT EXISTS NCRatingElimPlusBaseline (")')
LIVE_VIEW_SQL = literal_statement('TEXT("CREATE VIEW IF NOT EXISTS NCRatingElimPlusLive AS ")')
READ_SQL = literal_statement("const FString ReadSql =")
SET_TEMPLATE, SET_ARGUMENTS = printf_statement("const FString SetSql")
POOL_TEMPLATE, POOL_ARGUMENTS = printf_statement("const FString PoolSql")
LOAD = native_function(SOURCE, "void FElimPlusRatingSystem::LoadPlayerFromDB(")
FLUSH = native_function(SOURCE, "void FElimPlusRatingSystem::FlushAtMatchEnd(")
RANK = native_function(SOURCE, "int32 FElimPlusRatingSystem::GetPlayerGlobalRank(")
LOAD_TEMPLATE, LOAD_ARGUMENTS = printf_statement("const FString Sql = FString::Printf(", LOAD)
SEED_TEMPLATE, SEED_ARGUMENTS = printf_statement("const FString InsertSql", LOAD)
FLUSH_TEMPLATE, FLUSH_ARGUMENTS = printf_statement("const FString Sql = FString::Printf(", FLUSH)
FLUSH_V2_TEMPLATE, FLUSH_V2_ARGUMENTS = printf_statement("const FString V2Sql = FString::Printf(", FLUSH)
RANK_TEMPLATE, RANK_ARGUMENTS = printf_statement("const FString Sql = FString::Printf(", RANK)


def database():
    connection = sqlite3.connect(":memory:", isolation_level=None)
    connection.execute(RATING_TABLE_SQL)
    return connection


def add_player(connection, name, rating, rounds, days_ago, now=NOW):
    connection.execute(
        "INSERT INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, LastSeenUtc, TotalPoints, RoundsPlayed, "
        "PlayerName) VALUES (?, ?, 45.0, 0.03, ?, ?, ?, ?)",
        (name, rating, now - days_ago * DAY, rounds * 2.5, rounds, name))


def stored(connection):
    return dict(connection.execute("SELECT UniqueId, Rating FROM NCRatingElimPlus"))


def baseline_row(connection):
    return connection.execute("SELECT Id, Shift, PoolPlayers, PoolMean, CreatedUtc FROM NCRatingElimPlusBaseline").fetchall()


def read_baseline(connection):
    """ReadElimPlusBaseline: (has row, shift, pool players, pool mean), or None when the read cannot be trusted."""
    rows = wrapper_rows(connection, READ_SQL)
    if not rows or len(rows[0]) < 1:
        return None
    if int(rows[0][0]) <= 0:
        return (False, 0, 0, 0.0)
    if len(rows[0]) < 4:
        return None
    return (True, int(rows[0][1]), int(rows[0][2]), float(rows[0][3]))


def set_baseline(connection, now=NOW):
    connection.execute(render(SET_TEMPLATE, [MIN_POOL, SEED, now, MIN_POOL, MIN_POOL, ESTABLISHED,
                                             now - ACTIVE_DAYS * DAY, ESTABLISHED]))


class Instance(object):
    """One hub instance for one map load. aware=False is any build from before the baseline existed: it runs the
    same load and flush statements (they did not change) and knows nothing of the shift."""

    def __init__(self, connection, aware=True, guard=True, now=NOW):
        self.connection, self.aware, self.guard, self.now = connection, aware, guard, now
        self.known, self.shift = False, 0
        self.cache = {}                                         # id -> [live rating, lifetime rounds]

    def init_database(self):                                    # ResolveElimPlusBaseline, statement for statement
        if not self.aware:
            return self
        self.connection.execute(BASELINE_TABLE_SQL)
        state = read_baseline(self.connection)
        if state is not None and not state[0] and self.guard:
            set_baseline(self.connection, self.now)
            state = read_baseline(self.connection)
        self.known = state is not None
        self.shift = state[1] if state and state[0] else 0
        return self

    def load(self, player):                                     # LoadPlayerFromDB
        rows = wrapper_rows(self.connection, render(LOAD_TEMPLATE, [player]))
        if not rows:
            self.connection.execute(render(SEED_TEMPLATE, [player, SEED, 200.0, 0.03, self.now, player]))
            rows = wrapper_rows(self.connection, render(LOAD_TEMPLATE, [player]))
        rounds = int(rows[0][6])
        self.cache[player] = [live_from_stored(float(rows[0][0]), rounds, self.shift), rounds]
        return self.cache[player][0]

    def play(self, player, delta, rounds):                      # ProcessRound + RecordRoundPPR, in bulk
        self.cache[player][0] += delta
        self.cache[player][1] += rounds

    def flush(self):                                            # FlushAtMatchEnd, v3 shape
        for player, (live, rounds) in self.cache.items():
            self.connection.execute(render(FLUSH_TEMPLATE, [
                player, stored_from_live(live, rounds, self.shift), 45.0, 0.03, 0.0, 0, self.now,
                rounds * 2.5, rounds, rounds * 250.0, player]))

    def pool_mean(self):                                        # the live pool mean the step is computed from
        (row,) = wrapper_rows(self.connection, render(POOL_TEMPLATE, [ESTABLISHED, self.now - ACTIVE_DAYS * DAY]))
        return (int(row[0]), float(row[1]) + self.shift) if len(row) >= 2 else (int(row[0]), None)

    def rank(self, player):                                     # GetPlayerGlobalRank
        rows = wrapper_rows(self.connection, render(RANK_TEMPLATE, [self.shift, self.shift, player]))
        return int(rows[0][0]) if rows else 0


def sunk_hub(connection, now=NOW):
    """A ladder that has sunk: 14 regulars seen this fortnight (mean 1038.5), six who left long ago, a casual
    and someone who only ever logged in."""
    for index in range(14):
        add_player(connection, f"active{index}", 980.0 + 9 * index, 900, index, now)
    for index in range(6):
        add_player(connection, f"stranded{index}", 1240.0 + index, 300, 60 + index, now)
    add_player(connection, "casual", 1180.0, 7, 2, now)
    add_player(connection, "login-only", 1400.0, 0, 1, now)


ADAPTER = r'''
#include <cstdint>
#include <cstdio>
using int32 = int32_t;
struct FMath {
    template <class T> static T Clamp(const T X, const T Min, const T Max) { return X < Min ? Min : (X < Max ? X : Max); }
};
#include "TeamGlicko2Config.h"
namespace {
%s
}
int main() {
    const double means[] = {1400.0, 1395.0, 1393.8, 1390.0, 1389.9, 1300.0, 900.0, 1405.0, 1410.0, 1500.0, 1400.5};
    const int32 pools[] = {0, 9, 10, 11, 250};
    for (double mean : means) for (int32 pool : pools)
        std::printf("step %%.17g %%d %%.17g\n", mean, pool, ElimPlusRecentreStepPerRound(mean, pool));
    const double ratings[] = {1400.0, 1263.4, 987.654321, 1650.25, 0.0};
    const int32 rounds[] = {0, 1, 19, 20, 5000};
    const int32 shifts[] = {0, 137, 347, -25};
    for (double rating : ratings) for (int32 played : rounds) for (int32 shift : shifts) {
        const double live = ElimPlusLiveFromStored(rating, played, shift);
        std::printf("convert %%.17g %%d %%d %%.17g %%.17g %%.17g\n", rating, played, shift, live,
            ElimPlusStoredFromLive(rating, played, shift), ElimPlusStoredFromLive(live, played, shift));
    }
    std::printf("constants %%.17g %%.17g %%d %%d %%d\n", kElimPlusRecentreGainPerRound, kElimPlusRecentreCapPerRound,
        kElimPlusRecentreMinPoolPlayers, kElimPlusRecentreEstablishedRounds, kElimPlusRecentreActiveDays);
    return 0;
}
'''


class RecentreMathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-elimplus-recentre-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        constants = re.findall(r"^\tconst (?:double|int32)\s+kElimPlusRecentre\w+\s*=\s*[^;]+;", SOURCE, re.MULTILINE)
        if len(constants) != 5:
            raise AssertionError("Expected five recentre constants, found %d" % len(constants))
        definitions = constants + [native_function(SOURCE, signature) for signature in (
            "double ElimPlusRecentreStepPerRound(", "double ElimPlusLiveFromStored(", "double ElimPlusStoredFromLive(")]
        source = directory / "recentre.cpp"
        source.write_text(ADAPTER % "\n".join(definitions), encoding="utf-8")
        executable = directory / ("recentre.exe" if os.name == "nt" else "recentre")
        include = PLUGIN / "Source/Public/Glicko2"
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", f"/I{include}", str(source),
                       f"/Fe{executable}", f"/Fo{directory / 'recentre.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{include}",
                       str(source), "-o", str(executable)]
        built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=120)
        if built.returncode:
            raise AssertionError(f"Recentre adapter compilation failed:\n{built.stdout}\n{built.stderr}")
        ran = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=30)
        if ran.returncode:
            raise AssertionError(ran.stdout + ran.stderr)
        cls.lines = [line.split() for line in ran.stdout.splitlines()]

    def test_constants_are_the_backtested_rule(self):
        (constants,) = [line[1:] for line in self.lines if line[0] == "constants"]
        self.assertEqual([float(constants[0]), float(constants[1])] + [int(value) for value in constants[2:]],
                         [GAIN, CAP, MIN_POOL, ESTABLISHED, ACTIVE_DAYS])

    def test_step_moves_toward_the_seed_and_is_capped(self):
        steps = [(float(m), int(n), float(s)) for kind, m, n, s in (line for line in self.lines if line[0] == "step")]
        self.assertEqual(len(steps), 55)
        for mean, pool, step in steps:
            expected = 0.0 if pool < MIN_POOL else max(-CAP, min(CAP, GAIN * (SEED - mean)))
            self.assertAlmostEqual(step, expected, places=12, msg=f"mean {mean} pool {pool}")
        by_case = {(mean, pool): step for mean, pool, step in steps}
        self.assertEqual(by_case[(1400.0, 250)], 0.0)
        self.assertAlmostEqual(by_case[(1393.8, 250)], 0.186, places=9)      # the example in SERVER-ADMINS
        self.assertEqual(by_case[(900.0, 250)], CAP)                         # a sunk pool cannot run away
        self.assertEqual(by_case[(1500.0, 250)], -CAP)
        self.assertEqual(by_case[(900.0, 9)], 0.0)                           # nine players are not a pool

    def test_baseline_goes_on_when_read_and_comes_off_when_written(self):
        cases = [line[1:] for line in self.lines if line[0] == "convert"]
        self.assertEqual(len(cases), 100)
        for rating, played, shift, live, back, trip in cases:
            rating, played, shift = float(rating), int(played), int(shift)
            self.assertAlmostEqual(float(live), live_from_stored(rating, played, shift), places=9)
            self.assertAlmostEqual(float(back), stored_from_live(rating, played, shift), places=9)
            self.assertAlmostEqual(float(trip), rating, places=9, msg="write(read(x)) must be x")
            if played == 0 or shift == 0:
                self.assertEqual(float(live), rating)                        # a never-played seed row is not shifted
            else:
                self.assertAlmostEqual(float(live) - rating, shift, places=9)


class BaselineSqlTests(unittest.TestCase):
    def setUp(self):
        self.connection = database()
        self.addCleanup(self.connection.close)

    def test_statements_take_the_arguments_the_tests_feed_them(self):
        self.assertEqual(SET_ARGUMENTS, [
            "kElimPlusRecentreMinPoolPlayers", "TeamGlicko2::kDefaultRating", "static_cast<long long>(NowUtc)",
            "kElimPlusRecentreMinPoolPlayers", "kElimPlusRecentreMinPoolPlayers", "kElimPlusRecentreEstablishedRounds",
            "static_cast<long long>(NowUtc - static_cast<int64>(kElimPlusRecentreActiveDays) * 86400)",
            "kElimPlusRecentreEstablishedRounds"])
        self.assertEqual(POOL_ARGUMENTS[0], "kElimPlusRecentreEstablishedRounds")
        self.assertIn("kElimPlusRecentreActiveDays", POOL_ARGUMENTS[1])
        self.assertIn("86400", POOL_ARGUMENTS[1])
        self.assertEqual(LOAD_ARGUMENTS, ["*Esc"])
        self.assertEqual(RANK_ARGUMENTS, ["Impl->BaselineShift", "Impl->BaselineShift", "*Esc"])
        for template, arguments in ((SET_TEMPLATE, SET_ARGUMENTS), (POOL_TEMPLATE, POOL_ARGUMENTS),
                                    (LOAD_TEMPLATE, LOAD_ARGUMENTS), (SEED_TEMPLATE, SEED_ARGUMENTS),
                                    (FLUSH_TEMPLATE, FLUSH_ARGUMENTS), (FLUSH_V2_TEMPLATE, FLUSH_V2_ARGUMENTS),
                                    (RANK_TEMPLATE, RANK_ARGUMENTS)):
            self.assertEqual(len(SPECIFIER.findall(template)), len(arguments), template)

    def test_sunk_hub_is_measured_once_and_its_rows_are_left_alone(self):
        sunk_hub(self.connection)
        before = stored(self.connection)
        instance = Instance(self.connection).init_database()
        # The 14 regulars average 1038.5: half a point rounds away from zero.
        self.assertEqual(baseline_row(self.connection), [(1, 362, 14, 1038.5, NOW)])
        self.assertEqual((instance.known, instance.shift), (True, 362))
        self.assertEqual(stored(self.connection), before)                  # not one stored rating rewritten
        self.assertEqual(self.connection.execute(
            "SELECT typeof(Shift), typeof(MAX(Shift)) FROM NCRatingElimPlusBaseline").fetchone(), ("integer", "integer"))
        self.assertEqual(wrapper_rows(self.connection, READ_SQL), [["1", "362", "14", "1038.500000"]])
        players, mean = instance.pool_mean()
        self.assertEqual(players, 14)
        self.assertAlmostEqual(mean, 1400.5, places=3)                      # the guard starts from the seed

    def test_baseline_is_never_measured_twice(self):
        sunk_hub(self.connection)
        Instance(self.connection).init_database()
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating - 200")     # whatever happens later
        later = Instance(self.connection, now=NOW + 40 * DAY).init_database()
        self.assertEqual(later.shift, 362)
        self.assertEqual(baseline_row(self.connection), [(1, 362, 14, 1038.5, NOW)])

    def test_two_instances_starting_together_agree(self):
        sunk_hub(self.connection)
        self.connection.execute(BASELINE_TABLE_SQL)
        self.assertEqual(read_baseline(self.connection), (False, 0, 0, 0.0))             # both read "none"...
        set_baseline(self.connection)                                                    # ...the first one sets it
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating + 50")      # a flush lands in between
        set_baseline(self.connection, NOW + 5)                                           # the second is ignored
        self.assertEqual(read_baseline(self.connection), (True, 362, 14, 1038.5))
        self.assertEqual(len(baseline_row(self.connection)), 1)

    def test_quiet_hub_is_measured_on_every_established_player(self):
        for index in range(9):
            add_player(self.connection, f"recent{index}", 1000.0, 500, 1)    # nine recent: not a pool on their own
        for index in range(6):
            add_player(self.connection, f"old{index}", 1150.0, 500, 200)
        self.assertEqual(Instance(self.connection).init_database().shift, 340)
        self.assertEqual(baseline_row(self.connection), [(1, 340, 15, 1060.0, NOW)])

    def test_pool_above_the_seed_gives_a_negative_baseline(self):
        for index in range(12):
            add_player(self.connection, f"p{index}", 1412.4, 100, 1)
        self.assertEqual(Instance(self.connection).init_database().shift, -12)

    def test_new_reset_or_tiny_ladder_records_zero_and_is_not_measured_later(self):
        add_player(self.connection, "login-only", 1400.0, 0, 0)
        for index in range(9):
            add_player(self.connection, f"few{index}", 1100.0, 800, 1)       # nine sunk regulars: too few to measure
        first = Instance(self.connection).init_database()
        self.assertEqual((first.known, first.shift), (True, 0))
        self.assertEqual(baseline_row(self.connection), [(1, 0, 9, 1100.0, NOW)])
        for index in range(20):                                             # the ladder grows...
            add_player(self.connection, f"more{index}", 1460.0, 60, 1)
        self.assertEqual(Instance(self.connection).init_database().shift, 0)             # ...and nobody is moved
        empty = database()
        self.addCleanup(empty.close)
        self.assertEqual(Instance(empty).init_database().shift, 0)
        self.assertEqual(baseline_row(empty), [(1, 0, 0, 0.0, NOW)])        # AVG() of nothing is stored as 0, not NULL

    def test_switched_off_guard_sets_nothing_but_keeps_an_existing_baseline(self):
        sunk_hub(self.connection)
        off = Instance(self.connection, guard=False).init_database()
        self.assertEqual((off.known, off.shift), (True, 0))
        self.assertEqual(baseline_row(self.connection), [])
        self.assertEqual(off.load("active0"), 980.0)                        # ratings exactly as stored
        Instance(self.connection).init_database()
        self.assertEqual(Instance(self.connection, guard=False).init_database().shift, 362)

    def test_unreadable_baseline_is_not_trusted(self):
        self.assertIsNone(read_baseline_from([]))                           # the statement returned nothing
        self.assertIsNone(read_baseline_from([[]]))
        self.assertEqual(read_baseline_from([["0"]]), (False, 0, 0, 0.0))
        self.assertIsNone(read_baseline_from([["1", "137"]]))               # a row with empty columns
        self.assertEqual(read_baseline_from([["1", "137", "138", "1263.400024"]])[:3], (True, 137, 138))

    def test_load_and_flush_round_trip_through_the_stored_column(self):
        sunk_hub(self.connection)
        first = Instance(self.connection).init_database()
        self.assertAlmostEqual(first.load("active3"), 1007.0 + 362, places=3)
        self.assertEqual(first.load("login-only"), 1400.0)                  # never played: the untouched seed
        self.assertEqual(first.load("brand-new"), 1400.0)                   # no row: seeded, also untouched
        first.play("active3", -4.25, 12)
        first.play("login-only", +31.5, 9)
        first.play("brand-new", -18.0, 9)
        first.flush()
        rows = stored(self.connection)
        self.assertAlmostEqual(rows["active3"], 1007.0 - 4.25, places=3)    # stored moved by the match only
        self.assertAlmostEqual(rows["login-only"], 1431.5 - 362, places=6)  # first played row: baseline taken off
        self.assertAlmostEqual(rows["brand-new"], 1382.0 - 362, places=6)
        second = Instance(self.connection).init_database()
        self.assertAlmostEqual(second.load("active3"), 1007.0 + 362 - 4.25, places=3)
        self.assertAlmostEqual(second.load("login-only"), 1431.5, places=3)
        self.assertAlmostEqual(second.load("brand-new"), 1382.0, places=3)

    def test_flush_statements_are_the_shapes_older_builds_write(self):
        for template, arguments, version, count in ((FLUSH_TEMPLATE, FLUSH_ARGUMENTS, "3", 11),
                                                    (FLUSH_V2_TEMPLATE, FLUSH_V2_ARGUMENTS, "2", 9)):
            self.assertEqual(len(arguments), count)
            self.assertEqual(arguments[:2], ["*Esc", "StoredRating"])       # never the live rating
            self.assertNotIn("PR.GetRating()", arguments)
            self.assertTrue(template.rstrip().endswith(", %s);" % version), template)
        self.connection.execute(render(FLUSH_TEMPLATE, [
            "abc", 1412.5, 44.0, 0.03, 0.0, 0, NOW, 812.25, 320, 91000.0, "Player One"]))
        self.assertEqual(self.connection.execute(
            "SELECT Rating, RoundsPlayed, PlayerName, SchemaVersion FROM NCRatingElimPlus WHERE UniqueId='abc'"
        ).fetchone(), (1412.5, 320, "Player One", 3))
        self.connection.execute(render(FLUSH_V2_TEMPLATE, ["abc", 1399.0, 44.0, 0.03, 0.0, 0, NOW, 815.0, 321]))
        self.assertEqual(self.connection.execute(
            "SELECT Rating, RoundsPlayed, SchemaVersion FROM NCRatingElimPlus WHERE UniqueId='abc'"
        ).fetchone(), (1399.0, 321, 2))

    def test_rank_compares_live_ratings(self):
        add_player(self.connection, "seed-row", 1400.0, 0, 0)               # logged in once, never played
        add_player(self.connection, "regular", 1300.0, 500, 1)
        add_player(self.connection, "top", 1500.0, 500, 1)
        plain = Instance(self.connection, aware=False)
        self.assertEqual([plain.rank(name) for name in ("top", "seed-row", "regular")], [1, 2, 3])
        lifted = Instance(self.connection)
        lifted.shift = 137                                                  # regular is 1437 live: above the seed row
        self.assertEqual([lifted.rank(name) for name in ("top", "regular", "seed-row")], [1, 2, 3])
        self.assertEqual(lifted.rank("nobody"), 0)

    def test_live_view_shows_what_the_game_shows(self):
        sunk_hub(self.connection)
        self.connection.execute(BASELINE_TABLE_SQL)
        self.connection.execute(LIVE_VIEW_SQL)
        self.connection.execute(LIVE_VIEW_SQL)                              # IF NOT EXISTS: every map load runs it
        columns = [row[1] for row in self.connection.execute("PRAGMA table_info(NCRatingElimPlusLive)")]
        self.assertEqual(columns[:3], ["UniqueId", "PlayerName", "Rating"])
        self.assertEqual(columns[-1], "StoredRating")
        view = "SELECT UniqueId, Rating, StoredRating FROM NCRatingElimPlusLive"
        for name, live, kept in self.connection.execute(view):
            self.assertEqual((live, kept), (stored(self.connection)[name],) * 2)          # no baseline yet
        instance = Instance(self.connection).init_database()
        for name, live, kept in self.connection.execute(view):
            self.assertEqual(kept, stored(self.connection)[name])
            self.assertEqual(live, kept if name == "login-only" else kept + 362, name)
            self.assertAlmostEqual(live, instance.load(name), places=3, msg=name)        # the game and the view agree


def read_baseline_from(rows):
    """read_baseline on rows the wrapper might hand back, without a database."""
    class Canned(object):
        def execute(self, sql):
            return [tuple(row) for row in rows]
    return read_baseline(Canned())


class OlderBuildCoexistenceTests(unittest.TestCase):
    """The reason the baseline is applied on read. An instance with players in it outlives a hub restart, so a
    build from before the baseline can still be finishing a match when the first instance of this build starts."""

    def setUp(self):
        self.connection = database()
        self.addCleanup(self.connection.close)
        sunk_hub(self.connection)

    def test_match_in_flight_on_an_older_build_loses_nothing(self):
        old = Instance(self.connection, aware=False).init_database()        # match started before the update
        self.assertEqual(old.load("active0"), 980.0)
        self.assertEqual(old.load("active1"), 989.0)
        new = Instance(self.connection).init_database()                     # first instance of this build
        self.assertEqual(new.shift, 362)
        self.assertAlmostEqual(new.load("active2"), 998.0 + 362, places=3)
        old.play("active0", +6.5, 14)
        old.play("active1", -9.0, 14)
        new.play("active2", +3.0, 11)
        old.flush()                                                         # saved after the baseline was set
        new.flush()
        after = Instance(self.connection).init_database()
        self.assertAlmostEqual(after.load("active0"), 980.0 + 6.5 + 362, places=3)        # not 362 low
        self.assertAlmostEqual(after.load("active1"), 989.0 - 9.0 + 362, places=3)
        self.assertAlmostEqual(after.load("active2"), 998.0 + 3.0 + 362, places=3)

    def test_older_instance_that_keeps_running_stays_consistent(self):
        Instance(self.connection).init_database()
        old = Instance(self.connection, aware=False).init_database()        # loads AFTER the baseline was set
        self.assertEqual(old.load("active5"), 1025.0)                       # an older build shows the stored value
        old.play("active5", +10.0, 13)
        old.flush()
        new = Instance(self.connection).init_database()
        self.assertAlmostEqual(new.load("active5"), 1035.0 + 362, places=3)                # not shifted twice
        new.play("active5", -2.0, 10)
        new.flush()
        again = Instance(self.connection, aware=False).init_database()
        self.assertAlmostEqual(again.load("active5"), 1033.0, places=3)     # and back: a rollback is clean
        self.assertAlmostEqual(Instance(self.connection).init_database().load("active5"), 1033.0 + 362, places=3)

    def test_instance_mid_match_when_the_baseline_is_measured_again(self):
        first = Instance(self.connection).init_database()
        first.load("active7")
        self.connection.execute("DELETE FROM NCRatingElimPlusBaseline")     # the admin asks for a fresh measurement
        self.connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating + 20 WHERE UniqueId LIKE 'active%'")
        second = Instance(self.connection, now=NOW + DAY).init_database()
        self.assertEqual(second.shift, 342)
        first.play("active7", +5.0, 12)
        first.flush()                                                       # saved with the shift it loaded with
        # Stored 1043 when it loaded, +5 in the match: what the row would hold had no baseline ever existed.
        self.assertAlmostEqual(stored(self.connection)["active7"], 1048.0, places=3)
        self.assertAlmostEqual(Instance(self.connection).init_database().load("active7"), 1048.0 + 342, places=3)


class RecentreStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.init = native_function(SOURCE, "bool FElimPlusRatingSystem::InitDatabase(")
        cls.resolve = native_function(SOURCE, "void ResolveElimPlusBaseline(")
        cls.read = native_function(SOURCE, "bool ReadElimPlusBaseline(")
        cls.payload = native_function(SOURCE, "FString FElimPlusRatingSystem::BuildResultPayload(")
        cls.snapshot = native_function(SOURCE, "void FElimPlusRatingSystem::SnapshotMatchStart(")

    def test_baseline_is_settled_before_any_rating_is_read(self):
        self.assertEqual(self.init.count("ResolveElimPlusBaseline(World, *Impl);"), 1)
        self.assertLess(self.init.index("PRAGMA table_info(NCRatingElimPlus);"),
                        self.init.index("ResolveElimPlusBaseline(World, *Impl);"))
        retry = LOAD.index("if (!Impl->bBaselineKnown && Impl->RatingCache.Num() == 0)")
        self.assertLess(LOAD.index("if (Impl->RatingCache.Contains(UniqueId)) return;"), retry)
        self.assertLess(retry, LOAD.index("ResolveElimPlusBaseline(World, *Impl);"))
        self.assertLess(LOAD.index("ResolveElimPlusBaseline(World, *Impl);"), LOAD.index("auto TryLoadRow"))
        # Nothing but the resolver ever changes the three fields.
        for field in ("bBaselineKnown", "bHasBaseline", "BaselineShift"):
            self.assertEqual(len(re.findall(r"\b%s\s*=[^=]" % field, SOURCE)), 3, field)   # declaration + two in resolve
            self.assertEqual(len(re.findall(r"Impl\.%s\s*=[^=]" % field, self.resolve)), 2, field)

    def test_resolver_follows_the_model_used_by_these_tests(self):
        self.assertLess(self.resolve.index("Impl.bBaselineKnown = false;"), self.resolve.index("CREATE TABLE IF NOT EXISTS"))
        self.assertIn("bool bRead   = ReadElimPlusBaseline(World, bHasRow, Shift, PoolPlayers, PoolMean);", self.resolve)
        self.assertIn("if (bRead && !bHasRow && bGuardEnabled)", self.resolve)
        self.assertIn("bRead   = ExecSqlNoRows(World, SetSql) && ReadElimPlusBaseline(World, bHasRow, Shift, PoolPlayers, PoolMean);",
                      self.resolve)
        failed = self.resolve.index("if (!bRead)")
        self.assertLess(failed, self.resolve.index("Impl.bBaselineKnown = true;"))
        self.assertIn("return;", self.resolve[failed:self.resolve.index("Impl.bBaselineKnown = true;")])
        self.assertIn("Impl.BaselineShift  = bHasRow ? Shift : 0;", self.resolve)
        self.assertIn("Impl.bHasBaseline   = bHasRow;", self.resolve)
        self.assertLess(self.resolve.index("Impl.bHasBaseline   = false;"), self.resolve.index("CREATE TABLE IF NOT EXISTS"))
        self.assertIn("const bool bGuardEnabled = CVarElimPlusRecentre.GetValueOnGameThread() != 0;", self.resolve)
        self.assertIn("if (FCString::Atoi(*Rows[0].Text[0]) <= 0)", self.read)
        self.assertIn("if (Rows[0].Text.Num() < 4)", self.read)
        self.assertIn('TEXT("ncp.ElimPlusRecentre"), 1,', SOURCE)

    def test_one_conversion_each_way(self):
        self.assertEqual(SOURCE.count("ElimPlusLiveFromStored(StoredRating, RoundsPlayed, Impl->BaselineShift)"), 1)
        self.assertEqual(LOAD.count("ElimPlusLiveFromStored("), 1)
        self.assertIn("TeamGlicko2::PlayerRating PR(Rating, RD, Sigma);", LOAD)
        self.assertEqual(SOURCE.count("ElimPlusStoredFromLive(PR.GetRating(), RoundsPlayed, Impl->BaselineShift)"), 1)
        convert = FLUSH.index("ElimPlusStoredFromLive(")
        self.assertEqual(FLUSH.count("ElimPlusStoredFromLive("), 1)
        # The stored value is taken from the final live rating: after the recentre step and the bot-match clamp,
        # and with the RoundsPlayed that goes into the same row.
        self.assertLess(FLUSH.index("PR.SetRating(PR.GetRating() + RecentreAdd);"), convert)
        self.assertLess(FLUSH.index("PR.SetRating(static_cast<double>(FinalElo));"), convert)
        self.assertLess(FLUSH.index("const int32  RoundsPlayed = Impl->RoundsPlayedCache.FindRef(UniqueId);"), convert)
        self.assertLess(convert, FLUSH.index("const FString Sql = FString::Printf("))
        self.assertEqual(len(re.findall(r"PR\.SetRating\(", FLUSH[convert:])), 0)

    def test_step_needs_the_switch_a_hub_with_its_baseline_and_a_live_match(self):
        # bHasBaseline, not just the switch: a guard switched on mid-map on a hub that has not been measured
        # yet must not start stepping toward 1400 from wherever that ladder has sunk to.
        self.assertIn("const bool bRecentre = Impl->bHasBaseline && Impl->bMatchActive && "
                      "CVarElimPlusRecentre.GetValueOnGameThread() != 0;", FLUSH)
        self.assertLess(FLUSH.index("const bool bRecentre ="), FLUSH.index("Impl->bMatchActive = false;"))
        self.assertIn("FCString::Atod(*PoolRows[0].Text[1]) + static_cast<double>(Impl->BaselineShift);", FLUSH)

    def test_step_is_applied_once_before_the_rating_is_read(self):
        step = FLUSH.index("PR.SetRating(PR.GetRating() + RecentreAdd);")
        self.assertEqual(FLUSH.count("PR.SetRating(PR.GetRating() + RecentreAdd);"), 1)
        self.assertLess(step, FLUSH.index("const int32 NewEloRaw = FMath::RoundToInt(PR.GetRating());"))
        self.assertLess(FLUSH.index("Impl->LoadFailedGuard.Contains(UniqueId)"), step)
        self.assertLess(FLUSH.index("!Impl->ActiveHumansThisMatch.Contains(UniqueId)"), step)
        guard = FLUSH[FLUSH.rindex("if (bRecentre", 0, step):step]
        self.assertIn("Impl->HumansWithHumanOpposition.Contains(UniqueId)", guard)      # never on a bots-only match
        self.assertIn("Impl->RecentreStepThisMatch * static_cast<double>(RoundsThisMatch.FindRef(UniqueId))", guard)
        self.assertIn("Impl->RecentreThisMatch.Add(UniqueId, RecentreAdd);", FLUSH)

    def test_rounds_come_from_the_round_log_and_state_is_reset(self):
        self.assertIn("for (const FElimPlusRoundRecord& Rec : Impl->RoundLog)", FLUSH)
        self.assertIn("RoundsThisMatch.FindOrAdd(RoundPlayer.UniqueId) += 1;", FLUSH)
        for function in (FLUSH, self.snapshot):
            self.assertIn("Impl->RecentreThisMatch.Empty();", function)
            self.assertIn("Impl->RecentreStepThisMatch = 0.0;", function)
        self.assertLess(FLUSH.index("Impl->RecentreThisMatch.Empty();"), FLUSH.index("const bool bRecentre ="))

    def test_upload_reports_the_step_and_the_baseline(self):
        self.assertEqual(self.payload.count('Writer->WriteValue(TEXT("recentre_per_round"), Impl->RecentreStepThisMatch);'), 1)
        self.assertEqual(self.payload.count('Writer->WriteValue(TEXT("baseline_shift"), Impl->BaselineShift);'), 1)
        self.assertEqual(self.payload.count('Writer->WriteValue(TEXT("recentre"),'), 2)   # rostered players + leavers
        self.assertIn("Impl->RecentreThisMatch.FindRef(P.UniqueId)", self.payload)
        self.assertIn("Impl->RecentreThisMatch.FindRef(Id)", self.payload)

    def test_view_waits_for_a_confirmed_v3_table(self):
        view = self.init.index("CREATE VIEW IF NOT EXISTS NCRatingElimPlusLive")
        self.assertLess(self.init.index("ResolveElimPlusBaseline(World, *Impl);"), view)
        self.assertIn("if (Impl->Schema == FElimPlusRatingSystemImpl::EDbSchema::V3)",
                      self.init[self.init.index("ResolveElimPlusBaseline(World, *Impl);"):view])


class AdminGuideTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        start = ADMIN_GUIDE.index("### 12.3 ElimPlus ratings")
        cls.section = ADMIN_GUIDE[start:ADMIN_GUIDE.index("\n## ", start)]
        marker = cls.section.index("<!-- elimplus-admin-sql")
        cls.blocks = re.findall(r"```sql\n(.*?)```", cls.section[marker:], re.DOTALL)

    def hub(self):
        connection = database()
        self.addCleanup(connection.close)
        sunk_hub(connection)
        self.assertEqual(Instance(connection).init_database().shift, 362)
        return connection

    def test_reset_block_starts_a_clean_ladder(self):
        self.assertEqual(len(self.blocks), 2)
        connection = self.hub()
        connection.executescript(self.blocks[0])
        self.assertEqual((stored(connection), baseline_row(connection)), ({}, []))
        fresh = Instance(connection).init_database()
        self.assertEqual(fresh.shift, 0)
        self.assertEqual(fresh.load("returning"), 1400.0)
        fresh.play("returning", +12.0, 9)
        fresh.flush()
        self.assertAlmostEqual(stored(connection)["returning"], 1412.0, places=6)         # stored is live again

    def test_measure_again_block_takes_the_ratings_as_stored(self):
        connection = self.hub()
        connection.execute("UPDATE NCRatingElimPlus SET Rating = Rating - 30 WHERE UniqueId LIKE 'active%'")
        connection.executescript(self.blocks[1])
        self.assertEqual(Instance(connection).init_database().shift, 392)

    def test_guide_quotes_the_names_and_log_lines_the_code_uses(self):
        self.assertIn("| `ncp.ElimPlusRecentre` | `1` |", ADMIN_GUIDE)
        for name in ("NCRatingElimPlusLive", "NCRatingElimPlusBaseline", "StoredRating"):
            self.assertIn(name, self.section)
            self.assertIn(name, SOURCE)
        formats = dict(
            first='TEXT("Rating baseline set for this hub: %+d, measured on %d established players averaging %.1f. ',
            later='TEXT("Rating baseline %+d (measured on %d established players averaging %.1f); %s")',
            step='TEXT("Recentre: pool mean %.1f over %d established players seen in the last %d days -> %+.3f per round")')
        for text in formats.values():
            self.assertIn(text, SOURCE)
        self.assertIn('TEXT("recentre guard ON")', SOURCE)
        self.assertIn("Rating baseline set for this hub: +137, measured on 138 established players averaging 1263.4. ",
                      self.section)
        self.assertIn(render(text_literals(formats["later"]), [137, 138, 1263.4, "recentre guard ON"]), self.section)
        self.assertIn(render(text_literals(formats["step"]), [1393.8, 142, ACTIVE_DAYS, 0.186]), self.section)
        self.assertIn("SELECT PlayerName, ROUND(Rating), RoundsPlayed FROM NCRatingElimPlusLive WHERE RoundsPlayed >= 20 "
                      "ORDER BY Rating DESC;", self.section)

    def test_ladder_query_in_the_guide_runs(self):
        connection = self.hub()
        connection.execute(LIVE_VIEW_SQL)
        rows = connection.execute("SELECT PlayerName, ROUND(Rating), RoundsPlayed FROM NCRatingElimPlusLive "
                                  "WHERE RoundsPlayed >= 20 ORDER BY Rating DESC;").fetchall()
        self.assertEqual(len(rows), 20)
        self.assertEqual(rows[0], ("stranded5", 1245.0 + 362, 300))


if __name__ == "__main__":
    unittest.main()
