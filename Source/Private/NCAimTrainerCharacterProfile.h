#pragma once

// Frozen character profiles for the trainer's own native classes. Source:
// /Game/Blueprints/Netcode/TeamArenaCharacter from NCWepMut (content 3525360),
// uasset SHA256 23864d29ab5d43e6cd26ed1f8e9cc5f1d0cacd6fe114d293ac407bf96f0a90d2;
// /Game/Blueprints/Netcode/IGCharacterFootsteps from the supplied
// MutInstagibNCP content pak, uasset SHA256
// fbfbb9bff5eb1627a30db2e34c430bf49a9dde47149b4ea052e789da330ef394.
// Both cooked CDOs and BeginPlay bytecode were
// inspected: BeginPlay sets slide speeds to 1350 initial / 1100 sustained.
// Do not apply capsule/mesh profiles only to a live actor: stock UnCrouch and
// ApplyCharacterData restore their respective values from the native class CDO.
namespace NCAimTrainerCharacterProfile
{
    constexpr float WalkSpeed = 940.f;
    constexpr float CrouchedWalkSpeed = 315.f;
    constexpr float Acceleration = 5000.f;
    constexpr float CrouchedHalfHeight = 72.f;

    struct FProfile
    {
        float CapsuleRadius, CapsuleHalfHeight;
        float MeshScale, MeshZ;
        float StandingEyeHeight, CrouchedEyeHeight, DefaultCrouchedEyeHeight;
        float SlideEyeHeight, SlideTargetHeight;
    };

    inline FProfile TeamArena()
    {
        return { 40.f, 108.f, 1.f, -110.f, 83.f, 45.f, 40.f, 1.f, 69.f };
    }

    inline FProfile Instagib()
    {
        // IG's authored initial CrouchedEyeHeight is 43, but its inherited
        // DefaultCrouchedEyeHeight remains 40. UT recalculates from the latter.
        return { 38.f, 103.f, .95f, -110.f, 80.f, 43.f, 40.f, .95f, 69.f };
    }

    // Constructor-only: establishes both actor defaults and each class's CDO.
    // Keep the existing animation class, hand attachment and skin selection.
    template<class Pawn>
    void ApplyCharacter(Pawn& Character, const FProfile& Profile)
    {
        Character.GetCapsuleComponent()->InitCapsuleSize(Profile.CapsuleRadius, Profile.CapsuleHalfHeight);
        Character.GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, Profile.MeshZ));
        Character.GetMesh()->SetRelativeScale3D(FVector(Profile.MeshScale));
        Character.BaseEyeHeight = Character.DefaultBaseEyeHeight = Profile.StandingEyeHeight;
        Character.CrouchedEyeHeight = Profile.CrouchedEyeHeight;
        Character.DefaultCrouchedEyeHeight = Profile.DefaultCrouchedEyeHeight;
        Character.FloorSlideEyeHeight = Profile.SlideEyeHeight;
        Character.SlideTargetHeight = Profile.SlideTargetHeight;
    }

    template<class Movement>
    void ApplyTeamArenaMovement(Movement& Move)
    {
        // Inherited UT values are explicit only where the trainer previously
        // replaced them with exercise-specific speed limits.
        Move.MaxWalkSpeed = WalkSpeed;
        Move.MaxWalkSpeedCrouched = CrouchedWalkSpeed;
        Move.MaxAcceleration = Acceleration;
        Move.DefaultBrakingDecelerationWalking = 2000.f;
        Move.BrakingDecelerationWalking = 2000.f;
        Move.GroundFriction = 14.f;
        Move.DodgeAirControl = .55f;
        Move.CrouchedHalfHeight = CrouchedHalfHeight;
        Move.NetworkSimulatedSmoothLocationTime = .07f;
        Move.EasyImpactImpulse = 850.f;
        Move.EasyImpactDamage = 10.f;
        Move.FullImpactImpulse = 2000.f;
        Move.FullImpactDamage = 25.f;
        Move.ImpactMaxHorizontalVelocity = 2300.f;
        Move.MaxInitialFloorSlideSpeed = 1350.f;
        Move.MaxFloorSlideSpeed = 1100.f;
    }

    template<class Movement>
    void ApplyInstagibMovement(Movement& Move)
    {
        ApplyTeamArenaMovement(Move);
        Move.MaxFastAccelSpeed = 220.f;
        Move.DefaultBrakingDecelerationWalking = 2100.f;
        Move.DodgeAirControl = .6f;
        Move.MaxStepHeight = 53.f;
        Move.NetworkSimulatedSmoothLocationTime = .05f;
        Move.NetworkMaxSmoothUpdateDistance = 284.f;
    }
}
