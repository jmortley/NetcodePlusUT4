#pragma once

// Shared seats and collision geometry for the fixed trainer arena. Coordinates
// are relative to ArenaOrigin. Keep movement limits and platform support paired.
namespace NCAimTrainerLayout
{
    enum { TargetCount = 6, PopupSlotCount = 5, PopupSliderSlot = 2, PopupDodgerSlot = 5, HeadSlotCount = 5, PopupPlatformCount = 3 };
    // Gameplay movement profiles supply speed and acceleration. The input
    // driver anticipates stopping distance instead of limiting pawn speed.
    // Extra room beyond the predictive reversal threshold for tick integration.
    constexpr float WiggleSafetyMargin = 15.f;
    constexpr float HeadWiggleRange = 80.f;
    constexpr float PopupLongStrafeRange = 220.f;
    constexpr float InstagibStrafeRange = 400.f;

    struct FSeat
    {
        float MinX, MaxX;
        float CenterY, SpawnJitterY, WiggleRange;
        float FloorZ;
    };

    struct FBlock
    {
        float CenterX, CenterY;
        float SizeX, SizeY, Height;
    };

    constexpr float AirborneHazardZ = 20.f;
    constexpr float AirborneDropMinZ = 1350.f;
    constexpr float AirborneDropMaxZ = 1750.f;
    constexpr float AirborneJumpApexZ = 1550.f;
    // Fixed rocket view: ledge320 + TeamArena capsule108 + eye83. Keep target
    // capsules full-size while halving their center displacement from this eye.
    constexpr float AirborneRocketEyeZ = 511.f;
    constexpr float AirborneRocketHalfHeight = 108.f;

    inline float AirborneTargetHeight(float Height, bool bRockets)
    {
        return bRockets ? AirborneRocketEyeZ + 0.5f * (Height - AirborneRocketEyeZ) : Height;
    }

    inline float AirborneHazardHeight(bool bRockets)
    {
        return bRockets ? AirborneTargetHeight(AirborneHazardZ + AirborneRocketHalfHeight, true)
            - AirborneRocketHalfHeight : AirborneHazardZ;
    }

    inline float AirborneDropHeight(float Height, bool bRockets, bool bSideWall = false)
    {
        const float SpawnHeight = AirborneTargetHeight(Height, bRockets);
        if (!bRockets || bSideWall) { return SpawnHeight; }
        // Rear rocket targets start with 60% more clearance above the goo.
        // Measure from capsule contact, leaving the goo and jump arc in place.
        const float ContactHeight = AirborneHazardHeight(true) + AirborneRocketHalfHeight;
        return ContactHeight + 1.6f * (SpawnHeight - ContactHeight);
    }

    inline float AirborneJumpApex(bool bRockets) { return AirborneTargetHeight(AirborneJumpApexZ, bRockets); }

    // Retain the compact layout's launch velocity scale. Raised rear drops
    // use the same native gravity and velocities, giving them longer falls.
    inline float AirborneLaunchScale(bool bRockets) { return bRockets ? 0.70710678118f : 1.f; }

    inline FBlock AirborneFiringLedge(bool bRockets = false)
    {
        return { bRockets ? -800.f : -1800.f, 0.f, bRockets ? 400.f : 1000.f, 3600.f, 320.f };
    }

    inline float PracticeLaneX(int Scenario) { return AirborneFiringLedge(Scenario == 10).CenterX; }

    inline FBlock AirborneJumpPad(int Index, bool bRockets = false)
    {
        if (bRockets)
        {
            const float Height = AirborneTargetHeight(180.f + AirborneRocketHalfHeight + 2.f, true)
                - AirborneRocketHalfHeight - 2.f;
            return { Index == 0 ? -350.f : 150.f, Index == 0 ? -700.f : 700.f, 320.f, 220.f, Height };
        }
        return { Index == 0 ? 100.f : 1100.f, Index == 0 ? -1400.f : 1400.f, 700.f, 440.f, 180.f };
    }

    inline FSeat AirborneDropSeat(int Index, bool bSideWall = false, bool bRockets = false)
    {
        if (bRockets)
        {
            const FSeat Original = AirborneDropSeat(Index, bSideWall, false);
            const float LaneX = AirborneFiringLedge(true).CenterX;
            // Bring rear drops forward without moving the jumper or side lanes.
            // The nearer band still leaves 25 units beyond a full 40-radius
            // capsule at the right pad's rear edge (X=310).
            const float RearOffset = bSideWall && (Index == 0 || Index == 4) ? 0.f : Index % 2 == 0 ? 75.f : 200.f;
            return { LaneX + 0.5f * (Original.MinX - LaneX) - RearOffset, LaneX + 0.5f * (Original.MaxX - LaneX) - RearOffset,
                0.5f * Original.CenterY, 0.5f * Original.SpawnJitterY, 0.f, AirborneHazardHeight(true) };
        }
        // Only the outer pooled slots can choose their own side-wall lane:
        // one left and one right maximum. This X gap clears both pad platforms
        // even after the inward drift, so each drop reaches the goo normally.
        if (bSideWall && (Index == 0 || Index == 4))
        {
            return { 550.f, 650.f, Index == 0 ? -1660.f : 1660.f, 25.f, 0.f, AirborneHazardZ };
        }
        // Distinct rear X planes avoid the jumper's diagonal X=100..1100 crossing. Even at
        // the extremes every full target capsule clears the room and pads.
        return { Index % 2 == 0 ? 1700.f : 2250.f, Index % 2 == 0 ? 1900.f : 2500.f,
            float(Index - 2) * 600.f, 100.f, 0.f, AirborneHazardZ };
    }

