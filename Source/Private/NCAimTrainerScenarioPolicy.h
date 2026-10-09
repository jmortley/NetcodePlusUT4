#pragma once

#include "../Public/NCAimTrainerScenarioId.h"

// Timing and direction rules for the trainer presets. The authority
// supplies independent FRandomStream rolls; this helper never owns random state.
namespace NCAimTrainerScenarioPolicy
{
    enum { InstagibMaxActiveTargets = 6, ScenarioCount = NCAimTrainerScenarioId::ScenarioCount };

    inline bool IsValidScenario(int Scenario) { return Scenario >= 0 && Scenario < ScenarioCount; }
    inline bool IsTrackingScenario(int Scenario) { return Scenario == 0 || Scenario == 6; }
    inline bool IsInstagibScenario(int Scenario) { return Scenario == 2 || Scenario == 7; }
    inline bool IsAirborneScenario(int Scenario) { return Scenario >= 7 && Scenario <= 10; }
    inline bool IsRocketScenario(int Scenario) { return Scenario == 10; }
    inline bool IsHeadshotScenario(int Scenario) { return Scenario == 1 || Scenario == 4; }
    inline bool IsPopupScenario(int Scenario) { return Scenario == 2 || Scenario == 3 || Scenario == 5; }
    inline bool IsSACTFScenario(int Scenario) { return Scenario == 4 || Scenario == 5 || Scenario == 9; }
    inline bool IsSniperScenario(int Scenario) { return Scenario == 1 || Scenario == 3 || Scenario == 8 || IsSACTFScenario(Scenario); }
    inline bool HasHeadshotBonus(int Scenario) { return Scenario == 3 || Scenario == 5 || Scenario == 8 || Scenario == 9; }
    inline int ArenaScenario(int Scenario) { return IsRocketScenario(Scenario) ? 4 : IsAirborneScenario(Scenario) ? 3 : IsTrackingScenario(Scenario) ? 0 : IsHeadshotScenario(Scenario) ? 1 : 2; }

    inline float UnitRoll(float Roll)
    {
        if (!(Roll > 0.f)) { return 0.f; }
        return Roll < 1.f ? Roll : 1.f;
    }

    inline float WiggleHoldSeconds(float Roll)
    {
        return 0.12f + 0.16f * UnitRoll(Roll);
    }

    inline bool HasVariedPopupMovement(int Slot) { return Slot == 0 || Slot == 1 || Slot == 4; }

    inline int PopupSpawnVariant(int Slot, float Roll)
    {
        // Reuse the same timed actor across different seats, not extra targets
        // that would outpace the rifle or change the checkpoint target IDs.
        const float Choice = UnitRoll(Roll);
        if (Slot == 4) { return Choice < 0.4f ? 0 : Choice < 0.7f ? 1 : 2; }
        return Slot == 1 && Choice >= 0.5f ? 1 : 0;
    }

    inline float PopupStrafeHoldSeconds(int Slot, float PatternRoll, float JitterRoll, int Variant = 0)
    {
        if (!HasVariedPopupMovement(Slot)) { return WiggleHoldSeconds(JitterRoll); }
        // Give acceleration time to produce a readable movement across the
        // lane. Mix shorter reversals with longer commitments each decision.
        const float Jitter = UnitRoll(JitterRoll);
        if (Slot == 0 || (Slot == 4 && Variant != 1))
        {
            // Commit to longer runs on the left instead of mostly reversing
            // before the wider lane has produced useful lateral travel.
            return UnitRoll(PatternRoll) < 0.35f ? 0.35f + 0.20f * Jitter : 0.65f + 0.35f * Jitter;
        }
        return UnitRoll(PatternRoll) < 0.6f ? 0.24f + 0.18f * Jitter : 0.45f + 0.30f * Jitter;
    }

    enum EPopupAction { PopupStrafe, PopupSlide, PopupLongStrafe, PopupForwardDodge, PopupBackwardDodge, PopupDodgeSlide };

    inline int PopupAction(int Slot, int Variant, float Roll)
    {
        if (Slot == 2) { return PopupSlide; }
        const float Choice = UnitRoll(Roll);
        if (Slot == 0 || (Slot == 4 && Variant != 1))
        {
            if (Choice < 0.15f) { return PopupForwardDodge; }
            if (Choice < 0.30f) { return PopupBackwardDodge; }
            if (Choice < 0.40f) { return PopupDodgeSlide; }
            // Deep left seats have a narrow outer corridor. Use their checked
            // angled dodge/slide path instead of the foreground lateral slide.
            if (Choice < 0.60f && !(Slot == 4 && Variant == 2)) { return PopupSlide; }
            if (Choice < 0.80f && Slot == 0) { return PopupLongStrafe; }
        }
        else if (Slot == 4 && Choice < 0.45f) { return PopupSlide; }
        return PopupStrafe;
    }

