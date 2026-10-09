"""Compile actual healing methods against a small Unreal type adapter, not UBT.

This checks gameplay state and credit; engine replication/effects and Blueprint
banner timing still require the normal dedicated-server playtest. No sibling
plugin or third-party Python module is required.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


PLUGIN = Path(__file__).resolve().parents[2]
STOCK = PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTCharacter.cpp"


def native_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if not depth:
            return source[start:position + 1]
    raise AssertionError("Unclosed native function: " + signature)


def find_compiler():
    env = os.environ.copy()
    if os.name == "nt":
        env = {key.upper(): value for key, value in env.items()}
    for name in ("clang++", "g++", "cl"):
        if compiler := shutil.which(name):
            return compiler, env, name == "cl"
    if os.name == "nt":
        vswhere = Path(env.get("PROGRAMFILES(X86)", "C:/Program Files (x86)")) / (
            "Microsoft Visual Studio/Installer/vswhere.exe")
        if vswhere.is_file():
            found = subprocess.run([
                str(vswhere), "-latest", "-products", "*", "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find",
                r"VC\Auxiliary\Build\vcvars64.bat"], check=True,
                capture_output=True, text=True, timeout=30).stdout.strip().splitlines()
            if found:
                setup = subprocess.run(
                    f'cmd.exe /d /s /c ""{found[0]}" >nul && set"', check=True,
                    capture_output=True, text=True, env=env, timeout=30).stdout
                for line in setup.splitlines():
                    key, separator, value = line.partition("=")
                    if separator and key:
                        env[key.upper()] = value
                if compiler := shutil.which("cl", path=env.get("PATH")):
                    return compiler, env, True
    raise unittest.SkipTest("A C++ compiler (clang++, g++, or Visual Studio) is required")


ADAPTER = r'''
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
using int32 = int32_t;
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a, b); }
    template<class T> static T Max(T a, T b) { return std::max(a, b); }
    template<class T> static T Clamp(T a, T low, T high) { return Max(low, Min(a, high)); }
};
template<class T> struct DefaultClass {
    template<class U> U* GetDefaultObject() { static U value; return &value; }
};
class AUTArmor {
public:
    int32 ArmorAmount = 50;
    static DefaultClass<AUTArmor>* StaticClass() { static DefaultClass<AUTArmor> c; return &c; }
};
struct AUTGameMode {
    struct { AUTArmor* Value = nullptr; AUTArmor* GetDefaultObject() { return Value; } } StartingArmorClass;
    static DefaultClass<AUTGameMode>* StaticClass() { static DefaultClass<AUTGameMode> c; return &c; }
};
struct AUTPlayerState {};
struct AUTCharacter {
    virtual ~AUTCharacter() = default;
    bool Authority = true, Dead = false, PendingKill = false;
    int32 Health = 100, HealthMax = 100, ArmorAmount = 0;
    int32 HealthUpdates = 0, ArmorUpdates = 0;
    AUTPlayerState* PlayerState = nullptr;
    AUTArmor* ArmorType = nullptr;
    struct { void Empty() {} } ArmorRemovalAssists;
    bool HasAuthority() const { return Authority; }
    bool IsDead() const { return Dead; }
    bool IsPendingKillPending() const { return PendingKill; }
    void OnHealthUpdated() { ++HealthUpdates; }
    void OnArmorUpdated() { ++ArmorUpdates; }
    virtual int32 GetArmorAmount() const;
    virtual void SetArmorAmount(AUTArmor*, int32);
};
struct ATeamArenaCharacter : AUTCharacter {
    using Super = AUTCharacter;
    int32 BeltArmorRemaining = 0, HeadArmorChargeValue = 0;
    bool bHeadArmorCharge = false;
    AUTArmor* LastRegularArmorType = nullptr;
    int32 RestoreRegularArmor(int32 Amount, int32 MaxArmor);
    void SetArmorAmount(AUTArmor*, int32) override;
};
template<class T> T* Cast(AUTCharacter* value) { return dynamic_cast<T*>(value); }
struct AUWipeoutGame {
    bool Authority = true;
    bool HasAuthority() const { return Authority; }
    struct {
        std::map<AUTPlayerState*, int32> Values;
        int32& FindOrAdd(AUTPlayerState* player) { return Values[player]; }
    } HealingDoneThisMatch;
    void CreditHealing(AUTPlayerState*, int32);
    int32 HealCharacterAndCredit(AUTCharacter*, int32, AUTPlayerState*);
};
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T> T Character(int32 armor, int32 health = 100) {
    T value; value.ArmorAmount = armor; value.Health = health; return value;
}
template<class T> void Boundaries() {
    AUTArmor regular; AUTPlayerState healer;
    const int32 before[] = {0, 1, 98, 100, 150};
    const int32 after[] = {0, 6, 100, 100, 150};
    for (int i = 0; i < 5; ++i) {
        AUWipeoutGame game; T target = Character<T>(before[i]); target.ArmorType = &regular;
        Require(game.HealCharacterAndCredit(&target, 5, &healer) == 0, "armor-only return is HP-only");
        Require(target.GetArmorAmount() == after[i], "armor threshold or clamp at full HP");
        Require(target.Health == 100 && target.HealthUpdates == 0, "full HP changed");
        Require(target.ArmorUpdates == (before[i] != after[i]), "armor update notification");
        Require(target.ArmorType == &regular, "ordinary armor type changed");
        Require(game.HealingDoneThisMatch.Values.empty(), "armor-only tick credited healing");
    }
}
void HealingCredit() {
    AUWipeoutGame game; AUTPlayerState healer, patient;
    ATeamArenaCharacter target = Character<ATeamArenaCharacter>(98, 98);
    target.PlayerState = &patient; target.HealthMax = 200;
    Require(game.HealCharacterAndCredit(&target, 10, &healer) == 2, "return must be actual HP delta");
    Require(target.Health == 100 && target.GetArmorAmount() == 100, "simultaneous capped HP/armor");
    Require(target.HealthUpdates == 1 && target.ArmorUpdates == 1, "simultaneous notifications");
    Require(game.HealingDoneThisMatch.Values[&healer] == 2, "armor must not inflate teammate credit");
    target.Health = 78; target.HealthMax = 80; target.ArmorAmount = 20;
    Require(game.HealCharacterAndCredit(&target, 10, &healer) == 2, "lower HealthMax honored");
    Require(target.Health == 80 && target.GetArmorAmount() == 25, "independent HP and armor caps");
    Require(game.HealingDoneThisMatch.Values[&healer] == 4, "exact accumulated HP credit");
    target.PlayerState = &healer; target.Health = 70;
    Require(game.HealCharacterAndCredit(&target, 1, &healer) == 1, "self healing still applies");
    Require(target.Health == 71 && target.GetArmorAmount() == 30, "armor tick must be fixed five");
    Require(game.HealingDoneThisMatch.Values[&healer] == 4, "self healing credited");
    Require(game.HealCharacterAndCredit(&target, 1, nullptr) == 1, "missing healer blocked healing");
    Require(game.HealingDoneThisMatch.Values.size() == 1, "null healer acquired credit");
    target.Health = 150;
    Require(game.HealCharacterAndCredit(&target, 5, &healer) == 0, "overhealth granted HP");
    Require(target.Health == 150 && target.GetArmorAmount() == 40, "overhealth reduced or blocked armor");
}
template<class T> void Guards() {
    AUTPlayerState healer;
    for (int i = 0; i < 7; ++i) {
        AUWipeoutGame game; T target = Character<T>(50, 50); int32 amount = 5;
        if (i == 0) game.Authority = false;
        if (i == 1) target.Dead = true;
        if (i == 2) target.PendingKill = true;
        if (i == 3) target.Health = 0;
        if (i == 4) target.Health = -1;
        if (i == 5) amount = 0;
        if (i == 6) amount = -5;
        const int32 oldHealth = target.Health;
        Require(game.HealCharacterAndCredit(&target, amount, &healer) == 0, "invalid target/amount accepted");
        Require(target.Health == oldHealth && target.GetArmorAmount() == 50, "guard changed HP/armor");
        Require(target.HealthUpdates == 0 && target.ArmorUpdates == 0, "guard emitted update");
        Require(game.HealingDoneThisMatch.Values.empty(), "guard credited healing");
    }
    AUWipeoutGame game;
    Require(game.HealCharacterAndCredit(nullptr, 5, &healer) == 0, "null target accepted");
}
void SplitAndHelmet() {
    AUTArmor belt, regular; belt.ArmorAmount = 150;
    for (bool charged : {false, true}) {
        AUWipeoutGame game; ATeamArenaCharacter target = Character<ATeamArenaCharacter>(95);
        target.ArmorType = &belt; target.LastRegularArmorType = &regular;
        target.BeltArmorRemaining = 30; target.bHeadArmorCharge = charged; target.HeadArmorChargeValue = 25;
        game.HealCharacterAndCredit(&target, 5, nullptr);
        Require(target.GetArmorAmount() == 100 && target.BeltArmorRemaining == 30,
            "regular restoration replenished/converted belt points");
        Require(target.ArmorType == &belt && target.LastRegularArmorType == &regular, "mixed stack types changed");
        Require(target.bHeadArmorCharge == charged && target.HeadArmorChargeValue == 25,
            "helmet protection consumed or depleted helmet rearmed");
    }
}
void PureBeltFallback() {
    AUTArmor belt, regular; belt.ArmorAmount = 150;
    AUTGameMode* defaults = AUTGameMode::StaticClass()->GetDefaultObject<AUTGameMode>();
    AUTArmor* baseArmor = AUTArmor::StaticClass()->GetDefaultObject<AUTArmor>();
    for (AUTArmor* configured : {&regular, &belt, static_cast<AUTArmor*>(nullptr)}) {
        defaults->StartingArmorClass.Value = configured;
        ATeamArenaCharacter target = Character<ATeamArenaCharacter>(50);
        target.ArmorType = &belt; target.BeltArmorRemaining = 50;
        Require(target.RestoreRegularArmor(5, 100) == 5, "pure belt failed regular restoration");
        Require(target.GetArmorAmount() == 55 && target.BeltArmorRemaining == 50, "new points became belt");
        Require(target.ArmorType == &belt, "live belt lost its display type");
        Require(target.LastRegularArmorType == (configured == &regular ? &regular : baseArmor),
            "pure belt must retain a non-belt fallback CDO");
        Require(!target.bHeadArmorCharge && target.HeadArmorChargeValue == 0, "fallback granted helmet");
    }
    defaults->StartingArmorClass.Value = nullptr;
}
void DirectHelperGuards() {
    ATeamArenaCharacter target = Character<ATeamArenaCharacter>(98);
    target.Authority = false;
    Require(target.RestoreRegularArmor(5, 100) == 0 && target.GetArmorAmount() == 98, "helper authority guard");
    target.Authority = true;
    for (int32 cap : {-1, 0, 98})
        Require(target.RestoreRegularArmor(5, cap) == 0, "helper invalid/satisfied cap");
    Require(target.RestoreRegularArmor(0, 100) == 0, "helper zero amount");
    Require(target.RestoreRegularArmor(-1, 100) == 0, "helper negative amount");
    Require(target.RestoreRegularArmor(std::numeric_limits<int32>::max(), 150) == 2,
        "helper must cap at ordinary100 without addition overflow");
    Require(target.GetArmorAmount() == 100, "helper escaped soft limit");
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "boundaries") { Boundaries<ATeamArenaCharacter>(); Boundaries<AUTCharacter>(); }
    else if (name == "credit") HealingCredit();
    else if (name == "guards") { Guards<ATeamArenaCharacter>(); Guards<AUTCharacter>(); }
    else if (name == "split") SplitAndHelmet();
    else if (name == "fallback") PureBeltFallback();
    else if (name == "helper") DirectHelperGuards();
    else Require(false, "unknown case");
}
'''


class WipeoutHealingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-wipeout-healing-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        armor = (PLUGIN / "Source/Private/TeamArenaCharacter.cpp").read_text(encoding="utf-8-sig")
        game = (PLUGIN / "Source/Private/WipeoutGame.cpp").read_text(encoding="utf-8-sig")
        stock = STOCK.read_text(encoding="utf-8-sig")
        definitions = []
        for name in ("ArmorPlusSoftLimit", "ArmorPlusMaxTotal"):
            match = re.search(rf"\b{name}\s*=\s*(\d+)", armor)
            if match is None:
                raise AssertionError("Missing production constant: " + name)
            definitions.append(f"constexpr int32 {name} = {match.group(1)};")
        for source, signatures in (
            (stock, ("int32 AUTCharacter::GetArmorAmount", "void AUTCharacter::SetArmorAmount")),
            (armor, ("bool IsArmorPlusBelt", "int32 ATeamArenaCharacter::RestoreRegularArmor",
                     "void ATeamArenaCharacter::SetArmorAmount")),
            (game, ("void AUWipeoutGame::CreditHealing", "int32 AUWipeoutGame::HealCharacterAndCredit")),
        ):
            definitions.extend(native_function(source, signature) for signature in signatures)
        source = directory / "healing.cpp"
        source.write_text(ADAPTER + "\n".join(definitions) + CASES, encoding="utf-8")
        cls.executable = directory / ("healing.exe" if os.name == "nt" else "healing")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'healing.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Healing adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_armor_boundaries_at_full_health_for_native_and_stock_pawns(self):
        self.run_case("boundaries")

    def test_simultaneous_restoration_and_exact_hp_only_credit(self):
        self.run_case("credit")

    def test_null_dead_pending_invalid_amount_and_authority_guards(self):
        self.run_case("guards")

    def test_regular_restoration_preserves_belt_and_helmet_state(self):
        self.run_case("split")

    def test_pure_belt_keeps_regular_fallback_type_without_helmet_grant(self):
        self.run_case("fallback")

    def test_direct_helper_guards_and_saturating_amount(self):
        self.run_case("helper")


if __name__ == "__main__":
    unittest.main()
