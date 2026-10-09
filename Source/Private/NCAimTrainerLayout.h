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
    constexpr float PopupLongStrafeRange = 180.f;

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

    inline FSeat PopupSeat(int Index, int Variant = 0)
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
            if (Variant == 1) { return { -600.f, -300.f, 1450.f, 50.f, 100.f, 0.f }; }
            // The deep left corridor gives forward/backward dodges room while
            // retaining clear separation from the foreground dodger's X plane.
            if (Variant == 2) { return { 800.f, 1400.f, -1550.f, 25.f, 100.f, 0.f }; }
            return { -600.f, -300.f, -1450.f, 50.f, 100.f, 0.f };
        default:
            // Forward slide runway and a wider occasional strafe share this
            // low platform. Tighter spawn jitter leaves room for the full capsule.
            return { 1000.f, 2200.f, -850.f, 50.f, 120.f, PopupPlatform(0).Height };
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
