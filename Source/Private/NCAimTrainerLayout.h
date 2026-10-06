#pragma once

// Shared seats and collision geometry for the fixed trainer arena. Coordinates
// are relative to ArenaOrigin. Keep movement limits and platform support paired.
namespace NCAimTrainerLayout
{
    enum { TargetCount = 6, PopupSlotCount = 5, PopupSliderSlot = 2, PopupDodgerSlot = 5, HeadSlotCount = 5, PopupPlatformCount = 3 };
    constexpr float CapsuleRadius = 40.f;
    constexpr float CapsuleHalfHeight = 108.f;
    constexpr float WiggleSpeed = 220.f;
    constexpr float WiggleAcceleration = 7000.f;
    // Extra room beyond the reversal threshold for deceleration and one tick.
    constexpr float WiggleSafetyMargin = 15.f;
    constexpr float HeadWiggleRange = 80.f;

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
        const float Height = Index == 0 ? 1.f : Index == 1 ? 176.f : 320.f;
        return { 1100.f, float(Index - 1) * 850.f, 2600.f, 580.f, Height };
    }

    inline FSeat PopupSeat(int Index)
    {
        switch (Index)
        {
        case 1:
            // Leave the middle sightline open to the floor target behind this
            // block; its head would otherwise be obscured by the elevated pawn.
            return { 100.f, 2200.f, -175.f, 0.f, 44.f, PopupPlatform(1).Height };
        case 2:
            // Keep a runway toward the trainee for one native forward slide,
            // including its ending slowdown, before this appearance retires.
            return { 1000.f, 2200.f, 850.f, 85.f, 99.f, PopupPlatform(2).Height };
        case 3:
            // A standing head peeks over the central block. The capsule stays
            // behind the block's X=2400 rear face and moves along the floor.
            return { 2650.f, 2850.f, 0.f, 0.f, 60.5f, 0.f };
        case 4:
            // This near-left lane sits outside the platform footprints and in
            // a different angular band from the low-platform character.
            return { -300.f, 100.f, -1450.f, 35.f, 82.5f, 0.f };
        default:
            return { 100.f, 2200.f, -850.f, 85.f, 99.f, PopupPlatform(0).Height };
        }
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
