"""Run actual trainer startup functions against a minimal UE lifecycle adapter.

The adapter exposes the stock first-frame deferral and records inventory work.
It covers standalone map startup and a later joining player; it does not replace
a packaged playtest of Unreal's attachment actors or cooked weapon assets.
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
#define TEXT(x) x
constexpr int CLASS_Abstract = 1, LOAD_NoWarn = 1, NAME_None = 0, NAME_Spectating = 1;
struct FVector { FVector(float, float, float) {} FVector operator+(const FVector&) const { return *this; } };
struct FRotator { static FRotator ZeroRotator; };
FRotator FRotator::ZeroRotator;
enum class ETeleportType { TeleportPhysics };
struct UClass {
    int Kind;
    bool HasAnyClassFlags(int) const { return false; }
    bool IsChildOf(UClass* other) const { return other && other->Kind == Kind; }
};
UClass SniperType{1}, InstagibType{2};
template<class T> struct TSubclassOf {
    UClass* Value = nullptr;
    TSubclassOf() = default;
    TSubclassOf(UClass* value) : Value(value) {}
    TSubclassOf& operator=(UClass* value) { Value = value; return *this; }
    explicit operator bool() const { return Value != nullptr; }
    UClass* operator->() const { return Value; }
};
template<class T> UClass* LoadClass(void*, const char* path, void*, int) {
    return std::string(path).find("Instagib") != std::string::npos ? &InstagibType : &SniperType;
}
struct AUTWeapon { virtual ~AUTWeapon() = default; int Ammo = 0, MaxAmmo = 100, ShotsStatsName = 1; };
struct AUTPlusSniper : AUTWeapon { static UClass* StaticClass() { return &SniperType; } };
struct AUTPlusShockRifle : AUTWeapon {
    static UClass* StaticClass() { return &InstagibType; }
    bool HasSharedInstagibFireModes() const { return true; }
};
struct Movement {
    int Stops = 0, Disables = 0;
    void StopMovementImmediately() { ++Stops; }
    void DisableMovement() { ++Disables; }
};
struct APawn {
    virtual ~APawn() = default;
    int Teleports = 0, Destroys = 0;
    void SetActorLocationAndRotation(FVector, FRotator, bool, void*, ETeleportType) { ++Teleports; }
    void Destroy() { ++Destroys; }
};
struct AUTCharacter : APawn {
    Movement Move;
    AUTPlusSniper Sniper;
    AUTPlusShockRifle Instagib;
    bool bCanBeDamaged = true, Dead = false;
    int Discards = 0, Creates = 0, Switches = 0;
    bool IsDead() const { return Dead; }
    Movement* GetCharacterMovement() { return &Move; }
    void DiscardAllInventory() { ++Discards; }
    AUTWeapon* CreateInventory(TSubclassOf<AUTWeapon> type) {
        ++Creates;
        return type->Kind == 2 ? static_cast<AUTWeapon*>(&Instagib) : static_cast<AUTWeapon*>(&Sniper);
    }
    void SwitchWeapon(AUTWeapon*) { ++Switches; }
};
struct State { bool bOnlySpectator = false; };
struct AController {
    virtual ~AController() = default;
    State* PlayerState = nullptr;
    APawn* Pawn = nullptr;
    int Rotations = 0;
    APawn* GetPawn() const { return Pawn; }
    void SetControlRotation(FRotator) { ++Rotations; }
    void ClientSetRotation(FRotator, bool) { ++Rotations; }
};
struct APlayerController : AController {};
struct ANCAimTrainerPlayerController : APlayerController {
    int Spectating = 0, Messages = 0;
    void ChangeState(int) { ++Spectating; }
    void SetTrainerOnlineStatus(const char*) { ++Messages; }
};
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct World {
    bool Begun = false;
    float TimeSeconds = 0.f;
    bool HasBegunPlay() const { return Begun; }
};
struct BaseGame {
    World TheWorld;
    AUTCharacter SpawnedPawn;
    int NumPlayers = 1, ReadyCalls = 0, NextTickStarts = 0, Restarts = 0;
    bool AutoRestartOnLogin = false;
    World* GetWorld() { return &TheWorld; }
    virtual ~BaseGame() = default;
    virtual void RestartPlayer(AController* player) { ++Restarts; player->Pawn = &SpawnedPawn; }
    void PostLogin(APlayerController* player) { if (AutoRestartOnLogin) RestartPlayer(player); }
    bool ReadyToStartMatch_Implementation() {
        ++ReadyCalls;
        if (TheWorld.TimeSeconds == 0.f) { ++NextTickStarts; return false; }
        return true;
    }
};
struct ANCAimTrainerGame : BaseGame {
    using Super = BaseGame;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TSubclassOf<AUTWeapon> SniperClass, InstagibClass;
    FVector ArenaOrigin{0.f, 0.f, 50000.f};
    struct { int Scenario = 0; } Progress;
    int Publishes = 0, Fetches = 0;
    void PublishProgress() { ++Publishes; }
    void RefreshLeaderboard() { ++Fetches; }
    bool ReadyToStartMatch_Implementation();
    void PostLogin(APlayerController*);
    void RestartPlayer(AController*) override;
    bool ConfigurePawn();
};
void Require(bool value, const char* why) {
    if (!value) { std::cerr << why << '\n'; std::exit(1); }
}
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    State PlayerState;
    Fixture() { Player.PlayerState = &PlayerState; }
    void BeginWorld() { Game.TheWorld.Begun = true; Game.TheWorld.TimeSeconds = .016f; }
};
'''

CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case required"); const std::string name(argv[1]);
    Fixture f;
    if (name == "first_frame") {
        Require(!f.Game.ReadyToStartMatch_Implementation(), "started on first frame");
        Require(f.Game.ReadyCalls == 1 && f.Game.NextTickStarts == 1,
                "stock next-tick deferral was bypassed");
        f.BeginWorld();
        Require(f.Game.ReadyToStartMatch_Implementation(), "ready match never starts");
        f.Game.NumPlayers = 0;
        Require(!f.Game.ReadyToStartMatch_Implementation(), "empty server starts trainer");
    } else if (name == "pre_begin") {
        f.Game.TheWorld.TimeSeconds = 1.f;
        Require(!f.Game.ReadyToStartMatch_Implementation(), "time alone bypassed world BeginPlay");
        f.Game.RestartPlayer(&f.Player);
        Require(f.Game.Restarts == 0 && !f.Player.Pawn, "manual restart spawned before BeginPlay");
        f.Game.Trainee = &f.Player;
        f.Player.Pawn = &f.Game.SpawnedPawn;
        Require(!f.Game.ConfigurePawn(), "existing pawn configured before BeginPlay");
        Require(f.Game.SpawnedPawn.Discards == 0 && f.Game.SpawnedPawn.Creates == 0,
                "pre-BeginPlay configure touched inventory");
    } else if (name == "deferred_login") {
        f.Game.PostLogin(&f.Player);
        Require(f.Game.Trainee == &f.Player && !f.Player.Pawn && f.Game.Restarts == 0,
                "initial PostLogin did not defer spawning");
        f.BeginWorld();
        f.Game.RestartPlayer(&f.Player);
        Require(f.Game.Restarts == 1 && f.Game.SpawnedPawn.Creates == 1 && f.Game.RunWeapon,
                "deferred restart missed scenario equipment");
        Require(f.Game.SpawnedPawn.Move.Disables == 1 && !f.Game.SpawnedPawn.bCanBeDamaged,
                "deferred trainee was not configured");
    } else if (name == "existing_pawn_login") {
        f.Game.AutoRestartOnLogin = true;
        f.BeginWorld();
        f.Game.PostLogin(&f.Player);
        Require(f.Game.Restarts == 1 && f.Game.SpawnedPawn.Creates == 1,
                "in-progress login equipped more than once or missed equipment");
        Require(f.Game.Trainee == &f.Player && f.Game.RunWeapon, "existing pawn not adopted");
    } else if (name == "late_manual_restart") {
        f.BeginWorld();
        f.Game.PostLogin(&f.Player);
        Require(f.Game.Restarts == 1 && f.Game.SpawnedPawn.Creates == 1,
                "manual PostLogin restart configured inventory twice");
    } else if (name == "spectator") {
        f.BeginWorld();
        f.PlayerState.bOnlySpectator = true;
        f.Game.PostLogin(&f.Player);
        f.Game.RestartPlayer(&f.Player);
        Require(!f.Game.Trainee && f.Game.Restarts == 0 && f.Game.SpawnedPawn.Creates == 0,
                "spectator acquired trainee pawn");
    } else { Require(false, "unknown case"); }
}
'''


class AimTrainerStartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-startup-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "bool ANCAimTrainerGame::ReadyToStartMatch_Implementation",
            "void ANCAimTrainerGame::PostLogin",
            "void ANCAimTrainerGame::RestartPlayer",
            "bool ANCAimTrainerGame::ConfigurePawn",
        )
        source = directory / "trainer_startup.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_startup.exe" if os.name == "nt" else "trainer_startup")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_startup.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Startup adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stock_first_frame_deferral_is_preserved(self): self.run_case("first_frame")
    def test_no_spawn_or_inventory_before_world_begin_play(self): self.run_case("pre_begin")
    def test_initial_login_equips_after_deferred_restart(self): self.run_case("deferred_login")
    def test_in_progress_login_equips_existing_pawn_once(self): self.run_case("existing_pawn_login")
    def test_postlogin_manual_restart_equips_once(self): self.run_case("late_manual_restart")
    def test_spectator_never_receives_trainee_pawn(self): self.run_case("spectator")


if __name__ == "__main__":
    unittest.main()
