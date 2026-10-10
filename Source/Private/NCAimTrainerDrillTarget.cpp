#include "NetcodePlus.h"
#include "NCAimTrainerTarget.h"
#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerDrillPolicy.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "UTCharacterMovement.h"
#include "UTDamageType.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

void ANCAimTrainerTarget::ConfigureDrillMovement(bool bShock, const FVector& Origin)
{
    if (Role != ROLE_Authority) { return; }
    DrillMovementMode = bShock ? 2 : 1;
    DrillOrigin = Origin;
    DrillStrafeSide = 1.f;
    DrillAdvance = bShock ? 0.4f : 0.f;
    bDrillWallDodgePending = false;
    DrillDodgeBlockedUntil = DrillWallDodgeUntil = NextDrillWallAttempt = 0.f;
    bTrainerStrafe = true;
    bTrainerWiggle = bPopupEvasion = false;
}

void ANCAimTrainerTarget::ChooseDrillStrafe(float SideRoll, float DepthRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || DrillMovementMode == 0 || bRecenterWiggleAfterDodge
        || !GetCharacterMovement()->IsMovingOnGround() || !FMath::IsFinite(SideRoll) || !FMath::IsFinite(DepthRoll)) { return; }
    DrillStrafeSide = SideRoll < 0.5f ? -1.f : 1.f;
    const float Depth = FMath::Clamp(DepthRoll, 0.f, 1.f);
    // The carrier spends real time moving across the lane before a diagonal
    // dodge. Flak targets can cut both toward and away from the player.
    DrillAdvance = DrillMovementMode == 2 ? 0.15f + 0.6f * Depth : -0.7f + 1.4f * Depth;
}

void ANCAimTrainerTarget::TickDrillMovement(float DeltaSeconds)
{
    const FVector Offset = GetActorLocation() - DrillOrigin;
    FVector Input;
    if (DrillMovementMode == 1)
    {
        const FVector Radial = Offset.GetSafeNormal2D();
        Input = FVector(-Radial.Y, Radial.X, 0.f) * DrillStrafeSide + Radial * DrillAdvance;
        // Allow full dodge spacing in the enlarged room. Start steering inward
        // before reaching the walls; native acceleration still does the braking.
        const float Radius = Offset.Size2D();
        if (Radius > 675.f) { Input -= Radial * 2.f; }
        else if (Radius < 190.f) { Input += Radial * 2.f; }
    }
    else
    {
        const float Side = NCAimTrainerScenarioPolicy::BoundedStrafeDirection(Offset.Y, GetVelocity().Y,
            GetCharacterMovement()->MaxAcceleration, NCAimTrainerDrillPolicy::LaneHalfWidth - 50.f, DrillStrafeSide, DeltaSeconds);
        Input = FVector(-DrillAdvance, Side, 0.f);
        if (Offset.X < NCAimTrainerDrillPolicy::CaptureX + 450.f)
        {
            // Finish the capture instead of orbiting past the pad. Keep a
            // lateral feint while the direct route is already inside its width.
            Input = FVector(NCAimTrainerDrillPolicy::CaptureX - Offset.X, -Offset.Y, 0.f).GetSafeNormal2D();
            if (FMath::Abs(Offset.Y) < 120.f) { Input.Y += 0.55f * Side; }
        }
    }
    // Native acceleration and shock momentum own velocity throughout the run.
    AddMovementInput(Input.GetSafeNormal2D(), 1.f, true);
}

bool ANCAimTrainerTarget::CanStartTrainerDrillAction() const
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (Role != ROLE_Authority || !bTrainerVisible || DrillMovementMode == 0 || IsDead() || bIsCrouched
        || bRecenterWiggleAfterDodge || IsTrainerSliding() || !Movement || !Movement->IsMovingOnGround()
        || !Movement->CurrentFloor.IsWalkableFloor() || !Movement->CanDodge() || Movement->bIsDodging || Movement->bIsDodgeLanding
        || GetWorld()->GetTimeSeconds() < DrillDodgeBlockedUntil) { return false; }
    return DrillMovementMode != 2 || Movement->Velocity.X <= 100.f;
}

bool ANCAimTrainerTarget::TryTrainerDrillDodge(float DirectionRoll)
{
    if (!CanStartTrainerDrillAction() || !FMath::IsFinite(DirectionRoll)) { return false; }
    const FVector Offset = GetActorLocation() - DrillOrigin;
    const float Roll = FMath::Clamp(DirectionRoll, 0.f, 1.f);
    const float Side = Roll < 0.5f ? -1.f : 1.f;
    const float Variation = Roll < 0.5f ? Roll * 2.f : (Roll - 0.5f) * 2.f;
    bool bUseWall = false;
    FVector Direction;
    if (DrillMovementMode == 2)
    {
        if (Offset.X < NCAimTrainerDrillPolicy::CaptureX + 400.f) { return false; }
        // Sometimes launch toward a nearby wall, then use UT's actual airborne
        // wall trace to kick back into the lane. Other dodges cut across it.
        bUseWall = Variation < 0.28f && FMath::Abs(Offset.Y) > 220.f
            && FMath::Abs(Offset.Y) < 500.f && Offset.X > NCAimTrainerDrillPolicy::CaptureX + 750.f;
        const float DiagonalSide = bUseWall ? FMath::Sign(Offset.Y)
            : FMath::Abs(Offset.Y) > 280.f ? -FMath::Sign(Offset.Y) : Side;
        Direction = FVector(-(bUseWall ? 0.65f : 1.f), DiagonalSide * (0.85f + 0.65f * Variation), 0.f).GetSafeNormal2D();
    }
    else
    {
        const FVector Radial = Offset.GetSafeNormal2D();
        const float Depth = Offset.Size2D() > 495.f ? -1.4f : -0.65f + 1.3f * Variation;
        Direction = (FVector(-Radial.Y, Radial.X, 0.f) * Side + Radial * Depth).GetSafeNormal2D();
    }
    const bool bDodged = Dodge(Direction, FVector(-Direction.Y, Direction.X, 0.f));
    if (bDodged)
    {
        ConsumeMovementInputVector();
        bRecenterWiggleAfterDodge = true;
        bDrillWallDodgePending = bUseWall;
        DrillWallDodgeUntil = GetWorld()->GetTimeSeconds() + 0.75f;
        NextDrillWallAttempt = GetWorld()->GetTimeSeconds() + 0.08f;
    }
    return bDodged;
}

