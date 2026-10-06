"""Exercise the real trainer input/state functions with a small native adapter.

This verifies phase gates, focus handoff, request admission and the stock UT
deferred-fire drain after possession. It does not stand in for a UE client/server
playtest of cursor capture or replicated presentation.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <new>
#include <algorithm>
#include <cctype>
#define TEXT(value) value
enum class ESearchCase { IgnoreCase };
struct FString : std::string {
    using std::string::string;
    using std::string::operator=;
    bool Equals(const char* other, ESearchCase) const {
        std::string normalized = *this;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char c) { return char(std::toupper(c)); });
        return normalized == other;
    }
};
struct FPaths { static std::string GeneratedConfigDir() { return "Client/Config/"; } };
struct Config {
    std::string Choice;
    int Reads = 0;
    void GetString(const char* section, const char* key, FString& out, const std::string& path) {
        if (std::string(section) != "WeaponSkinsPlus" || std::string(key) != "HitscanChoice"
            || path != "Client/Config/Mod.ini") std::abort();
        ++Reads; out = Choice;
    }
} TestConfig;
Config* GConfig = &TestConfig;
using uint8 = uint8_t;
using int32 = int32_t;
constexpr int32 INDEX_NONE = -1;
constexpr int ROLE_Authority = 3;
constexpr int NAME_Playing = 1, NAME_Spectating = 2;
#define UE_SERVER 0
enum class FKey { Other, F6, M, One, Two, Three, NumPadOne, NumPadTwo, NumPadThree, Enter };
using EKeys = FKey;
enum EInputEvent { IE_Pressed, IE_Released, IE_Repeat };
struct FPlatformTime { static double Now; static double Seconds() { return Now; } };
double FPlatformTime::Now = 100.;
struct AClientHitsounds { static int Warmups; static void EnsureCatalog() { ++Warmups; } };
int AClientHitsounds::Warmups = 0;
struct FNCAimTrainerProgress { uint8 Scenario = 0, Phase = 0; int Score = 0; bool bMovementPractice = false; };
struct FVector { float X, Y, Z; FVector(float x=0, float y=0, float z=0): X(x), Y(y), Z(z) {} };
enum MovementMode { MOVE_None, MOVE_Walking, MOVE_Falling };
struct APawn { virtual ~APawn() = default; virtual void PawnStartFire(uint8) {} };
struct UCharacterMovementComponent {
    virtual ~UCharacterMovementComponent() = default;
    bool Enabled = true, Constrained = false;
    bool bWantsToCrouch = false, Crouched = false;
    float Speed = 0.f;
    MovementMode Mode = MOVE_Walking;
    FVector Normal, Origin;
    void DisableMovement() { Enabled = false; Mode = MOVE_None; }
    void SetMovementMode(MovementMode mode) { Mode = mode; Enabled = mode != MOVE_None; }
    void SetPlaneConstraintNormal(FVector v) { Normal = v; }
    void SetPlaneConstraintOrigin(FVector v) { Origin = v; }
    void SetPlaneConstraintEnabled(bool enabled) { Constrained = enabled; }
    void StopMovementImmediately() { Speed = 0.f; }
    void UnCrouch(bool) { Crouched = false; }
};
struct AUTCharacter;
struct UNCAimTrainerMovement : UCharacterMovementComponent {
    AUTCharacter* Owner = nullptr;
    void ResetTrainerMovement(bool practice);
};
struct AUTCharacter : APawn {
    UNCAimTrainerMovement Move;
    int Fires[2] = {0, 0}, Stops[2] = {0, 0};
    FVector MovementInput;
    bool bPressedJump = false;
    AUTCharacter() { Move.Owner = this; }
    UCharacterMovementComponent* GetCharacterMovement() { return &Move; }
    FVector GetActorLocation() const { return FVector(100, 200, 300); }
    void AddMovementInput(FVector direction, float value) {
        MovementInput = FVector(direction.X * value, direction.Y * value, direction.Z * value);
    }
    void StartFire(uint8 mode) { ++Fires[mode]; }
    void StopFire(uint8 mode) { ++Stops[mode]; }
};
// The actual shared reset helper is covered in test_aim_trainer_movement.py.
// Here its effects let us test when the real controller invokes it.
void UNCAimTrainerMovement::ResetTrainerMovement(bool practice) {
    StopMovementImmediately(); bWantsToCrouch = false; Owner->bPressedJump = false;
    UnCrouch(false); SetPlaneConstraintNormal(FVector(1, 0, 0));
    SetPlaneConstraintOrigin(FVector(-1800, 0, 50108)); SetPlaneConstraintEnabled(practice);
    if (practice) SetMovementMode(MOVE_Walking); else DisableMovement();
}
struct FDeferredFireInput {
    uint8 FireMode = 0; bool bStartFire = false;
    FDeferredFireInput() = default;
    FDeferredFireInput(uint8 mode, bool start) : FireMode(mode), bStartFire(start) {}
};
struct FDeferredInputs : std::vector<FDeferredFireInput> { void Empty() { clear(); } };
void* operator new(std::size_t size, FDeferredInputs& inputs) {
    (void)size; inputs.emplace_back(); return &inputs.back();
}
void operator delete(void* pointer, FDeferredInputs& inputs) noexcept {
    (void)pointer; inputs.pop_back();
}
struct MockPlayerState { bool bOnlySpectator = false; };
struct MockGameState { bool HasMatchStarted() const { return true; } };
struct MockWorld { MockGameState State; MockGameState* GetGameState() { return &State; } };
template<class T> T* Cast(APawn* pawn) { return dynamic_cast<T*>(pawn); }
template<class T> T* Cast(UCharacterMovementComponent* movement) { return dynamic_cast<T*>(movement); }
struct AUTPlayerController {
    int BaseKeys = 0;
    int Jumps = 0, Crouches = 0, CrouchToggles = 0;
    int MoveInputLocks = 0, Restarts = 0;
    bool IgnoreLook = false, bFirePressed = false, bAltFirePressed = false;
    bool bPlayerIsWaiting = false, bAutoCam = false;
    bool bIsHoldingFloorSlide = false;
    int StateName = NAME_Playing;
    float MovementForwardAxis = 0.f, MovementStrafeAxis = 0.f;
    APawn* Pawn = nullptr;
private:
    // Match stock access: subclasses must use GetPawn()/GetUTCharacter().
    AUTCharacter* UTCharacter = nullptr;
public:
    MockPlayerState* PlayerState = nullptr;
    MockWorld World;
    FDeferredInputs DeferredFireInputs;
    APawn* GetPawn() const { return Pawn; }
    MockWorld* GetWorld() { return &World; }
    bool IsInState(int state) const { return StateName == state; }
    void PlayMenuSelectSound() {}
    void ServerRestartPlayer() {}
    void ServerRestartPlayerAltFire() {}
    void ViewSelf() {}
    void BeginPlay() {}
    void ClientRestart_Implementation(APawn* pawn) {
        Pawn = pawn; MoveInputLocks = 0; ++Restarts;
        UTCharacter = Cast<AUTCharacter>(pawn);
        if (auto character = Cast<AUTCharacter>(pawn)) character->Move.SetMovementMode(MOVE_Walking);
    }
    bool IsMoveInputIgnored() const { return MoveInputLocks > 0; }
    void SetIgnoreMoveInput(bool value) { MoveInputLocks += value ? 1 : -1; }
    bool InputKey(FKey, EInputEvent, float, bool) { ++BaseKeys; return false; }
    void OnFire();
    void OnAltFire();
    void OnStopFire();
    void OnStopAltFire();
    void ApplyDeferredFireInputs();
    void Jump() { ++Jumps; }
    void Crouch() { ++Crouches; }
    void ToggleCrouch() { ++CrouchToggles; }
};
struct ANCAimTrainerPlayerController : AUTPlayerController {
    using Super = AUTPlayerController;
    FNCAimTrainerProgress TrainerProgress;
    double NextTrainerRequestTime[4] = { 0., 0., 0., 0. };
    uint8 LastPresentedPhase = 255;
    bool bLastPresentedMovementPractice = false;
    bool bTrackingPrimaryHeld = false, bTrackingAltHeld = false;
    bool InputFocus = true, Local = true;
    int Role = ROLE_Authority, Selects = 0, Starts = 0, Aborts = 0, NetUpdates = 0, InputUpdates = 0;
    uint8 LastSelection = 255;
    int MovementSelections = 0;
    bool LastMovementSelection = false;
    bool HasTrainerInputFocus() const { return InputFocus; }
    bool IsLocalController() const { return Local; }
    bool LastLightningChoice = false;
    void ServerTrainerSelectScenario(uint8 value, bool lightning) { ++Selects; LastSelection = value; LastLightningChoice = lightning; }
    void ServerTrainerStart(bool lightning) { ++Starts; LastLightningChoice = lightning; }
    void ServerTrainerAbort() { ++Aborts; }
    void ServerTrainerSetMovementPractice(bool enabled) { ++MovementSelections; LastMovementSelection = enabled; }
    void ForceNetUpdate() { ++NetUpdates; }
    void UpdateInputMode() { ++InputUpdates; }
    void ClientRestart_Implementation(APawn*);
    void ApplyTrainerMovementMode();
    void MoveForward(float);
    void MoveRight(float);
    void Jump();
    void Crouch();
    void ToggleCrouch();
    bool IsTrainerMenuVisible() const;
    bool InputKey(FKey, EInputEvent, float, bool);
    void OnFire();
    void OnAltFire();
    void OnStopFire();
    void OnStopAltFire();
    void SetTrackingFireHeld(bool, bool);
    void SelectTrainerScenario(uint8);
    void StartTrainerRun();
    bool PrefersTrainerLightningGun() const;
    void ReturnToTrainerMenu();
    void ToggleTrainerMovementPractice();
    bool AdmitTrainerRequest(uint8);
    void SetTrainerProgress(const FNCAimTrainerProgress&);
    void OnRep_TrainerProgress();
};
'''

CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void MenuControls() {
    ANCAimTrainerPlayerController pc;
    for (uint8 phase : {uint8(0), uint8(3)}) {
        pc.TrainerProgress.Phase = phase;
        Require(pc.InputKey(EKeys::Three, IE_Pressed, 1.f, false), "menu mode key escaped");
        Require(pc.LastSelection == 2, "scenario mapping wrong");
        const int selects = pc.Selects;
        pc.InputKey(EKeys::Three, IE_Repeat, 1.f, false);
        pc.InputKey(EKeys::Three, IE_Released, 0.f, false);
        Require(pc.Selects == selects, "repeat/release generated extra requests");
        Require(pc.InputKey(EKeys::Enter, IE_Pressed, 1.f, false), "start key escaped");
        const int starts = pc.Starts;
        pc.InputKey(EKeys::Enter, IE_Released, 0.f, false);
        Require(pc.Starts == starts, "key release started another run");
        Require(pc.InputKey(EKeys::M, IE_Pressed, 1.f, false), "movement key escaped menu");
        const int movementSelections = pc.MovementSelections;
        pc.InputKey(EKeys::M, IE_Repeat, 1.f, false);
        pc.InputKey(EKeys::M, IE_Released, 0.f, false);
        Require(pc.MovementSelections == movementSelections && pc.LastMovementSelection,
                "movement toggle repeated or requested wrong option");
    }
    const int selects = pc.Selects;
    pc.SelectTrainerScenario(255);
    Require(pc.Selects == selects, "invalid scenario sent");
}
void Focus() {
    ANCAimTrainerPlayerController pc;
    pc.InputFocus = false;
    for (FKey key : {EKeys::One, EKeys::Enter, EKeys::F6, EKeys::M})
        Require(!pc.InputKey(key, IE_Pressed, 1.f, false), "stock menu/chat key intercepted");
    Require(pc.BaseKeys == 4 && pc.Selects == 0 && pc.Starts == 0 && pc.Aborts == 0 && pc.MovementSelections == 0,
            "focus handoff changed trainer state");
}
void ActiveControls() {
    ANCAimTrainerPlayerController pc;
    for (uint8 phase : {uint8(1), uint8(2)}) {
        pc.TrainerProgress.Phase = phase;
        pc.InputKey(EKeys::One, IE_Pressed, 1.f, false);
        pc.InputKey(EKeys::Enter, IE_Pressed, 1.f, false);
        pc.SelectTrainerScenario(1);
        pc.StartTrainerRun();
        pc.InputKey(EKeys::M, IE_Pressed, 1.f, false);
        pc.ToggleTrainerMovementPractice();
        Require(pc.Selects == 0 && pc.Starts == 0 && pc.MovementSelections == 0,
                "active/countdown run replaced or movement option changed locally");
        Require(pc.InputKey(EKeys::F6, IE_Pressed, 1.f, false), "abort key not consumed");
    }
    Require(pc.Aborts == 2, "abort unavailable during countdown/run");
}
void FireGates() {
    // Standalone and an owning network client both drain through the same stock
    // method; assert arrival at the pawn, not merely entry into Super::OnFire.
    for (int role : {ROLE_Authority, 1}) {
        ANCAimTrainerPlayerController pc;
        AUTCharacter pawn;
        pc.Role = role;
        pc.BeginPlay();
        pc.ClientRestart_Implementation(&pawn);
        for (uint8 scenario = 0; scenario < 3; ++scenario) {
            pc.TrainerProgress.Scenario = scenario;
            for (uint8 phase = 0; phase < 4; ++phase) {
                pc.TrainerProgress.Phase = phase;
                const int before = pawn.Fires[0], beforeAlt = pawn.Fires[1];
                pc.OnFire(); pc.OnAltFire();
                Require(pawn.Fires[0] == before, "stock fire did not remain deferred");
                pc.ApplyDeferredFireInputs();
                const int expectedPrimary = phase == 2 && scenario != 0 ? 1 : 0;
                const int expectedAlt = phase == 2 ? 1 : 0;
                Require(pawn.Fires[0] == before + expectedPrimary && pawn.Fires[1] == beforeAlt + expectedAlt,
                        "trainer fire never reached pawn or escaped its phase/scenario gate");
                Require(pc.DeferredFireInputs.empty(), "deferred fire queue did not drain");
                Require(!pawn.Move.Enabled && !pc.IgnoreLook, "shooting unlocked translation or locked view");
                pc.OnStopFire(); pc.OnStopAltFire(); pc.ApplyDeferredFireInputs();
            }
        }
    }
}
void TrackingBeamHold() {
    for (int role : {ROLE_Authority, 1}) {
        ANCAimTrainerPlayerController pc; AUTCharacter pawn;
        pc.Role = role; pc.ClientRestart_Implementation(&pawn);
        pc.TrainerProgress.Phase = 2; pc.TrainerProgress.Scenario = 0;
        pc.OnFire(); pc.OnFire(); pc.OnAltFire(); pc.ApplyDeferredFireInputs();
        Require(pawn.Fires[0] == 0 && pawn.Fires[1] == 1,
                "tracking plasma fired or two buttons restarted the real beam");
        pc.OnStopFire(); pc.ApplyDeferredFireInputs();
        Require(pawn.Stops[1] == 0, "primary release stopped still-held secondary beam");
        pc.OnStopAltFire(); pc.ApplyDeferredFireInputs();
        Require(pawn.Stops[1] == 1, "final tracking button did not stop beam");
        pc.OnAltFire(); pc.OnFire(); pc.OnStopAltFire(); pc.ApplyDeferredFireInputs();
        Require(pawn.Fires[1] == 2 && pawn.Stops[1] == 1, "reverse order broke held-beam aggregation");
        pc.LastPresentedPhase = 2; pc.TrainerProgress.Phase = 3; pc.OnRep_TrainerProgress();
        pc.ApplyDeferredFireInputs();
        Require(!pc.bTrackingPrimaryHeld && !pc.bTrackingAltHeld && pawn.Stops[1] == 2,
                "results did not clear held beam");
        pc.TrainerProgress.Phase = 2; pc.OnFire(); pc.ApplyDeferredFireInputs();
        Require(pawn.Fires[1] == 3, "retry inherited stuck held-input state");
        pc.ClientRestart_Implementation(&pawn);
        Require(!pc.bTrackingPrimaryHeld && !pc.bTrackingAltHeld, "possession retained old beam inputs");
    }
}
void ExternalFireLock() {
    ANCAimTrainerPlayerController pc;
    AUTCharacter pawn;
    pc.ClientRestart_Implementation(&pawn);
    pc.TrainerProgress.Phase = 2;
    pc.TrainerProgress.Scenario = 2;
    pc.SetIgnoreMoveInput(true);
    pc.OnFire(); pc.OnAltFire();
    pc.ApplyDeferredFireInputs();
    Require(pawn.Fires[0] == 0 && pawn.Fires[1] == 0 && pc.MoveInputLocks == 1,
            "trainer bypassed or erased an external input lock");
    pc.SetIgnoreMoveInput(false);
    pc.OnFire(); pc.OnAltFire();
    pc.ApplyDeferredFireInputs();
    Require(pawn.Fires[0] == 1 && pawn.Fires[1] == 1, "firing did not resume after external unlock");
}
void Admission() {
    ANCAimTrainerPlayerController pc;
    Require(pc.AdmitTrainerRequest(0), "first selection rejected");
    Require(!pc.AdmitTrainerRequest(0), "selection flood not bounded");
    Require(pc.AdmitTrainerRequest(1), "quick select/start dropped");
    Require(pc.AdmitTrainerRequest(2), "abort blocked by start");
    Require(pc.AdmitTrainerRequest(3), "movement choice blocked by another action");
    Require(!pc.AdmitTrainerRequest(3), "movement option spam not bounded");
    Require(!pc.AdmitTrainerRequest(4), "next invalid action accepted");
    Require(!pc.AdmitTrainerRequest(255), "out-of-bounds action accepted");
    FPlatformTime::Now += .16;
    Require(pc.AdmitTrainerRequest(0), "throttle never recovered");
}
void StatePublish() {
    ANCAimTrainerPlayerController pc;
    AUTCharacter pawn;
    pc.ClientRestart_Implementation(&pawn);
    FNCAimTrainerProgress next;
    next.Phase = 2; next.Score = 123;
    pc.Role = 1;
    pc.SetTrainerProgress(next);
    Require(pc.TrainerProgress.Score == 0 && pc.NetUpdates == 0, "client set authoritative score");
    pc.Role = ROLE_Authority;
    pc.SetTrainerProgress(next);
    Require(pc.TrainerProgress.Score == 123 && pc.NetUpdates == 1 && pawn.Stops[0] == 0,
            "run start presentation incorrect");
    next.Phase = 3;
    pc.SetTrainerProgress(next);
    pc.ApplyDeferredFireInputs();
    Require(pawn.Stops[0] == 1 && pawn.Stops[1] == 1, "held fire not released at run end");
    for (int i = 0; i < 20; ++i) pc.SetTrainerProgress(next);
    pc.ApplyDeferredFireInputs();
    Require(pawn.Stops[0] == 1 && pawn.Stops[1] == 1 && pc.NetUpdates == 2,
            "repeated presentation samples spammed fire releases or forced replication");
}
void PossessionLock() {
    ANCAimTrainerPlayerController pc;
    AUTCharacter pawn;
    pc.BeginPlay();
    pc.BeginPlay();
    Require(pc.MoveInputLocks == 0 && !pc.IgnoreLook, "initial trainer input lock blocks stock fire or view");
    for (int i = 0; i < 3; ++i) {
        pc.ClientRestart_Implementation(&pawn);
        Require(pc.MoveInputLocks == 0 && !pc.IgnoreLook && !pawn.Move.Enabled,
                "possession blocked stock firing/view or restored translation/gravity");
    }
    ANCAimTrainerPlayerController remote;
    remote.Local = false;
    remote.BeginPlay(); remote.ClientRestart_Implementation(&pawn);
    Require(remote.MoveInputLocks == 0, "nonlocal controller acquired a client input lock");
}
void MovementPractice() {
    for (int role : {ROLE_Authority, 1}) {
        ANCAimTrainerPlayerController pc;
        AUTCharacter pawn;
        pc.Role = role;
        pc.ClientRestart_Implementation(&pawn);
        Require(!pawn.Move.Enabled && !pawn.Move.Constrained && pawn.Move.Normal.X == 1.f
                && pawn.Move.Origin.X == -1800.f, "fixed lane not initialized on possession");
        pc.Jump(); pc.Crouch(); pc.ToggleCrouch();
        Require(pc.Jumps == 0 && pc.Crouches == 0 && pc.CrouchToggles == 0, "fixed mode queued mobility actions");
        pc.TrainerProgress.bMovementPractice = true;
        pc.OnRep_TrainerProgress();
        Require(pawn.Move.Mode == MOVE_Walking && pawn.Move.Constrained && !pc.IsMoveInputIgnored(),
                "movement practice did not unlock walking or blocked firing");
        pc.Jump(); pc.Crouch(); pc.ToggleCrouch();
        Require(pc.Jumps == 1 && pc.Crouches == 1 && pc.CrouchToggles == 1, "practice blocked mobility actions");
        pc.MoveRight(-.7f);
        Require(pawn.MovementInput.X == 0.f && pawn.MovementInput.Y == -.7f && pawn.MovementInput.Z == 0.f
                && pc.MovementStrafeAxis == -.7f, "strafe input not world-Y or stock dodge axis lost");
        pc.MovementForwardAxis = 1.f;
        pc.MoveForward(1.f);
        Require(pc.MovementForwardAxis == 0.f && pawn.MovementInput.X == 0.f,
                "forward input changed distance or left a dodge axis");
        pawn.Move.SetMovementMode(MOVE_Falling);
        for (int i = 0; i < 10; ++i) pc.OnRep_TrainerProgress();
        Require(pawn.Move.Mode == MOVE_Falling, "score replication cancelled a jump/dodge");
        pc.ClientRestart_Implementation(&pawn);
        Require(pawn.Move.Mode == MOVE_Walking, "practice possession lost its movement option");
        pc.TrainerProgress.bMovementPractice = false;
        pc.OnRep_TrainerProgress();
        Require(pawn.Move.Mode == MOVE_None && !pc.IsMoveInputIgnored(), "fixed mode used global input gate");
        pawn.MovementInput = FVector();
        pc.MoveRight(1.f);
        Require(pawn.MovementInput.Y == 0.f, "fixed mode added movement input");
    }
}
void MovementRetryPosture() {
    for (int role : {ROLE_Authority, 1}) {
        ANCAimTrainerPlayerController pc;
        AUTCharacter pawn;
        pc.Role = role;
        pc.TrainerProgress.bMovementPractice = true;
        pc.ClientRestart_Implementation(&pawn);
        pc.TrainerProgress.Phase = 3;
        pc.OnRep_TrainerProgress();
        pawn.Move.Crouched = pawn.Move.bWantsToCrouch = pawn.bPressedJump = pc.bIsHoldingFloorSlide = true;
        pawn.Move.SetMovementMode(MOVE_Falling);
        pawn.Move.Speed = 1500.f;
        pc.TrainerProgress.Phase = 1;
        pc.OnRep_TrainerProgress();
        Require(pawn.Move.Mode == MOVE_Walking && !pawn.Move.Crouched && !pawn.Move.bWantsToCrouch
                && !pawn.bPressedJump && !pc.bIsHoldingFloorSlide && pawn.Move.Speed == 0.f,
                "retry retained crouched capsule, airborne velocity or held mobility state");
        pawn.Move.SetMovementMode(MOVE_Falling);
        pawn.Move.Speed = 400.f;
        for (int i = 0; i < 10; ++i) pc.OnRep_TrainerProgress();
        Require(pawn.Move.Mode == MOVE_Falling && pawn.Move.Speed == 400.f,
                "repeated countdown updates cancelled new movement input");
    }
}
void HitsoundWarmup() {
    ANCAimTrainerPlayerController pc;
    pc.TrainerProgress.Phase = 0;
    pc.OnRep_TrainerProgress();
    Require(AClientHitsounds::Warmups == 1, "picker did not prepare hitsound assets");
    for (int i = 0; i < 20; ++i) pc.OnRep_TrainerProgress();
    Require(AClientHitsounds::Warmups == 1, "progress samples repeated catalog preparation");
    ANCAimTrainerPlayerController remote;
    remote.Local = false;
    remote.OnRep_TrainerProgress();
    Require(AClientHitsounds::Warmups == 1, "nonlocal controller prepared audio assets");
}
void HitscanPreference() {
    ANCAimTrainerPlayerController pc;
    for (const char* choice : {"", "Sniper", "LG", "lg", "lG", "invalid"}) {
        TestConfig.Choice = choice;
        const bool expected = std::string(choice) == "LG" || std::string(choice) == "lg" || std::string(choice) == "lG";
        pc.SelectTrainerScenario(1);
        Require(pc.LastLightningChoice == expected, "menu did not forward the owning player's saved hitscan preference");
        pc.StartTrainerRun();
        Require(pc.LastLightningChoice == expected, "start did not resample and send the hitscan preference");
    }
    TestConfig.Choice = "LG";
    pc.Local = false;
    const int reads = TestConfig.Reads;
    Require(!pc.PrefersTrainerLightningGun() && TestConfig.Reads == reads,
            "nonlocal controller read the server's config as a client preference");
    pc.Local = true;
    for (uint8 phase : {uint8(1), uint8(2)}) {
        pc.TrainerProgress.Phase = phase;
        const int selects = pc.Selects, starts = pc.Starts;
        pc.SelectTrainerScenario(1); pc.StartTrainerRun();
        Require(pc.Selects == selects && pc.Starts == starts && TestConfig.Reads == reads,
                "active run admitted a hitscan preference change");
    }
    GConfig = nullptr;
    Require(!pc.PrefersTrainerLightningGun(), "unavailable settings did not fall back to sniper");
    GConfig = &TestConfig;
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required"); const std::string name(argv[1]);
    if (name == "menu") MenuControls();
    else if (name == "focus") Focus();
    else if (name == "active") ActiveControls();
    else if (name == "fire") FireGates();
    else if (name == "tracking_beam") TrackingBeamHold();
    else if (name == "external_lock") ExternalFireLock();
    else if (name == "admission") Admission();
    else if (name == "state") StatePublish();
    else if (name == "possession") PossessionLock();
    else if (name == "movement") MovementPractice();
    else if (name == "retry_posture") MovementRetryPosture();
    else if (name == "hitsound_warmup") HitsoundWarmup();
    else if (name == "hitscan_preference") HitscanPreference();
    else Require(false, "unknown case");
}
'''


class AimTrainerControllerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-controller-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerPlayerController.cpp").read_text(encoding="utf-8-sig")
        stock = (PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTPlayerController.cpp").read_text(encoding="utf-8-sig")
        stock_signatures = (
            "void AUTPlayerController::OnFire",
            "void AUTPlayerController::OnAltFire",
            "void AUTPlayerController::OnStopFire",
            "void AUTPlayerController::OnStopAltFire",
            "void AUTPlayerController::ApplyDeferredFireInputs",
        )
        signatures = (
            "void ANCAimTrainerPlayerController::ClientRestart_Implementation",
            "void ANCAimTrainerPlayerController::ApplyTrainerMovementMode",
            "void ANCAimTrainerPlayerController::MoveForward",
            "void ANCAimTrainerPlayerController::MoveRight",
            "void ANCAimTrainerPlayerController::Jump",
            "void ANCAimTrainerPlayerController::Crouch",
            "void ANCAimTrainerPlayerController::ToggleCrouch",
            "bool ANCAimTrainerPlayerController::IsTrainerMenuVisible",
            "bool ANCAimTrainerPlayerController::InputKey",
            "void ANCAimTrainerPlayerController::OnFire",
            "void ANCAimTrainerPlayerController::OnAltFire",
            "void ANCAimTrainerPlayerController::OnStopFire",
            "void ANCAimTrainerPlayerController::OnStopAltFire",
            "void ANCAimTrainerPlayerController::SetTrackingFireHeld",
            "void ANCAimTrainerPlayerController::SelectTrainerScenario",
            "void ANCAimTrainerPlayerController::StartTrainerRun",
            "bool ANCAimTrainerPlayerController::PrefersTrainerLightningGun",
            "void ANCAimTrainerPlayerController::ReturnToTrainerMenu",
            "void ANCAimTrainerPlayerController::ToggleTrainerMovementPractice",
            "bool ANCAimTrainerPlayerController::AdmitTrainerRequest",
            "void ANCAimTrainerPlayerController::SetTrainerProgress",
            "void ANCAimTrainerPlayerController::OnRep_TrainerProgress",
        )
        source = directory / "trainer_controller.cpp"
        source.write_text("\n".join(
            [ADAPTER] + [native_function(stock, s) for s in stock_signatures]
            + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_controller.exe" if os.name == "nt" else "trainer_controller")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_controller.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Controller adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_menu_controls_and_repeat_suppression(self): self.run_case("menu")
    def test_stock_menu_and_chat_keep_input_focus(self): self.run_case("focus")
    def test_cannot_replace_active_run_and_can_abort(self): self.run_case("active")
    def test_each_scenario_uses_real_fire_only_during_run(self): self.run_case("fire")
    def test_tracking_buttons_hold_one_real_secondary_beam_until_both_release(self): self.run_case("tracking_beam")
    def test_external_ignore_input_still_blocks_stock_firing(self): self.run_case("external_lock")
    def test_request_throttle_allows_quick_select_then_start(self): self.run_case("admission")
    def test_authority_and_one_time_held_fire_release(self): self.run_case("state")
    def test_possession_keeps_movement_locked_and_mouse_look_live(self): self.run_case("possession")
    def test_movement_practice_stays_lateral_and_score_updates_preserve_jumps(self): self.run_case("movement")
    def test_new_countdown_resets_crouched_or_airborne_owner_without_repeated_resets(self): self.run_case("retry_posture")
    def test_first_local_picker_prepares_hitsounds_once(self): self.run_case("hitsound_warmup")
    def test_saved_local_hitscan_choice_is_sent_only_with_menu_and_start_requests(self): self.run_case("hitscan_preference")


if __name__ == "__main__":
    unittest.main()
