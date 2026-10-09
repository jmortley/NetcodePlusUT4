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
#include <vector>
#include <algorithm>
#include <memory>
#define TEXT(x) x
#define UE_LOG(Category, Level, ...) IgnoreLog(__VA_ARGS__)
template<class... Args> void IgnoreLog(Args...) {}
struct LogName { const char* operator*() const { return "fixture"; } };
template<class T> LogName GetNameSafe(T) { return LogName(); }
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
    Text ToLower() const { Text result(*this); std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return result; }
};
enum class EGuidFormats { DigitsWithHyphens };
struct FGuid { static FGuid NewGuid() { return FGuid(); } Text ToString(EGuidFormats) { return Text("unique-run"); } };
int GetTypeHash(const Text&) { return 123; }
struct FRotator {
    float Pitch=0.f, Yaw=0.f, Roll=0.f;
    FRotator() = default;
    FRotator(float pitch,float yaw,float roll) : Pitch(pitch),Yaw(yaw),Roll(roll) {}
    bool IsZero() const { return Pitch==0.f && Yaw==0.f && Roll==0.f; }
    static FRotator ZeroRotator;
};
FRotator FRotator::ZeroRotator;
enum class ETeleportType { TeleportPhysics };
struct AUTCharacter;
struct UClass {
    int Kind;
    template<class T> const T* GetDefaultObject() const;
    bool HasAnyClassFlags(int) const { return false; }
    bool IsChildOf(UClass* other) const {
        return other && (other->Kind == Kind || (Kind == 3 && other->Kind == 4)
                        || ((Kind == 6 || Kind == 11) && other->Kind == 1));
    }
};
UClass SniperType{1}, InstagibType{2}, LinkType{3}, LinkBaseType{4}, BeamStateType{5}, LightningType{6};
UClass TrainerType{7}, InstagibTrainerType{8}, TargetType{9}, InstagibTargetType{10};
UClass SACTFSniperType{11}, SACTFTrainerType{12}, SACTFTargetType{13};
struct ANCAimTrainerCharacter { static UClass* StaticClass() { return &TrainerType; } };
struct ANCAimTrainerInstagibCharacter { static UClass* StaticClass() { return &InstagibTrainerType; } };
struct ANCAimTrainerSACTFCharacter { static UClass* StaticClass() { return &SACTFTrainerType; } };
bool MissingLinkAsset = false, WrongLinkAsset = false;
bool MissingLightningAsset = false, WrongLightningAsset = false;
int LightningLoads = 0, SACTFLoads = 0;
bool MissingSACTFAsset = false, WrongSACTFAsset = false;
std::string LastLoadedPath;
template<class T> struct TSubclassOf {
    UClass* Value = nullptr;
    TSubclassOf() = default;
    TSubclassOf(UClass* value) : Value(value) {}
    TSubclassOf& operator=(UClass* value) { Value = value; return *this; }
    explicit operator bool() const { return Value != nullptr; }
    UClass* operator->() const { return Value; }
    UClass* operator*() const { return Value; }
};
template<class T> UClass* LoadClass(void*, const char* path, void*, int) {
    LastLoadedPath = path;
    if (LastLoadedPath.find("Link") != std::string::npos) {
        return MissingLinkAsset ? nullptr : WrongLinkAsset ? &SniperType : &LinkType;
    }
    if (LastLoadedPath == "/Game/Blueprints/Netcode/UTNPLightningGun.UTNPLightningGun_C") {
        ++LightningLoads;
        return MissingLightningAsset ? nullptr : WrongLightningAsset ? &InstagibType : &LightningType;
    }
    if (LastLoadedPath == "/Game/Blueprints/Netcode/SACTFSniper.SACTFSniper_C") {
        ++SACTFLoads;
        return MissingSACTFAsset ? nullptr : WrongSACTFAsset ? &InstagibType : &SACTFSniperType;
    }
    return std::string(path).find("Instagib") != std::string::npos ? &InstagibType : &SniperType;
}
struct AUTWeapon {
    virtual ~AUTWeapon() = default;
    int Ammo = 0, MaxAmmo = 100, ShotsStatsName = 1;
    float BeamRefire = .12f;
    float GetRefireTime(int) const { return BeamRefire; }
    void StopFire(int) {}
};
struct AUTPlusSniper : AUTWeapon {
    bool bTrackImpressive = true;
    int HeadshotDamageType = 11;
    static UClass* StaticClass() { return &SniperType; }
};
struct LightningGun : AUTPlusSniper {
    LightningGun() { ShotsStatsName = 2; HeadshotDamageType = 22; }
};
struct SACTFSniper : AUTPlusSniper {
    SACTFSniper() { ShotsStatsName = 3; HeadshotDamageType = 33; BeamRefire = .7f; }
};
struct AUTPlusShockRifle : AUTWeapon {
    bool bTrackImpressive = true;
    static UClass* StaticClass() { return &InstagibType; }
    bool HasSharedInstagibFireModes() const { return true; }
};
template<class T> struct BeamArray : std::vector<T> {
    BeamArray() : std::vector<T>(2) {}
    bool IsValidIndex(int index) const { return index >= 0 && index < int(this->size()); }
};
struct UUTWeaponStateFiringLinkBeam_NCP {
    bool CorrectType = true;
    static UClass* StaticClass() { return &BeamStateType; }
    bool IsA(UClass* type) const { return CorrectType && type == &BeamStateType; }
};
struct AUTWeap_LinkGun_NCP : AUTWeapon {
    static UClass* StaticClass() { return &LinkBaseType; }
    struct BeamInfo { int Damage = 10, DamageType = 7; float TraceRange = 1800.f; };
    BeamArray<BeamInfo> InstantHitInfo;
    BeamArray<UUTWeaponStateFiringLinkBeam_NCP*> FiringState;
    UUTWeaponStateFiringLinkBeam_NCP BeamState;
    AUTWeap_LinkGun_NCP() { FiringState[0] = FiringState[1] = &BeamState; }
};
struct Movement {
    static UClass* StaticClass() { return &TrainerType; }
    UClass* GetClass() const { return StaticClass(); }
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
    UClass* ClassType = &TrainerType;
    UClass* GetClass() const { return ClassType; }
    virtual float GetSimpleCollisionHalfHeight() const { return 108.f; }
    void SetActorLocationAndRotation(FVector p, FRotator, bool, void*, ETeleportType) { ++Teleports; Position=p; }
    void SetActorLocation(FVector p, bool, void*, ETeleportType) { ++Teleports; Position=p; }
    FVector GetActorLocation() const { return Position; }
    void Destroy() { ++Destroys; }
    bool IsPendingKillPending() const { return Destroys > 0; }
};
struct AUTCharacter : APawn {
    struct Capsule { float HalfHeight=108.f; float GetScaledCapsuleHalfHeight() const { return HalfHeight; } } Shape;
    Movement Move;
    AUTPlusSniper Sniper;
    LightningGun Lightning;
    SACTFSniper SACTF;
    AUTPlusShockRifle Instagib;
    AUTWeap_LinkGun_NCP Link;
    bool bCanBeDamaged = true, Dead = false;
    int Discards = 0, Creates = 0, Switches = 0;
    bool IsDead() const { return Dead; }
    Movement* GetCharacterMovement() { return &Move; }
    const Capsule* GetCapsuleComponent() const { return &Shape; }
    float GetSimpleCollisionHalfHeight() const override { return Shape.HalfHeight; }
    void DiscardAllInventory() { ++Discards; }
    AUTWeapon* CreateInventory(TSubclassOf<AUTWeapon> type) {
        ++Creates;
        if (type->Kind == 11) return &SACTF;
        if (type->Kind == 6) return &Lightning;
        if (type->Kind == 3) return &Link;
        return type->Kind == 2 ? static_cast<AUTWeapon*>(&Instagib) : static_cast<AUTWeapon*>(&Sniper);
    }
    void SwitchWeapon(AUTWeapon*) { ++Switches; }
};
template<class T> const T* UClass::GetDefaultObject() const {
    static AUTCharacter team, instagib;
    team.ClassType = &TrainerType; team.Shape.HalfHeight = 108.f;
    instagib.ClassType = &InstagibTrainerType; instagib.Shape.HalfHeight = 103.f;
    return static_cast<const T*>(Kind == 8 ? &instagib : &team);
}
struct USkeletalMeshComponent { void* SkeletalMesh=nullptr; TSubclassOf<APawn> AnimClass; };
struct ANCAimTrainerTarget : AUTCharacter {
    int Hides = 0;
    int* CharacterData = nullptr;
    static UClass* StaticClass() { return &TargetType; }
    bool HasCharacterAssets() const { return true; }
    const USkeletalMeshComponent* GetMesh() const { return nullptr; }
    void HideTarget() { ++Hides; }
};
struct ANCAimTrainerInstagibTarget : ANCAimTrainerTarget {
    static UClass* StaticClass() { return &InstagibTargetType; }
};
struct ANCAimTrainerSACTFTarget : ANCAimTrainerTarget {
    static UClass* StaticClass() { return &SACTFTargetType; }
};
template<> const ANCAimTrainerTarget* UClass::GetDefaultObject<ANCAimTrainerTarget>() const {
    static ANCAimTrainerTarget team, instagib;
    team.Shape.HalfHeight = 108.f; instagib.Shape.HalfHeight = 103.f;
    return Kind == 10 ? &instagib : &team;
}
struct ANCAimTrainerArena {
    bool IsPendingKillPending() const { return false; }
    bool HasArenaAssets() const { return true; }
    uint8 AssignedScenario=0;
    void SetScenario(uint8 scenario) { AssignedScenario=scenario; }
};
enum class ESpawnActorCollisionHandlingMethod { AlwaysSpawn };
struct FActorSpawnParameters { ESpawnActorCollisionHandlingMethod SpawnCollisionHandlingOverride; };
template<class T> struct TArray : std::vector<T> {
    int Num() const { return int(this->size()); }
    void Add(T value) { this->push_back(value); }
    template<class Predicate> void RemoveAll(Predicate predicate) {
        this->erase(std::remove_if(this->begin(), this->end(), predicate), this->end());
    }
};
struct State { bool bOnlySpectator = false; };
struct AController {
    virtual ~AController() = default;
    State* PlayerState = nullptr;
    APawn* Pawn = nullptr;
    int Rotations = 0;
    FRotator ControlRotation, ClientRotation;
    bool ClientResetCamera = false;
    APawn* GetPawn() const { return Pawn; }
    void UnPossess() { Pawn = nullptr; }
    void SetControlRotation(FRotator rotation) { ++Rotations; ControlRotation=rotation; }
    void ClientSetRotation(FRotator rotation, bool resetCamera) {
        ++Rotations; ClientRotation=rotation; ClientResetCamera=resetCamera;
    }
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
    ANCAimTrainerArena Room;
    std::vector<std::unique_ptr<ANCAimTrainerTarget>> SpawnedTargets;
    template<class T> T* SpawnActor(FVector, FRotator, FActorSpawnParameters) { return static_cast<T*>(&Room); }
    template<class T> T* SpawnActor(UClass* type, FVector position, FRotator, FActorSpawnParameters) {
        auto target = std::unique_ptr<ANCAimTrainerTarget>(new ANCAimTrainerTarget);
        target->ClassType = type; target->Position = position;
        target->Shape.HalfHeight = type->GetDefaultObject<ANCAimTrainerTarget>()->Shape.HalfHeight;
        T* result = static_cast<T*>(target.get()); SpawnedTargets.push_back(std::move(target)); return result;
    }
};
struct BaseGame {
    World TheWorld;
    AUTCharacter SpawnedPawn;
    int NumPlayers = 1, ReadyCalls = 0, NextTickStarts = 0, Restarts = 0;
    bool AutoRestartOnLogin = false, FailNextRestart = false;
    World* GetWorld() { return &TheWorld; }
    virtual ~BaseGame() = default;
    virtual UClass* GetDefaultPawnClassForController_Implementation(AController*) { return &TrainerType; }
    virtual void RestartPlayer(AController* player) {
        ++Restarts;
        if (FailNextRestart) { FailNextRestart = false; return; }
        SpawnedPawn.ClassType = GetDefaultPawnClassForController_Implementation(player);
        SpawnedPawn.Shape.HalfHeight = SpawnedPawn.ClassType->GetDefaultObject<AUTCharacter>()->Shape.HalfHeight;
        player->Pawn = &SpawnedPawn;
        // Stock spawning inherits the map start orientation. Moving the pawn
        // into the trainer room alone does not update either controller view.
        player->SetControlRotation(FRotator(19.f,137.f,0.f));
        player->ClientSetRotation(FRotator(19.f,137.f,0.f),false);
    }
    void PostLogin(APlayerController* player) { if (AutoRestartOnLogin) RestartPlayer(player); }
    bool ReadyToStartMatch_Implementation() {
        ++ReadyCalls;
        if (TheWorld.TimeSeconds == 0.f) { ++NextTickStarts; return false; }
        return true;
    }
};
struct FNCAimTrainerProgress {
    uint8 Scenario = 0, Phase = 0;
    bool bMovementPractice = false, bUseLightningGun = false;
    float RemainingSeconds = 60;
};
struct ANCAimTrainerGame : BaseGame {
    using Super = BaseGame;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TSubclassOf<AUTWeapon> SniperClass, LightningClass, SACTFSniperClass, InstagibClass, LinkClass;
    FVector ArenaOrigin{0.f, 0.f, 50000.f};
    FNCAimTrainerProgress Progress;
    int Publishes = 0, Fetches = 0;
    Text SetupError, RunId;
    FString UnrankedReason;
    int NetMode=NM_Standalone;
    void* BaseMutator=nullptr;
    bool bRankedRun=false, bPreviousContact=false, bPreviousFiring=false;
    float PhaseStartedAt=0;
    double TrackedSeconds=0, FiredSeconds=0;
    struct { void Initialize(int) {} } Schedule;
    using Room = ANCAimTrainerArena;
    Room TheRoom;
    Room* Arena=&TheRoom;
    TArray<ANCAimTrainerTarget*> Targets;
    struct Settings { float Dilation=1.f; float GetEffectiveTimeDilation() { return Dilation; } } Options;
    Settings* GetWorldSettings() { return &Options; }
    int GetNetMode() { return NetMode; }
    static int StaticClass() { return 1; }
    int GetClass() { return StaticClass(); }
    bool IsTrainee(const ANCAimTrainerPlayerController* pc) const { return pc && pc==Trainee; }
    void HideAllTargets() {}
    bool EnsureArena();
    bool FailSetup(const char* message) { SetupError = message; return false; }
    void PublishProgress() { ++Publishes; }
    void RefreshLeaderboard() { ++Fetches; }
    void ResetLocalSession() {}
    void StartLocalSession() {}
    bool ReadyToStartMatch_Implementation();
    void PostLogin(APlayerController*);
    void RestartPlayer(AController*) override;
    UClass* GetDefaultPawnClassForController_Implementation(AController*) override;
    bool ConfigurePawn();
    bool IsInsidePracticeLane(const AUTCharacter*) const;
    void SelectScenario(ANCAimTrainerPlayerController*,uint8,bool=false);
    void SetMovementPractice(ANCAimTrainerPlayerController*,bool);
    void StartTraining(ANCAimTrainerPlayerController*,bool=false);
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
        Require(f.Game.Progress.Phase==1 && f.Game.bRankedRun && f.Game.Progress.bMovementPractice,
            "standard movement run cannot enter its own ranked board or lost option");
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
    } else if (name == "run_clock_reset") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        for(int run=0;run<2;++run) {
            f.Game.TrackedSeconds=12.0; f.Game.FiredSeconds=25.0;
            f.Game.bPreviousContact=true; f.Game.bPreviousFiring=true;
            f.Game.StartTraining(&f.Player);
            Require(f.Game.Progress.Phase==1 && f.Game.TrackedSeconds==0.0 && f.Game.FiredSeconds==0.0
                    && !f.Game.bPreviousContact && !f.Game.bPreviousFiring,
                    "new run carried previous contact/firing clocks into score or accuracy");
            f.Game.Progress.Phase=2;
            f.Game.AbortTraining(&f.Player);
        }
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
    } else if (name == "tracking_weapon") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        Require(f.Game.Progress.Scenario == 0 && f.Game.RunWeapon == &f.Game.SpawnedPawn.Link,
                "tracking did not equip the normal NCP Link class");
        Require(LastLoadedPath == "/Game/Blueprints/Netcode/NCPLinkGun.NCPLinkGun_C",
                "tracking resolved a different weapon content asset");
        f.Game.SelectScenario(&f.Player,1);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Sniper, "headshot selection lost sniper");
        f.Game.SelectScenario(&f.Player,2);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Instagib, "popup selection lost instagib");
        f.Game.SelectScenario(&f.Player,0);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Link, "returning to tracking retained precision weapon");
    } else if (name == "precision_popup") {
        f.BeginWorld(); f.Game.NetMode=1; f.Game.PostLogin(&f.Player);
        for (bool lightning : {false,true}) {
            f.Game.SelectScenario(&f.Player,2); // Deliberately start on IG pawn/weapon.
            f.Game.SelectScenario(&f.Player,3,lightning);
            Require(f.Game.Progress.Scenario==3 && f.Game.Progress.bUseLightningGun==lightning
                    && f.Game.RunWeapon==(lightning ? static_cast<AUTWeapon*>(&f.Game.SpawnedPawn.Lightning)
                        : static_cast<AUTWeapon*>(&f.Game.SpawnedPawn.Sniper)),"precision popup did not equip selected NCP rifle");
            Require(f.Game.SpawnedPawn.ClassType==&TrainerType && f.Game.Arena->AssignedScenario==2,
                    "precision popup lost TeamArena pawn or popup cover geometry");
            f.Game.SetMovementPractice(&f.Player,true); f.Game.StartTraining(&f.Player,lightning);
            Require(f.Game.bRankedRun && f.Game.Progress.bMovementPractice && f.Game.Progress.Scenario==3,
                    "precision movement preset not eligible for its own board");
            f.Game.AbortTraining(&f.Player);
        }
    } else if (name == "sactf_select") {
        f.BeginWorld(); f.Game.NetMode=1; f.Game.PostLogin(&f.Player);
        f.Game.SelectScenario(&f.Player,1,true); // Prime the unrelated Lightning cache.
        for (uint8 scenario : {uint8(4), uint8(5)}) {
            for (bool lightning : {false,true}) {
                f.Game.SelectScenario(&f.Player,scenario,lightning);
                Require(f.Game.Progress.Scenario==scenario && !f.Game.Progress.bUseLightningGun
                        && f.Game.RunWeapon==&f.Game.SpawnedPawn.SACTF,
                        "SACTF selection fell back to Sniper/Lightning or kept Lightning preference");
                Require(f.Game.SpawnedPawn.ClassType==&SACTFTrainerType
                        && f.Game.Arena->AssignedScenario==(scenario==4 ? 1 : 2),
                        "SACTF mode selected wrong native profile or headshot/popup arena");
                for(auto* target : f.Game.Targets)
                    Require(target->GetClass()==&SACTFTargetType, "SACTF target pool retained another movement profile");
                Require(LastLoadedPath=="/Game/Blueprints/Netcode/SACTFSniper.SACTFSniper_C"
                        && SACTFLoads==1 && f.Game.RunWeapon->GetRefireTime(0)==.7f,
                        "SACTF asset path/cache or weapon refire was lost");
                f.Game.SetMovementPractice(&f.Player,true);
                f.Game.StartTraining(&f.Player,lightning);
                Require(f.Game.Progress.Phase==1 && f.Game.bRankedRun && f.Game.Progress.bMovementPractice
                        && !f.Game.Progress.bUseLightningGun && f.Game.RunWeapon==&f.Game.SpawnedPawn.SACTF,
                        "SACTF start lost its weapon or separately ranked movement preset");
                f.Game.AbortTraining(&f.Player);
            }
        }
        f.Game.SelectScenario(&f.Player,1,true);
        Require(f.Game.RunWeapon==&f.Game.SpawnedPawn.Lightning && LightningLoads==1,
                "SACTF cache replaced the normal Lightning scenario");
        f.Game.SelectScenario(&f.Player,5,true);
        Require(f.Game.RunWeapon==&f.Game.SpawnedPawn.SACTF && SACTFLoads==1,
                "returning to SACTF reused the wrong cached rifle");
    } else if (name == "sactf_assets") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        MissingSACTFAsset=true;
        for(uint8 scenario : {uint8(4),uint8(5)}) {
            f.Game.SelectScenario(&f.Player,scenario,true);
            Require(!f.Game.RunWeapon && f.Game.Progress.Phase==0
                    && f.Game.SetupError.find("MutSaCTF")!=std::string::npos,
                    "missing SACTF pak silently equipped another weapon or hid recovery instruction");
            f.Game.StartTraining(&f.Player,true);
            Require(f.Game.Progress.Phase==0 && !f.Game.RunWeapon,
                    "missing SACTF rifle was allowed to start");
        }
        const int failedLoads=SACTFLoads;
        MissingSACTFAsset=false;
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase==1 && f.Game.RunWeapon==&f.Game.SpawnedPawn.SACTF
                && SACTFLoads==failedLoads+1 && f.Game.SetupError.empty(),
                "SACTF lookup failure was cached permanently after content became available");
    } else if (name == "sactf_content") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        WrongSACTFAsset=true;
        const int creates=f.Game.SpawnedPawn.Creates;
        f.Game.SelectScenario(&f.Player,4,true);
        Require(!f.Game.RunWeapon && !f.Game.SetupError.empty() && f.Game.SpawnedPawn.Creates==creates,
                "SACTF asset with wrong native base was equipped");
        WrongSACTFAsset=false; f.Game.SACTFSniperClass=nullptr;
        f.Game.SelectScenario(&f.Player,5,false);
        Require(f.Game.RunWeapon==&f.Game.SpawnedPawn.SACTF,
                "corrected SACTF native class did not recover");
        const auto* rifle=Cast<AUTPlusSniper>(f.Game.RunWeapon);
        Require(rifle && rifle->HeadshotDamageType==33 && rifle->ShotsStatsName==3,
                "SACTF rifle lost its own damage type or shot counter");
        f.Game.SpawnedPawn.SACTF.ShotsStatsName=NAME_None;
        Require(!f.Game.ConfigurePawn() && f.Game.SetupError.find("shot counter")!=std::string::npos,
                "SACTF rifle without authoritative shot counter was accepted");
    } else if (name == "lightning_select") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        f.Game.SelectScenario(&f.Player,1,true);
        Require(f.Game.Progress.bUseLightningGun && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning,
                "headshot selection ignored Lightning preference");
        Require(LastLoadedPath == "/Game/Blueprints/Netcode/UTNPLightningGun.UTNPLightningGun_C",
                "Lightning selection resolved an invented or stock asset");
        const auto* rifle = Cast<AUTPlusSniper>(f.Game.RunWeapon);
        Require(rifle && rifle->ShotsStatsName == 2 && rifle->HeadshotDamageType == 22,
                "Lightning lost its own shot counter or headshot damage type");
        Require(f.Game.ConfigurePawn() && LightningLoads == 1,
                "valid Lightning class was not cached for re-equipping");
        f.Game.SelectScenario(&f.Player,2,true);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Instagib,
                "Lightning preference replaced the instagib scenario weapon");
        f.Game.SelectScenario(&f.Player,0,true);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Link,
                "Lightning preference replaced the tracking scenario weapon");
        f.Game.SelectScenario(&f.Player,1,false);
        Require(!f.Game.Progress.bUseLightningGun && f.Game.RunWeapon == &f.Game.SpawnedPawn.Sniper,
                "selecting Sniper retained the cached Lightning weapon");
        Require(f.Game.RunWeapon->ShotsStatsName == 1, "Sniper inherited Lightning's shot counter");
        f.Game.SelectScenario(&f.Player,1,true);
        Require(f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning && LightningLoads == 1,
                "switching back to Lightning used the Sniper cache");
    } else if (name == "lightning_start") {
        f.BeginWorld(); f.Game.NetMode=1; f.Game.PostLogin(&f.Player);
        f.Game.SelectScenario(&f.Player,1,false);
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 1 && f.Game.Progress.bUseLightningGun
                && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning && f.Game.bRankedRun,
                "start did not use the current Lightning preference in the ranked headshot preset");
        f.Game.Progress.Phase = 3;
        f.Game.StartTraining(&f.Player,false);
        Require(f.Game.Progress.Phase == 1 && !f.Game.Progress.bUseLightningGun
                && f.Game.RunWeapon == &f.Game.SpawnedPawn.Sniper && f.Game.bRankedRun,
                "retry did not use the updated Sniper preference");
    } else if (name == "lightning_lifecycle") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player); f.Game.SelectScenario(&f.Player,1,true);
        for (bool movement : {true, false}) {
            f.Game.SetMovementPractice(&f.Player,movement);
            Require(f.Game.Progress.bUseLightningGun && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning,
                    "movement option reset the chosen headshot weapon");
        }
        f.Game.StartTraining(&f.Player,true);
        for (uint8 phase : {uint8(1), uint8(2)}) {
            f.Game.Progress.Phase=phase;
            const int creates = f.Game.SpawnedPawn.Creates, publishes = f.Game.Publishes;
            f.Game.SelectScenario(&f.Player,1,false);
            f.Game.StartTraining(&f.Player,false);
            Require(f.Game.Progress.Phase == phase && f.Game.Progress.bUseLightningGun
                    && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning
                    && f.Game.SpawnedPawn.Creates == creates && f.Game.Publishes == publishes,
                    "countdown or active-run request changed the selected weapon");
        }
        f.Game.AbortTraining(&f.Player);
        Require(f.Game.Progress.Phase == 0 && f.Game.Progress.Scenario == 1
                && f.Game.Progress.bUseLightningGun && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning,
                "abort forgot the selected Lightning weapon");
    } else if (name == "lightning_authority") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player); f.Game.SelectScenario(&f.Player,1,true);
        ANCAimTrainerPlayerController stranger;
        const int creates = f.Game.SpawnedPawn.Creates, publishes = f.Game.Publishes;
        for (auto* requestor : {&stranger, static_cast<ANCAimTrainerPlayerController*>(nullptr)}) {
            f.Game.SelectScenario(requestor,1,false);
            f.Game.StartTraining(requestor,false);
            f.Game.SetMovementPractice(requestor,true);
            f.Game.AbortTraining(requestor);
        }
        f.Game.SelectScenario(&f.Player,6,false);
        Require(f.Game.Progress.Phase == 0 && f.Game.Progress.Scenario == 1
                && f.Game.Progress.bUseLightningGun && !f.Game.Progress.bMovementPractice
                && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning
                && f.Game.SpawnedPawn.Creates == creates && f.Game.Publishes == publishes,
                "unauthorized or invalid-scenario request changed the trainee's choice");
    } else if (name == "lightning_assets") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player); f.Game.SelectScenario(&f.Player,1,false);
        MissingLightningAsset = true;
        f.Game.SelectScenario(&f.Player,1,true);
        Require(!f.Game.RunWeapon && f.Game.Progress.bUseLightningGun && LightningLoads == 1,
                "missing Lightning content fell back to a cached Sniper");
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 0 && !f.Game.RunWeapon && LightningLoads == 2,
                "missing Lightning started a run or stopped retrying the asset lookup");
        MissingLightningAsset = false;
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 1 && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning
                && LightningLoads == 3, "later Lightning pak mount did not recover startup");
    } else if (name == "lightning_content") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        WrongLightningAsset = true;
        const int creates = f.Game.SpawnedPawn.Creates;
        f.Game.SelectScenario(&f.Player,1,true);
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 0 && !f.Game.RunWeapon && f.Game.SpawnedPawn.Creates == creates,
                "wrong native base class was accepted as Lightning content");
        WrongLightningAsset = false; f.Game.LightningClass = nullptr;
        f.Game.SpawnedPawn.Lightning.ShotsStatsName = NAME_None;
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 0 && !f.Game.SetupError.empty(),
                "Lightning without its own shot counter entered countdown");
        f.Game.SpawnedPawn.Lightning.ShotsStatsName = 2;
        f.Game.StartTraining(&f.Player,true);
        Require(f.Game.Progress.Phase == 1 && f.Game.RunWeapon == &f.Game.SpawnedPawn.Lightning,
                "valid Lightning content could not recover startup");
    } else if (name == "tracking_assets") {
        f.BeginWorld(); MissingLinkAsset = true; f.Game.PostLogin(&f.Player);
        Require(!f.Game.RunWeapon && !f.Game.ConfigurePawn(), "missing Link silently started tracking");
        f.Game.StartTraining(&f.Player);
        Require(f.Game.Progress.Phase == 0, "missing Link admitted countdown");
        MissingLinkAsset = false; WrongLinkAsset = true;
        Require(!f.Game.ConfigurePawn() && !f.Game.RunWeapon, "non-Link class accepted for tracking");
        WrongLinkAsset = false; f.Game.LinkClass = nullptr;
        Require(f.Game.ConfigurePawn() && f.Game.RunWeapon == &f.Game.SpawnedPawn.Link,
                "later Link pak availability did not recover tracking");
    } else if (name == "tracking_beam_content") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        auto& link = f.Game.SpawnedPawn.Link;
        for (float range : {1600.f, 1799.f, 1799.999f, 0.f, -1.f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
            link.InstantHitInfo[1].TraceRange = range;
            Require(!f.Game.ConfigurePawn(), "beam below 1800 or nonfinite range admitted standard targets");
            f.Game.StartTraining(&f.Player);
            Require(f.Game.Progress.Phase == 0 && !f.Game.SetupError.empty(),
                    "invalid beam range entered a run despite failed content validation");
        }
        link.InstantHitInfo[1].TraceRange = 1800.f; link.BeamState.CorrectType = false;
        Require(!f.Game.ConfigurePawn(), "non-NCP beam state accepted");
        link.BeamState.CorrectType = true; link.FiringState[1] = nullptr;
        Require(!f.Game.ConfigurePawn(), "missing beam state accepted");
        link.FiringState[1] = &link.BeamState; link.InstantHitInfo[1].Damage = 0;
        Require(!f.Game.ConfigurePawn(), "zero-damage beam accepted");
        link.InstantHitInfo[1].Damage = 10; link.InstantHitInfo[1].DamageType = 0;
        Require(!f.Game.ConfigurePawn(), "beam without damage type accepted");
        link.InstantHitInfo[1].DamageType = 7;
        Require(f.Game.ConfigurePawn(), "valid normal NCP Link at exactly 1800 range rejected");
        link.InstantHitInfo.clear();
        Require(!f.Game.ConfigurePawn(), "missing beam hit-info slot accepted");
    } else if (name == "tracking_refire") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        auto& link = f.Game.SpawnedPawn.Link;
        for (float interval : {0.f, -.1f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
            link.BeamRefire = interval;
            Require(!f.Game.ConfigurePawn(), "invalid beam refire admitted damage division");
            f.Game.StartTraining(&f.Player);
            Require(f.Game.Progress.Phase == 0, "invalid beam refire admitted countdown");
        }
        link.BeamRefire = .12f;
        Require(f.Game.ConfigurePawn(), "valid positive beam refire did not recover");
        f.Game.StartTraining(&f.Player);
        Require(f.Game.Progress.Phase == 1, "valid beam could not start after correcting refire");
    } else if (name == "profile_replacement") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        const int initialRestarts = f.Game.Restarts;
        f.Game.SelectScenario(&f.Player, 1);
        Require(f.Game.Restarts == initialRestarts, "same Team profile needlessly respawned trainee");
        f.Game.SelectScenario(&f.Player, 2);
        Require(f.Game.Restarts == initialRestarts+1 && f.Game.SpawnedPawn.Destroys == 1
                && f.Player.GetPawn()->GetClass() == &InstagibTrainerType
                && f.Game.SpawnedPawn.Position.Z == 50103.f,
                "Instagib scenario did not replace the pawn with its own class/standing seat");
        f.Game.SpawnedPawn.Shape.HalfHeight = 72.f;
        Require(f.Game.ConfigurePawn() && f.Game.SpawnedPawn.Position.Z == 50103.f
                && f.Game.IsInsidePracticeLane(&f.Game.SpawnedPawn),
                "crouched live capsule replaced the Instagib class standing height");
        for (int phase : {1,2}) {
            f.Game.Progress.Phase = uint8(phase);
            f.Game.SelectScenario(&f.Player, 0);
            Require(f.Game.Progress.Scenario == 2 && f.Game.Restarts == initialRestarts+1,
                    "countdown or active run changed the pawn profile");
        }
        f.Game.Progress.Phase = 0;
        f.Game.SelectScenario(&f.Player, 0);
        Require(f.Game.Restarts == initialRestarts+2 && f.Game.SpawnedPawn.Destroys == 2
                && f.Player.GetPawn()->GetClass() == &TrainerType && f.Game.SpawnedPawn.Position.Z == 50108.f,
                "leaving Instagib retained the smaller native pawn");
    } else if (name == "profile_spawn_recovery") {
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        f.Game.FailNextRestart = true;
        f.Game.SelectScenario(&f.Player, 2);
        Require(!f.Player.GetPawn() && !f.Game.RunWeapon && !f.Game.SetupError.empty()
                && f.Game.Progress.Phase == 0, "failed replacement did not fail closed");
        const int failedRestarts = f.Game.Restarts;
        f.Game.SelectScenario(&f.Player, 2);
        Require(f.Game.Restarts == failedRestarts+1 && f.Player.GetPawn()
                && f.Player.GetPawn()->GetClass() == &InstagibTrainerType
                && f.Game.SpawnedPawn.Position.Z == 50103.f && f.Game.RunWeapon == &f.Game.SpawnedPawn.Instagib
                && f.Game.SetupError.empty(), "retry did not recover missing pawn with selected profile");
    } else if (name == "profile_view_to_instagib" || name == "profile_view_from_instagib"
               || name == "profile_view_retry") {
        // Exercise ConfigurePawn's Super::RestartPlayer paths, which bypass
        // the trainer RestartPlayer override that otherwise resets the view.
        f.BeginWorld(); f.Game.PostLogin(&f.Player);
        uint8 scenario=2;
        if (name == "profile_view_from_instagib") {
            f.Game.SelectScenario(&f.Player,2);
            scenario=0;
        } else if (name == "profile_view_retry") {
            f.Game.FailNextRestart=true;
            f.Game.SelectScenario(&f.Player,2);
            Require(!f.Player.GetPawn(),"fixture did not enter missing-pawn recovery");
        }
        f.Player.SetControlRotation(FRotator(-31.f,-77.f,0.f));
        f.Player.ClientSetRotation(FRotator(-31.f,-77.f,0.f),false);
        const int previousRestarts=f.Game.Restarts;
        f.Game.SelectScenario(&f.Player,scenario);
        Require(f.Game.Restarts==previousRestarts+1 && f.Player.GetPawn()
                && f.Player.GetPawn()->GetClass()==(scenario==2 ? &InstagibTrainerType : &TrainerType),
                "fixture did not respawn the requested native profile");
        Require(f.Player.ControlRotation.IsZero(),"profile respawn retained map-start server aim");
        Require(f.Player.ClientRotation.IsZero() && f.Player.ClientResetCamera,
                "profile respawn retained map-start owning-client camera");
    } else if (name == "target_pool_profiles") {
        Require(f.Game.EnsureArena() && f.Game.Targets.Num() == NCAimTrainerLayout::TargetCount,
                "initial target pool was incomplete");
        for (int scenario : {0,1,2,2,1,0}) {
            const auto oldTargets = f.Game.Targets;
            const size_t before = f.Game.TheWorld.SpawnedTargets.size();
            UClass* expectedClass = scenario == 2 ? &InstagibTargetType : &TargetType;
            const bool replace = oldTargets[0]->GetClass() != expectedClass;
            f.Game.Progress.Scenario = uint8(scenario);
            Require(f.Game.EnsureArena() && f.Game.Targets.Num() == NCAimTrainerLayout::TargetCount,
                    "profile change left an incomplete target pool");
            Require(f.Game.TheWorld.SpawnedTargets.size() == before + (replace ? NCAimTrainerLayout::TargetCount : 0),
                    "pool replaced unchanged profiles or reused wrong native classes");
            for (int slot=0; slot<f.Game.Targets.Num(); ++slot) {
                const auto* target = f.Game.Targets[slot];
                const auto seat = slot < NCAimTrainerLayout::HeadSlotCount
                    ? NCAimTrainerLayout::HeadSeat(slot) : NCAimTrainerLayout::PopupDodgerSeat();
                Require(target->GetClass() == expectedClass && !target->IsPendingKillPending()
                        && target->Position.Z == 50000.f+seat.FloorZ+(scenario==2 ? 103.f : 108.f),
                        "target spawned with wrong profile or standing capsule support");
                Require(oldTargets[slot]->Destroys == (replace ? 1 : 0),
                        "replacement left old targets alive or destroyed retained ones");
            }
        }
        f.Game.Targets[1]->Destroy(); f.Game.Targets[3] = nullptr;
        Require(f.Game.EnsureArena() && f.Game.Targets.Num() == NCAimTrainerLayout::TargetCount,
                "destroyed/null target entries did not recover within selected profile");
    } else { Require(false, "unknown case"); }
}
'''


class AimTrainerStartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler, cls.environment, cls.msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-startup-")
        cls.addClassCleanup(cls.temporary.cleanup)
        native = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "bool ANCAimTrainerGame::EnsureArena",
            "bool ANCAimTrainerGame::ReadyToStartMatch_Implementation",
            "void ANCAimTrainerGame::PostLogin",
            "void ANCAimTrainerGame::RestartPlayer",
            "UClass* ANCAimTrainerGame::GetDefaultPawnClassForController_Implementation",
            "bool ANCAimTrainerGame::ConfigurePawn",
            "bool ANCAimTrainerGame::IsInsidePracticeLane",
            "void ANCAimTrainerGame::SelectScenario",
            "void ANCAimTrainerGame::SetMovementPractice",
            "void ANCAimTrainerGame::StartTraining",
            "void ANCAimTrainerGame::AbortTraining",
        )
        layout = (PLUGIN / "Source/Private/NCAimTrainerLayout.h").as_posix()
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()
        cls.translation = "\n".join([ADAPTER, f'#include "{layout}"', f'#include "{policy}"']
                                    + [native_function(native, s) for s in signatures] + [CASES])
        cls.configure_function = native_function(native, "bool ANCAimTrainerGame::ConfigurePawn")
        cls.executable = cls.compile_fixture("trainer_startup", cls.translation)

    @classmethod
    def compile_fixture(cls, name, translation):
        directory = Path(cls.temporary.name)
        source = directory / (name + ".cpp")
        source.write_text(translation, encoding="utf-8")
        executable = directory / (name + (".exe" if os.name == "nt" else ""))
        if cls.msvc:
            command = [cls.compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{executable}", f"/Fo{directory / (name + '.obj')}"]
        else:
            command = [cls.compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Startup adapter compilation failed:\n{result.stdout}\n{result.stderr}")
        return executable

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stock_first_frame_deferral_is_preserved(self): self.run_case("first_frame")
    def test_no_spawn_or_inventory_before_world_begin_play(self): self.run_case("pre_begin")
    def test_initial_login_equips_after_deferred_restart(self): self.run_case("deferred_login")
    def test_in_progress_login_equips_existing_pawn_once(self): self.run_case("existing_pawn_login")
    def test_postlogin_manual_restart_equips_once(self): self.run_case("late_manual_restart")
    def test_spectator_never_receives_trainee_pawn(self): self.run_case("spectator")
    def test_movement_choice_survives_run_lifecycle_and_can_rank_separately(self): self.run_case("movement_lifecycle")
    def test_precision_popup_equips_saved_rifle_and_teamarena_pawn_with_movement_board(self): self.run_case("precision_popup")
    def test_sactf_modes_use_exact_cached_rifle_profile_and_arena_independent_of_lightning(self): self.run_case("sactf_select")
    def test_missing_sactf_content_fails_closed_and_recovers_after_mount(self): self.run_case("sactf_assets")
    def test_sactf_requires_sniper_native_base_and_own_shot_counter(self): self.run_case("sactf_content")
    def test_restart_clears_firing_and_contact_clocks(self): self.run_case("run_clock_reset")
    def test_lane_accepts_crouching_jumping_but_rejects_escapes(self): self.run_case("lane_bounds")
    def test_each_scenario_equips_its_real_weapon(self): self.run_case("tracking_weapon")
    def test_headshot_selection_uses_exact_lightning_asset_and_independent_cache(self): self.run_case("lightning_select")
    def test_start_and_retry_apply_current_headshot_weapon_choice(self): self.run_case("lightning_start")
    def test_lightning_choice_survives_movement_and_abort_but_cannot_change_midrun(self): self.run_case("lightning_lifecycle")
    def test_other_players_and_invalid_scenarios_cannot_change_headshot_choice(self): self.run_case("lightning_authority")
    def test_missing_lightning_fails_closed_and_recovers_after_mount(self): self.run_case("lightning_assets")
    def test_lightning_requires_sniper_base_and_its_own_shot_counter(self): self.run_case("lightning_content")
    def test_tracking_requires_link_assets_and_recovers_after_mount(self): self.run_case("tracking_assets")
    def test_tracking_requires_real_beam_state_damage_and_range(self): self.run_case("tracking_beam_content")
    def test_tracking_refire_must_be_finite_and_positive(self): self.run_case("tracking_refire")
    def test_scenario_changes_replace_native_pawn_only_outside_run_and_use_class_height(self): self.run_case("profile_replacement")
    def test_failed_profile_replacement_recovers_on_next_selection(self): self.run_case("profile_spawn_recovery")
    def test_instagib_profile_respawn_resets_server_aim_and_client_camera(self): self.run_case("profile_view_to_instagib")
    def test_team_profile_respawn_resets_server_aim_and_client_camera(self): self.run_case("profile_view_from_instagib")
    def test_missing_pawn_retry_resets_server_aim_and_client_camera(self): self.run_case("profile_view_retry")
    def test_target_pool_replaces_profiles_and_uses_each_class_standing_height(self): self.run_case("target_pool_profiles")

    def test_camera_regression_catches_missing_authority_or_client_reset(self):
        # Mutate only temporary translation units. Each side is necessary:
        # resetting authority aim must not mask an uncorrected remote camera.
        for name, reset in (
            ("authority", "Trainee->SetControlRotation(FRotator::ZeroRotator);"),
            ("client", "Trainee->ClientSetRotation(FRotator::ZeroRotator, true);"),
        ):
            with self.subTest(reset=name):
                self.assertEqual(self.configure_function.count(reset), 1)
                broken = self.configure_function.replace(reset, "", 1)
                translation = self.translation.replace(self.configure_function, broken, 1)
                executable = self.compile_fixture("missing_camera_reset_" + name, translation)
                for case in ("profile_view_to_instagib", "profile_view_from_instagib", "profile_view_retry"):
                    result = subprocess.run([str(executable), case], env=self.environment,
                                            capture_output=True, text=True, timeout=15)
                    self.assertNotEqual(result.returncode, 0, f"{case} missed absent {name} reset")
                    self.assertIn("profile respawn retained map-start", result.stderr)


if __name__ == "__main__":
    unittest.main()
