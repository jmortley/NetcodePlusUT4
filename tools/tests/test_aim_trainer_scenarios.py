"""Native checks for scenario policy and the actual target scheduler methods.

Compile production timing, initialization, activation, hit and spawn methods
against a small actor adapter. Unreal dodge physics, replicated target movement
and the rendered firing cadence still require a packaged playtest.
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
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#define TEXT(value) value
using int32 = int;
constexpr int INDEX_NONE = -1;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static bool IsNearlyEqual(float a, float b) { return std::fabs(a - b) <= .0001f; }
    static float Max(float a, float b) { return a > b ? a : b; }
};
struct FVector {
    float X, Y, Z;
    FVector(float x=0, float y=0, float z=0) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& other) const { return FVector(X + other.X, Y + other.Y, Z + other.Z); }
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int Num() const { return int(this->size()); }
    bool IsValidIndex(int index) const { return index >= 0 && index < Num(); }
    void Add(T value) { this->push_back(value); }
    int IndexOfByKey(T value) const {
        for (int i = 0; i < Num(); ++i) if ((*this)[i] == value) return i;
        return INDEX_NONE;
    }
};
struct AActor { virtual ~AActor() = default; };
struct AUTPlayerState : AActor {};
struct AController : AActor { AUTPlayerState* PlayerState = nullptr; };
struct ANCAimTrainerPlayerController : AController {
    int Notifications = 0;
    void NotifyTrainerHit(float) { ++Notifications; }
};
struct AUTWeapon : AActor {
    float Refire = 1.f;
    float GetRefireTime(int) const { return Refire; }
    float GetWeaponShotsStats(AUTPlayerState*) const { return 0.f; }
};
struct AUTWeaponFix : AUTWeapon { float GetHitValidationPredictionTime() const { return 0.f; } };
struct AUTPlusSniper : AUTWeaponFix { int HeadshotDamageType = 5; };
struct AUTWeap_LinkGun_NCP : AUTWeaponFix {
    struct BeamInfo { int DamageType = 7; };
    TArray<BeamInfo> InstantHitInfo;
    bool IsFiring() const { return true; }
    int GetCurrentFireMode() const { return 1; }
};
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct FDamageEvent { int DamageTypeClass = 5; };
struct UClass { bool Instagib; template<class T> const T* GetDefaultObject() const; };
UClass TeamTargetClass{false}, InstagibTargetClass{true};
struct ANCAimTrainerTarget : AActor {
    bool InstagibProfile = true;
    struct Capsule { float HalfHeight = 108.f; float GetScaledCapsuleHalfHeight() const { return HalfHeight; } } Shape;
    const Capsule* GetCapsuleComponent() const { return &Shape; }
    UClass* GetClass() const { return InstagibProfile ? &InstagibTargetClass : &TeamTargetClass; }
    bool Visible = false, Strafing = false, Crouched = false;
    bool CanStand = true, CanCrouch = true, CanDodge = true, CanSlide = true;
    int Activations = 0, Hides = 0, Wiggles = 0, Reversals = 0;
    int CrouchRequests = 0, StandRequests = 0, DodgeAttempts = 0, Dodges = 0;
    int SlideAttempts = 0, Slides = 0;
    int TrackingSlideAttempts = 0, TrackingSlides = 0;
    float WiggleRange = 0.f;
    FVector Position;
    void ActivateTarget(const FVector& position, bool strafe) {
        Visible = true; Strafing = strafe; Crouched = false; Position = position; ++Activations;
    }
    bool IsAvailable() const { return Visible; }
    bool IsPendingKillPending() const { return false; }
    float GetAppearanceTime() const { return 0.f; }
    void HideTarget() { Visible = false; Crouched = false; ++Hides; }
    void StartWiggle(float range) { WiggleRange = range; ++Wiggles; }
    void ReverseStrafe() { ++Reversals; }
    bool TryTrainerDodge(float) { ++DodgeAttempts; if (!CanDodge) return false; ++Dodges; return true; }
    bool TryTrainerSlideForward() { ++SlideAttempts; if (!CanSlide) return false; ++Slides; return true; }
    bool Sliding = false;
    int LongStrafes = 0;
    bool TryTrainerPopupSlide(int) { return TryTrainerSlideForward(); }
    bool IsTrainerSliding() const { return Sliding; }
    bool StartPopupLongStrafe(float,float,float) { ++LongStrafes; return true; }
    bool TryTrainerTrackingSlide(float) { ++TrackingSlideAttempts; if (!CanSlide) return false; ++TrackingSlides; return true; }
    bool SetTrainerCrouched(bool crouch) {
        if (crouch) { ++CrouchRequests; if (!CanCrouch) return false; }
        else { ++StandRequests; if (!CanStand) return false; }
        Crouched = crouch; return true;
    }
};
template<class T> const T* UClass::GetDefaultObject() const {
    static ANCAimTrainerTarget team, instagib;
    team.Shape.HalfHeight = 108.f; instagib.Shape.HalfHeight = 103.f;
    return static_cast<const T*>(Instagib ? &instagib : &team);
}
struct ANCAimTrainerGame {
    struct {
        int Phase = 1, Scenario = 2, TargetsExpired = 0, Hits = 0, Headshots = 0;
        float RemainingSeconds = 3.f;
    } Progress;
    struct World { float Now = 10.f; float GetTimeSeconds() const { return Now; } } TheWorld;
    struct {
        float Roll = .5f;
        int SlotChoice = 0;
        float FRand() const { return Roll; }
        float FRandRange(float low, float high) const { return low + (high - low) * Roll; }
        int RandRange(int low, int high) const { return low + std::min(SlotChoice, high - low); }
    } Schedule;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TArray<ANCAimTrainerTarget*> Targets;
    FVector ArenaOrigin;
    float PhaseStartedAt = 0.f, LastTraceTime = 0.f, NextDirectionTime = 0.f, NextDodgeTime = 0.f;
    float NextPopupTime = 0.f, PopupRefireSeconds = 1.f, ShotStatBaseline = 0.f;
    float NextPopupSlideTime[NCAimTrainerLayout::TargetCount] = {};
    float NextPopupLongStrafeTime[NCAimTrainerLayout::TargetCount] = {};
    float NextTrackingSlideTime = 0.f;
    float NextTargetTime[NCAimTrainerLayout::TargetCount] = {}, TargetExpiry[NCAimTrainerLayout::TargetCount] = {};
    float NextWiggleTime[NCAimTrainerLayout::TargetCount] = {};
    TArray<float> NextCrouchTime = TArray<float>(NCAimTrainerLayout::TargetCount, 0.f);
    TArray<float> CrouchEndTime = TArray<float>(NCAimTrainerLayout::TargetCount, 0.f);
    bool bRankedRun = true;
    std::string UnrankedReason;
    World* GetWorld() { return &TheWorld; }
    bool IsTrainee(AController* player) const { return player && player == Trainee; }
    void PublishProgress() {}
    int LocalAppearances[6] = {};
    void InvalidateLocalRun() {}
    void BeginLocalRecording() {}
    bool PrepareLocalRecording() { return true; }
    void UpdateShotCount() {}
    void RecordLocalTarget(int, bool, bool=false) {}
    void BeginActiveRun();
    void HideAllTargets();
    void ActivateSlot(int32, float);
    void UpdateTargets(float);
    void UpdatePopupDodger(float);
    void UpdateTrackingMovement(float);
    float RecordTargetHit(ANCAimTrainerTarget*, float, const FDamageEvent&, AController*, AActor*);
};
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    AUTPlayerState PlayerState;
    AUTPlusSniper Gun;
    ANCAimTrainerTarget Targets[NCAimTrainerLayout::TargetCount];
    Fixture() {
        Player.PlayerState = &PlayerState; Game.Trainee = &Player; Game.RunWeapon = &Gun;
        for (auto& target : Targets) Game.Targets.Add(&target);
    }
    void Start() {
        for (auto& target : Targets) target.InstagibProfile = Game.Progress.Scenario == 2;
        Game.BeginActiveRun();
    }
    void At(float now) { Game.TheWorld.Now = now; Game.UpdateTargets(now); }
    float Hit(int slot) { return Game.RecordTargetHit(&Targets[slot], 100.f, FDamageEvent(), &Player, &Gun); }
    // Existing opportunity/cadence assertions count the five timed seats only.
    // The permanent dodger has separate lifecycle checks below.
    int Activations() const {
        int total = 0; for (int i=0; i<NCAimTrainerLayout::PopupSlotCount; ++i) total += Targets[i].Activations; return total;
    }
    int Visible() const {
        int total = 0; for (int i=0; i<NCAimTrainerLayout::PopupSlotCount; ++i) total += int(Targets[i].Visible); return total;
    }
};
'''


CASES = r'''
void Require(bool condition, const char* why) {
    if (!condition) { std::cerr << why << '\n'; std::exit(1); }
}
void StrafeMix() {
    using namespace NCAimTrainerScenarioPolicy;
    int shortCount = 0, longCount = 0;
    for (int sample = 0; sample < 1000; ++sample) {
        const float pattern = (float(sample) + 0.5f) / 1000.f;
        for (float jitter : {0.f, 0.5f, 1.f}) {
            const float hold = StrafeHoldSeconds(pattern, jitter);
            Require(hold >= .14f && hold <= .81f, "strafe hold outside practice window");
            if (hold <= .35f) ++shortCount;
            else { Require(hold >= .45f, "hold in unintended middle gap"); ++longCount; }
        }
    }
    Require(shortCount == 2250 && longCount == 750, "short reversals no longer dominate 3:1");
    Require(StrafeHoldSeconds(.7499f, 1.f) < StrafeHoldSeconds(.75f, 0.f), "mix boundary lost");
    Require(StrafeHoldSeconds(0.f, 0.f) < StrafeHoldSeconds(0.f, 1.f), "short strafe has no timing variation");
    Require(StrafeHoldSeconds(1.f, 0.f) < StrafeHoldSeconds(1.f, 1.f), "long strafe has no timing variation");
}
void Dodges() {
    using namespace NCAimTrainerScenarioPolicy;
    for (float roll : {0.f, .25f, .5f, .75f, 1.f}) {
        Require(DodgeDelaySeconds(roll) >= 1.8f && DodgeDelaySeconds(roll) <= 3.8f,
                "dodges are no longer occasional or exceed the delay limit");
        for (float offset : {500.f, 799.f, 800.f, 1200.f}) {
            Require(DodgeDirection(offset, roll) == -1.f, "right edge admits outward dodge");
            Require(DodgeDirection(-offset, roll) == 1.f, "left edge admits outward dodge");
        }
    }
    for (float offset : {-499.99f, 0.f, 499.99f}) {
        Require(DodgeDirection(offset, 0.f) == -1.f && DodgeDirection(offset, 1.f) == 1.f,
                "central dodge cannot select both directions");
    }
    Require(DodgeDirection(0.f, .4999f) == -1.f && DodgeDirection(0.f, .5f) == 1.f,
            "central dodge distribution is biased");
}
void RefireAndBacklog() {
    using namespace NCAimTrainerScenarioPolicy;
    Require(InstagibMaxActiveTargets == 6, "popup plus permanent dodger capacity lost");
    for (float actualRefire : {0.f, .4f, 1.f, 1.5f, 2.f}) {
        const float refire = actualRefire < 1.f ? 1.f : actualRefire;
        for (float roll : {0.f, .01f, .5f, .99f, 1.f}) {
            const float delay = PopupSpawnDelay(actualRefire, roll);
            const float exposure = PopupExposure(actualRefire, roll);
            Require(delay >= refire && delay <= 1.101f * refire, "spawn outruns weapon or adds excessive downtime");
            Require(exposure >= 5.5f * refire && exposure <= 6.81f * refire, "exposure drift");
            // A shot just preceded a full backlog. Clear the older four, then
            // the new target, with .35 refire intervals of reaction time.
            const float lastBacklogShot = .99f * refire
                + (NCAimTrainerLayout::PopupSlotCount - 1) * refire + .35f * refire;
            Require(lastBacklogShot < exposure, "five-target backlog expires before its fifth legal shot");
        }
        // Under the fastest arrival rate, a ready player who waits .25s to
        // acquire each target can finish a minute without compulsory misses.
        float readyAt = 0.f;
        for (float appearance = 0.f; appearance < 60.f; appearance += PopupSpawnDelay(actualRefire, 0.f)) {
            const float acquiredAt = appearance + .25f * refire;
            const float shotAt = acquiredAt > readyAt ? acquiredAt : readyAt;
            Require(shotAt < appearance + PopupExposure(actualRefire, 0.f), "sustained cadence builds impossible backlog");
            readyAt = shotAt + refire;
        }
    }
}
void RollBoundaries() {
    using namespace NCAimTrainerScenarioPolicy;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float roll : {-10.f, 10.f, nan}) {
        Require(std::isfinite(StrafeHoldSeconds(roll, roll)), "invalid roll poisoned strafe deadline");
        Require(DodgeDelaySeconds(roll) >= 1.8f && DodgeDelaySeconds(roll) <= 3.8f, "invalid roll poisoned dodge deadline");
        Require(PopupSpawnDelay(1.f, roll) >= 1.f, "invalid roll bypassed weapon cadence");
        Require(PopupExposure(1.f, roll) >= 5.5f, "invalid roll shortened exposure");
    }
    Require(PopupSpawnDelay(nan, 0.f) == 1.f, "invalid weapon interval bypassed minimum");
}
void InitialSpawn() {
    Fixture f; f.Game.Schedule.SlotChoice = 2; f.Start();
    Require(f.Game.Progress.Phase == 2 && f.Game.Progress.RemainingSeconds == 60.f, "run did not start");
    f.At(10.f);
    Require(f.Visible() == 1 && f.Activations() == 1 && f.Targets[2].Visible,
            "initial popup burst or random eligible slot selection lost");
    const float next = f.Game.NextPopupTime;
    Require(next > 11.f && next < 11.11f, "first spawn deadline ignores one-second refire");
    f.At(10.25f); f.At(10.5f); f.At(11.f); f.At(next - .001f);
    Require(f.Activations() == 1, "initial slots retain quarter-second burst");
    f.At(next);
    Require(f.Visible() == 2 && f.Activations() == 2, "second popup did not wait for global deadline");
    for (int expected = 3; expected <= 5; ++expected) {
        f.At(f.Game.NextPopupTime);
        Require(f.Visible() == expected && f.Activations() == expected,
                "five different popup stations no longer overlap");
    }
}
void Replacements() {
    Fixture hit; hit.Start(); hit.At(10.f);
    const float afterHit = hit.Game.NextPopupTime;
    hit.At(10.1f);
    Require(hit.Hit(0) > 0.f && hit.Game.Progress.Hits == 1 && !hit.Targets[0].Visible,
            "fixture hit did not retire target through production hit method");
    hit.At(10.1f); hit.At(afterHit - .001f);
    Require(hit.Activations() == 1 && hit.Game.NextPopupTime == afterHit, "hit replacement bypasses global deadline");
    hit.At(afterHit);
    Require(hit.Activations() == 2 && hit.Targets[0].Activations == 2, "hit slot not reusable at deadline");

    Fixture expiry; expiry.Start(); expiry.At(10.f);
    const float afterExpiry = expiry.Game.NextPopupTime;
    // Expiring early isolates the scheduler from the policy's longer exposure.
    expiry.Game.TargetExpiry[0] = afterExpiry - .1f;
    expiry.At(afterExpiry - .1f);
    Require(expiry.Game.Progress.TargetsExpired == 1 && expiry.Visible() == 0, "expired target was not retired");
    expiry.At(afterExpiry - .001f);
    Require(expiry.Activations() == 1 && expiry.Game.Progress.TargetsExpired == 1,
            "expiry replacement bypasses gate or duplicate expiry scored");
    expiry.At(afterExpiry);
    Require(expiry.Activations() == 2 && expiry.Targets[0].Activations == 2, "expired slot not reusable at deadline");
}
void StallNoCatchup() {
    Fixture f; f.Start(); f.At(10.f);
    for (int i = 1; i < 5; ++i) f.At(f.Game.NextPopupTime);
    Require(f.Visible() == 5, "fixture did not fill popup stations");
    f.At(25.f);
    Require(f.Game.Progress.TargetsExpired == 5 && f.Activations() == 6 && f.Visible() == 1,
            "long stall spawned a catch-up burst");
    Require(f.Game.NextPopupTime > 26.f, "deadline advanced from stale time instead of this frame");
    f.At(25.f); f.At(25.25f); f.At(26.f);
    Require(f.Activations() == 6 && f.Game.Progress.TargetsExpired == 5, "repeated delayed ticks cause catch-up spawn or expiry");
}
void HeadshotSlots() {
    Fixture f; f.Game.Progress.Scenario = 1; f.Start();
    f.Game.NextPopupTime = 10000.f; // The popup scheduler must not throttle heads.
    f.At(10.f); Require(f.Visible() == 1, "headshot first slot drift");
    f.At(10.249f); Require(f.Visible() == 1, "headshot second slot appeared early");
    f.At(10.25f); Require(f.Visible() == 2, "headshot quarter-second spacing lost");
    f.At(10.5f); Require(f.Visible() == 3, "headshot third station throttled by popup gate");
    f.At(10.75f); f.At(11.f);
    Require(f.Visible() == 5, "expanded headshot stations were not activated");
    Require(f.Hit(0) > 0.f && f.Game.Progress.Headshots == 1, "headshot not accepted");
    const float respawn = f.Game.NextTargetTime[0];
    Require(FMath::IsNearlyEqual(respawn, 11.35f), "headshot hit delay changed");
    f.At(respawn - .001f); Require(f.Activations() == 5, "headshot hit respawn appeared early");
    f.At(respawn); Require(f.Activations() == 6, "headshot hit respawn blocked by global popup gate");
    f.Game.TargetExpiry[1] = respawn;
    f.At(respawn);
    Require(!f.Targets[1].Visible && f.Game.Progress.TargetsExpired == 1, "headshot expiry not retired");
    const float expiryRespawn = f.Game.NextTargetTime[1];
    Require(expiryRespawn >= respawn + .25f && expiryRespawn <= respawn + .65f, "headshot expiry hide interval changed");
    f.At(expiryRespawn); Require(f.Targets[1].Activations == 2, "headshot expiry respawn blocked");
}
void InitializeRefire() {
    for (float refire : {.5f, 1.f, 1.5f, std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity()}) {
        Fixture f; f.Gun.Refire = refire;
        f.Game.NextPopupTime = 900.f; f.Game.PopupRefireSeconds = 99.f; f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot] = 850.f;
        for (int i = 0; i < NCAimTrainerLayout::TargetCount; ++i) {
            f.Game.NextTargetTime[i] = 900.f; f.Game.TargetExpiry[i] = 800.f; f.Game.NextWiggleTime[i] = 700.f;
            f.Game.NextCrouchTime[i] = 600.f; f.Game.CrouchEndTime[i] = 500.f;
        }
        f.Start();
        const float expected = std::isfinite(refire) && refire > 1.f ? refire : 1.f;
        Require(f.Game.PopupRefireSeconds == expected && f.Game.NextPopupTime == 10.f,
                "restart retained previous cadence or unsafe refire");
        Require(f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot] == 0.f, "restart retained a prior slide deadline");
        Require(f.Game.bRankedRun == (refire == 1.f), "altered or invalid weapon cadence remained ranked");
        for (int i = 0; i < 5; ++i) {
            Require(f.Game.NextTargetTime[i] == 10.f && f.Game.TargetExpiry[i] == 0.f && f.Game.NextWiggleTime[i] == 10.f
                    && f.Game.NextCrouchTime[i] == 0.f && f.Game.CrouchEndTime[i] == 0.f,
                    "restart retained prior slot schedule");
        }
        Require(f.Game.NextTargetTime[NCAimTrainerLayout::PopupDodgerSlot] == 10.f
                && f.Game.TargetExpiry[NCAimTrainerLayout::PopupDodgerSlot] == 70.f
                && f.Game.NextCrouchTime[NCAimTrainerLayout::PopupDodgerSlot] == 0.f
                && f.Game.CrouchEndTime[NCAimTrainerLayout::PopupDodgerSlot] == 0.f,
                "initial persistent dodger retained prior appearance state");
        f.At(10.f);
        Require(f.Game.NextPopupTime > 10.f + expected && f.Game.TargetExpiry[0] >= 10.f + 5.5f * expected,
                "actual refire did not reach production spawn or exposure method");
    }
    Fixture tracking; tracking.Game.Progress.Scenario = 0; tracking.Start(); tracking.At(10.f); tracking.At(20.f);
    Require(tracking.Visible() == 1 && tracking.Activations() == 1 && tracking.Targets[0].Strafing,
            "tracking started extra slots or lost strafe activation");
    Require(tracking.Targets[0].Position.X == -800.f && tracking.Targets[0].Wiggles == 0,
            "tracking target moved outside Link range or acquired precision wiggle");
}
void LayoutAndWiggles() {
    using namespace NCAimTrainerLayout;
    Require(TargetCount == 6 && PopupSlotCount == 5 && HeadSlotCount == 5 && PopupDodgerSlot == 5,
            "separate popup, headshot and persistent dodger slots lost");
    for (int scenario : {1, 2}) {
        const float standing = scenario == 2 ? 103.f : 108.f;
        const float radius = scenario == 2 ? 38.f : 40.f;
        for (float roll : {0.f, .5f, 1.f}) {
            Fixture f; f.Game.Progress.Scenario = scenario; f.Game.Schedule.Roll = roll; f.Start();
            f.Game.ArenaOrigin = FVector(50.f, -80.f, 50000.f);
            for (int slot = 0; slot < 5; ++slot) {
                const FSeat seat = scenario == 1 ? HeadSeat(slot) : PopupSeat(slot);
                f.Game.ActivateSlot(slot, 10.f);
                const auto& target = f.Targets[slot];
                const float x = target.Position.X - f.Game.ArenaOrigin.X;
                const float y = target.Position.Y - f.Game.ArenaOrigin.Y;
                const float z = target.Position.Z - f.Game.ArenaOrigin.Z;
                Require(x >= seat.MinX && x <= seat.MaxX && std::fabs(y - seat.CenterY) <= seat.SpawnJitterY + .01f
                        && z == seat.FloorZ + standing, "activation ignored authored layout bounds or profile height");
                Require(target.Wiggles == 1 && target.WiggleRange == seat.WiggleRange && !target.Strafing,
                        "precision target did not start its authored wiggle");
                Require(f.Game.NextWiggleTime[slot] >= 10.12f && f.Game.NextWiggleTime[slot] <= 10.29f,
                        "initial wiggle decision deadline lost");
                if (scenario == 2 && slot < 3) {
                    const FBlock platform = PopupPlatform(slot);
                    Require(std::fabs(y - platform.CenterY) + seat.WiggleRange + radius + WiggleSafetyMargin <= platform.SizeY * .5f,
                            "popup motion would leave its supporting platform");
                    Require(std::fabs(x - platform.CenterX) + radius <= platform.SizeX * .5f,
                            "popup spawn has no support beneath its capsule");
                }
                if (scenario == 1) {
                    const FBlock cover = HeadCover(slot);
                    Require(seat.WiggleRange + radius < cover.SizeY * .5f && x - radius > cover.CenterX + cover.SizeX * .5f,
                            "headshot wiggle exposes body around or inside cover");
                    Require(z + standing > cover.Height && z - standing < cover.Height,
                            "headshot body/head do not straddle cover height");
                    Require(f.Game.TargetExpiry[slot] == 16.5f, "five-head exposure reverted to short preset");
                }
            }
            if (scenario == 2) {
                const FBlock cover = PopupPlatform(1);
                const auto& peek = f.Targets[3];
                Require(peek.Position.X - f.Game.ArenaOrigin.X - radius > cover.CenterX + cover.SizeX * .5f
                        && peek.Position.Z - f.Game.ArenaOrigin.Z == standing,
                        "head-peek seat is not behind the low central platform on the floor");
                Require(f.Targets[4].Position.Y - f.Game.ArenaOrigin.Y < -1300.f, "additional side lane lost");
            }
            f.Game.NextPopupTime = 10000.f;
            const float reverseAt = f.Game.NextWiggleTime[0];
            f.At(reverseAt - .001f);
            Require(f.Targets[0].Reversals == 0, "wiggle reversed before deadline");
            f.At(reverseAt);
            Require(f.Targets[0].Reversals == 1 && f.Game.NextWiggleTime[0] > reverseAt,
                    "active precision wiggle never reversed");
            f.Targets[0].HideTarget(); f.Game.NextTargetTime[0] = 10000.f;
            f.At(reverseAt + 1.f);
            Require(f.Targets[0].Reversals == 1, "hidden target kept reversing");
        }
    }
}
void CrouchScenarioScope() {
    for (int scenario : {0,1}) {
        Fixture f; f.Game.Progress.Scenario = scenario; f.Start(); f.At(10.f);
        for (int slot=0; slot<5; ++slot) {
            const bool tracking = scenario == 0 && slot == 0;
            Require(tracking ? f.Game.NextCrouchTime[slot] >= 16.f && f.Game.NextCrouchTime[slot] <= 20.f
                             : f.Game.NextCrouchTime[slot] == 0.f,
                    "crouch was not limited to the active Link target or selected instagib appearance");
            // The instagib UpdateTargets path must not process Link deadlines.
            f.Game.NextCrouchTime[slot]=10.1f;
        }
        f.At(10.2f);
        for (const auto& target:f.Targets) Require(target.CrouchRequests==0, "other scenario executed instagib crouch");
    }
    for (float roll:{0.f,.649f,.65f,1.f}) {
        Fixture f; f.Game.Schedule.Roll=roll; f.Game.Schedule.SlotChoice=1; f.Start(); f.At(10.f);
        const bool selected=roll<.65f;
        Require((f.Game.NextCrouchTime[1]>10.f)==selected, "instagib crouch selection boundary drifted");
        if(selected) Require(f.Game.NextCrouchTime[1]>=11.5f && f.Game.NextCrouchTime[1]<=13.5f,
                             "crouch does not leave an initial standing opportunity");
    }
}
void CrouchOncePerAppearance() {
    Fixture f; f.Game.Schedule.SlotChoice=1; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    const float due=f.Game.NextCrouchTime[1];
    f.At(due-.001f);
    Require(!f.Targets[1].Crouched && f.Targets[1].CrouchRequests==0,"target crouched before deadline");
    f.At(due);
    Require(f.Targets[1].Crouched && f.Targets[1].CrouchRequests==1 && f.Game.NextCrouchTime[1]==0.f,
            "due crouch was missed or left a repeating request");
    const float end=f.Game.CrouchEndTime[1];
    Require(end>=due+.25f && end<=due+.45f,"crouch hold outside its brief exposure interruption");
    f.At(end-.001f); Require(f.Targets[1].Crouched,"target stood before hold ended");
    f.At(end);
    Require(!f.Targets[1].Crouched && f.Game.CrouchEndTime[1]==0.f && f.Targets[1].StandRequests==1,
            "completed crouch left the target hidden or standing request active");
    f.At(end+.1f); f.At(end+.3f); f.At(end+.7f);
    Require(f.Targets[1].CrouchRequests==1 && f.Targets[1].StandRequests==1,"appearance repeated its crouch/stand");
    Fixture failed; failed.Game.Schedule.SlotChoice=1; failed.Start(); failed.At(10.f); failed.Game.NextPopupTime=10000.f;
    failed.Targets[1].CanCrouch=false;
    const float failAt=failed.Game.NextCrouchTime[1]; failed.At(failAt); failed.At(failAt+.1f);
    Require(failed.Targets[1].CrouchRequests==1 && failed.Game.CrouchEndTime[1]==0.f,
            "failed crouch was retried or created a bogus standing deadline");
}
void CrouchExpiryGuard() {
    for (float refire:{1.f,1.5f,2.f}) {
        for (float margin:{-.001f,.001f}) {
            Fixture f; f.Gun.Refire=refire; f.Game.Schedule.SlotChoice=1; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
            const float due=f.Game.NextCrouchTime[1];
            const float hold=NCAimTrainerScenarioPolicy::CrouchHoldSeconds(f.Game.Schedule.Roll);
            f.Game.TargetExpiry[1]=due+hold+refire+.1f+margin;
            const float expiry=f.Game.TargetExpiry[1];
            f.At(due);
            const bool allowed=margin>0.f;
            Require(f.Targets[1].Crouched==allowed && f.Targets[1].CrouchRequests==(allowed?1:0),
                    "near-expiry crouch did not preserve one real refire interval plus reaction margin");
            Require(f.Game.TargetExpiry[1]==expiry && f.Game.NextCrouchTime[1]==0.f,
                    "guard changed lifetime or left a doomed crouch queued");
            if(allowed) {
                f.At(f.Game.CrouchEndTime[1]);
                Require(!f.Targets[1].Crouched && expiry-f.Game.TheWorld.Now>refire,
                        "target stood with less than a rifle interval remaining");
            }
        }
    }
}
void BlockedStandingRetry() {
    Fixture f; f.Game.Schedule.SlotChoice=1; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    f.At(f.Game.NextCrouchTime[1]);
    const float end=f.Game.CrouchEndTime[1]; f.Targets[1].CanStand=false;
    f.At(end); f.At(end+.02f);
    Require(f.Targets[1].Crouched && f.Game.CrouchEndTime[1]==end && f.Targets[1].StandRequests==2,
            "blocked native uncrouch lost its retry deadline");
    f.Targets[1].CanStand=true; f.At(end+.04f);
    Require(!f.Targets[1].Crouched && f.Game.CrouchEndTime[1]==0.f && f.Targets[1].StandRequests==3,
            "target did not stand after clearance recovered");
    f.At(end+.06f);
    Require(f.Targets[1].StandRequests==3 && f.Targets[1].CrouchRequests==1,
            "standing recovery left repeated state transitions");
}
void CrouchReuseReset() {
    Fixture f; f.Game.Schedule.SlotChoice=1; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    f.At(f.Game.NextCrouchTime[1]); Require(f.Targets[1].Crouched,"fixture did not crouch");
    Require(f.Hit(1)>0.f && !f.Targets[1].Visible,"crouched target hit was not accepted");
    f.Game.Schedule.Roll=1.f; // Replacement appearance deliberately skips crouch.
    f.Game.NextPopupTime=f.Game.TheWorld.Now+.1f; f.At(f.Game.NextPopupTime);
    Require(f.Targets[1].Visible && f.Targets[1].Activations==2 && !f.Targets[1].Crouched
            && f.Game.NextCrouchTime[1]==0.f && f.Game.CrouchEndTime[1]==0.f,
            "reused target inherited old crouch posture or deadlines");
    f.Game.Schedule.Roll=.1f;
    f.Game.ActivateSlot(1,f.Game.TheWorld.Now+.1f);
    Require(f.Game.NextCrouchTime[1]>f.Game.TheWorld.Now+1.5f && f.Game.CrouchEndTime[1]==0.f,
            "new appearance cannot schedule a fresh crouch after previous one");
}
void PersistentDodgerStartsAndDoesNotExpire() {
    using namespace NCAimTrainerLayout;
    Fixture f; f.Game.ArenaOrigin=FVector(50.f,-80.f,50000.f); f.Start();
    auto& target=f.Targets[PopupDodgerSlot];
    const FSeat seat=PopupDodgerSeat();
    Require(target.Visible && target.Strafing && target.Activations==1 && f.Activations()==0,
            "instagib does not begin with exactly one permanent dodger");
    Require(target.Position.X==f.Game.ArenaOrigin.X+seat.MinX
            && target.Position.Y==f.Game.ArenaOrigin.Y+seat.CenterY
            && target.Position.Z==f.Game.ArenaOrigin.Z+seat.FloorZ+103.f,
            "persistent dodger ignored its supported open floor lane");
    Require(target.Wiggles==0 && f.Game.TargetExpiry[PopupDodgerSlot]==70.f
            && f.Game.NextCrouchTime[PopupDodgerSlot]==0.f && f.Game.CrouchEndTime[PopupDodgerSlot]==0.f,
            "permanent dodger acquired popup lifetime, wiggle or crouch");
    for(int i=0;i<PopupSlotCount;++i) f.At(f.Game.NextPopupTime);
    Require(f.Visible()==PopupSlotCount && target.Visible, "six simultaneous instagib targets cannot be displayed");
    // Isolate the persistent slot from timed target expiry and stale requests.
    const float popupDeadline=f.Game.NextPopupTime;
    f.Game.NextCrouchTime[PopupDodgerSlot]=10.1f; f.Game.CrouchEndTime[PopupDodgerSlot]=10.2f;
    f.Game.UpdatePopupDodger(69.99f);
    Require(target.Visible && target.Activations==1 && target.Hides==0 && f.Game.Progress.TargetsExpired==0,
            "persistent dodger timed out or accrued an expiry penalty");
    Require(target.CrouchRequests==0 && target.StandRequests==0 && target.Wiggles==0
            && f.Game.NextPopupTime==popupDeadline, "persistent lane mutated popup schedule or posture");
}
void PersistentDodgerRefillsWithoutConsumingPopupCadence() {
    using namespace NCAimTrainerLayout;
    Fixture f; f.Start(); f.At(10.f);
    const float deadline=f.Game.NextPopupTime;
    auto& target=f.Targets[PopupDodgerSlot];
    f.Game.TheWorld.Now=10.1f;
    Require(f.Hit(PopupDodgerSlot)>0.f && f.Game.Progress.Hits==1 && !target.Visible,
            "permanent dodger cannot be scored and retired normally");
    Require(f.Hit(PopupDodgerSlot)==0.f, "hidden dodger accepted duplicate damage");
    f.Game.NextTargetTime[PopupDodgerSlot]=900.f; // Timed seats cannot starve this slot.
    f.Game.NextCrouchTime[PopupDodgerSlot]=10.2f; f.Game.CrouchEndTime[PopupDodgerSlot]=10.3f;
    f.At(10.101f);
    Require(target.Visible && target.Activations==2 && target.Strafing && target.Wiggles==0,
            "hit permanent target was not restored on the next update");
    Require(f.Activations()==1 && f.Game.NextPopupTime==deadline && f.Game.Progress.TargetsExpired==0,
            "dodger replacement consumed popup deadline or created expiry penalty");
    Require(f.Game.NextCrouchTime[PopupDodgerSlot]==0.f && f.Game.CrouchEndTime[PopupDodgerSlot]==0.f
            && f.Game.TargetExpiry[PopupDodgerSlot]==70.f,
            "permanent target reuse inherited posture or a popup lifetime");
    const float nextDodge=f.Game.NextDodgeTime;
    Require(nextDodge>=10.300f && nextDodge<=10.652f && nextDodge<11.1f,
            "replacement's first dodge no longer starts before the next rifle shot");
    f.At(deadline);
    Require(f.Activations()==2 && target.Activations==2, "popup deadline stopped after independent dodger refill");
}
void PersistentDodgerCadenceAndNativeRejection() {
    using namespace NCAimTrainerScenarioPolicy;
    for(float roll:{0.f,.5f,1.f,-1.f,10.f,std::numeric_limits<float>::quiet_NaN()}) {
        const float delay=PopupDodgeDelaySeconds(roll);
        Require(std::isfinite(delay) && delay>=1.15f && delay<=2.101f,
                "persistent dodge cadence left the frequent bounded range");
        const float initial=PopupFirstDodgeDelaySeconds(roll);
        Require(std::isfinite(initial) && initial>=.2f && initial<=.551f,
                "first dodge cannot happen before the next one-second rifle shot");
    }
    Fixture f; f.Start(); auto& target=f.Targets[NCAimTrainerLayout::PopupDodgerSlot];
    const float first=f.Game.NextDodgeTime;
    Require(first>=10.2f && first<=10.551f,"initial permanent dodger waits through the first rifle interval");
    f.Game.UpdatePopupDodger(first-.001f); Require(target.DodgeAttempts==0,"dodger attempted before deadline");
    target.CanDodge=false; f.Game.UpdatePopupDodger(first);
    Require(target.DodgeAttempts==1 && target.Dodges==0 && FMath::IsNearlyEqual(f.Game.NextDodgeTime,first+.2f),
            "native dodge rejection was bypassed or delayed for a complete interval");
    f.Game.UpdatePopupDodger(first+.199f); Require(target.DodgeAttempts==1,"native dodge retry busy-looped");
    target.CanDodge=true; f.Game.UpdatePopupDodger(first+.2f);
    Require(target.DodgeAttempts==2 && target.Dodges==1
            && f.Game.NextDodgeTime>=first+.2f+1.15f && f.Game.NextDodgeTime<=first+.2f+2.101f,
            "recovered native dodge did not establish a fresh bounded interval");
    const int before=target.DodgeAttempts;
    f.Game.UpdatePopupDodger(60.f); f.Game.UpdatePopupDodger(60.f);
    Require(target.DodgeAttempts==before+1 && f.Game.NextDodgeTime>61.14f,
            "stalled persistent lane replayed multiple missed dodges");
    const float directionAt=f.Game.NextDirectionTime;
    const int reversals=target.Reversals;
    f.Game.UpdatePopupDodger(directionAt-.001f);
    Require(target.Reversals==reversals,"persistent strafe reversed before scheduled decision");
    f.Game.UpdatePopupDodger(directionAt);
    Require(target.Reversals==reversals+1 && f.Game.NextDirectionTime>directionAt,
            "persistent dodger lost random short strafe decisions");
}
void PersistentDodgerCannotLeakIntoOtherPhasesOrScenarios() {
    using namespace NCAimTrainerLayout;
    for(int scenario:{0,1}) {
        Fixture f; f.Game.Progress.Scenario=scenario; f.Start(); f.At(11.5f);
        f.Game.ActivateSlot(PopupDodgerSlot,11.5f);
        f.Game.UpdatePopupDodger(11.5f);
        Require(!f.Targets[PopupDodgerSlot].Visible && f.Targets[PopupDodgerSlot].Activations==0,
                "sixth instagib pawn appeared in tracking or headshots");
        if(scenario==1) Require(f.Visible()==HeadSlotCount,"headshot station count changed");
        else Require(f.Visible()==1,"tracking gained extra targets");
    }
    for(int phase:{0,1,3}) {
        Fixture f; f.Start(); f.Targets[PopupDodgerSlot].HideTarget(); f.Game.Progress.Phase=phase;
        f.Game.UpdatePopupDodger(11.f);
        Require(!f.Targets[PopupDodgerSlot].Visible && f.Targets[PopupDodgerSlot].Activations==1,
                "inactive trainer phase respawned the permanent target");
    }
    Fixture ended; ended.Start(); ended.Targets[PopupDodgerSlot].HideTarget();
    ended.Game.UpdatePopupDodger(70.f);
    Require(!ended.Targets[PopupDodgerSlot].Visible && ended.Targets[PopupDodgerSlot].Activations==1,
            "round deadline admitted an extra permanent target appearance");
}
void UpperPlatformSlideScope() {
    using namespace NCAimTrainerLayout;
    for (int scenario : {0, 1, 2, 3}) {
        for (int slot = 0; slot < TargetCount; ++slot) {
            for (float roll : {0.f, .5f, 1.f}) {
                Fixture f; f.Game.Progress.Scenario = scenario; f.Game.Schedule.Roll = roll;
                if(scenario==3) f.Gun.Refire=1.3f; f.Start(); f.Game.NextPopupTime = 10000.f;
                f.Game.ActivateSlot(slot, 10.f);
                const bool selected = scenario >= 2 && (slot == PopupSliderSlot || ((slot==0 || slot==4) && roll<.45f));
                Require((f.Game.NextPopupSlideTime[slot] > 0.f) == selected,
                        "forward slide escaped the upper-right instagib seat");
                if (selected) {
                    Require(f.Game.NextPopupSlideTime[slot] >= 10.8f && f.Game.NextPopupSlideTime[slot] <= 11.41f,
                            "slide delay lacks expected variation");
                    Require(f.Game.NextCrouchTime[slot] == 0.f && f.Game.CrouchEndTime[slot] == 0.f,
                            "independent crouch competes with slide posture");
                    if(slot==PopupSliderSlot) Require(f.Targets[slot].Position.X >= 1000.f && f.Targets[slot].Position.Z == (scenario==2?423.f:428.f),
                            "slider lost its elevated runway");
                }
                f.At(11.5f);
                for (int index = 0; index < TargetCount; ++index) {
                    Require(f.Targets[index].SlideAttempts == int(selected && index == slot),
                            "unselected target attempted a slide");
                }
            }
        }
    }
}
void PopupLongStrafeLifecycle() {
    for (int scenario : {2,3}) {
        for (float roll : {.1f,.5f,.9f}) {
            Fixture f; f.Game.Progress.Scenario=scenario; f.Gun.Refire=scenario==2?1.f:1.3f;
            f.Game.Schedule.Roll=roll; f.Start(); f.Game.NextPopupTime=10000.f;
            for(int slot=0;slot<5;++slot) {
                f.Game.ActivateSlot(slot,10.f);
                Require((f.Game.NextPopupLongStrafeTime[slot]>0)==(slot==0 && roll<.65f),
                        "long strafe appeared outside rear-left popup seat or probability changed");
            }
            if(roll>=.65f) continue;
            f.Game.NextPopupSlideTime[0]=0;
            const float due=f.Game.NextPopupLongStrafeTime[0];
            Require(due>=12.3f && due<=12.91f && f.Game.NextCrouchTime[0]==0,
                    "long strafe conflicts with crouch or precedes slide recovery");
            f.Targets[0].Sliding=true; f.At(due);
            Require(f.Targets[0].LongStrafes==0 && FMath::IsNearlyEqual(f.Game.NextPopupLongStrafeTime[0],due+.15f),
                    "long strafe interrupted native slide or lost its short retry");
            f.Targets[0].Sliding=false; f.At(due+.15f); f.At(due+.25f);
            Require(f.Targets[0].LongStrafes==1 && f.Game.NextPopupLongStrafeTime[0]==0,
                    "long strafe did not start once after slide recovery");
            f.Game.ActivateSlot(0,due+.5f);
            Require(f.Game.NextPopupLongStrafeTime[0]>due+.5f,"reused rear target inherited prior deadline");
            f.Game.HideAllTargets();
            for(int slot=0;slot<6;++slot) Require(f.Game.NextPopupLongStrafeTime[slot]==0
                    && f.Game.NextPopupSlideTime[slot]==0,"aborted run retained popup motion deadlines");
        }
    }
    for(bool endOfRun:{false,true}) {
        Fixture f; f.Start(); f.Game.NextPopupTime=10000.f; f.Game.ActivateSlot(0,10.f);
        const float due=f.Game.NextPopupLongStrafeTime[0];
        const float remaining=NCAimTrainerScenarioPolicy::PopupLongStrafeHoldSeconds(.5f)+1.f-.01f;
        if(endOfRun) f.Game.PhaseStartedAt=due+remaining-60.f;
        else f.Game.TargetExpiry[0]=due+remaining;
        f.At(due);
        Require(f.Targets[0].LongStrafes==0 && f.Game.NextPopupLongStrafeTime[0]==0,
                "late long strafe left less than a refire interval to shoot afterward");
    }
}
void PrecisionPopupCadence() {
    for(float refire:{1.f,1.3f,1.5f,std::numeric_limits<float>::quiet_NaN()}) {
        Fixture f; f.Game.Progress.Scenario=3; f.Gun.Refire=refire; f.Start();
        Require(f.Game.bRankedRun==FMath::IsNearlyEqual(refire,1.3f),
                "precision popup accepted modified rifle refire");
        if(refire!=1.3f) continue;
        f.At(10.f);
        Require(f.Game.NextPopupTime>=11.3f && f.Game.TargetExpiry[0]>=17.15f
                && f.Targets[5].Visible && f.Targets[0].Position.Z==109.f,
                "precision popup cadence, persistent dodger or TeamArena standing height lost");
    }
}
void UpperPlatformSlideOnceAndReuse() {
    using namespace NCAimTrainerLayout;
    for (bool accepted : {false, true}) {
        Fixture f; f.Start(); f.Game.NextPopupTime = 10000.f;
        f.Game.ActivateSlot(PopupSliderSlot, 10.f); f.Targets[PopupSliderSlot].CanSlide = accepted;
        const float due = f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot];
        f.At(due - .001f);
        Require(f.Targets[PopupSliderSlot].SlideAttempts == 0, "slide started before its deadline");
        f.At(due); f.At(due + .1f); f.At(due + .8f);
        Require(f.Targets[PopupSliderSlot].SlideAttempts == 1 && f.Targets[PopupSliderSlot].Slides == int(accepted)
                && f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot] == 0.f, "slide retried or started more than once per appearance");
        Require(f.Hit(PopupSliderSlot) > 0.f && !f.Targets[PopupSliderSlot].Visible,
                "slider stopped accepting normal instagib hits");
        f.Game.ActivateSlot(PopupSliderSlot, due + 1.f);
        const float next = f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot];
        Require(next > due + 1.f, "reused target retained previous slide deadline");
        f.At(next);
        Require(f.Targets[PopupSliderSlot].SlideAttempts == 2, "new appearance did not regain its slide");
    }
    Fixture hidden; hidden.Start(); hidden.Game.NextPopupTime = 10000.f;
    hidden.Game.ActivateSlot(PopupSliderSlot, 10.f);
    const float due = hidden.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot];
    hidden.Hit(PopupSliderSlot); hidden.At(due);
    Require(hidden.Targets[PopupSliderSlot].SlideAttempts == 0, "hidden target attempted a queued slide");
}
void UpperPlatformSlideExpiryAndRoundGuard() {
    using namespace NCAimTrainerLayout;
    for (float refire : {1.f, 1.5f}) {
        for (bool roundLimit : {false, true}) {
            for (float remaining : {refire + .99f, refire + 1.01f}) {
                Fixture f; f.Gun.Refire = refire; f.Start(); f.Game.NextPopupTime = 10000.f;
                f.Game.ActivateSlot(PopupSliderSlot, 10.f);
                const float due = f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot];
                if (roundLimit) f.Game.PhaseStartedAt = due + remaining - 60.f;
                else f.Game.TargetExpiry[PopupSliderSlot] = due + remaining;
                const float expiry = f.Game.TargetExpiry[PopupSliderSlot];
                f.At(due); f.At(due + .1f);
                Require(f.Targets[PopupSliderSlot].SlideAttempts == int(remaining > refire + 1.f),
                        "slide failed to preserve native duration and next rifle shot before deadline");
                Require(f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot] == 0.f && f.Game.TargetExpiry[PopupSliderSlot] == expiry,
                        "slide extended exposure or left a retry queued");
            }
        }
    }
}
void TrackingSlideCadenceAndScope() {
    for (float roll : {0.f, .5f, 1.f}) {
        Fixture f; f.Game.Progress.Scenario = 0; f.Game.Schedule.Roll = roll;
        f.Game.NextTrackingSlideTime = 900.f;
        f.Start(); f.At(10.f);
        f.Game.NextCrouchTime[0] = 10000.f; // Isolate slide cadence from the independent crouch scheduler.
        const float due = f.Game.NextTrackingSlideTime;
        Require(due >= 14.f && due <= 17.f, "tracking slide did not receive a fresh varied deadline");
        f.Game.UpdateTrackingMovement(due - .001f);
        Require(f.Targets[0].TrackingSlideAttempts == 0, "tracking slide started early");
        f.Targets[0].CanSlide = false;
        f.Game.UpdateTrackingMovement(due);
        Require(f.Targets[0].TrackingSlideAttempts == 1 && f.Targets[0].TrackingSlides == 0
                && std::fabs(f.Game.NextTrackingSlideTime - due - .2f) < .001f,
                "airborne/cooldown slide rejection bypassed native gate or lost bounded retry");
        f.Game.UpdateTrackingMovement(due + .1f);
        Require(f.Targets[0].TrackingSlideAttempts == 1, "rejected slide retried every tick");
        f.Targets[0].CanSlide = true;
        f.Game.UpdateTrackingMovement(f.Game.NextTrackingSlideTime);
        const float next = f.Game.NextTrackingSlideTime;
        Require(f.Targets[0].TrackingSlides == 1 && next >= due + 4.19f && next <= due + 7.21f,
                "successful slide did not restore occasional cadence");
        f.Game.UpdateTrackingMovement(next + 10.f);
        Require(f.Targets[0].TrackingSlides == 2 && f.Game.NextTrackingSlideTime >= next + 14.f,
                "tracking slide caught up in a burst after a stall");
        Require(f.Targets[0].DodgeAttempts > 0 && f.Targets[0].Reversals > 0,
                "slide scheduling removed existing dodges or strafes");
        f.Game.NextTrackingSlideTime = 69.5f;
        f.Game.UpdateTrackingMovement(69.5f);
        Require(f.Targets[0].TrackingSlides == 2 && f.Game.NextTrackingSlideTime == 70.f,
                "slide started too close to the run end");
    }
    for (int scenario : {0,1,2}) for (int phase : {0,1,2,3}) {
        Fixture f; f.Game.Progress.Scenario = scenario; f.Start(); f.Game.ActivateSlot(0,10.f);
        f.Game.Progress.Phase = phase; f.Game.NextTrackingSlideTime = 10.f;
        f.Game.UpdateTrackingMovement(10.f);
        Require(f.Targets[0].TrackingSlideAttempts == int(scenario == 0 && phase == 2),
                "tracking slide escaped active Link practice");
    }
    Fixture hidden; hidden.Game.Progress.Scenario = 0; hidden.Start(); hidden.Game.NextTrackingSlideTime = 10.f;
    hidden.Game.UpdateTrackingMovement(10.f);
    Require(hidden.Targets[0].TrackingSlideAttempts == 0, "hidden tracking target tried to slide");
}
void TrackingCrouchPolicyAndCadence() {
    using namespace NCAimTrainerScenarioPolicy;
    for (float roll : {-std::numeric_limits<float>::infinity(), -1.f, 0.f, .25f, .5f, 1.f, 2.f,
                       std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        const float delay = TrackingCrouchDelaySeconds(roll), hold = TrackingCrouchHoldSeconds(roll);
        Require(std::isfinite(delay) && delay >= 6.f && delay <= 10.f
                && std::isfinite(hold) && hold >= .2f && hold <= .45f,
                "invalid random roll escaped bounded tracking crouch timing");
    }
    for (float roll : {0.f, .5f, 1.f}) {
        Fixture f; f.Game.Progress.Scenario = 0; f.Game.Schedule.Roll = roll; f.Start(); f.At(10.f);
        f.Game.NextTrackingSlideTime = f.Game.NextDodgeTime = 10000.f;
        const float delay = 6.f + 4.f * roll, hold = .2f + .25f * roll;
        const float due = f.Game.NextCrouchTime[0];
        Require(std::fabs(due - 10.f - delay) < .001f && f.Game.CrouchEndTime[0] == 0.f,
                "first tracking crouch was not scheduled from activation with varied delay");
        f.Game.UpdateTrackingMovement(due - .001f);
        Require(f.Targets[0].CrouchRequests == 0, "tracking target crouched early");
        f.Game.UpdateTrackingMovement(due);
        const float end = f.Game.CrouchEndTime[0];
        Require(f.Targets[0].Crouched && f.Targets[0].CrouchRequests == 1
                && f.Game.NextCrouchTime[0] == 0.f && std::fabs(end - due - hold) < .001f,
                "tracking crouch missed its brief hold or repeated its start request");
        f.Game.UpdateTrackingMovement(end - .001f);
        Require(f.Targets[0].Crouched && f.Targets[0].StandRequests == 0, "tracking target stood early");
        f.Game.UpdateTrackingMovement(end);
        Require(!f.Targets[0].Crouched && f.Game.CrouchEndTime[0] == 0.f
                && std::fabs(f.Game.NextCrouchTime[0] - end - delay) < .001f,
                "successful standing did not schedule another independently delayed crouch");
        const float next = f.Game.NextCrouchTime[0];
        f.Game.UpdateTrackingMovement(next + 10.f);
        Require(f.Targets[0].CrouchRequests == 2 && f.Targets[0].StandRequests == 1
                && std::fabs(f.Game.CrouchEndTime[0] - next - 10.f - hold) < .001f,
                "scheduler caught up in a burst or shortened a crouch after a stalled tick");
    }
}
void TrackingCrouchRetryAndConflicts() {
    Fixture f; f.Game.Progress.Scenario = 0; f.Start(); f.At(10.f);
    f.Game.NextTrackingSlideTime = f.Game.NextDodgeTime = 10000.f;
    const float due = f.Game.NextCrouchTime[0];
    f.Targets[0].CanCrouch = false;
    f.Game.UpdateTrackingMovement(due);
    Require(f.Targets[0].CrouchRequests == 1 && !f.Targets[0].Crouched && f.Game.CrouchEndTime[0] == 0.f
            && std::fabs(f.Game.NextCrouchTime[0] - due - .2f) < .001f,
            "native airborne/slide rejection lost the bounded crouch retry");
    f.Game.UpdateTrackingMovement(due + .1f);
    Require(f.Targets[0].CrouchRequests == 1, "blocked crouch retried every tick");
    const float retry = f.Game.NextCrouchTime[0];
    f.Targets[0].CanCrouch = true;
    f.Game.NextTrackingSlideTime = f.Game.NextDodgeTime = f.Game.NextDirectionTime = retry;
    f.Game.UpdateTrackingMovement(retry);
    Require(f.Targets[0].Crouched && f.Targets[0].CrouchRequests == 2
            && f.Targets[0].TrackingSlideAttempts == 0 && f.Targets[0].DodgeAttempts == 0,
            "due dodge or slide replaced a successfully started tracking crouch");
    const int reversals = f.Targets[0].Reversals;
    f.Game.NextDirectionTime = retry + .1f;
    f.Game.UpdateTrackingMovement(retry + .1f);
    Require(f.Targets[0].Reversals == reversals + 1 && f.Targets[0].TrackingSlideAttempts == 0
            && f.Targets[0].DodgeAttempts == 0,
            "holding crouch stopped A/D reversals or allowed another movement action");
    const float end = f.Game.CrouchEndTime[0];
    f.Targets[0].CanStand = false;
    f.Game.UpdateTrackingMovement(end); f.Game.UpdateTrackingMovement(end + .02f);
    Require(f.Targets[0].Crouched && f.Targets[0].StandRequests == 2 && f.Game.CrouchEndTime[0] == end
            && f.Game.NextCrouchTime[0] == 0.f && f.Targets[0].TrackingSlideAttempts == 0
            && f.Targets[0].DodgeAttempts == 0,
            "blocked standing lost its retry or allowed a conflicting dodge/slide");
    f.Targets[0].CanStand = true;
    f.Game.UpdateTrackingMovement(end + .04f);
    Require(!f.Targets[0].Crouched && f.Targets[0].StandRequests == 3 && f.Game.CrouchEndTime[0] == 0.f
            && std::fabs(f.Game.NextCrouchTime[0] - end - .04f - 8.f) < .001f
            && f.Targets[0].TrackingSlideAttempts == 1 && f.Targets[0].DodgeAttempts == 1,
            "standing recovery did not resume movement and restart crouch spacing from actual recovery");
}
void TrackingCrouchScopeAndEndGuard() {
    for (int scenario : {0,1,2}) for (int phase : {0,1,2,3}) {
        Fixture f; f.Game.Progress.Scenario = scenario; f.Start(); f.Game.ActivateSlot(0,10.f);
        f.Game.Progress.Phase = phase; f.Game.NextCrouchTime[0] = 10.f;
        f.Game.UpdateTrackingMovement(10.f);
        Require(f.Targets[0].CrouchRequests == int(scenario == 0 && phase == 2),
                "tracking crouch escaped active Link practice");
    }
    for (int invalid : {0,1,2}) {
        Fixture f; f.Game.Progress.Scenario = 0; f.Start(); f.At(10.f); f.Game.NextCrouchTime[0] = 10.f;
        if (invalid == 0) f.Targets[0].Visible = false;
        if (invalid == 1) f.Game.Targets[0] = nullptr;
        if (invalid == 2) f.Game.Targets.clear();
        f.Game.UpdateTrackingMovement(10.f);
        Require(f.Targets[0].CrouchRequests == 0, "missing or hidden target acquired a crouch");
    }
    for (float margin : {-.001f,.001f}) {
        Fixture f; f.Game.Progress.Scenario = 0; f.Start(); f.At(10.f);
        const float hold = NCAimTrainerScenarioPolicy::TrackingCrouchHoldSeconds(f.Game.Schedule.Roll);
        const float due = 70.f - hold - .1f - margin;
        f.Game.NextCrouchTime[0] = due; f.Game.UpdateTrackingMovement(due);
        Require(f.Targets[0].Crouched == (margin > 0.f) && f.Game.NextCrouchTime[0] == 0.f
                && f.Game.TargetExpiry[0] == 70.f,
                "late tracking crouch did not preserve its complete hold plus ending margin");
        if (margin > 0.f) {
            f.Game.UpdateTrackingMovement(f.Game.CrouchEndTime[0]);
            Require(!f.Targets[0].Crouched && f.Targets[0].StandRequests == 1,
                    "permitted late crouch did not finish before the run ended");
        }
        f.Game.NextCrouchTime[0] = 70.f;
        const int requests = f.Targets[0].CrouchRequests;
        f.Game.UpdateTrackingMovement(70.f); f.Game.UpdateTrackingMovement(70.1f);
        Require(f.Targets[0].CrouchRequests == requests, "round-end guard allowed a new crouch");
    }
}
void TrackingCrouchCleanupAndReuse() {
    for (int phase : {0,3}) {
        Fixture f; f.Game.Progress.Scenario = 0; f.Start(); f.At(10.f);
        f.Game.UpdateTrackingMovement(f.Game.NextCrouchTime[0]);
        Require(f.Targets[0].Crouched, "cleanup fixture did not crouch");
        for (int slot = 1; slot < NCAimTrainerLayout::TargetCount; ++slot) {
            f.Game.NextCrouchTime[slot] = 17.f; f.Game.CrouchEndTime[slot] = 18.f;
        }
        f.Game.Progress.Phase = phase; f.Game.HideAllTargets();
        for (int slot = 0; slot < NCAimTrainerLayout::TargetCount; ++slot)
            Require(!f.Targets[slot].Visible && !f.Targets[slot].Crouched
                    && f.Game.NextCrouchTime[slot] == 0.f && f.Game.CrouchEndTime[slot] == 0.f,
                    "abort/results cleanup retained crouch posture or stale scheduled actions");
        Require(f.Game.NextTrackingSlideTime == 0.f && f.Game.NextPopupSlideTime[NCAimTrainerLayout::PopupSliderSlot] == 0.f,
                "crouch cleanup left a slide queued");
        f.Game.UpdateTrackingMovement(20.f);
        Require(f.Targets[0].CrouchRequests == 1, "hidden completed run executed another crouch");
        f.Game.Progress.Phase = 2; f.Game.Schedule.Roll = 0.f;
        f.Game.NextCrouchTime[0] = 14.f; f.Game.CrouchEndTime[0] = 15.f;
        f.Game.ActivateSlot(0,25.f);
        Require(f.Targets[0].Visible && !f.Targets[0].Crouched && f.Game.NextCrouchTime[0] == 31.f
                && f.Game.CrouchEndTime[0] == 0.f,
                "reused tracking target inherited an old crouch instead of fresh activation timing");
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    if (name == "standing_profile") {
        for (int scenario : {0,1,2}) {
            Fixture f; f.Game.Progress.Scenario=scenario; f.Start();
            f.Game.ArenaOrigin=FVector(40.f,90.f,50000.f);
            const int count=scenario==0 ? 1 : scenario==1 ? NCAimTrainerLayout::HeadSlotCount : NCAimTrainerLayout::TargetCount;
            for (int slot=0; slot<count; ++slot) {
                f.Targets[slot].Shape.HalfHeight=72.f; f.Targets[slot].Crouched=true;
                f.Game.ActivateSlot(slot,10.f);
                const float floor=scenario==0 ? 0.f : scenario==1 ? NCAimTrainerLayout::HeadSeat(slot).FloorZ
                    : slot==NCAimTrainerLayout::PopupDodgerSlot ? NCAimTrainerLayout::PopupDodgerSeat().FloorZ
                    : NCAimTrainerLayout::PopupSeat(slot).FloorZ;
                Require(f.Targets[slot].Position.Z==50000.f+floor+(scenario==2 ? 103.f : 108.f),
                        "reappearing crouched target used live height instead of native class standing height");
            }
        }
    }
    else if (name == "strafe") StrafeMix();
    else if (name == "dodges") Dodges();
    else if (name == "refire") RefireAndBacklog();
    else if (name == "bounds") RollBoundaries();
    else if (name == "initial") InitialSpawn();
    else if (name == "replacements") Replacements();
    else if (name == "stall") StallNoCatchup();
    else if (name == "heads") HeadshotSlots();
    else if (name == "initialize") InitializeRefire();
    else if (name == "layout") LayoutAndWiggles();
    else if (name == "crouch_scope") CrouchScenarioScope();
    else if (name == "crouch_once") CrouchOncePerAppearance();
    else if (name == "crouch_expiry") CrouchExpiryGuard();
    else if (name == "crouch_blocked") BlockedStandingRetry();
    else if (name == "crouch_reuse") CrouchReuseReset();
    else if (name == "persistent_start") PersistentDodgerStartsAndDoesNotExpire();
    else if (name == "persistent_refill") PersistentDodgerRefillsWithoutConsumingPopupCadence();
    else if (name == "persistent_cadence") PersistentDodgerCadenceAndNativeRejection();
    else if (name == "persistent_scope") PersistentDodgerCannotLeakIntoOtherPhasesOrScenarios();
    else if (name == "slide_scope") UpperPlatformSlideScope();
    else if (name == "popup_long") PopupLongStrafeLifecycle();
    else if (name == "precision_popup") PrecisionPopupCadence();
    else if (name == "slide_once") UpperPlatformSlideOnceAndReuse();
    else if (name == "slide_expiry") UpperPlatformSlideExpiryAndRoundGuard();
    else if (name == "tracking_slide") TrackingSlideCadenceAndScope();
    else if (name == "tracking_crouch_cadence") TrackingCrouchPolicyAndCadence();
    else if (name == "tracking_crouch_retry") TrackingCrouchRetryAndConflicts();
    else if (name == "tracking_crouch_scope") TrackingCrouchScopeAndEndGuard();
    else if (name == "tracking_crouch_cleanup") TrackingCrouchCleanupAndReuse();
    else Require(false, "unknown case");
}
'''


class AimTrainerScenarioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-scenarios-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").read_text(encoding="utf-8-sig")
        layout = (PLUGIN / "Source/Private/NCAimTrainerLayout.h").read_text(encoding="utf-8-sig")
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "void ANCAimTrainerGame::BeginActiveRun",
            "void ANCAimTrainerGame::HideAllTargets",
            "void ANCAimTrainerGame::ActivateSlot",
            "float ANCAimTrainerGame::RecordTargetHit",
            "void ANCAimTrainerGame::UpdateTargets",
            "void ANCAimTrainerGame::UpdatePopupDodger",
            "void ANCAimTrainerGame::UpdateTrackingMovement",
        )
        source = directory / "trainer_scenarios.cpp"
        source.write_text("\n".join([policy.replace("#pragma once", ""), layout.replace("#pragma once", ""), ADAPTER]
                                    + [native_function(game, signature) for signature in signatures]
                                    + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_scenarios.exe" if os.name == "nt" else "trainer_scenarios")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_scenarios.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Scenario policy compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_crouched_target_reappearance_uses_selected_class_standing_height(self): self.run_case("standing_profile")

    def test_short_reversals_dominate_but_keep_timing_variation(self): self.run_case("strafe")
    def test_dodges_choose_both_sides_and_turn_inward_near_edges(self): self.run_case("dodges")
    def test_popup_capacity_and_exposure_allow_one_second_refire(self): self.run_case("refire")
    def test_roll_boundaries_preserve_cadence_and_finite_deadlines(self): self.run_case("bounds")
    def test_initial_popup_slots_wait_for_one_global_deadline(self): self.run_case("initial")
    def test_hit_and_expiry_replacements_share_global_gate(self): self.run_case("replacements")
    def test_long_stall_spawns_once_and_starts_next_deadline_from_now(self): self.run_case("stall")
    def test_headshot_slots_keep_existing_initial_hit_and_expiry_delays(self): self.run_case("heads")
    def test_run_resets_cadence_and_rejects_altered_weapon_from_rankings(self): self.run_case("initialize")
    def test_five_slot_geometry_and_wiggle_stay_within_cover_and_platforms(self): self.run_case("layout")
    def test_link_and_instagib_use_separate_crouch_schedulers(self): self.run_case("crouch_scope")
    def test_crouch_runs_once_per_appearance_and_failed_requests_do_not_repeat(self): self.run_case("crouch_once")
    def test_crouch_guard_preserves_full_rifle_refire_before_expiry(self): self.run_case("crouch_expiry")
    def test_blocked_uncrouch_retries_until_native_clearance_recovers(self): self.run_case("crouch_blocked")
    def test_reused_target_drops_previous_crouch_deadlines(self): self.run_case("crouch_reuse")
    def test_permanent_dodger_starts_immediately_and_never_times_out(self): self.run_case("persistent_start")
    def test_permanent_dodger_refills_without_consuming_popup_deadline(self): self.run_case("persistent_refill")
    def test_permanent_dodger_respects_native_rejection_and_bounded_random_cadence(self): self.run_case("persistent_cadence")
    def test_permanent_dodger_is_excluded_from_headshots_tracking_and_inactive_phases(self): self.run_case("persistent_scope")
    def test_popup_slides_select_only_eligible_seats_in_both_weapon_variants(self): self.run_case("slide_scope")
    def test_rear_popup_long_strafe_scope_slide_conflicts_cleanup_and_exposure_guards(self): self.run_case("popup_long")
    def test_precision_popup_uses_real_rifle_cadence_and_teamarena_targets(self): self.run_case("precision_popup")
    def test_slider_attempts_once_per_appearance_and_resets_after_hit(self): self.run_case("slide_once")
    def test_slider_preserves_refire_opportunity_before_expiry_or_round_end(self): self.run_case("slide_expiry")
    def test_tracking_slide_cadence_retries_and_scope_preserve_native_movement_gates(self): self.run_case("tracking_slide")
    def test_tracking_crouch_random_bounds_holds_and_repeat_spacing(self): self.run_case("tracking_crouch_cadence")
    def test_tracking_crouch_retries_native_rejection_and_excludes_dodge_slide_while_strafing(self): self.run_case("tracking_crouch_retry")
    def test_tracking_crouch_scope_and_end_margin(self): self.run_case("tracking_crouch_scope")
    def test_tracking_crouch_cleanup_on_abort_results_and_target_reuse(self): self.run_case("tracking_crouch_cleanup")


if __name__ == "__main__":
    unittest.main()