    inline FBlock PopupPlatform(int Index)
    {
        // IGCharacterFootsteps is shorter than TeamArena. Keep the rear head
        // peek open with its actual 103-half-height, 95%-scale body.
        const float Height = Index == 0 ? 1.f : Index == 1 ? 160.f : 320.f;
        return { 1100.f, float(Index - 1) * 850.f, 2600.f, 580.f, Height };
    }

    inline int PopupSeatVariantCount(int Index)
    {
        return Index == 4 ? 3 : Index == 1 ? 2 : 1;
    }

    inline FSeat PopupSeat(int Index, int Variant = 0, bool bInstagib = false)
    {
        switch (Index)
        {
        case 1:
            // Leave the middle sightline open to the floor target behind this
            // block; its head would otherwise be obscured by the elevated pawn.
            return { 100.f, 2200.f, Variant == 1 ? 178.f : -178.f, 0.f, 47.f, PopupPlatform(1).Height };
        case 2:
            // Keep a runway toward the trainee for one native forward slide,
            // including its ending slowdown, before this appearance retires.
            return { 1000.f, 2200.f, 850.f, 85.f, 99.f, PopupPlatform(2).Height };
        case 3:
            // An IG-sized standing head peeks over the central block. The capsule stays
            // behind the block's X=2400 rear face and moves along the floor.
            return { 2650.f, 2850.f, 0.f, 0.f, 60.5f, 0.f };
        case 4:
            // The right alternative stays in front of the tall platform so
            // the fixed-position trainee can actually see the whole target.
            if (Variant == 1) { return { -600.f, -300.f, bInstagib ? 1400.f : 1450.f, 50.f, bInstagib ? 160.f : 100.f, 0.f }; }
            // The deep left corridor gives forward/backward dodges room while
            // retaining clear separation from the foreground dodger's X plane.
            if (Variant == 2) { return { 800.f, 1400.f, bInstagib ? -1300.f : -1550.f, 25.f, bInstagib ? 320.f : 150.f, 0.f }; }
            return { -600.f, -300.f, bInstagib ? -1300.f : -1450.f, 50.f, bInstagib ? 320.f : 180.f, 0.f };
        default:
            // Forward slide runway and a wider occasional strafe share this
            // low platform. Tighter spawn jitter leaves room for the full capsule.
            return { 1000.f, 2200.f, -850.f, 10.f, bInstagib ? InstagibStrafeRange : 180.f, PopupPlatform(0).Height };
        }
    }

    inline bool CanPopupDodgePath(int Slot, float StartX, float StartY, float EndX, float EndY,
        float CapsuleRadius, float ResumeHalfWidth)
    {
        // The caller predicts UT's actual dodge velocity, including preserved
        // perpendicular momentum. Enclose its whole travel and the subsequent
        // walking band, rather than testing only the starting capsule.
        if (!(CapsuleRadius > 0.f) || !(ResumeHalfWidth >= 0.f)
            || !(StartX > -3200.f && StartX < 3200.f && EndX > -3200.f && EndX < 3200.f)
            || !(StartY > -1800.f && StartY < 1800.f && EndY > -1800.f && EndY < 1800.f)) { return false; }
        const float MinX = StartX < EndX ? StartX : EndX;
        const float MaxX = StartX > EndX ? StartX : EndX;
        const float MinY = (StartY < EndY ? StartY : EndY) - ResumeHalfWidth - WiggleSafetyMargin;
        const float MaxY = (StartY > EndY ? StartY : EndY) + ResumeHalfWidth + WiggleSafetyMargin;
        if (Slot != 0 && Slot != 4) { return false; }
        // Only the two left variants dodge. The near left appearance can head
        // away from the trainee; a deeper appearance can choose either X sign.
        // Reserve both capsules at the permanent dodger's X=-800 plane.
        // The left platform is only one unit tall, so native movement can step
        // onto the surrounding floor. Exclude the central 160-unit cover and
        // right platform, rather than confining real dodges to that tiny step.
        return MinX - 2.f * CapsuleRadius > -800.f && MaxX + CapsuleRadius < 3200.f
            && MinY - CapsuleRadius > -1800.f && MaxY + CapsuleRadius < -290.f;
    }

    inline FSeat PopupDodgerSeat()
    {
        // The open foreground lane stays clear of every platform. Its 800-unit
        // walking reversal threshold leaves additional room for native dodges;
        // the scenario policy turns outward dodges inward beyond 500 units.
        return { -800.f, -800.f, 0.f, 0.f, 800.f, 0.f };
    }

    inline FSeat HeadSeat(int Index)
    {
        return { 900.f, 900.f, float(Index - 2) * 650.f, 0.f, HeadWiggleRange, 0.f };
    }

    inline FBlock HeadCover(int Index)
    {
        return { 600.f, HeadSeat(Index).CenterY, 180.f, 460.f, 176.f };
    }
}
