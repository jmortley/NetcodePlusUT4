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
#include <cmath>
#include <limits>
#include <cstdint>
#define TEXT(x) x
using uint8 = uint8_t;
using int32 = int32_t;
using FString = std::string;
constexpr int CLASS_Abstract = 1, LOAD_NoWarn = 1, NAME_None = 0, NAME_Spectating = 1;
constexpr int MOVE_None = 0, MOVE_Walking = 1, NM_Standalone = 0;
struct FVector {
    float X, Y, Z;
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& b) const { return FVector(X+b.X, Y+b.Y, Z+b.Z); }
    FVector operator-(const FVector& b) const { return FVector(X-b.X, Y-b.Y, Z-b.Z); }
    float SizeSquared() const { return X*X+Y*Y+Z*Z; }
};
struct FMath {
    static bool IsFinite(float n) { return std::isfinite(n); }
    static float Abs(float n) { return std::abs(n); }
    static bool IsNearlyEqual(float a, float b) { return Abs(a-b) < .0001f; }
};
struct Text : std::string {
    using std::string::string;
    using std::string::operator=;
    void Empty() { clear(); }
};
enum class EGuidFormats { DigitsWithHyphens };
struct FGuid { static FGuid NewGuid() { return FGuid(); } Text ToString(EGuidFormats) { return Text("unique-run"); } };
int GetTypeHash(const Text&) { return 123; }
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
struct AUTWeapon { virtual ~AUTWeapon() = default; int Ammo = 0, MaxAmmo = 100, ShotsStatsName = 1; void StopFire(int) {} };
struct AUTPlusSniper : AUTWeapon { static UClass* StaticClass() { return &SniperType; } };
struct AUTPlusShockRifle : AUTWeapon {
    static UClass* StaticClass() { return &InstagibType; }
    bool HasSharedInstagibFireModes() const { return true; }
};
struct Movement {
    int Stops = 0, Disables = 0;
    int Mode = MOVE_None, UnCrouches = 0;
    bool bWantsToCrouch = false, Constrained = false;
    FVector PlaneOrigin{0,0,0}, PlaneNormal{0,0,0};
    void StopMovementImmediately() { ++Stops; }
    void DisableMovement() { ++Disables; Mode = MOVE_None; }
    void UnCrouch(bool) { ++UnCrouches; }
    void SetPlaneConstraintNormal(FVector n) { PlaneNormal = n; }
    void SetPlaneConstraintOrigin(FVector n) { PlaneOrigin = n; }
    void SetPlaneConstraintEnabled(bool v) { Constrained = v; }
    void SetMovementMode(int v) { Mode = v; }
    void ResetTrainerMovement(bool practice) {
        StopMovementImmediately(); bWantsToCrouch=false; UnCrouch(false);
        SetPlaneConstraintNormal(FVector(1,0,0));
        SetPlaneConstraintOrigin(FVector(-1800,0,50108));
        SetPlaneConstraintEnabled(practice);
        if(practice) SetMovementMode(MOVE_Walking); else DisableMovement();
    }
};
using UCharacterMovementComponent = Movement;
using UNCAimTrainerMovement = Movement;
struct APawn {
    virtual ~APawn() = default;
    int Teleports = 0, Destroys = 0;
    FVector Position{0,0,0};
    void SetActorLocationAndRotation(FVector p, FRotator, bool, void*, ETeleportType) { ++Teleports; Position=p; }
    void SetActorLocation(FVector p, bool, void*, ETeleportType) { ++Teleports; Position=p; }
    FVector GetActorLocation() const { return Position; }
    void Destroy() { ++Destroys; }
};
struct AUTCharacter : APawn {
    struct Capsule { float HalfHeight=108.f; float GetScaledCapsuleHalfHeight() const { return HalfHeight; } } Shape;
    Movement Move;
    AUTPlusSniper Sniper;
    AUTPlusShockRifle Instagib;
    bool bCanBeDamaged = true, Dead = false;
    int Discards = 0, Creates = 0, Switches = 0;
    bool IsDead() const { return Dead; }
    Movement* GetCharacterMovement() { return &Move; }
    const Capsule* GetCapsuleComponent() const { return &Shape; }
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
    void SetTrainerOnlineStatus(const std::string&) { ++Messages; }
};
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct World {
    bool Begun = false;
    float TimeSeconds = 0.f;
    bool HasBegunPlay() const { return Begun; }
    float GetTimeSeconds() const { return TimeSeconds; }
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
struct FNCAimTrainerProgress {
    uint8 Scenario = 0, Phase = 0;
    bool bMovementPractice = false;
    float RemainingSeconds = 60;
};
struct ANCAimTrainerGame : BaseGame {
    using Super = BaseGame;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TSubclassOf<AUTWeapon> SniperClass, InstagibClass;
    FVector ArenaOrigin{0.f, 0.f, 50000.f};
    FNCAimTrainerProgress Progress;
    int Publishes = 0, Fetches = 0;
    Text SetupError, RunId;
    FString UnrankedReason;
    int NetMode=NM_Standalone;
    void* BaseMutator=nullptr;
    bool bRankedRun=false, bPreviousContact=false;
    float PhaseStartedAt=0;
    double TrackedSeconds=0;
    struct { void Initialize(int) {} } Schedule;
    struct Room { void SetScenario(uint8) {} } TheRoom;
    Room* Arena=&TheRoom;
    struct Settings { float Dilation=1.f; float GetEffectiveTimeDilation() { return Dilation; } } Options;
    Settings* GetWorldSettings() { return &Options; }
    int GetNetMode() { return NetMode; }
    static int StaticClass() { return 1; }
    int GetClass() { return StaticClass(); }
    bool IsTrainee(const ANCAimTrainerPlayerController* pc) const { return pc && pc==Trainee; }
    void HideAllTargets() {}
    bool EnsureArena() { return true; }
    bool FailSetup(const char* message) { SetupError = message; return false; }
    void PublishProgress() { ++Publishes; }
    void RefreshLeaderboard() { ++Fetches; }
    bool ReadyToStartMatch_Implementation();
    void PostLogin(APlayerController*);
    void RestartPlayer(AController*) override;
    bool ConfigurePawn();
    bool IsInsidePracticeLane(const AUTCharacter*) const;
    void SelectScenario(ANCAimTrainerPlayerController*,uint8);
    void SetMovementPractice(ANCAimTrainerPlayerController*,bool);
    void StartTraining(ANCAimTrainerPlayerController*);
    void AbortTraining(ANCAimTrainerPlayerController*);
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
    } else if (name == "movement_lifecycle") {
        f.BeginWorld(); f.Game.NetMode=1; f.Game.PostLogin(&f.Player);
        f.Game.SetMovementPractice(&f.Player,true);
        Require(f.Game.Progress.bMovementPractice && f.Game.SpawnedPawn.Move.Constrained
            && f.Game.SpawnedPawn.Move.Mode==MOVE_Walking,"practice did not enable physics");
        f.Game.SelectScenario(&f.Player,2);
        Require(f.Game.Progress.bMovementPractice,"scenario selection forgot practice");
        f.Game.StartTraining(&f.Player);
        Require(f.Game.Progress.Phase==1 && !f.Game.bRankedRun && f.Game.Progress.bMovementPractice,
            "movement practice entered ranked pool or lost option");
        f.Game.SetMovementPractice(&f.Player,false);
        Require(f.Game.Progress.bMovementPractice,"countdown allowed option switch");
        f.Game.Progress.Phase=2;
        f.Game.SetMovementPractice(&f.Player,false);
        Require(f.Game.Progress.bMovementPractice,"active run allowed option switch");
        f.Game.AbortTraining(&f.Player);
        Require(f.Game.Progress.bMovementPractice && f.Game.Progress.Scenario==2,"cancel forgot choices");
        f.Game.SetMovementPractice(&f.Player,false);
        Require(!f.Game.SpawnedPawn.Move.Constrained && f.Game.SpawnedPawn.Move.Mode==MOVE_None,
            "fixed mode left physics enabled");
        f.Game.StartTraining(&f.Player);
        Require(f.Game.bRankedRun,"unchanged fixed network preset cannot rank");
    } else if (name == "lane_bounds") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        AUTCharacter& pawn=f.Game.SpawnedPawn;
        Require(f.Game.IsInsidePracticeLane(&pawn),"fixed anchor rejected");
        pawn.Position.Y=30;
        Require(!f.Game.IsInsidePracticeLane(&pawn),"fixed run allowed strafing");
        f.Game.Progress.bMovementPractice=true;
        Require(f.Game.IsInsidePracticeLane(&pawn),"normal strafe rejected");
        pawn.Shape.HalfHeight=69; pawn.Position.Z=50069;
        Require(f.Game.IsInsidePracticeLane(&pawn),"crouch treated as escaping lane");
        pawn.Shape.HalfHeight=108; pawn.Position.Z=50400; pawn.Position.Y=1700;
        Require(f.Game.IsInsidePracticeLane(&pawn),"lateral jump treated as escaping lane");
        pawn.Position.X+=10;
        Require(!f.Game.IsInsidePracticeLane(&pawn),"forward drift permitted");
        pawn.Position.X=-1800; pawn.Position.Y=1850;
        Require(!f.Game.IsInsidePracticeLane(&pawn),"outside wall accepted");
        pawn.Position.Y=0; pawn.Position.Z=50050;
        Require(!f.Game.IsInsidePracticeLane(&pawn),"floor penetration accepted");
        pawn.Position.Z=52000;
        Require(!f.Game.IsInsidePracticeLane(&pawn),"ceiling penetration accepted");
        pawn.Position.Z=50108; pawn.Position.X=std::numeric_limits<float>::quiet_NaN();
        Require(!f.Game.IsInsidePracticeLane(&pawn),"nonfinite position accepted");
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
            "bool ANCAimTrainerGame::IsInsidePracticeLane",
            "void ANCAimTrainerGame::SelectScenario",
            "void ANCAimTrainerGame::SetMovementPractice",
            "void ANCAimTrainerGame::StartTraining",
            "void ANCAimTrainerGame::AbortTraining",
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
    def test_movement_choice_survives_run_lifecycle_and_cannot_rank(self): self.run_case("movement_lifecycle")
    def test_lane_accepts_crouching_jumping_but_rejects_escapes(self): self.run_case("lane_bounds")


if __name__ == "__main__":
    unittest.main()
