"""Compile the production shot-flush and movement-send methods with small adapters.

Stock UT move flags and batching predicates are compiled too. The transport is a
recorder: these tests prove submission order, not network delivery order. No UBT.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using int32 = int32_t;
using uint8 = uint8_t;
using uint64 = uint64_t;
constexpr uint64 MAX_uint64 = std::numeric_limits<uint64>::max();
constexpr float KINDA_SMALL_NUMBER = 0.0001f;
using FName = std::string;
const FName NAME_None;
uint64 GFrameCounter = 100;
enum ENetMode { NM_Standalone, NM_DedicatedServer, NM_ListenServer, NM_Client };
enum ENetRole { ROLE_None, ROLE_SimulatedProxy, ROLE_AutonomousProxy, ROLE_Authority };
struct FVector {
    float X = 0.f, Y = 0.f, Z = 0.f;
    FVector() = default;
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
    bool Equals(const FVector& other, float tolerance) const {
        return std::fabs(X - other.X) <= tolerance && std::fabs(Y - other.Y) <= tolerance
            && std::fabs(Z - other.Z) <= tolerance;
    }
    bool operator!=(const FVector& other) const { return !Equals(other, 0.f); }
    float operator|(const FVector& other) const { return X*other.X + Y*other.Y + Z*other.Z; }
    float SizeSquared() const { return *this | *this; }
};
struct FRotator { float Pitch = 0.f, Yaw = 0.f, Roll = 0.f; };
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Abs(float value) { return std::fabs(value); }
};
template<class T> struct TestArray {
    std::vector<T> Items;
    int32 Num() const { return static_cast<int32>(Items.size()); }
    T& Last() { return Items.back(); }
    const T& operator[](int32 index) const { return Items[static_cast<std::size_t>(index)]; }
};
template<class T> struct TestPtr {
    T* Value;
    TestPtr(T* value = nullptr) : Value(value) {}
    bool IsValid() const { return Value != nullptr; }
    T* Get() const { return Value; }
    T* operator->() const { return Value; }
};
struct UPrimitiveComponent { bool Relative = false; };
namespace MovementBaseUtility {
    bool UseRelativeLocation(UPrimitiveComponent* base) { return base && base->Relative; }
}
struct FSavedMove_Character;
using FSavedMovePtr = TestPtr<FSavedMove_Character>;
struct FSavedMove_Character {
    virtual ~FSavedMove_Character() = default;
    enum { FLAG_Custom_1 = 0x20, FLAG_Custom_2 = 0x40, FLAG_Custom_3 = 0x80 };
    float TimeStamp = 0.f, AccelMag = 0.f, AccelMagThreshold = 1.f, AccelDotThreshold = 0.9f;
    bool bOldTimeStampBeforeReset = false, bPressedJump = false, bWantsToCrouch = false;
    FVector SavedLocation, SavedRelativeLocation, Acceleration, AccelNormal;
    FRotator SavedControlRotation;
    TestPtr<UPrimitiveComponent> EndBase;
    FName EndBoneName;
    uint8 MovementMode = 1;
    virtual uint8 GetCompressedFlags() const = 0;
    virtual bool IsImportantMove(const FSavedMovePtr&) const = 0;
};
struct FSavedMove_UTCharacter : FSavedMove_Character {
    bool bShotSpawned = false, bPressedDodgeForward = false, bPressedDodgeBack = false;
    bool bPressedDodgeLeft = false, bPressedDodgeRight = false, bPressedSlide = false;
    bool bSavedIsDodgeLanding = false, bSavedIsRolling = false;
    bool bSavedWantsWallSlide = false, bSavedWantsSlide = false;
    bool NeedsRotationSent() const;
    bool IsCriticalMove(const FSavedMovePtr&) const;
    bool IsImportantMove(const FSavedMovePtr&) const override;
    uint8 GetCompressedFlags() const override;
};
struct FNetworkPredictionData_Client_Character {
    TestArray<FSavedMovePtr> SavedMoves;
    FSavedMovePtr LastAckedMove;
    bool bUpdatePosition = false;
    float ClientUpdateTime = 1.f, CurrentTimeStamp = 1.005f;
};
struct APlayerCameraManager {
    bool bUseClientSideCameraUpdates = true, bShouldSendClientSideCameraUpdate = false;
};
struct AController { virtual ~AController() = default; };
struct APlayerController : AController { APlayerCameraManager* PlayerCameraManager = nullptr; };
struct WorldSettings { float GetEffectiveTimeDilation() const { return 1.f; } };
struct BasedMovement { FName BoneName; FVector Location; };
struct ACharacter {
    virtual ~ACharacter() = default;
    AController* Controller = nullptr;
    WorldSettings Settings;
    AController* GetController() const { return Controller; }
    WorldSettings* GetWorldSettings() { return &Settings; }
};
struct Event {
    std::string Kind;
    float TimeStamp = 0.f, Yaw = 0.f, Pitch = 0.f;
    uint8 Flags = 0;
    FVector Location;
    UPrimitiveComponent* Base = nullptr;
    FName Bone;
    explicit Event(const char* kind) : Kind(kind) {}
};
struct AUTCharacter : ACharacter {
    ENetRole Role = ROLE_AutonomousProxy;
    bool Local = true, Dead = false, bClientUpdating = false;
    FVector Location;
    UPrimitiveComponent* Base = nullptr;
    BasedMovement Based;
    std::vector<Event> Events;
    bool IsLocallyControlled() const { return Local; }
    bool IsDead() const { return Dead; }
    FVector GetActorLocation() const { return Location; }
    UPrimitiveComponent* GetMovementBase() const { return Base; }
    const BasedMovement& GetBasedMovement() const { return Based; }
    void Record(const char* kind, float timestamp, uint8 flags, float yaw = 0.f, float pitch = 0.f) {
        Event event(kind); event.TimeStamp = timestamp; event.Flags = flags;
        event.Yaw = yaw; event.Pitch = pitch; Events.push_back(event);
    }
    void UTServerMoveOld(float t, FVector, float yaw, uint8 flags) { Record("old", t, flags, yaw); }
    void UTServerMoveSaved(float t, FVector, uint8 flags, float yaw, float pitch) {
        Record("saved", t, flags, yaw, pitch);
    }
    void UTServerMoveQuick(float t, FVector, uint8 flags) { Record("quick", t, flags); }
    void UTServerMove(float t, FVector, FVector location, uint8 flags, float yaw, float pitch,
                      UPrimitiveComponent* base, const FName& bone, uint8) {
        Record("full", t, flags, yaw, pitch);
        Events.back().Location = location; Events.back().Base = base; Events.back().Bone = bone;
    }
};
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct UUTCharacterMovement {
    bool CanDelaySendingMove(const FSavedMovePtr&);
};
struct UTeamArenaCharacterMovement : UUTCharacterMovement {
    ACharacter* CharacterOwner = nullptr;
    FNetworkPredictionData_Client_Character* Data = nullptr;
    ENetMode NetMode = NM_Client;
    bool bJustTeleported = false, Falling = false;
    uint8 PackedMode = 1;
    FVector Velocity;
    uint64 LastPreparedMoveFrame = MAX_uint64;
    float LastPreparedMoveTimeStamp = -1.f;
    ENetMode GetNetMode() const { return NetMode; }
    bool IsFalling() const { return Falling; }
    uint8 PackNetworkMovementMode() const { return PackedMode; }
    FNetworkPredictionData_Client_Character* GetPredictionData_Client_Character() { return Data; }
    bool FlushPendingMoveForShot();
    void UTCallServerMove();
};
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    UTeamArenaCharacterMovement Movement;
    FNetworkPredictionData_Client_Character Data;
    AUTCharacter Character;
    APlayerController Controller;
    APlayerCameraManager Camera;
    UPrimitiveComponent Base;
    FSavedMove_UTCharacter Move, Older, Ack;
    Fixture() {
        GFrameCounter = 100;
        Movement.CharacterOwner = &Character; Movement.Data = &Data;
        Character.Controller = &Controller; Controller.PlayerCameraManager = &Camera;
        Move.TimeStamp = Data.CurrentTimeStamp;
        Move.SavedControlRotation.Yaw = 37.f; Move.SavedControlRotation.Pitch = -12.f;
        Data.SavedMoves.Items.push_back(&Move);
    }
    void Prepare() {
        Movement.UTCallServerMove();
        Require(Character.Events.empty(), "fixture's fresh move was not batched");
    }
    bool Flush() { return Movement.FlushPendingMoveForShot(); }
};
void FreshFlush() {
    Fixture f; f.Prepare();
    const FVector location = f.Move.SavedLocation;
    const float stamp = f.Move.TimeStamp;
    Require(f.Flush(), "fresh unsent same-frame move was rejected");
    f.Character.Events.push_back(Event("fire"));
    Require(f.Character.Events.size() == 2 && f.Character.Events[0].Kind == "full"
        && f.Character.Events[1].Kind == "fire", "movement was not submitted before fire");
    const Event& sent = f.Character.Events[0];
    Require((sent.Flags & FSavedMove_Character::FLAG_Custom_3) != 0,
        "flushed move omitted stock shot marker");
    Require(sent.Yaw == 37.f && sent.Pitch == -12.f, "flushed move omitted saved aim rotation");
    Require(sent.TimeStamp == stamp && f.Move.TimeStamp == stamp
        && f.Move.SavedLocation.Equals(location, 0.f), "flush rewrote movement position or timestamp");
    Require(f.Data.ClientUpdateTime == stamp, "flush did not advance stock submission watermark");
    Require(f.Camera.bShouldSendClientSideCameraUpdate, "normal camera submission side effect lost");
    Require(!f.Flush() && f.Character.Events.size() == 2, "same move was submitted twice");
}
void AlreadySentWithOldMove() {
    Fixture f;
    f.Ack.TimeStamp = 0.98f; f.Data.LastAckedMove = &f.Ack;
    f.Older.TimeStamp = 0.99f; f.Older.bPressedDodgeLeft = true;
    f.Data.SavedMoves.Items.insert(f.Data.SavedMoves.Items.begin(), &f.Older);
    f.Move.bShotSpawned = true; f.Movement.UTCallServerMove();
    Require(f.Character.Events.size() == 2 && f.Character.Events[0].Kind == "old"
        && f.Character.Events[1].Kind == "full", "fixture did not exercise stock old-move resend");
    Require(!f.Flush() && f.Character.Events.size() == 2,
        "already submitted shot caused redundant old-move resend");
}
void FrameFreshness() {
    Fixture f; f.Prepare(); ++GFrameCounter;
    Require(!f.Flush() && !f.Move.bShotSpawned, "old-frame queued move was accepted");
    f.Movement.UTCallServerMove();
    Require(f.Movement.LastPreparedMoveFrame == 100 && !f.Flush(),
        "reentry refreshed the freshness stamp of an older saved move");
    f.Move.TimeStamp = f.Data.CurrentTimeStamp = 1.006f;
    f.Movement.UTCallServerMove();
    Require(f.Movement.LastPreparedMoveFrame == 101 && f.Flush(),
        "a genuinely new move did not become eligible in its own frame");
}
void TimestampReset() {
    Fixture f; f.Movement.LastPreparedMoveTimeStamp = 240.f;
    f.Movement.LastPreparedMoveFrame = 99;
    f.Data.ClientUpdateTime = 0.f;
    f.Move.TimeStamp = f.Data.CurrentTimeStamp = 0.005f;
    f.Prepare();
    Require(f.Movement.LastPreparedMoveTimeStamp == 0.005f && f.Flush(),
        "new post-reset timestamp was compared monotonically against old epoch");
    Fixture stale; stale.Prepare(); stale.Move.bOldTimeStampBeforeReset = true;
    Require(!stale.Flush() && !stale.Move.bShotSpawned, "pre-reset move was accepted");
}
void Rejections() {
    for (int which = 0; which < 22; ++which) {
        Fixture f; f.Prepare(); ACharacter nonUt; UPrimitiveComponent otherBase;
        switch (which) {
            case 0: f.Movement.CharacterOwner = nullptr; break;
            case 1: f.Movement.CharacterOwner = &nonUt; break;
            case 2: f.Movement.NetMode = NM_Standalone; break;
            case 3: f.Movement.NetMode = NM_DedicatedServer; break;
            case 4: f.Movement.NetMode = NM_ListenServer; break;
            case 5: f.Character.Role = ROLE_Authority; break;
            case 6: f.Character.Role = ROLE_SimulatedProxy; break;
            case 7: f.Character.Local = false; break;
            case 8: f.Character.Dead = true; break;
            case 9: f.Character.bClientUpdating = true; break;
            case 10: f.Movement.bJustTeleported = true; break;
            case 11: f.Movement.Data = nullptr; break;
            case 12: f.Data.bUpdatePosition = true; break;
            case 13: f.Data.SavedMoves.Items.clear(); break;
            case 14: f.Data.SavedMoves.Last() = nullptr; break;
            case 15: f.Character.Location.X = 1.f; break;
            case 16: f.Move.MovementMode = 2; break;
            case 17: f.Move.EndBase = &otherBase; break;
            case 18: f.Move.EndBoneName = "changed-bone"; break;
            case 19: f.Data.CurrentTimeStamp += 0.001f; break;
            case 20: f.Movement.LastPreparedMoveTimeStamp -= 0.001f; break;
            case 21: f.Data.ClientUpdateTime = f.Move.TimeStamp + 0.01f; break;
        }
        Require(!f.Flush() && f.Character.Events.empty() && !f.Move.bShotSpawned,
            "invalid owner/context/move was marked or sent");
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity()}) {
        Fixture move; move.Prepare(); move.Move.TimeStamp = invalid;
        Require(!move.Flush(), "nonfinite move timestamp was accepted");
        Fixture sent; sent.Prepare(); sent.Data.ClientUpdateTime = invalid;
        Require(!sent.Flush(), "nonfinite submission watermark was accepted");
    }
}
void RelativeBase() {
    Fixture f; f.Base.Relative = true; f.Character.Base = &f.Base; f.Move.EndBase = &f.Base;
    f.Character.Based.BoneName = f.Move.EndBoneName = "platform";
    f.Character.Based.Location = f.Move.SavedRelativeLocation = FVector(3.f, 4.f, 5.f);
    f.Prepare(); f.Character.Based.Location.X += 1.f;
    Require(!f.Flush() && !f.Move.bShotSpawned,
        "same world position hid a changed transmitted relative position");
    f.Character.Based.Location = f.Move.SavedRelativeLocation;
    Require(f.Flush(), "unchanged relative-base movement was rejected");
    const Event& sent = f.Character.Events.back();
    Require(sent.Location.Equals(FVector(3.f, 4.f, 5.f), 0.f)
        && sent.Base == &f.Base && sent.Bone == "platform", "relative move payload changed");
}
void QueuedBacklog() {
    Fixture f; f.Older.TimeStamp = 1.002f;
    f.Data.SavedMoves.Items.insert(f.Data.SavedMoves.Items.begin(), &f.Older);
    f.Prepare(); Require(f.Flush(), "fresh end of pending batch was rejected");
    Require(f.Character.Events.size() == 2 && f.Character.Events[0].Kind == "quick"
        && f.Character.Events[1].Kind == "full", "pending batch send order changed");
    Require(!f.Older.bShotSpawned && f.Move.bShotSpawned,
        "flush marked a prior queued move instead of the fresh frame");
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "fresh") FreshFlush();
    else if (name == "sent") AlreadySentWithOldMove();
    else if (name == "frame") FrameFreshness();
    else if (name == "reset") TimestampReset();
    else if (name == "guards") Rejections();
    else if (name == "base") RelativeBase();
    else if (name == "backlog") QueuedBacklog();
    else Require(false, "unknown case");
}
'''


class PrecisionMoveFlushTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-precision-move-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        movement = (PLUGIN / "Source/Private/TeamArenaCharacterMovement.cpp").read_text(encoding="utf-8-sig")
        stock_path = PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTCharMovementReplication.cpp"
        stock = stock_path.read_text(encoding="utf-8-sig")
        definitions = []
        for source, signatures in (
            (stock, ("bool UUTCharacterMovement::CanDelaySendingMove",
                     "bool FSavedMove_UTCharacter::NeedsRotationSent",
                     "bool FSavedMove_UTCharacter::IsCriticalMove",
                     "bool FSavedMove_UTCharacter::IsImportantMove",
                     "uint8 FSavedMove_UTCharacter::GetCompressedFlags")),
            (movement, ("bool UTeamArenaCharacterMovement::FlushPendingMoveForShot",
                        "void UTeamArenaCharacterMovement::UTCallServerMove")),
        ):
            definitions.extend(native_function(source, signature) for signature in signatures)
        source = directory / "flush.cpp"
        source.write_text(ADAPTER + "\n".join(definitions) + CASES, encoding="utf-8")
        cls.executable = directory / ("flush.exe" if os.name == "nt" else "flush")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'flush.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Movement adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fresh_move_submits_existing_marker_and_rotation_before_fire(self):
        self.run_case("fresh")

    def test_already_sent_move_does_not_resend_old_important_move(self):
        self.run_case("sent")

    def test_only_current_frame_is_fresh_and_reentry_does_not_refresh_it(self):
        self.run_case("frame")

    def test_timestamp_reset_accepts_new_epoch_but_rejects_old_moves(self):
        self.run_case("reset")

    def test_invalid_owner_context_correction_and_move_guards(self):
        self.run_case("guards")

    def test_relative_transmitted_position_and_base_bone_are_preserved(self):
        self.run_case("base")

    def test_prior_queued_frames_send_first_without_acquiring_shot_marker(self):
        self.run_case("backlog")


class PrecisionMoveFlushIntegrationTests(unittest.TestCase):
    def test_only_client_precision_hitscan_initial_fire_has_flush_hook(self):
        weapon = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        fire = native_function(weapon, "void AUTWeaponFix::FireShot()")
        client = native_function(fire, "if (Role < ROLE_Authority)")
        block = native_function(client, "if (CVarFlushMoveBeforePrecisionFire.GetValueOnGameThread()")
        for required in ("Cast<AUTPlusShockRifle>(this)", "Cast<AUTPlusSniper>(this)",
                         "bTrackHitScanReplication", "InstantHitInfo.IsValidIndex(CurrentFireMode)",
                         "InstantHitInfo[CurrentFireMode].DamageType != nullptr",
                         "InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.f",
                         "ProjClass[CurrentFireMode] == nullptr", "UTOwner->GetWeapon() == this"):
            self.assertIn(required, block)
        self.assertIn("CVarFlushMoveBeforePrecisionFire.GetValueOnGameThread() != 0", block)
        self.assertEqual(weapon.count("->FlushPendingMoveForShot()"), 1)
        self.assertLess(client.index("->FlushPendingMoveForShot()"),
                        client.index("\n\t\tServerStartFireFixed("))
        for signature in ("void AUTWeaponFix::QueueResendStartFireFixed",
                          "void AUTWeaponFix::ResendServerStartFireFixed_Implementation"):
            self.assertNotIn("FlushPendingMoveForShot", native_function(weapon, signature))

if __name__ == "__main__":
    unittest.main()
