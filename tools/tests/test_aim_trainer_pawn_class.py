"""Exercise stock InitGame pawn selection before the trainer's actual overrides.

The stock block is extracted from this checkout rather than rewritten in a
mock. The adapter replaces class loading, so it verifies lifecycle selection
and override policy without claiming to load cooked character assets.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(value) value
#define UE_LOG(...) do {} while (0)
constexpr int LOAD_NoWarn = 1;
struct FString : std::string {
    using std::string::string;
    using std::string::operator=;
    bool IsEmpty() const { return empty(); }
    void Empty() { clear(); }
    const char* operator*() const { return c_str(); }
};
struct UClass {
    FString Path;
    explicit UClass(const char* path) : Path(path) {}
    FString GetPathName() const { return Path; }
    FString GetName() const { return Path; }
    static UClass* StaticClass() { static UClass type("Class"); return &type; }
};
UClass DefaultType("/Game/RestrictedAssets/Blueprints/DefaultCharacter.DefaultCharacter_C");
UClass TrainerType("/Script/NetcodePlus.NCAimTrainerCharacter");
UClass OverrideType("/Script/OtherMode.CustomPawn");
struct APawn {};
struct AController { bool Remote = false; };
struct ANCAimTrainerCharacter { static UClass* StaticClass() { return &TrainerType; } };
struct FStringAssetReference {
    FString Path;
    FStringAssetReference() = default;
    explicit FStringAssetReference(const char* path) : Path(path) {}
    explicit FStringAssetReference(const FString& path) : Path(path) {}
    FString ToString() const { return Path; }
};
template<class T> struct TAssetSubclassOf {
    FStringAssetReference Reference;
    TAssetSubclassOf() = default;
    TAssetSubclassOf(UClass* type) { Reference.Path = type ? type->Path : FString(); }
    TAssetSubclassOf(const FStringAssetReference& reference) : Reference(reference) {}
    bool IsNull() const { return Reference.Path.empty(); }
    FStringAssetReference ToStringReference() const { return Reference; }
    TAssetSubclassOf& operator=(UClass* type) {
        Reference.Path = type ? type->Path : FString(); return *this;
    }
    TAssetSubclassOf& operator=(const FStringAssetReference& reference) {
        Reference = reference; return *this;
    }
};
template<class T> struct TSubclassOf {
    UClass* Value = nullptr;
    TSubclassOf() = default;
    TSubclassOf(UClass* value) : Value(value) {}
    operator UClass*() const { return Value; }
    UClass* operator->() const { return Value; }
};
template<class T> T* Cast(UClass* value) { return static_cast<T*>(value); }
std::vector<std::string> LoadedClasses;
UClass* StaticLoadObject(UClass*, void*, const char* path, void*, int) {
    LoadedClasses.emplace_back(path);
    for (UClass* candidate : {&DefaultType, &TrainerType, &OverrideType}) {
        if (candidate->Path == path) return candidate;
    }
    return nullptr;
}
struct AUTBaseGameMode {
    TAssetSubclassOf<APawn> PlayerPawnObject{&DefaultType};
    FString PawnClassOverride;
    UClass* DefaultPawnClass = &DefaultType;
    int NetMode = 0;
    void InitGame(const FString&, const FString&, FString&);
};
struct ANCAimTrainerGame : AUTBaseGameMode {
    using Super = AUTBaseGameMode;
    struct Session { int MaxPlayers = 99; } HostedSession;
    Session* GameSession = &HostedSession;
    int DefaultMaxPlayers = 99, BotFillCount = 10, GoalScore = 30, TimeLimit = 20;
    bool bRequireReady = true, bRequireFull = true, bDelayedStart = true;
    bool bRemovePawnsAtStart = true, bPlayersStartWithArmor = true;
    struct Inventory { void Empty() {} } DefaultInventory;
    ANCAimTrainerGame() { DefaultPawnClass = ANCAimTrainerCharacter::StaticClass(); }
    void InitGame(const FString&, const FString&, FString&);
    UClass* GetDefaultPawnClassForController_Implementation(AController*);
};
void Require(bool value, const char* why) {
    if (!value) { std::cerr << why << '\n'; std::exit(1); }
}
'''

CASES = r'''
void CheckStartup(bool remote, const char* overridePath) {
    ANCAimTrainerGame game;
    game.NetMode = remote ? 1 : 0;
    game.PawnClassOverride = overridePath;
    AController player; player.Remote = remote;
    FString error;
    LoadedClasses.clear();
    game.InitGame("DM-DeckTest", remote ? "?InstanceID=7" : "", error);
    Require(error.IsEmpty(), "trainer introduced startup error");
    Require(game.DefaultPawnClass == &TrainerType, "stock InitGame replaced trainer pawn");
    Require(game.PawnClassOverride == overridePath, "trainer changed a GlobalConfig pawn override");
    Require(game.GetDefaultPawnClassForController_Implementation(&player) == &TrainerType,
            "spawn selector returned a pawn without the trainer movement component");
    const bool configuredOverride = overridePath[0] != '\0';
    Require(LoadedClasses.size() == (configuredOverride ? 2u : 1u)
            && LoadedClasses[0] == TrainerType.Path,
            "native trainer pawn path was not selected before stock initialization");
    if (configuredOverride) {
        Require(LoadedClasses[1] == overridePath,
                "trainer hid the GlobalConfig override during stock initialization/config saving");
    }
    Require(game.DefaultMaxPlayers == 1 && game.GameSession->MaxPlayers == 1 && game.BotFillCount == 0,
            "pawn fix lost trainer session defaults");
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    if (name == "constructor_only_reproduces") {
        AUTBaseGameMode game;
        game.DefaultPawnClass = &TrainerType;
        FString error;
        game.InitGame("DM-DeckTest", "", error);
        Require(game.DefaultPawnClass == &DefaultType,
                "stock selection fixture no longer reproduces constructor-only overwrite");
        game.PawnClassOverride = OverrideType.Path;
        game.InitGame("DM-DeckTest", "", error);
        Require(game.DefaultPawnClass == &OverrideType, "stock configured override path was not exercised");
    } else if (name == "standalone") {
        CheckStartup(false, "");
    } else if (name == "network") {
        CheckStartup(true, "");
    } else if (name == "configured_override") {
        CheckStartup(false, *OverrideType.Path);
        CheckStartup(true, *OverrideType.Path);
    } else if (name == "late_default_change") {
        for (bool remote : {false, true}) {
            ANCAimTrainerGame game;
            AController player; player.Remote = remote;
            FString error;
            game.InitGame("DM-DeckTest", "", error);
            // Stock RestartPlayer eventually uses the virtual selector. A
            // later assignment must not substitute an incompatible pawn.
            game.DefaultPawnClass = &OverrideType;
            Require(game.GetDefaultPawnClassForController_Implementation(&player) == &TrainerType,
                    "late default-class assignment bypassed trainer spawn selector");
        }
    } else {
        Require(false, "unknown case");
    }
}
'''


class AimTrainerPawnClassTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-pawn-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        stock = (PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTBaseGameMode.cpp").read_text(encoding="utf-8-sig")
        stock_init = native_function(stock, "void AUTBaseGameMode::InitGame")
        # Compile the exact pawn-selection block. Everything after it deals
        # with unrelated instance analytics, server IDs and game-mode options.
        stock_selection = stock_init.split("// Grab the InstanceID if it's there.", 1)
        if len(stock_selection) != 2:
            raise AssertionError("Stock InitGame pawn-selection boundary changed; re-audit extraction")
        stock_selection = stock_selection[0] + "\n(void)MapName; (void)Options; (void)ErrorMessage;\n}\n"
        signatures = (
            "void ANCAimTrainerGame::InitGame",
            "UClass* ANCAimTrainerGame::GetDefaultPawnClassForController_Implementation",
        )
        source = directory / "trainer_pawn_class.cpp"
        source.write_text("\n".join(
            [ADAPTER, stock_selection] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_pawn_class.exe" if os.name == "nt" else "trainer_pawn_class")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_pawn_class.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Pawn-class adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stock_initializer_reproduces_constructor_only_overwrite(self): self.run_case("constructor_only_reproduces")
    def test_standalone_startup_selects_trainer_pawn(self): self.run_case("standalone")
    def test_network_startup_selects_trainer_pawn(self): self.run_case("network")
    def test_global_override_is_preserved_but_cannot_replace_trainer_pawn(self): self.run_case("configured_override")
    def test_late_default_class_change_cannot_replace_spawn_selector(self): self.run_case("late_default_change")


if __name__ == "__main__":
    unittest.main()
