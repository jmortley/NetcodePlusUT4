#pragma once

// Timing and direction rules shared by the revision-5/6 trainer presets. The authority
// supplies independent FRandomStream rolls; this helper never owns random state.
namespace NCAimTrainerScenarioPolicy
{
    enum { InstagibMaxActiveTargets = 6 };

    inline float UnitRoll(float Roll)
    {
        if (!(Roll > 0.f)) { return 0.f; }
        return Roll < 1.f ? Roll : 1.f;
    }

    inline float WiggleHoldSeconds(float Roll)
    {
        return 0.12f + 0.16f * UnitRoll(Roll);
    }

    inline bool ShouldCrouch(float Roll) { return UnitRoll(Roll) < 0.65f; }
    inline float CrouchDelaySeconds(float Roll) { return 1.5f + 2.f * UnitRoll(Roll); }
    inline float CrouchHoldSeconds(float Roll) { return 0.25f + 0.20f * UnitRoll(Roll); }

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

    inline float PopupRefireSeconds(float RefireSeconds)
    {
        // Faster custom weapons cannot turn the standard preset into bursts.
        return RefireSeconds >= 1.f ? RefireSeconds : 1.f;
    }

    inline float PopupSpawnDelay(float RefireSeconds, float Roll)
    {
        // Applied to one global spawn deadline, including replacements. Missed
        // deadlines start from the current time; never catch up in a burst.
        return PopupRefireSeconds(RefireSeconds) * (1.f + 0.10f * UnitRoll(Roll));
    }

    inline float PopupExposure(float RefireSeconds, float Roll)
    {
        // Five targets can overlap. A new target at the back of a full queue
        // gets five refire intervals plus at least half an interval to aim.
        return PopupRefireSeconds(RefireSeconds) * (5.5f + 1.3f * UnitRoll(Roll));
    }
}
