#pragma once

// Timing and direction rules for the revision-10 trainer preset. The authority
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
    inline bool ShouldPopupSlide(int Slot, float Roll)
    {
        return Slot == 2 || ((Slot == 0 || Slot == 4) && UnitRoll(Roll) < 0.45f);
    }
    inline bool ShouldPopupLongStrafe(int Slot, float Roll)
    {
        return Slot == 0 && UnitRoll(Roll) < 0.65f;
    }
    inline float PopupLongStrafeDelaySeconds(float Roll) { return 2.3f + 0.6f * UnitRoll(Roll); }
    inline float PopupLongStrafeHoldSeconds(float Roll) { return 0.50f + 0.25f * UnitRoll(Roll); }
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