bool ANCAimTrainerTarget::TryTrainerDrillWallDodge()
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (Role != ROLE_Authority || !bTrainerVisible || DrillMovementMode != 2 || !bDrillWallDodgePending
        || !Movement || IsDead() || bIsCrouched) { return false; }
    const float Now = GetWorld()->GetTimeSeconds();
    if (Now >= DrillWallDodgeUntil || Movement->IsMovingOnGround() || Movement->Velocity.X > 100.f
        || Now < DrillDodgeBlockedUntil)
    {
        bDrillWallDodgePending = false;
        return false;
    }
    const FVector Offset = GetActorLocation() - DrillOrigin;
    if (!Movement->IsFalling() || !Movement->CanDodge() || Now < NextDrillWallAttempt || FMath::Abs(Offset.Y) < 500.f) { return false; }
    NextDrillWallAttempt = Now + 0.04f;
    const FVector Direction = FVector(-0.7f, -FMath::Sign(Offset.Y), 0.f).GetSafeNormal2D();
    // Dodge -> PerformDodge -> CanWallDodge checks actual collision, wall normal,
    // native cooldown and repeat-wall restrictions. No synthetic air impulse.
    if (!Dodge(Direction, FVector(-Direction.Y, Direction.X, 0.f))) { return false; }
    bDrillWallDodgePending = false;
    ConsumeMovementInputVector();
    bRecenterWiggleAfterDodge = true;
    return true;
}

void ANCAimTrainerTarget::ApplyTrainerShockMomentum(const FDamageEvent& Event)
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (Role != ROLE_Authority || DrillMovementMode != 2 || !Movement || !Event.IsOfType(FUTPointDamageEvent::ClassID)) { return; }
    // UTGetDamageMomentum is not exported by the Windows game module. A
    // validated shock primary already supplies the native point-event impulse.
    FVector Momentum = static_cast<const FUTPointDamageEvent&>(Event).Momentum;
    if (Momentum.ContainsNaN()) { return; }
    const UUTDamageType* Type = Event.DamageTypeClass ? Cast<UUTDamageType>(Event.DamageTypeClass->GetDefaultObject()) : nullptr;
    if (Type && Type->bForceZMomentum && Movement->IsMovingOnGround())
    {
        Momentum.Z = FMath::Max(Momentum.Z, Type->ForceZMomentumPct * Momentum.Size());
    }
    // Damage queues its native impulse for the movement tick. Block both dodge
    // routes immediately, even before that queued impulse changes Velocity.
    bDrillWallDodgePending = false;
    DrillDodgeBlockedUntil = GetWorld()->GetTimeSeconds() + 0.4f;
    Movement->UUTCharacterMovement::AddDampedImpulse(Momentum, false);
}

void ANCAimTrainerTarget::UpdateDrillFlag()
{
    if (GetNetMode() == NM_DedicatedServer) { return; }
    const ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(GetWorld()->GetFirstPlayerController());
    const bool bShow = bTrainerVisible && PC && NCAimTrainerScenarioPolicy::IsShockDefense(PC->GetTrainerProgress().Scenario);
    if (bShow && !DrillFlagPole.IsValid())
    {
        // Cosmetic flag: no CTF objective actor, controller, or replicated
        // PlayerState is needed for these pooled practice targets.
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"), nullptr, LOAD_NoWarn | LOAD_Quiet);
        for (int32 Part = 0; Part < 2; ++Part)
        {
            UStaticMeshComponent* FlagPart = NewObject<UStaticMeshComponent>(this);
            FlagPart->SetupAttachment(RootComponent);
            FlagPart->SetStaticMesh(Cube);
            FlagPart->SetRelativeLocation(Part == 0 ? FVector(-30.f, 0.f, 95.f) : FVector(-30.f, 42.f, 152.f));
            FlagPart->SetRelativeScale3D(Part == 0 ? FVector(0.035f, 0.035f, 2.3f) : FVector(0.025f, 0.85f, 0.65f));
            FlagPart->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            FlagPart->SetCastShadow(false);
            if (Material)
            {
                UMaterialInstanceDynamic* Tint = UMaterialInstanceDynamic::Create(Material, FlagPart);
                Tint->SetVectorParameterValue(TEXT("Color"), Part == 0 ? FLinearColor::White : FLinearColor(1.f, 0.7f, 0.02f));
                FlagPart->SetMaterial(0, Tint);
            }
            FlagPart->RegisterComponent();
            if (Part == 0) { DrillFlagPole = FlagPart; }
            else { DrillFlagBanner = FlagPart; }
        }
    }
    if (DrillFlagPole.IsValid()) { DrillFlagPole->SetHiddenInGame(!bShow); }
    if (DrillFlagBanner.IsValid()) { DrillFlagBanner->SetHiddenInGame(!bShow); }
}