    inline float PopupDodgeDelaySecondsForAppearance(float Roll) { return 0.65f + 0.70f * UnitRoll(Roll); }
    inline float PopupDodgeAngleDegrees(float Roll) { return 12.f + 16.f * UnitRoll(Roll); }

    inline float BoundedStrafeDirection(float Offset, float Velocity, float Acceleration,
        float Range, float RequestedDirection, float DeltaSeconds)
    {
        // Conservative stopping distance ignores the additional ground friction.
        // Include a frame of travel so reversal does not wait for the edge.
        const float Step = DeltaSeconds > 0.f ? DeltaSeconds : 0.f;
        const float Brake = Acceleration > 1.f ? Acceleration : 1.f;
        const float Speed = Velocity < 0.f ? -Velocity : Velocity;
        const float Stop = Offset + Velocity * (Speed / (2.f * Brake) + Step);
        if (Offset >= Range || Stop >= Range) { return -1.f; }
        if (Offset <= -Range || Stop <= -Range) { return 1.f; }
        return RequestedDirection;
    }

    inline bool ShouldCrouch(float Roll) { return UnitRoll(Roll) < 0.65f; }
    inline float CrouchDelaySeconds(float Roll) { return 1.5f + 2.f * UnitRoll(Roll); }
    inline float CrouchHoldSeconds(float Roll) { return 0.25f + 0.20f * UnitRoll(Roll); }
    inline float PopupSlideDelaySeconds(float Roll) { return 0.8f + 0.6f * UnitRoll(Roll); }
    inline float PopupLongStrafeDelaySeconds(float Roll) { return 0.8f + 0.5f * UnitRoll(Roll); }
    inline float PopupLongStrafeHoldSeconds(float Roll) { return 0.70f + 0.30f * UnitRoll(Roll); }
    inline float PopupLongStrafeDirection(float Offset, float Roll)
    {
        if (Offset > 20.f) { return -1.f; }
        if (Offset < -20.f) { return 1.f; }
        return UnitRoll(Roll) < 0.5f ? -1.f : 1.f;
    }
    inline float TrackingSlideDelaySeconds(float Roll) { return 4.f + 3.f * UnitRoll(Roll); }
    inline float TrackingCrouchDelaySeconds(float Roll) { return 6.f + 4.f * UnitRoll(Roll); }
    inline float TrackingCrouchHoldSeconds(float Roll) { return 0.20f + 0.25f * UnitRoll(Roll); }

    inline float StrafeHoldSeconds(float PatternRoll, float JitterRoll)
    {
        // Most decisions produce a short reversal; occasional longer holds
        // prevent the target from settling into a regular left-right rhythm.
        const float Jitter = UnitRoll(JitterRoll);
        return UnitRoll(PatternRoll) < 0.75f ? 0.14f + 0.20f * Jitter : 0.45f + 0.35f * Jitter;
    }

    inline float DodgeDelaySeconds(float Roll)
    {
        return 1.8f + 2.f * UnitRoll(Roll);
    }

    inline float PopupDodgeDelaySeconds(float Roll)
    {
        return 1.15f + 0.95f * UnitRoll(Roll);
    }

    inline float PopupFirstDodgeDelaySeconds(float Roll)
    {
        // Get moving before the next rifle shot after a replacement appears.
        return 0.2f + 0.35f * UnitRoll(Roll);
    }

    inline float DodgeDirection(float Offset, float Roll)
    {
        // A native UT dodge covers much more ground than a short strafe.
        // Turn it inward before the target approaches either lane boundary.
        if (Offset >= 500.f) { return -1.f; }
        if (Offset <= -500.f) { return 1.f; }
        return UnitRoll(Roll) < 0.5f ? -1.f : 1.f;
    }

    inline float TrackingSlideDirection(float Offset, float Roll)
    {
        // A lateral native slide needs the same early inward turn as a dodge.
        // Use the shared guard even when a preceding dodge overshot the walk lane.
        return DodgeDirection(Offset, Roll);
    }

    inline float PopupRefireSeconds(float RefireSeconds)
    {
        // SACTF is the fastest supported rifle. Game initialization separately
        // validates each preset's exact refire and clamps other pop-ups to 1s.
        return RefireSeconds >= 0.7f ? RefireSeconds : 0.7f;
    }

    inline float PopupSpawnDelay(float RefireSeconds, float Roll)
    {
        // Applied to one global spawn deadline, including replacements. Missed
        // deadlines start from the current time; never catch up in a burst.
        return PopupRefireSeconds(RefireSeconds) * (1.f + 0.10f * UnitRoll(Roll));
    }

    inline float PopupExposure(float RefireSeconds, float Roll)
    {
        // Prioritize fresh targets instead of banking a full five-shot queue.
        // Even the shortest exposure retains several legal rifle opportunities.
        return PopupRefireSeconds(RefireSeconds) * (4.5f + 0.9f * UnitRoll(Roll));
    }
}
