#pragma once

#include <cmath>

// Ballistic timing uses the target's native gravity. No teleporting along an
// animation curve and no altered world time are needed for the jump pads.
namespace NCAimTrainerAirbornePolicy
{
    inline bool JumpArc(float GravityZ, float StartZ, float EndZ, float ApexZ,
        float& UpSpeed, float& FlightSeconds)
    {
        const float Gravity = -GravityZ;
        if (!std::isfinite(Gravity) || !std::isfinite(StartZ) || !std::isfinite(EndZ)
            || !std::isfinite(ApexZ) || Gravity < 1.f || ApexZ <= StartZ || ApexZ <= EndZ) { return false; }
        UpSpeed = std::sqrt(2.f * Gravity * (ApexZ - StartZ));
        FlightSeconds = UpSpeed / Gravity + std::sqrt(2.f * (ApexZ - EndZ) / Gravity);
        return std::isfinite(FlightSeconds) && FlightSeconds > 0.f;
    }

    inline float SpawnDelay(float Roll) { return 0.45f + 0.25f * Roll; }

    inline bool IsDiagonalDrop(float Roll) { return Roll < 0.6f; }

    inline float DropExposure(float GravityZ, float HeightAboveHazard, float UpSpeed)
    {
        const float Gravity = -GravityZ;
        if (!std::isfinite(Gravity) || !std::isfinite(HeightAboveHazard)
            || !std::isfinite(UpSpeed) || Gravity < 1.f || HeightAboveHazard <= 0.f) { return 0.f; }
        const float ContactTime = (UpSpeed + std::sqrt(UpSpeed * UpSpeed + 2.f * Gravity * HeightAboveHazard)) / Gravity;
        // Retire a second before the previous goo-contact deadline. Very short
        // rocket falls retain a 0.75s opportunity instead of an instant expiry.
        // Physical goo contact can still end an appearance sooner.
        return ContactTime > 1.75f ? ContactTime - 1.f : 0.75f;
    }

    inline bool UseSideWallSeat(int DropIndex, float Roll)
    {
        return (DropIndex == 0 || DropIndex == 4) && Roll < 0.45f;
    }

    // Scale vertical trajectory time for falling slots only. The target uses
    // rate-squared gravity and rate-scaled launch Z, preserving the arc height.
    inline float FlightRate(int Scenario) { return Scenario == 8 ? 0.93f * 0.95f : Scenario == 7 ? 0.95f * 0.95f : 1.f; }

    inline bool AtHazard(float FeetZ, float HazardZ) { return FeetZ <= HazardZ; }

    inline bool ValidRocketAge(float Now, float Created, float RunStart, float Appearance)
    {
        return std::isfinite(Now) && std::isfinite(Created) && Created >= RunStart
            && Created >= Appearance && Created <= Now && Now - Created <= 5.f;
    }
}
