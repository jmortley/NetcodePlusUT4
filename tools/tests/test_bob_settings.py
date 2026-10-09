"""Compile the production bob reconciliation helper against small Unreal adapters.

The adapters intentionally expose no profile or save APIs. These tests exercise
the actual helper; engine ticker timing and the stock settings UI need a playtest.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
using TCHAR = char;
#define TEXT(value) value
enum ENetMode { NM_Standalone, NM_DedicatedServer, NM_ListenServer, NM_Client };
struct ULocalPlayer;
struct UWorld {
    ENetMode Mode = NM_Client;
    std::vector<ULocalPlayer*> Players;
    ENetMode GetNetMode() const { return Mode; }
};
struct APlayerController { virtual ~APlayerController() = default; };
struct AUTPlayerController : APlayerController {
    bool Local = true;
    UWorld* World = nullptr;
    float EyeOffsetGlobalScaling = 1.f;
    float WeaponBobGlobalScaling = 1.f;
    bool IsLocalController() const { return Local; }
    UWorld* GetWorld() const { return World; }
};
struct ULocalPlayer { APlayerController* PlayerController = nullptr; };
struct UEngine { int IteratorCalls = 0; };
template<class T> T* Cast(APlayerController* value) { return dynamic_cast<T*>(value); }
struct FLocalPlayerIterator {
    std::vector<ULocalPlayer*>& Players;
    std::size_t Position = 0;
    FLocalPlayerIterator(UEngine* engine, UWorld* world) : Players(world->Players) {
        ++engine->IteratorCalls;
        SkipNullPlayers();
    }
    void SkipNullPlayers() {
        while (Position < Players.size() && !Players[Position]) ++Position;
    }
    explicit operator bool() const { return Position < Players.size(); }
    void operator++() { ++Position; SkipNullPlayers(); }
    ULocalPlayer* operator->() const { return Players[Position]; }
};
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
};
struct FConfigCacheIni {
    std::map<std::string, float> Values;
    int Reads = 0;
    bool GetFloat(const TCHAR* section, const TCHAR* key, float& value,
                  const std::string& filename) {
        ++Reads;
        if (std::string(section) != "/Script/UnrealTournament.UTPlayerController"
            || filename != "Game.ini") return false;
        const auto found = Values.find(key);
        if (found == Values.end()) return false;
        value = found->second;
        return true;
    }
};
UEngine* GEngine = nullptr;
FConfigCacheIni* GConfig = nullptr;
const std::string GGameIni = "Game.ini";
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    UWorld World;
    UEngine Engine;
    FConfigCacheIni Config;
    AUTPlayerController Controller;
    ULocalPlayer Player;
    Fixture() {
        GEngine = &Engine; GConfig = &Config;
        Controller.World = &World;
        Player.PlayerController = &Controller;
        World.Players.push_back(&Player);
    }
    void Set(float view, float weapon) {
        Config.Values["EyeOffsetGlobalScaling"] = view;
        Config.Values["WeaponBobGlobalScaling"] = weapon;
    }
    void Tick() { ReconcileLocalBobFromGameIni(&World); }
};
void ZeroOverrides() {
    for (float previous : {1.f, 0.5f, -1.f, 1.e-9f, -1.e-9f}) {
        Fixture f; f.Set(0.f, 0.f);
        f.Controller.EyeOffsetGlobalScaling = previous;
        f.Controller.WeaponBobGlobalScaling = previous;
        f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.f,
            "configured view zero left a nonzero or tiny stale value");
        Require(f.Controller.WeaponBobGlobalScaling == 0.f,
            "configured weapon zero left a nonzero or tiny stale value");
        f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.f
            && f.Controller.WeaponBobGlobalScaling == 0.f, "stable settings drifted");
    }
}
void FractionalAndCustomValues() {
    for (float weapon : {0.125f, 0.8125f, 2.5f, -0.25f}) {
        Fixture f; f.Set(0.375f, weapon); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.375f,
            "fractional view scaling changed");
        Require(f.Controller.WeaponBobGlobalScaling == weapon,
            "fractional or custom weapon scaling was rounded or clamped");
    }
    for (ENetMode mode : {NM_Standalone, NM_ListenServer, NM_Client}) {
        Fixture f; f.World.Mode = mode; f.Set(0.25f, 0.75f); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.25f
            && f.Controller.WeaponBobGlobalScaling == 0.75f,
            "rendered client or listen-server host was excluded");
    }
}
void MissingKeys() {
    Fixture f;
    f.Controller.EyeOffsetGlobalScaling = 0.2f;
    f.Controller.WeaponBobGlobalScaling = 0.8f;
    f.Tick();
    Require(f.Controller.EyeOffsetGlobalScaling == 0.2f
        && f.Controller.WeaponBobGlobalScaling == 0.8f, "absent keys forced defaults");
    f.Config.Values["EyeOffsetGlobalScaling"] = 0.f; f.Tick();
    Require(f.Controller.EyeOffsetGlobalScaling == 0.f
        && f.Controller.WeaponBobGlobalScaling == 0.8f, "view-only key altered weapon");
    f.Config.Values.clear();
    f.Controller.EyeOffsetGlobalScaling = 0.6f;
    f.Config.Values["WeaponBobGlobalScaling"] = 0.f; f.Tick();
    Require(f.Controller.EyeOffsetGlobalScaling == 0.6f
        && f.Controller.WeaponBobGlobalScaling == 0.f, "weapon-only key altered view");
}
void NonfiniteValues() {
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        Fixture f; f.Set(invalid, invalid); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 1.f
            && f.Controller.WeaponBobGlobalScaling == 1.f, "nonfinite config applied");
        f.Set(invalid, 0.4f); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 1.f
            && f.Controller.WeaponBobGlobalScaling == 0.4f,
            "invalid view blocked valid weapon or changed view");
        f.Set(0.3f, invalid); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.3f
            && f.Controller.WeaponBobGlobalScaling == 0.4f,
            "invalid weapon blocked valid view or changed weapon");
        f.Controller.EyeOffsetGlobalScaling = invalid;
        f.Controller.WeaponBobGlobalScaling = invalid;
        f.Set(0.f, 0.5f); f.Tick();
        Require(f.Controller.EyeOffsetGlobalScaling == 0.f
            && f.Controller.WeaponBobGlobalScaling == 0.5f,
            "valid settings did not replace a nonfinite live value");
    }
}
void ContextGuards() {
    for (int guard = 0; guard < 4; ++guard) {
        Fixture f; f.Set(0.f, 0.f);
        if (guard == 0) ReconcileLocalBobFromGameIni(nullptr);
        if (guard == 1) { GEngine = nullptr; f.Tick(); }
        if (guard == 2) { GConfig = nullptr; f.Tick(); }
        if (guard == 3) { f.World.Mode = NM_DedicatedServer; f.Tick(); }
        Require(f.Controller.EyeOffsetGlobalScaling == 1.f
            && f.Controller.WeaponBobGlobalScaling == 1.f, "invalid context changed bob");
        Require(f.Config.Reads == 0 && f.Engine.IteratorCalls == 0,
            "invalid context accessed config or iterated players");
    }
    Fixture empty; empty.Set(0.f, 0.f); empty.World.Players.clear(); empty.Tick();
}
void LocalPlayersAndWorlds() {
    Fixture f; f.Set(0.f, 0.25f);
    UWorld otherWorld;
    AUTPlayerController remote, wrongWorld, secondLocal;
    remote.World = &f.World; remote.Local = false;
    wrongWorld.World = &otherWorld; secondLocal.World = &f.World;
    APlayerController nonUt;
    ULocalPlayer remotePlayer, oldPlayer, secondPlayer, nonUtPlayer, nullController;
    remotePlayer.PlayerController = &remote;
    oldPlayer.PlayerController = &wrongWorld;
    secondPlayer.PlayerController = &secondLocal;
    nonUtPlayer.PlayerController = &nonUt;
    f.World.Players = {nullptr, &remotePlayer, &oldPlayer, &nonUtPlayer,
                      &nullController, &f.Player, &secondPlayer, nullptr};
    f.Tick();
    Require(f.Controller.EyeOffsetGlobalScaling == 0.f
        && f.Controller.WeaponBobGlobalScaling == 0.25f
        && secondLocal.EyeOffsetGlobalScaling == 0.f
        && secondLocal.WeaponBobGlobalScaling == 0.25f,
        "did not reconcile every valid local player");
    Require(remote.EyeOffsetGlobalScaling == 1.f && remote.WeaponBobGlobalScaling == 1.f,
        "remote controller changed");
    Require(wrongWorld.EyeOffsetGlobalScaling == 1.f
        && wrongWorld.WeaponBobGlobalScaling == 1.f, "another world's controller changed");
}
void ReapplyTravelAndConfigChanges() {
    Fixture f; f.Set(0.f, 0.25f); f.Tick();
    // Simulate a later profile application overwriting live controller values.
    f.Controller.EyeOffsetGlobalScaling = 1.f;
    f.Controller.WeaponBobGlobalScaling = 1.f; f.Tick();
    Require(f.Controller.EyeOffsetGlobalScaling == 0.f
        && f.Controller.WeaponBobGlobalScaling == 0.25f, "late profile stomp survived");
    AUTPlayerController replacement; replacement.World = &f.World;
    f.Player.PlayerController = &replacement; f.Tick();
    Require(replacement.EyeOffsetGlobalScaling == 0.f
        && replacement.WeaponBobGlobalScaling == 0.25f, "replacement controller missed sync");
    UWorld nextWorld; replacement.World = &nextWorld;
    nextWorld.Players.push_back(&f.Player);
    f.Set(0.625f, 2.f); ReconcileLocalBobFromGameIni(&nextWorld);
    Require(replacement.EyeOffsetGlobalScaling == 0.625f
        && replacement.WeaponBobGlobalScaling == 2.f,
        "travel or settings-dialog config update retained stale cached settings");
    Require(f.Controller.EyeOffsetGlobalScaling == 0.f
        && f.Controller.WeaponBobGlobalScaling == 0.25f, "retired controller changed");
    f.Config.Values.clear();
    replacement.EyeOffsetGlobalScaling = 0.9f;
    replacement.WeaponBobGlobalScaling = 0.8f;
    ReconcileLocalBobFromGameIni(&nextWorld);
    Require(replacement.EyeOffsetGlobalScaling == 0.9f
        && replacement.WeaponBobGlobalScaling == 0.8f, "removed keys remained enforced");
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "zero") ZeroOverrides();
    else if (name == "values") FractionalAndCustomValues();
    else if (name == "missing") MissingKeys();
    else if (name == "nonfinite") NonfiniteValues();
    else if (name == "guards") ContextGuards();
    else if (name == "local") LocalPlayersAndWorlds();
    else if (name == "reload") ReapplyTravelAndConfigChanges();
    else Require(false, "unknown case");
}
'''


class BobSettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-bob-settings-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        module = (PLUGIN / "Source/Private/NetcodePlus.cpp").read_text(encoding="utf-8-sig")
        helper = native_function(module, "static void ReconcileLocalBobFromGameIni")
        source = directory / "bob.cpp"
        source.write_text(ADAPTER + helper + CASES, encoding="utf-8")
        cls.executable = directory / ("bob.exe" if os.name == "nt" else "bob")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'bob.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Bob adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_configured_zero_replaces_full_fractional_and_tiny_stale_values(self):
        self.run_case("zero")

    def test_fractional_custom_weapon_and_rendered_net_modes(self):
        self.run_case("values")

    def test_missing_keys_leave_each_setting_independent(self):
        self.run_case("missing")

    def test_nonfinite_config_is_ignored_and_nonfinite_live_values_are_repaired(self):
        self.run_case("nonfinite")

    def test_null_context_and_dedicated_server_guards(self):
        self.run_case("guards")

    def test_only_local_ut_controllers_in_the_current_world_are_changed(self):
        self.run_case("local")

    def test_profile_reapply_controller_replacement_travel_and_config_changes(self):
        self.run_case("reload")


if __name__ == "__main__":
    unittest.main()
