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
struct AUTWeap_LinkGun_Shaft_NCP : AUTWeaponFix {
    struct BeamInfo { int DamageType = 7; };
    TArray<BeamInfo> InstantHitInfo;
    bool IsFiring() const { return true; }
    int GetCurrentFireMode() const { return 1; }
};
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct FDamageEvent { int DamageTypeClass = 5; };
struct ANCAimTrainerTarget : AActor {
    bool Visible = false, Strafing = false, Crouched = false;
    bool CanStand = true, CanCrouch = true;
    int Activations = 0, Hides = 0, Wiggles = 0, Reversals = 0;
    int CrouchRequests = 0, StandRequests = 0;
    float WiggleRange = 0.f;
    FVector Position;
    void ActivateTarget(const FVector& position, bool strafe) {
        Visible = true; Strafing = strafe; Crouched = false; Position = position; ++Activations;
    }
    bool IsAvailable() const { return Visible; }
    float GetAppearanceTime() const { return 0.f; }
    void HideTarget() { Visible = false; Crouched = false; ++Hides; }
    void StartWiggle(float range) { WiggleRange = range; ++Wiggles; }
    void ReverseStrafe() { ++Reversals; }
    bool SetTrainerCrouched(bool crouch) {
        if (crouch) { ++CrouchRequests; if (!CanCrouch) return false; }
        else { ++StandRequests; if (!CanStand) return false; }
        Crouched = crouch; return true;
    }
};
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
    float NextTargetTime[5] = {}, TargetExpiry[5] = {}, NextWiggleTime[5] = {};
    float NextCrouchTime[5] = {}, CrouchEndTime[5] = {};
    float NextTrackingHitSoundTime = 0.f;
    bool bRankedRun = true;
    std::string UnrankedReason;
    World* GetWorld() { return &TheWorld; }
    bool IsTrainee(AController* player) const { return player && player == Trainee; }
    void PublishProgress() {}
    void BeginActiveRun();
    void ActivateSlot(int32, float);
    void UpdateTargets(float);
    float RecordTargetHit(ANCAimTrainerTarget*, float, const FDamageEvent&, AController*, AActor*);
};
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    AUTPlayerState PlayerState;
    AUTPlusSniper Gun;
    ANCAimTrainerTarget Targets[5];
    Fixture() {
        Player.PlayerState = &PlayerState; Game.Trainee = &Player; Game.RunWeapon = &Gun;
        for (auto& target : Targets) Game.Targets.Add(&target);
    }
    void Start() { Game.BeginActiveRun(); }
    void At(float now) { Game.TheWorld.Now = now; Game.UpdateTargets(now); }
    float Hit(int slot) { return Game.RecordTargetHit(&Targets[slot], 100.f, FDamageEvent(), &Player, &Gun); }
    int Activations() const {
        int total = 0; for (const auto& target : Targets) total += target.Activations; return total;
    }
    int Visible() const {
        int total = 0; for (const auto& target : Targets) total += int(target.Visible); return total;
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
    Require(InstagibMaxActiveTargets == 5, "expanded target capacity lost");
    for (float actualRefire : {0.f, .4f, 1.f, 1.5f, 2.f}) {
        const float refire = actualRefire < 1.f ? 1.f : actualRefire;
        for (float roll : {0.f, .01f, .5f, .99f, 1.f}) {
            const float delay = PopupSpawnDelay(actualRefire, roll);
            const float exposure = PopupExposure(actualRefire, roll);
            Require(delay > refire && delay <= 1.36f * refire, "spawn outruns weapon or adds excessive downtime");
            Require(exposure >= 5.5f * refire && exposure <= 6.81f * refire, "exposure drift");
            // A shot just preceded a full backlog. Clear the older four, then
            // the new target, with .35 refire intervals of reaction time.
            const float lastBacklogShot = .99f * refire
                + (InstagibMaxActiveTargets - 1) * refire + .35f * refire;
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
        Require(PopupSpawnDelay(1.f, roll) > 1.f, "invalid roll bypassed weapon cadence");
        Require(PopupExposure(1.f, roll) >= 5.5f, "invalid roll shortened exposure");
    }
    Require(PopupSpawnDelay(nan, 0.f) > 1.f, "invalid weapon interval bypassed minimum");
}
void InitialSpawn() {
    Fixture f; f.Game.Schedule.SlotChoice = 2; f.Start();
    Require(f.Game.Progress.Phase == 2 && f.Game.Progress.RemainingSeconds == 60.f, "run did not start");
    f.At(10.f);
    Require(f.Visible() == 1 && f.Activations() == 1 && f.Targets[2].Visible,
            "initial popup burst or random eligible slot selection lost");
    const float next = f.Game.NextPopupTime;
    Require(next > 11.f && next < 11.36f, "first spawn deadline ignores one-second refire");
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
        f.Game.NextPopupTime = 900.f; f.Game.PopupRefireSeconds = 99.f;
        for (int i = 0; i < 5; ++i) {
            f.Game.NextTargetTime[i] = 900.f; f.Game.TargetExpiry[i] = 800.f; f.Game.NextWiggleTime[i] = 700.f;
            f.Game.NextCrouchTime[i] = 600.f; f.Game.CrouchEndTime[i] = 500.f;
        }
        f.Start();
        const float expected = std::isfinite(refire) && refire > 1.f ? refire : 1.f;
        Require(f.Game.PopupRefireSeconds == expected && f.Game.NextPopupTime == 10.f,
                "restart retained previous cadence or unsafe refire");
        Require(f.Game.bRankedRun == (refire == 1.f), "altered or invalid weapon cadence remained ranked");
        for (int i = 0; i < 5; ++i) {
            Require(f.Game.NextTargetTime[i] == 10.f && f.Game.TargetExpiry[i] == 0.f && f.Game.NextWiggleTime[i] == 10.f
                    && f.Game.NextCrouchTime[i] == 0.f && f.Game.CrouchEndTime[i] == 0.f,
                    "restart retained prior slot schedule");
        }
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
    Require(TargetCount == 5 && PopupSlotCount == 5 && HeadSlotCount == 5, "five-slot layout lost");
    for (int scenario : {1, 2}) {
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
                        && z == seat.FloorZ + CapsuleHalfHeight, "activation ignored authored layout bounds");
                Require(target.Wiggles == 1 && target.WiggleRange == seat.WiggleRange && !target.Strafing,
                        "precision target did not start its authored wiggle");
                Require(f.Game.NextWiggleTime[slot] >= 10.12f && f.Game.NextWiggleTime[slot] <= 10.29f,
                        "initial wiggle decision deadline lost");
                if (scenario == 2 && slot < 3) {
                    const FBlock platform = PopupPlatform(slot);
                    Require(std::fabs(y - platform.CenterY) + seat.WiggleRange + CapsuleRadius + WiggleSafetyMargin <= platform.SizeY * .5f,
                            "popup motion would leave its supporting platform");
                    Require(std::fabs(x - platform.CenterX) + CapsuleRadius <= platform.SizeX * .5f,
                            "popup spawn has no support beneath its capsule");
                }
                if (scenario == 1) {
                    const FBlock cover = HeadCover(slot);
                    Require(seat.WiggleRange + CapsuleRadius < cover.SizeY * .5f && x - CapsuleRadius > cover.CenterX + cover.SizeX * .5f,
                            "headshot wiggle exposes body around or inside cover");
                    Require(z + CapsuleHalfHeight > cover.Height && z - CapsuleHalfHeight < cover.Height,
                            "headshot body/head do not straddle cover height");
                    Require(f.Game.TargetExpiry[slot] == 16.5f, "five-head exposure reverted to short preset");
                }
            }
            if (scenario == 2) {
                const FBlock cover = PopupPlatform(1);
                const auto& peek = f.Targets[3];
                Require(peek.Position.X - f.Game.ArenaOrigin.X - CapsuleRadius > cover.CenterX + cover.SizeX * .5f
                        && peek.Position.Z - f.Game.ArenaOrigin.Z == CapsuleHalfHeight,
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
            Require(f.Game.NextCrouchTime[slot]==0.f, "non-instagib scenario scheduled a crouch");
            // Even a stale deadline must not leak a crouch into other modes.
            f.Game.NextCrouchTime[slot]=10.1f;
        }
        f.At(10.2f);
        for (const auto& target:f.Targets) Require(target.CrouchRequests==0, "other scenario executed instagib crouch");
    }
    for (float roll:{0.f,.649f,.65f,1.f}) {
        Fixture f; f.Game.Schedule.Roll=roll; f.Start(); f.At(10.f);
        const bool selected=roll<.65f;
        Require((f.Game.NextCrouchTime[0]>10.f)==selected, "instagib crouch selection boundary drifted");
        if(selected) Require(f.Game.NextCrouchTime[0]>=11.5f && f.Game.NextCrouchTime[0]<=13.5f,
                             "crouch does not leave an initial standing opportunity");
    }
}
void CrouchOncePerAppearance() {
    Fixture f; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    const float due=f.Game.NextCrouchTime[0];
    f.At(due-.001f);
    Require(!f.Targets[0].Crouched && f.Targets[0].CrouchRequests==0,"target crouched before deadline");
    f.At(due);
    Require(f.Targets[0].Crouched && f.Targets[0].CrouchRequests==1 && f.Game.NextCrouchTime[0]==0.f,
            "due crouch was missed or left a repeating request");
    const float end=f.Game.CrouchEndTime[0];
    Require(end>=due+.25f && end<=due+.45f,"crouch hold outside its brief exposure interruption");
    f.At(end-.001f); Require(f.Targets[0].Crouched,"target stood before hold ended");
    f.At(end);
    Require(!f.Targets[0].Crouched && f.Game.CrouchEndTime[0]==0.f && f.Targets[0].StandRequests==1,
            "completed crouch left the target hidden or standing request active");
    f.At(end+.1f); f.At(end+.3f); f.At(end+.7f);
    Require(f.Targets[0].CrouchRequests==1 && f.Targets[0].StandRequests==1,"appearance repeated its crouch/stand");
    Fixture failed; failed.Start(); failed.At(10.f); failed.Game.NextPopupTime=10000.f;
    failed.Targets[0].CanCrouch=false;
    const float failAt=failed.Game.NextCrouchTime[0]; failed.At(failAt); failed.At(failAt+.1f);
    Require(failed.Targets[0].CrouchRequests==1 && failed.Game.CrouchEndTime[0]==0.f,
            "failed crouch was retried or created a bogus standing deadline");
}
void CrouchExpiryGuard() {
    for (float refire:{1.f,1.5f,2.f}) {
        for (float margin:{-.001f,.001f}) {
            Fixture f; f.Gun.Refire=refire; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
            const float due=f.Game.NextCrouchTime[0];
            const float hold=NCAimTrainerScenarioPolicy::CrouchHoldSeconds(f.Game.Schedule.Roll);
            f.Game.TargetExpiry[0]=due+hold+refire+.1f+margin;
            const float expiry=f.Game.TargetExpiry[0];
            f.At(due);
            const bool allowed=margin>0.f;
            Require(f.Targets[0].Crouched==allowed && f.Targets[0].CrouchRequests==(allowed?1:0),
                    "near-expiry crouch did not preserve one real refire interval plus reaction margin");
            Require(f.Game.TargetExpiry[0]==expiry && f.Game.NextCrouchTime[0]==0.f,
                    "guard changed lifetime or left a doomed crouch queued");
            if(allowed) {
                f.At(f.Game.CrouchEndTime[0]);
                Require(!f.Targets[0].Crouched && expiry-f.Game.TheWorld.Now>refire,
                        "target stood with less than a rifle interval remaining");
            }
        }
    }
}
void BlockedStandingRetry() {
    Fixture f; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    f.At(f.Game.NextCrouchTime[0]);
    const float end=f.Game.CrouchEndTime[0]; f.Targets[0].CanStand=false;
    f.At(end); f.At(end+.02f);
    Require(f.Targets[0].Crouched && f.Game.CrouchEndTime[0]==end && f.Targets[0].StandRequests==2,
            "blocked native uncrouch lost its retry deadline");
    f.Targets[0].CanStand=true; f.At(end+.04f);
    Require(!f.Targets[0].Crouched && f.Game.CrouchEndTime[0]==0.f && f.Targets[0].StandRequests==3,
            "target did not stand after clearance recovered");
    f.At(end+.06f);
    Require(f.Targets[0].StandRequests==3 && f.Targets[0].CrouchRequests==1,
            "standing recovery left repeated state transitions");
}
void CrouchReuseReset() {
    Fixture f; f.Start(); f.At(10.f); f.Game.NextPopupTime=10000.f;
    f.At(f.Game.NextCrouchTime[0]); Require(f.Targets[0].Crouched,"fixture did not crouch");
    Require(f.Hit(0)>0.f && !f.Targets[0].Visible,"crouched target hit was not accepted");
    f.Game.Schedule.Roll=1.f; // Replacement appearance deliberately skips crouch.
    f.Game.NextPopupTime=f.Game.TheWorld.Now+.1f; f.At(f.Game.NextPopupTime);
    Require(f.Targets[0].Visible && f.Targets[0].Activations==2 && !f.Targets[0].Crouched
            && f.Game.NextCrouchTime[0]==0.f && f.Game.CrouchEndTime[0]==0.f,
            "reused target inherited old crouch posture or deadlines");
    f.Game.Schedule.Roll=.1f;
    f.Game.ActivateSlot(0,f.Game.TheWorld.Now+.1f);
    Require(f.Game.NextCrouchTime[0]>f.Game.TheWorld.Now+1.5f && f.Game.CrouchEndTime[0]==0.f,
            "new appearance cannot schedule a fresh crouch after previous one");
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    if (name == "strafe") StrafeMix();
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
            "void ANCAimTrainerGame::ActivateSlot",
            "float ANCAimTrainerGame::RecordTargetHit",
            "void ANCAimTrainerGame::UpdateTargets",
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
    def test_only_selected_instagib_appearances_schedule_crouch(self): self.run_case("crouch_scope")
    def test_crouch_runs_once_per_appearance_and_failed_requests_do_not_repeat(self): self.run_case("crouch_once")
    def test_crouch_guard_preserves_full_rifle_refire_before_expiry(self): self.run_case("crouch_expiry")
    def test_blocked_uncrouch_retries_until_native_clearance_recovers(self): self.run_case("crouch_blocked")
    def test_reused_target_drops_previous_crouch_deadlines(self): self.run_case("crouch_reuse")


if __name__ == "__main__":
    unittest.main()
