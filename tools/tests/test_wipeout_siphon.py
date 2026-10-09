"""Compile the production Siphon damage callback against small Unreal adapters.

These cases cover healing amounts and guards. Blueprint asset defaults, engine
damage dispatch, replication, and pickup lifetime still need an engine playtest.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
using int32 = int32_t;
using uint64 = uint64_t;
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a, b); }
    template<class T> static T Clamp(T a, T low, T high) {
        return std::max(low, std::min(a, high));
    }
    static int32 CeilToInt(float value) { return static_cast<int32>(std::ceil(value)); }
    static bool IsFinite(float value) { return std::isfinite(value); }
};
template<class K, class V> struct TestMap {
    std::map<K, V> Values;
    bool Contains(K key) const { return Values.count(key) != 0; }
    void Add(K key, V value) { Values[key] = value; }
    V& operator[](K key) { return Values[key]; }
    V& FindOrAdd(K key) { return Values[key]; }
};
struct AUTSiphonPowerup {
    float SiphonPercent = 0.75f;
    int32 HealCap = 199;
    static void* StaticClass() { return nullptr; }
};
struct AUTCharacter {
    int32 Health = 100, HealthUpdates = 0;
    bool Dead = false, PendingKill = false;
    AUTSiphonPowerup* Inventory = nullptr;
    // Stock IsDead does not inspect Health, so the production callback must.
    bool IsDead() const { return Dead || PendingKill; }
    bool IsPendingKillPending() const { return PendingKill; }
    template<class T> T* FindInventoryType(void*, bool) { return Inventory; }
    void OnHealthUpdated() { ++HealthUpdates; }
};
struct TeamInfo { int32 TeamIndex = 0; };
struct AUTPlayerState {
    TeamInfo* Team = nullptr;
    AUTCharacter* Character = nullptr;
    uint64 Identity = 0;
    AUTCharacter* GetUTCharacter() { return Character; }
};
struct TestGameState {
    bool OnSameTeam(AUTPlayerState* a, AUTPlayerState* b) const {
        return a->Team && b->Team && a->Team->TeamIndex != 255
            && b->Team->TeamIndex != 255 && a->Team->TeamIndex == b->Team->TeamIndex;
    }
};
struct TestBaseGame {
    void ScoreDamage_Implementation(int32, AUTPlayerState*, AUTPlayerState*) {}
};
struct AUWipeoutGame : TestBaseGame {
    using Super = TestBaseGame;
    TestGameState* UTGameState = nullptr;
    bool bRoundInProgress = true;
    TestMap<AUTPlayerState*, float> PlayerRoundDamage;
    TestMap<uint64, int32> LifeDamageMap;
    int32 Team0RoundDamage = 0, Team1RoundDamage = 0;
    uint64 MakeDamagePairKey(AUTPlayerState* from, AUTPlayerState* to) {
        return (from->Identity << 32) | to->Identity;
    }
    void ScoreDamage_Implementation(int32, AUTPlayerState*, AUTPlayerState*);
};
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    AUWipeoutGame Game;
    TestGameState State;
    TeamInfo AttackerTeam, VictimTeam;
    AUTPlayerState Attacker, Victim;
    AUTCharacter AttackerPawn, VictimPawn;
    AUTSiphonPowerup Siphon;
    Fixture() {
        Game.UTGameState = &State;
        VictimTeam.TeamIndex = 1;
        Attacker.Team = &AttackerTeam; Victim.Team = &VictimTeam;
        Attacker.Character = &AttackerPawn; Victim.Character = &VictimPawn;
        Attacker.Identity = 1; Victim.Identity = 2;
        AttackerPawn.Inventory = &Siphon;
    }
    void Hit(int32 amount) { Game.ScoreDamage_Implementation(amount, &Victim, &Attacker); }
};
void ProportionalHealing() {
    Fixture one; one.Hit(1);
    Require(one.AttackerPawn.Health == 101, "one damage filled health instead of healing one");
    Require(one.AttackerPawn.HealthUpdates == 1, "one-point healing missed health notification");
    Fixture hundred; hundred.Hit(100);
    Require(hundred.AttackerPawn.Health == 175, "100 damage must heal 75");
    Require(hundred.Game.PlayerRoundDamage.Values[&hundred.Attacker] == 100.f,
        "siphon percentage altered credited damage");
    Fixture rounding; rounding.Hit(2);
    Require(rounding.AttackerPawn.Health == 102, "existing ceiling rounding changed");
}
void HealthCap() {
    for (int32 before : {198, 199, 225}) {
        Fixture f; f.AttackerPawn.Health = before; f.Hit(100);
        const int32 expected = before < 199 ? 199 : before;
        Require(f.AttackerPawn.Health == expected, "siphon exceeded cap or reduced existing overhealth");
        Require(f.AttackerPawn.HealthUpdates == (before < 199), "cap emitted wrong notification");
    }
}
void FractionBounds() {
    for (float configured : {0.75f, 75.f}) {
        Fixture one; one.Siphon.SiphonPercent = configured; one.Hit(1);
        Require(one.AttackerPawn.Health == 101, "legacy percent-style value caused one-point overhealing");
        Fixture hundred; hundred.Siphon.SiphonPercent = configured; hundred.Hit(100);
        Require(hundred.AttackerPawn.Health == 175, "legacy percent-style 75 must equal fraction 0.75");
    }
    Fixture oversized; oversized.AttackerPawn.Health = 10;
    oversized.Siphon.SiphonPercent = 200.f; oversized.Hit(100);
    Require(oversized.AttackerPawn.Health == 110, "values above 100 percent must be capped");
    for (float fraction : {-2.f, 0.f, std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()}) {
        Fixture f; f.Siphon.SiphonPercent = fraction; f.Hit(100);
        Require(f.AttackerPawn.Health == 100 && f.AttackerPawn.HealthUpdates == 0,
            "nonpositive or nonfinite siphon fraction changed health");
    }
}
void DamageBounds() {
    for (int32 amount : {300, 301, 100000}) {
        Fixture f; f.AttackerPawn.Health = 1; f.Siphon.SiphonPercent = 0.5f; f.Hit(amount);
        Require(f.AttackerPawn.Health == 151, "healing used unbounded damage instead of 300-point credit");
        Require(f.Game.PlayerRoundDamage.Values[&f.Attacker] == 300.f,
            "damage credit escaped existing 300-point bound");
    }
}
void RejectedDamage() {
    for (int scenario = 0; scenario < 8; ++scenario) {
        Fixture f; int32 amount = 100;
        AUTPlayerState* victim = &f.Victim; AUTPlayerState* attacker = &f.Attacker;
        if (scenario == 0) amount = 0;
        if (scenario == 1) amount = -5;
        if (scenario == 2) f.Game.bRoundInProgress = false;
        if (scenario == 3) f.VictimTeam.TeamIndex = f.AttackerTeam.TeamIndex;
        if (scenario == 4) f.Attacker.Team = nullptr;
        if (scenario == 5) victim = nullptr;
        if (scenario == 6) attacker = nullptr;
        if (scenario == 7) f.Game.UTGameState = nullptr;
        f.Game.ScoreDamage_Implementation(amount, victim, attacker);
        Require(f.AttackerPawn.Health == 100 && f.AttackerPawn.HealthUpdates == 0,
            "invalid damage context healed attacker");
        Require(f.Game.PlayerRoundDamage.Values.empty(), "invalid damage context earned round damage");
    }
    Fixture self;
    // Stock OnSameTeam returns false for unassigned team 255, even for the
    // same player. This exercises the explicit self guard independently.
    self.AttackerTeam.TeamIndex = 255;
    self.Game.ScoreDamage_Implementation(100, &self.Attacker, &self.Attacker);
    Require(self.AttackerPawn.Health == 100 && self.AttackerPawn.HealthUpdates == 0,
        "self damage healed attacker");
}
void AttackerGuards() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        Fixture f;
        if (scenario == 0) f.AttackerPawn.Dead = true;
        if (scenario == 1) f.AttackerPawn.PendingKill = true;
        if (scenario == 2) f.AttackerPawn.Health = 0;
        if (scenario == 3) f.AttackerPawn.Health = -1;
        if (scenario == 4) f.AttackerPawn.Inventory = nullptr;
        if (scenario == 5) f.Attacker.Character = nullptr;
        const int32 before = f.AttackerPawn.Health; f.Hit(100);
        Require(f.AttackerPawn.Health == before && f.AttackerPawn.HealthUpdates == 0,
            "dead, dying, absent or unpowered attacker healed");
    }
}
void KillingBlow() {
    Fixture f;
    // TakeDamage has already subtracted HP and stripped overkill from its
    // ScoreDamage argument. A 100-point hit on 10 HP therefore supplies 10.
    f.VictimPawn.Health = -90; f.Hit(10);
    Require(f.AttackerPawn.Health == 108, "killing blow lost healing from consumed victim health");
    Require(f.Game.PlayerRoundDamage.Values[&f.Attacker] == 10.f,
        "killing blow reintroduced overkill or used remaining victim health");
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "proportional") ProportionalHealing();
    else if (name == "cap") HealthCap();
    else if (name == "fractions") FractionBounds();
    else if (name == "damage") DamageBounds();
    else if (name == "rejected") RejectedDamage();
    else if (name == "attacker") AttackerGuards();
    else if (name == "lethal") KillingBlow();
    else Require(false, "unknown case");
}
'''


class WipeoutSiphonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-wipeout-siphon-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        game = (PLUGIN / "Source/Private/WipeoutGame.cpp").read_text(encoding="utf-8-sig")
        callback = native_function(game, "void AUWipeoutGame::ScoreDamage_Implementation")
        source = directory / "siphon.cpp"
        source.write_text(ADAPTER + callback + CASES, encoding="utf-8")
        cls.executable = directory / ("siphon.exe" if os.name == "nt" else "siphon")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'siphon.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Siphon adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_one_damage_and_seventy_five_percent_healing(self):
        self.run_case("proportional")

    def test_cap_preserves_existing_overhealth(self):
        self.run_case("cap")

    def test_legacy_percent_style_and_invalid_fraction_bounds(self):
        self.run_case("fractions")

    def test_siphon_uses_existing_bounded_damage_credit(self):
        self.run_case("damage")

    def test_nonpositive_self_friendly_and_invalid_context_guards(self):
        self.run_case("rejected")

    def test_dead_dying_missing_and_unpowered_attacker_guards(self):
        self.run_case("attacker")

    def test_killing_blow_uses_consumed_health_without_overkill(self):
        self.run_case("lethal")


if __name__ == "__main__":
    unittest.main()
