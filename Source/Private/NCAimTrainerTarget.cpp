#include "NCAimTrainerTarget.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLayout.h"
#include "NCAimTrainerCharacterProfile.h"
#include "NCPlusForceModels.h"
#include "UTCharacterMovement.h"
#include "UTCharacterContent.h"
#include "UTWeaponAttachment.h"
#include "Animation/AnimInstance.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"

ANCAimTrainerTarget::ANCAimTrainerTarget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // CharacterContent supplies a skin, not the pawn animation Blueprint. Native
    // UTCharacter subclasses also lack BaseUTCharacter's capsule-relative mesh
    // placement. Keep those authored defaults on our CDO so ApplyCharacterData's
    // class-default scale calculation remains correct on every application.
    static ConstructorHelpers::FClassFinder<AUTCharacter> CharacterTemplate(
        TEXT("/Game/RestrictedAssets/Blueprints/BaseUTCharacter"));
    if (CharacterTemplate.Class)
    {
        const AUTCharacter* Template = CharacterTemplate.Class->GetDefaultObject<AUTCharacter>();
        if (Template && Template->GetMesh())
        {
            GetMesh()->SetRelativeTransform(Template->GetMesh()->GetRelativeTransform());
            GetMesh()->SetAnimInstanceClass(Template->GetMesh()->AnimClass);
        }
    }
    // Use the requested UT3 third-person animation set on every target variant.
    // Missing UT3 content fails HasCharacterAssets instead of changing the preset.
    static ConstructorHelpers::FClassFinder<UAnimInstance> TrainerAnimation(
        TEXT("/Game/RestrictedAssets/Character/Base/Blueprints/Base_3p_AnimBP_UT3"));
    GetMesh()->SetAnimInstanceClass(TrainerAnimation.Class);
    // Third-person attachment supplies the rifle stance and hand placement.
    // Targets need no firing weapon, inventory, or combat controller.
    static ConstructorHelpers::FClassFinder<AUTWeaponAttachment> TrainerRifle(
        TEXT("/Game/RestrictedAssets/Weapons/ShockRifle/ShockAttachment"));
    WeaponAttachmentClass = TrainerRifle.Class;
    bAlwaysRelevant = true;
    NetUpdateFrequency = 60.f;
    MinNetUpdateFrequency = 30.f;
    Health = HealthMax = 100;
    ArmorAmount = 0;
    AutoPossessAI = EAutoPossessAI::Disabled;
    GetCharacterMovement()->bRunPhysicsWithNoController = true;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->bUseControllerDesiredRotation = false;
    NCAimTrainerCharacterProfile::ApplyCharacter(*this, NCAimTrainerCharacterProfile::TeamArena());
    NCAimTrainerCharacterProfile::ApplyTeamArenaMovement(*UTCharacterMovement);
    GetCharacterMovement()->GetNavAgentPropertiesRef().bCanCrouch = true;
    // Head position and animation must update even on a dedicated server.
    GetMesh()->MeshComponentUpdateFlag = EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones;
    GetMesh()->bEnableUpdateRateOptimizations = false;
}

ANCAimTrainerInstagibTarget::ANCAimTrainerInstagibTarget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    NCAimTrainerCharacterProfile::ApplyCharacter(*this, NCAimTrainerCharacterProfile::Instagib());
    NCAimTrainerCharacterProfile::ApplyInstagibMovement(*UTCharacterMovement);
}

ANCAimTrainerSACTFTarget::ANCAimTrainerSACTFTarget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    NCAimTrainerCharacterProfile::ApplySACTFMovement(*UTCharacterMovement);
}

void ANCAimTrainerTarget::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    // EnsureArena validates immediately after SpawnActor, including while the
    // world is beginning play. PostInitializeComponents precedes that return;
    // BeginPlay need not have run yet. Preserve the pawn animation class because
    // Malcolm's CharacterContent mesh deliberately has no AnimClass of its own.
    if (CharacterData)
    {
        UClass* PawnAnimClass = GetMesh()->AnimClass;
        ApplyCharacterData(CharacterData);
        GetMesh()->SetAnimInstanceClass(PawnAnimClass);
    }
}

void ANCAimTrainerTarget::ApplyCharacterData(TSubclassOf<AUTCharacterContent> /*Data*/)
{
    // Training geometry must match the same Malcolm skeleton for every viewer.
    // F5 can recolor that mesh, but must never replace it with another model.
    const ANCAimTrainerTarget* Defaults = GetClass()->GetDefaultObject<ANCAimTrainerTarget>();
    if (!Defaults || !Defaults->CharacterData) { return; }
    UClass* PawnAnimClass = GetMesh()->AnimClass;
    Super::ApplyCharacterData(Defaults->CharacterData);
    GetMesh()->SetAnimInstanceClass(PawnAnimClass);
    TrainerTintMaterials.Reset();
    NextTrainerTintTime = 0.f;
}

void ANCAimTrainerTarget::UpdateTrainerTint()
{
    if (GetNetMode() == NM_DedicatedServer) { return; }
    // Restore only parameters this trainer changed. Neither toggling colors
    // nor changing a palette rebuilds the mesh or restarts its animation.
    for (const FTrainerMaterialTint& Saved : TrainerTintMaterials)
    {
        UMaterialInstanceDynamic* Material = Saved.Material.Get();
        if (!Material) { continue; }
        for (const auto& Pair : Saved.Vectors) { Material->SetVectorParameterValue(Pair.Key, Pair.Value); }
        for (const auto& Pair : Saved.Scalars) { Material->SetScalarParameterValue(Pair.Key, Pair.Value); }
    }
    TrainerTintMaterials.Reset();
    if (!NCPlusForceModels::IsEnabled()) { return; }
    // Targets have no team/player state; resolve the existing enemy palette
    // against the local viewer without inventing a replicated gameplay team.
    const int32 EnemyTeam = NCPlusForceModels::GetViewerTeam(GetWorld()) == 1 ? 0 : 1;
    const FNCPlusModelSettings Side = NCPlusForceModels::GetModelSettings(EnemyTeam, false, GetWorld());
    const TSubclassOf<AUTCharacterContent> SelectedModel = NCPlusForceModels::GetModelClass(Side);
    if (!Side.bTint && !(SelectedModel && NCPlusForceModels::IsModelAllowed(SelectedModel))) { return; }
    const float Glow = FMath::Clamp(Side.Brightness, 1.f, 3.5f);
    FLinearColor Colour = NCPlusForceModels::GetSkinColour(Side) * Glow;
    Colour.A = 1.f;
    const float Emissive = FMath::Min((Glow - 1.f) * 1.25f, 2.5f);
    const TArray<FName>& Params = NCPlusForceModels::TeamColourParamNames();
    for (UMaterialInstanceDynamic* Material : GetBodyMIs())
    {
        if (!Material) { continue; }
        const FString Name = Material->Parent ? Material->Parent->GetName() : Material->GetName();
        if (NCPlusForceModels::IsRecolorSkippedMaterial(Name) || NCPlusForceModels::IsBakedMaterial(Name)) { continue; }
        FTrainerMaterialTint Saved;
        Saved.Material = Material;
        for (const FName& Param : Params)
        {
            FLinearColor Original;
            if (Material->GetVectorParameterValue(Param, Original))
            {
                Saved.Vectors.Add(Param, Original);
                Material->SetVectorParameterValue(Param, Colour);
            }
        }
        if (Saved.Vectors.Num() == 0) { continue; }
        const FName ScalarNames[] = { TEXT("TeamSelect"), TEXT("Team Color Blend Max"), TEXT("Emissive Max"), TEXT("Emission Power") };
        const float ScalarValues[] = { 255.f, 1.f, Emissive, Emissive };
        for (int32 Index = 0; Index < 4; ++Index)
        {
            float Original = 0.f;
            if (Material->GetScalarParameterValue(ScalarNames[Index], Original))
            {
                Saved.Scalars.Add(ScalarNames[Index], Original);
                Material->SetScalarParameterValue(ScalarNames[Index], ScalarValues[Index]);
            }
        }
        TrainerTintMaterials.Add(Saved);
    }
}

void ANCAimTrainerTarget::BeginPlay()
{
    Super::BeginPlay();
    // No weapon switch occurs on these unpossessed pawns. Create the normal
    // third-person attachment explicitly, including offline/listen-server play.
    UpdateWeaponAttachment();
    OnRep_TrainerVisible();
}

void ANCAimTrainerTarget::UpdateWeaponAttachment()
{
    Super::UpdateWeaponAttachment();
    if (WeaponAttachment)
    {
        // Cosmetic only: the held rifle must never intercept practice shots.
        WeaponAttachment->SetActorEnableCollision(false);
        WeaponAttachment->SetActorHiddenInGame(!bTrainerVisible);
        if (WeaponAttachment->Mesh) { WeaponAttachment->Mesh->bCastHiddenShadow = false; }
    }
}

bool ANCAimTrainerTarget::HasCharacterAssets() const
{
    return CharacterData && GetMesh() && GetMesh()->SkeletalMesh && GetMesh()->AnimClass;
}

void ANCAimTrainerTarget::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ANCAimTrainerTarget, bTrainerVisible);
    DOREPLIFETIME(ANCAimTrainerTarget, TrainerHeadshotScale);
    DOREPLIFETIME(ANCAimTrainerTarget, TrainerFlightRate);
}

void ANCAimTrainerTarget::OnRep_TrainerVisible()
{
    SetActorHiddenInGame(!bTrainerVisible);
    // Attachments are separate actors and do not inherit the pawn's hidden flag.
    if (WeaponAttachment) { WeaponAttachment->SetActorHiddenInGame(!bTrainerVisible); }
    SetActorEnableCollision(bTrainerVisible);
    if (!bTrainerVisible)
    {
        // These pawns are reused. Stopping the curve alone leaves its previous
        // shader value behind, so clear both before the next appearance.
        SetBodyColorFlash(nullptr, true);
        for (UMaterialInstanceDynamic* Material : BodyMIs)
        {
            if (Material) { Material->SetVectorParameterValue(TEXT("HitFlashColor"), FLinearColor::Transparent); }
        }
    }
}

void ANCAimTrainerTarget::ActivateTarget(const FVector& Location, bool bStrafe)
{
    if (Role != ROLE_Authority) { return; }
    bTrainerAirborne = false;
    bTrainerStrafe = bStrafe;
    bTrainerWiggle = false;
    StrafeRange = 800.f;
    StrafeCenter = Location;
    StrafeDirection = 1.f;
    // Restore posture at the new clear seat, not under a previous station's
    // cover. Uncrouch can move the capsule center; the final placement below
    // restores the exact standing anchor after that native collision change.
    if (bIsCrouched)
    {
        SetActorLocationAndRotation(Location, FRotator(0.f, 180.f, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
    }
    ResetTargetMovement();
    SetActorLocationAndRotation(Location, FRotator(0.f, 180.f, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
    if (bIsCrouched) { HideTarget(); return; }
    // Reappearing targets are a new opportunity, not a rewindable old body.
    SavedPositions.Reset();
    SavedCapsulePostures.Reset();
    SpawnProtectionStartTime = -1000.f;
    AppearanceTime = GetWorld()->GetTimeSeconds();
    bTrainerVisible = true;
    GetCharacterMovement()->SetMovementMode(bStrafe ? MOVE_Walking : MOVE_Flying);
    OnRep_TrainerVisible();
    ForceNetUpdate();
}

void ANCAimTrainerTarget::HideTarget()
{
    if (Role != ROLE_Authority) { return; }
    bTrainerVisible = false;
    bTrainerAirborne = false;
    bTrainerStrafe = false;
    bTrainerWiggle = false;
    ResetTargetMovement();
    GetCharacterMovement()->DisableMovement();
    SavedPositions.Reset();
    SavedCapsulePostures.Reset();
    OnRep_TrainerVisible();
    ForceNetUpdate();
}

void ANCAimTrainerTarget::ActivateAirborneTarget(const FVector& Location, const FVector& LaunchVelocity, float FlightRate)
{
    if (Role != ROLE_Authority || Location.ContainsNaN() || LaunchVelocity.ContainsNaN()
        || !FMath::IsFinite(FlightRate) || FlightRate <= 0.f || FlightRate > 1.f) { return; }
    ActivateTarget(Location, false);
    if (!bTrainerVisible) { return; }
    bTrainerAirborne = true;
    TrainerFlightRate = FlightRate;
    OnRep_TrainerFlightRate();
    // z_s(t) = z_1(s*t): scale the initial vertical speed by s and gravity by
    // s squared. The native arc reaches each height at exactly the chosen rate.
    FVector ScaledVelocity = LaunchVelocity;
    ScaledVelocity.Z *= TrainerFlightRate;
    LaunchAirborneTarget(ScaledVelocity);
}

void ANCAimTrainerTarget::OnRep_TrainerFlightRate()
{
    // GravityScale itself is not replicated by CharacterMovement. Simulated
    // proxies need this same scale while extrapolating replicated velocity.
    const ANCAimTrainerTarget* Defaults = GetClass()->GetDefaultObject<ANCAimTrainerTarget>();
    if (Defaults && Defaults->GetCharacterMovement())
    {
        GetCharacterMovement()->GravityScale = Defaults->GetCharacterMovement()->GravityScale
            * TrainerFlightRate * TrainerFlightRate;
    }
}

bool ANCAimTrainerTarget::LaunchAirborneTarget(const FVector& LaunchVelocity)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerAirborne || IsDead()
        || LaunchVelocity.ContainsNaN()) { return false; }
    // CharacterMovement owns the full ballistic arc and replicated landing.
    // Clear input and deferred impulses because these actors are pooled.
    GetCharacterMovement()->StopMovementImmediately();
    GetCharacterMovement()->PendingLaunchVelocity = FVector::ZeroVector;
    ConsumeMovementInputVector();
    GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    LaunchCharacter(LaunchVelocity, true, true);
    ForceNetUpdate();
    return true;
}

void ANCAimTrainerTarget::SetTrainerSpeedScale(float Scale)
{
    if (Role != ROLE_Authority || !FMath::IsFinite(Scale) || Scale <= 0.f) { return; }
    const ANCAimTrainerTarget* Defaults = GetClass()->GetDefaultObject<ANCAimTrainerTarget>();
    const UUTCharacterMovement* DefaultMove = Defaults ? Cast<UUTCharacterMovement>(Defaults->GetCharacterMovement()) : nullptr;
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (!DefaultMove || !Movement) { return; }
    Scale = FMath::Clamp(Scale, 0.1f, 2.f);
    // Keep short A/D reversals as fast as the top-speed increase. UT blends
    // its initial acceleration below MaxFastAccelSpeed and restores walking
    // braking from DefaultBrakingDecelerationWalking after each movement tick.
    Movement->MaxAcceleration = DefaultMove->MaxAcceleration * Scale;
    Movement->FastInitialAcceleration = DefaultMove->FastInitialAcceleration * Scale;
    Movement->MaxFastAccelSpeed = DefaultMove->MaxFastAccelSpeed * Scale;
    Movement->DodgeLandingAcceleration = DefaultMove->DodgeLandingAcceleration * Scale;
    Movement->MaxFallingAcceleration = DefaultMove->MaxFallingAcceleration * Scale;
    Movement->FloorSlideAcceleration = DefaultMove->FloorSlideAcceleration * Scale;
    Movement->DefaultBrakingDecelerationWalking = DefaultMove->DefaultBrakingDecelerationWalking * Scale;
    Movement->BrakingDecelerationWalking = DefaultMove->BrakingDecelerationWalking * Scale;
    Movement->MaxWalkSpeed = DefaultMove->MaxWalkSpeed * Scale;
    Movement->MaxWalkSpeedCrouched = DefaultMove->MaxWalkSpeedCrouched * Scale;
    Movement->DodgeImpulseHorizontal = DefaultMove->DodgeImpulseHorizontal * Scale;
    Movement->DodgeMaxHorizontalVelocity = DefaultMove->DodgeMaxHorizontalVelocity * Scale;
    Movement->MaxInitialFloorSlideSpeed = DefaultMove->MaxInitialFloorSlideSpeed * Scale;
    Movement->MaxFloorSlideSpeed = DefaultMove->MaxFloorSlideSpeed * Scale;
}

void ANCAimTrainerTarget::StartWiggle(float HalfWidth)
{
    if (Role != ROLE_Authority || !bTrainerVisible || bTrainerAirborne || !FMath::IsFinite(HalfWidth) || HalfWidth <= 0.f) { return; }
    bTrainerStrafe = true;
    bTrainerWiggle = true;
    StrafeRange = FMath::Clamp(HalfWidth, 20.f, NCAimTrainerLayout::InstagibStrafeRange);
    WiggleRange = StrafeRange;
    PopupLongStrafeEndTime = 0.f;
    GetCharacterMovement()->SetMovementMode(MOVE_Walking);
}

bool ANCAimTrainerTarget::StartPopupLongStrafe(float HalfWidth, float HoldSeconds, float DirectionRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerWiggle || IsDead()
        || bIsCrouched || IsTrainerSliding() || IsTrainerLongStrafing() || bRecenterWiggleAfterDodge
        || !GetCharacterMovement()->IsMovingOnGround()
        || !FMath::IsFinite(HalfWidth) || !FMath::IsFinite(HoldSeconds)
        || HalfWidth <= WiggleRange || HoldSeconds <= 0.f) { return false; }
    StrafeRange = FMath::Clamp(HalfWidth, WiggleRange, NCAimTrainerLayout::PopupLongStrafeRange);
    if (StrafeRange <= WiggleRange) { return false; }
    // Cross the lane from whichever side the wiggle reached. Near the center,
    // use a fresh random direction. Native acceleration and braking still apply.
    StrafeDirection = NCAimTrainerScenarioPolicy::PopupLongStrafeDirection(
        GetActorLocation().Y - StrafeCenter.Y, DirectionRoll);
    PopupLongStrafeEndTime = GetWorld()->GetTimeSeconds() + FMath::Clamp(HoldSeconds, 0.4f, 1.f);
    return true;
}

void ANCAimTrainerTarget::ResetTargetMovement()
{
    GetCharacterMovement()->StopMovementImmediately();
    GetCharacterMovement()->PendingLaunchVelocity = FVector::ZeroVector;
    SetTrainerSpeedScale(1.f);
    TrainerFlightRate = 1.f;
    OnRep_TrainerFlightRate();
    ConsumeMovementInputVector();
    if (UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement()))
    {
        Movement->ClearDodgeInput();
        Movement->ClearFloorSlideTap();
        Movement->ResetTimers();
        Movement->ClearFallingStateFlags();
        Movement->bWasFloorSliding = false;
        Movement->bIsDodgeLanding = false;
    }
    bPressedJump = false;
    bRepFloorSliding = false;
    TrainerSlideDirection = FVector::ZeroVector;
    StrafeAxis = FVector(0.f, 1.f, 0.f);
    PopupMoveMinimum = PopupMoveMaximum = PopupMoveDirection = FVector::ZeroVector;
    bPopupEvasion = bPopupNeedsDecision = false;
    PopupLongStrafeEndTime = 0.f;
    WiggleRange = 0.f;
    bRecenterWiggleAfterSlide = false;
    bRecenterWiggleAfterDodge = false;
    bTrainerDodgeSlidePending = false;
    SetTrainerCrouched(false);
}

bool ANCAimTrainerTarget::SetTrainerCrouched(bool bCrouch)
{
    if (Role != ROLE_Authority) { return false; }
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (!Movement || Movement->bIsFloorSliding
        || (bCrouch && (!bTrainerVisible || !bTrainerStrafe || IsDead() || IsTrainerLongStrafing() || bRecenterWiggleAfterDodge
            || !Movement->IsMovingOnGround())))
    {
        return false;
    }
    const bool bWasCrouched = bIsCrouched;
    Movement->bWantsToCrouch = bCrouch;
    // Direct movement posture skips the player crouch-to-slide gesture while
    // retaining native capsule adjustment, animation callbacks and replication.
    if (bCrouch && !bIsCrouched) { Movement->Crouch(false); }
    else if (!bCrouch && bIsCrouched) { Movement->UnCrouch(false); }
    if (bCrouch && !bIsCrouched) { Movement->bWantsToCrouch = false; }
    if (bWasCrouched != bIsCrouched) { ForceNetUpdate(); }
    return bIsCrouched == bCrouch;
}

void ANCAimTrainerTarget::ReverseStrafe()
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || IsTrainerSliding() || IsTrainerLongStrafing()
        || bRecenterWiggleAfterDodge || !GetCharacterMovement()->IsMovingOnGround()) { return; }
    const float Offset = (GetActorLocation() - StrafeCenter) | StrafeAxis;
    StrafeDirection = Offset >= StrafeRange ? -1.f : Offset <= -StrafeRange ? 1.f : -StrafeDirection;
}

void ANCAimTrainerTarget::ConfigurePopupStrafe(const FVector& Center, float HalfWidth, float DirectionRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || bTrainerAirborne
        || Center.ContainsNaN() || !FMath::IsFinite(HalfWidth) || HalfWidth <= 0.f) { return; }
    // Spawn offset and walking anchor are separate: varying the starting side
    // must not move the safe lane toward a wall. Preserve native speed/braking.
    StrafeCenter = Center;
    StrafeRange = FMath::Clamp(HalfWidth, 20.f, bTrainerWiggle ? NCAimTrainerLayout::InstagibStrafeRange : 800.f);
    if (bTrainerWiggle) { WiggleRange = StrafeRange; }
    StrafeDirection = DirectionRoll < 0.5f ? -1.f : 1.f;
}

bool ANCAimTrainerTarget::TryTrainerDodge(float DirectionRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || bTrainerWiggle || !GetCharacterMovement()->IsMovingOnGround()) { return false; }
    float Direction = NCAimTrainerScenarioPolicy::DodgeDirection((GetActorLocation() - StrafeCenter) | StrafeAxis, DirectionRoll);
    const FVector Cross(StrafeAxis.Y, -StrafeAxis.X, 0.f);
    if (StrafeAxis.X > 0.5f)
    {
        // The new lengthwise left lane needs its own full-path check. Include
        // retained perpendicular momentum and landing drift; choose the other
        // direction when necessary instead of dodging into cover or a wall.
        UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
        if (!Movement || !FMath::IsFinite(Movement->GetGravityZ()) || Movement->GetGravityZ() >= 0.f) { return false; }
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::PopupDodgerSeat(NCAimTrainerLayout::PopupLeftDodgerSlot);
        const FVector Start = GetActorLocation() - StrafeCenter + FVector(Seat.MinX, Seat.CenterY, 0.f);
        auto HasRoom = [&](float Sign)
        {
            const FVector Launch = Movement->DodgeImpulseHorizontal * StrafeAxis * Sign + (Movement->Velocity | Cross) * Cross;
            const float Speed = FMath::Min(Launch.Size2D(), Movement->DodgeMaxHorizontalVelocity) * MaxSpeedPctModifier;
            const float Time = -2.f * Movement->DodgeImpulseVertical / Movement->GetGravityZ() + 0.06f
                + Movement->DodgeLandingSpeedFactor * (Movement->DodgeResetInterval + 0.1f);
            if (!FMath::IsFinite(Speed) || !FMath::IsFinite(Time) || Speed <= 0.f || Time <= 0.f) { return false; }
            const FVector End = Start + Launch.GetSafeNormal2D() * Speed * Time;
            return NCAimTrainerLayout::CanPopupDodgePath(NCAimTrainerLayout::PopupLeftDodgerSlot,
                Start.X, Start.Y, End.X, End.Y, GetCapsuleComponent()->GetScaledCapsuleRadius(), 0.f);
        };
        if (!HasRoom(Direction)) { Direction = -Direction; }
        if (!HasRoom(Direction)) { return false; }
    }
    // Use UT's normal impulse, cooldown, landing and replicated movement event.
    // Never simulate a dodge by teleporting or assigning horizontal velocity.
    if (!Dodge(StrafeAxis * Direction, Cross)) { return false; }
    StrafeDirection = Direction;
    return true;
}

void ANCAimTrainerTarget::SetPopupStrafeAxis(const FVector& Axis)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || bTrainerWiggle || bTrainerAirborne
        || Axis.ContainsNaN() || Axis.Z != 0.f || Axis.IsNearlyZero()) { return; }
    StrafeAxis = Axis.GetSafeNormal2D();
}

bool ANCAimTrainerTarget::SetPopupMovement(const FVector& Minimum, const FVector& Maximum, const FVector& Direction)
{
    if (Role != ROLE_Authority || !bTrainerVisible || bTrainerAirborne || IsDead()
        || Minimum.ContainsNaN() || Maximum.ContainsNaN() || Direction.ContainsNaN()
        || Minimum.X >= Maximum.X || Minimum.Y >= Maximum.Y || Direction.Z != 0.f || Direction.IsNearlyZero()
        || IsTrainerSliding() || IsTrainerLongStrafing() || bRecenterWiggleAfterDodge
        || !GetCharacterMovement()->IsMovingOnGround()) { return false; }
    PopupMoveMinimum = Minimum;
    PopupMoveMaximum = Maximum;
    PopupMoveDirection = Direction.GetSafeNormal2D();
    bTrainerStrafe = true;
    bPopupEvasion = true;
    bPopupNeedsDecision = false;
    return true;
}

bool ANCAimTrainerTarget::NeedsPopupMovementDecision() const
{
    return bTrainerVisible && bPopupEvasion && bPopupNeedsDecision && !bRecenterWiggleAfterDodge
        && !IsTrainerSliding() && GetCharacterMovement()->IsMovingOnGround();
}

bool ANCAimTrainerTarget::TryTrainerPopupDodge(int32 Slot, const FVector& Direction, const FVector& ArenaOrigin, bool bSlideOnLanding)
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    const float HorizontalSizeSquared = Direction.X * Direction.X + Direction.Y * Direction.Y;
    if (Role != ROLE_Authority || !bTrainerVisible || (!bTrainerWiggle && !bPopupEvasion) || IsDead()
        || bIsCrouched || IsTrainerSliding() || IsTrainerLongStrafing() || bRecenterWiggleAfterDodge
        || !Movement || !Movement->IsMovingOnGround() || !Movement->CurrentFloor.IsWalkableFloor()
        || Movement->bIsDodging || Movement->bIsDodgeLanding
        || !FMath::IsFinite(Direction.X) || !FMath::IsFinite(Direction.Y) || !FMath::IsFinite(Direction.Z)
        || Direction.Z != 0.f || HorizontalSizeSquared < 0.999f || HorizontalSizeSquared > 1.001f
        || (bSlideOnLanding && Direction.X <= 0.f))
    {
        return false;
    }
    const FVector DodgeDirection = Direction.GetSafeNormal2D();
    const FVector DodgeCross(-DodgeDirection.Y, DodgeDirection.X, 0.f);
    // Native dodges retain perpendicular velocity. Predict that full impulse,
    // airtime and ending drift before choosing a lane, including the longer
    // distance when this appearance requests a native slide on landing.
    const float Gravity = -Movement->GetGravityZ();
    if (!FMath::IsFinite(Gravity) || Gravity <= 0.f || !FMath::IsFinite(MaxSpeedPctModifier)
        || MaxSpeedPctModifier <= 0.f || !FMath::IsFinite(Movement->DodgeImpulseVertical)
        || Movement->DodgeImpulseVertical <= 0.f) { return false; }
    FVector LaunchVelocity = Movement->DodgeImpulseHorizontal * DodgeDirection
        + (Movement->Velocity | DodgeCross) * DodgeCross;
    const float LaunchSpeed = FMath::Min(LaunchVelocity.Size2D(), Movement->DodgeMaxHorizontalVelocity) * MaxSpeedPctModifier;
    const FVector TravelDirection = LaunchVelocity.GetSafeNormal2D();
    const float FlightTime = 2.f * Movement->DodgeImpulseVertical / Gravity + 0.06f;
    float TravelDistance = LaunchSpeed * FlightTime;
    if (bSlideOnLanding)
    {
        const float SlideSpeed = FMath::Max(Movement->MaxFloorSlideSpeed,
            FMath::Min(LaunchSpeed, Movement->MaxInitialFloorSlideSpeed));
        TravelDistance += SlideSpeed * (Movement->FloorSlideDuration + 0.1f
            + Movement->FloorSlideEndingSpeedFactor * (Movement->DodgeResetInterval + 0.1f));
    }
    else
    {
        TravelDistance += LaunchSpeed * Movement->DodgeLandingSpeedFactor * (Movement->DodgeResetInterval + 0.1f);
    }
    const FVector Start = GetActorLocation() - ArenaOrigin;
    const FVector End = Start + TravelDirection * TravelDistance;
    const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    if (!FMath::IsFinite(Start.X) || !FMath::IsFinite(Start.Y) || !FMath::IsFinite(End.X) || !FMath::IsFinite(End.Y)
        || !FMath::IsFinite(TravelDistance) || TravelDistance <= 0.f
        || !NCAimTrainerLayout::CanPopupDodgePath(Slot, Start.X, Start.Y, End.X, End.Y, Radius, bPopupEvasion ? 0.f : WiggleRange))
    {
        return false;
    }
    if (!Dodge(DodgeDirection, DodgeCross)) { return false; }
    // Native UT owns impulse, airborne motion, cooldown and its movement event.
    // Choose fresh evasion input after landing instead of returning to an old
    // spawn anchor or countersteering the diagonal while it lands.
    ConsumeMovementInputVector();
    bRecenterWiggleAfterDodge = true;
    bPopupNeedsDecision = bPopupEvasion;
    bTrainerDodgeSlidePending = bSlideOnLanding;
    if (bSlideOnLanding)
    {
        TrainerSlideDirection = Movement->Velocity.GetSafeNormal2D();
        Movement->UpdateFloorSlide(true);
        AddMovementInput(TrainerSlideDirection, 1.f, true);
    }
    StrafeDirection = DodgeDirection.Y < 0.f ? -1.f : 1.f;
    ForceNetUpdate();
    return true;
}

bool ANCAimTrainerTarget::IsTrainerSliding() const
{
    const UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    return Movement && Movement->bIsFloorSliding;
}

bool ANCAimTrainerTarget::TryTrainerSlideForward()
{
    if (!bTrainerWiggle) { return false; }
    return StartTrainerSlide(FVector(-1.f, 0.f, 0.f));
}

bool ANCAimTrainerTarget::TryTrainerPopupSlide(int32 Slot, int32 Variant)
{
    if (!bTrainerWiggle) { return false; }
    if (Slot == 0 || Slot == NCAimTrainerLayout::PopupSliderSlot) { return TryTrainerSlideForward(); }
    if (Slot != 4 || (Variant != 0 && Variant != 1)
        || !StartTrainerSlide(FVector(0.f, Variant == 1 ? -1.f : 1.f, 0.f))) { return false; }
    // Each near-floor target slides into its open lateral lane. Retain the new
    // endpoint instead of walking all the way back to its original appearance.
    bRecenterWiggleAfterSlide = true;
    return true;
}

bool ANCAimTrainerTarget::TryTrainerTrackingSlide(float DirectionRoll)
{
    if (!bTrainerStrafe || bTrainerWiggle) { return false; }
    const ANCAimTrainerTarget* Defaults = GetClass()->GetDefaultObject<ANCAimTrainerTarget>();
    const UUTCharacterMovement* DefaultMove = Defaults ? Cast<UUTCharacterMovement>(Defaults->GetCharacterMovement()) : nullptr;
    const UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    // A 30% faster slide needs an earlier inward turn to remain inside the
    // fixed trainee's 1800-unit beam range, including native slide exit drift.
    const float TurnThreshold = DefaultMove && Movement && Movement->MaxFloorSlideSpeed > DefaultMove->MaxFloorSlideSpeed ? 300.f : 500.f;
    const float Offset = GetActorLocation().Y - StrafeCenter.Y;
    const float Direction = Offset >= TurnThreshold ? -1.f : Offset <= -TurnThreshold ? 1.f
        : NCAimTrainerScenarioPolicy::TrackingSlideDirection(Offset, DirectionRoll);
    if (!StartTrainerSlide(FVector(0.f, Direction, 0.f))) { return false; }
    StrafeDirection = Direction;
    return true;
}

bool ANCAimTrainerTarget::StartTrainerSlide(const FVector& Direction)
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    if (Role != ROLE_Authority || !bTrainerVisible || IsDead()
        || !Movement || !Movement->IsMovingOnGround() || !Movement->CurrentFloor.IsWalkableFloor()
        || IsTrainerLongStrafing() || bRecenterWiggleAfterDodge || !CanSlide() || !Movement->CanDodge()) { return false; }
    ConsumeMovementInputVector();
    Movement->bWantsToCrouch = false;
    // This invokes UT's real impulse, movement event, timing and slide posture.
    // A controllerless target has no saved-move flags to replicate the state.
    Movement->PerformFloorSlide(Direction, Movement->CurrentFloor.HitResult.ImpactNormal);
    if (!Movement->bIsFloorSliding) { return false; }
    TrainerSlideDirection = Direction;
    bRepFloorSliding = true;
    Movement->Crouch(false);
    AddMovementInput(TrainerSlideDirection, 1.f, true);
    ForceNetUpdate();
    return true;
}

void ANCAimTrainerTarget::Tick(float DeltaSeconds)
{
    if (Role == ROLE_Authority)
    {
        if (IsTrainerLongStrafing() && GetWorld()->GetTimeSeconds() >= PopupLongStrafeEndTime)
        {
            // Return with normal movement if the long hold reached outside the
            // short wiggle band. Never move the center or teleport back to it.
            PopupLongStrafeEndTime = 0.f;
            StrafeRange = WiggleRange;
        }
        // Stock CheckJumpInput retires this flag only on locally controlled
        // pawns. These targets have no controller or client saved moves.
        UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
        if (bTrainerDodgeSlidePending && Movement)
        {
            if (Movement->bIsFloorSliding)
            {
                // ProcessLanded has already applied the real UT floor-slide
                // impulse. Controllerless authority pawns need its posture and
                // replicated flag mirrored just like our explicit floor slides.
                bTrainerDodgeSlidePending = false;
                Movement->ClearFloorSlideTap();
                TrainerSlideDirection = Movement->Velocity.GetSafeNormal2D();
                bRecenterWiggleAfterSlide = true;
                bRepFloorSliding = true;
                Movement->Crouch(false);
                ForceNetUpdate();
            }
            else if (Movement->IsFalling())
            {
                // Native ProcessLanded requires movement intent to turn the
                // held slide input into a slide. Keep it along the actual dodge
                // velocity so ordinary wiggle decisions cannot redirect it.
                AddMovementInput(TrainerSlideDirection, 1.f, true);
            }
            else if (Movement->IsMovingOnGround() && !Movement->bIsDodging)
            {
                bTrainerDodgeSlidePending = false;
                Movement->ClearFloorSlideTap();
                TrainerSlideDirection = FVector::ZeroVector;
            }
        }
        if (Movement && Movement->bIsFloorSliding
            && Movement->GetCurrentMovementTime() >= Movement->FloorSlideEndTime)
        {
            // CheckJumpInput normally retires slides on the owning client.
            // Retain bWasFloorSliding so UT applies its normal ending slowdown.
            Movement->bIsFloorSliding = false;
            Movement->ClearFloorSlideTap();
            bRepFloorSliding = false;
            StrafeCenter.X = GetActorLocation().X;
            if (bRecenterWiggleAfterSlide) { StrafeCenter.Y = GetActorLocation().Y; }
            bRecenterWiggleAfterSlide = false;
            TrainerSlideDirection = FVector::ZeroVector;
            ConsumeMovementInputVector();
            SetTrainerCrouched(false);
            UpdateCrouchedEyeHeight();
            ForceNetUpdate();
        }
        if (Movement && Movement->bIsDodgeLanding
            && Movement->GetCurrentMovementTime() >= Movement->DodgeResetTime + Movement->DodgeLandingTimeAdjust)
        {
            Movement->bIsDodgeLanding = false;
        }
        if (bRecenterWiggleAfterDodge && Movement && Movement->IsMovingOnGround()
            && !Movement->bIsDodging && !Movement->bIsDodgeLanding
            && Movement->GetCurrentMovementTime() >= Movement->DodgeResetTime)
        {
            StrafeCenter.X = GetActorLocation().X;
            StrafeCenter.Y = GetActorLocation().Y;
            bRecenterWiggleAfterDodge = false;
            ConsumeMovementInputVector();
        }
    }
    if (Role == ROLE_Authority && bTrainerVisible && bTrainerStrafe && (!bRecenterWiggleAfterDodge || IsTrainerSliding())
        && GetCharacterMovement()->IsMovingOnGround())
    {
        if (IsTrainerSliding())
        {
            // Preserve the direction chosen at slide start. Ordinary strafe
            // input must not countersteer either a forward or a lateral slide.
            AddMovementInput(TrainerSlideDirection, 1.f, true);
        }
        else if (bPopupEvasion)
        {
            // Apply the chosen two-dimensional input continuously. Decisions
            // can cut a run short; reaching a fixed point never makes it stop.
            // Brake inward before an edge using the actual velocity, including
            // momentum retained after a diagonal dodge or slide.
            if (!bPopupNeedsDecision)
            {
                const FVector Center = (PopupMoveMinimum + PopupMoveMaximum) * 0.5f;
                const FVector HalfSize = (PopupMoveMaximum - PopupMoveMinimum) * 0.5f;
                const FVector Offset = GetActorLocation() - Center;
                const FVector Velocity = GetVelocity();
                const float Brake = 0.5f * GetCharacterMovement()->MaxAcceleration;
                PopupMoveDirection.X = NCAimTrainerScenarioPolicy::BoundedStrafeDirection(Offset.X, Velocity.X,
                    Brake, HalfSize.X, PopupMoveDirection.X, DeltaSeconds);
                PopupMoveDirection.Y = NCAimTrainerScenarioPolicy::BoundedStrafeDirection(Offset.Y, Velocity.Y,
                    Brake, HalfSize.Y, PopupMoveDirection.Y, DeltaSeconds);
                PopupMoveDirection = PopupMoveDirection.GetSafeNormal2D();
                AddMovementInput(PopupMoveDirection, 1.f, true);
            }
        }
        else
        {
            const float Offset = (GetActorLocation() - StrafeCenter) | StrafeAxis;
            // Drive A/D input with the real gameplay acceleration/speed. Turn
            // early enough to brake inside the seat instead of slowing the pawn.
            StrafeDirection = NCAimTrainerScenarioPolicy::BoundedStrafeDirection(Offset,
                GetVelocity() | StrafeAxis, GetCharacterMovement()->MaxAcceleration,
                StrafeRange, StrafeDirection, DeltaSeconds);
            AddMovementInput(StrafeAxis * StrafeDirection, 1.f, true);
        }
    }
    Super::Tick(DeltaSeconds);
    if (GetNetMode() != NM_DedicatedServer && GetWorld()->GetTimeSeconds() >= NextTrainerTintTime)
    {
        UpdateTrainerTint();
        NextTrainerTintTime = GetWorld()->GetTimeSeconds() + 0.25f;
    }
}

FVector ANCAimTrainerTarget::GetHeadLocation(float PredictionTime)
{
    // These fixed-model targets use their visible head pose, including offline
    // where there is no client head-offset claim. Native sniper validation
    // reads the shared HeadRadius, including the HS-only preset adjustment.
    return AUTCharacter::GetHeadLocation(PredictionTime);
}

void ANCAimTrainerTarget::SetTrainerHeadshotScale(float Scale)
{
    if (Role != ROLE_Authority || !FMath::IsFinite(Scale) || Scale < 1.f || Scale > 1.15f) { return; }
    TrainerHeadshotScale = Scale;
    OnRep_TrainerHeadshotScale();
    ForceNetUpdate();
}

void ANCAimTrainerTarget::OnRep_TrainerHeadshotScale()
{
    // NCP's client-claimed head validation reads HeadRadius directly, while
    // unclaimed hits use IsHeadShot. Set their common native geometry on both
    // ends; HeadScale remains untouched so the visible head never grows.
    const ANCAimTrainerTarget* Defaults = GetClass()->GetDefaultObject<ANCAimTrainerTarget>();
    if (Defaults) { HeadRadius = Defaults->HeadRadius * TrainerHeadshotScale; }
}

void ANCAimTrainerTarget::NotifyBlockedHeadShot(AUTCharacter* /*ShotInstigator*/)
{
    // Training targets stay alive after scoring. That must not be interpreted
    // by the sniper as a helmet surviving a headshot. Accepted hits have their
    // own configured NCP confirmation; these targets never have head armor.
}

float ANCAimTrainerTarget::TakeDamage(float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer)
{
    ANCAimTrainerGame* Game = GetWorld() ? Cast<ANCAimTrainerGame>(GetWorld()->GetAuthGameMode()) : nullptr;
    const float AcceptedDamage = Game && bTrainerVisible ? Game->RecordTargetHit(this, Damage, Event, Instigator, Causer) : 0.f;
    if (AcceptedDamage > 0.f && bTrainerVisible)
    {
        // Tracking targets survive beam contact. Keep UT's damage-type body
        // flash and native LastTakeHitInfo replication without taking health,
        // applying knockback, or entering ordinary frag/scoring paths.
        const int32 HitDamage = FMath::RoundToInt(FMath::Clamp(AcceptedDamage, 1.f, 255.f));
        SetLastTakeHitInfo(HitDamage, HitDamage, FVector::ZeroVector, nullptr, Event);
        // The stock helper infers overhealth from post-damage Health. Ours was
        // never reduced. Its cosmetic event is skipped on dedicated servers,
        // so also clear that inferred armor marker before replication here.
        LastTakeHitInfo.HitArmor = nullptr;
        ForceNetUpdate();
    }
    return AcceptedDamage;
}

void ANCAimTrainerTarget::PlayTakeHitEffects_Implementation()
{
    // Training targets never have armor or overhealth. Use the actual weapon
    // damage type's body effect on standalone and replicated remote hits.
    LastTakeHitInfo.HitArmor = nullptr;
    if (bTrainerVisible) { Super::PlayTakeHitEffects_Implementation(); }
}

ANCAimTrainerArena::ANCAimTrainerArena(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    bReplicates = true;
    bAlwaysRelevant = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("ArenaRoot"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> Grid(TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
    BlockMesh = Cube.Object;
    BlockMaterial = Grid.Object;
    AddBlock(TEXT("Floor"), FVector(0, 0, -50), FVector(6400, 3600, 100));
    AddBlock(TEXT("Ceiling"), FVector(0, 0, 2050), FVector(6400, 3600, 100));
    AddBlock(TEXT("BackWall"), FVector(-3250, 0, 1000), FVector(100, 3600, 2000));
    AddBlock(TEXT("FarWall"), FVector(3250, 0, 1000), FVector(100, 3600, 2000));
    AddBlock(TEXT("LeftWall"), FVector(0, -1850, 1000), FVector(6400, 100, 2000));
    AddBlock(TEXT("RightWall"), FVector(0, 1850, 1000), FVector(6400, 100, 2000));
    // Cover is in front of a real full-size pawn; the normal weapon trace must
    // clear it before the native sniper head test can award a point.
    for (int32 Index = 0; Index < NCAimTrainerLayout::HeadSlotCount; ++Index)
    {
        const NCAimTrainerLayout::FBlock Block = NCAimTrainerLayout::HeadCover(Index);
        Cover.Add(AddBlock(FName(*FString::Printf(TEXT("HeadCover%d"), Index)),
            FVector(Block.CenterX, Block.CenterY, Block.Height * 0.5f), FVector(Block.SizeX, Block.SizeY, Block.Height)));
    }
    for (int32 Index = 0; Index < NCAimTrainerLayout::PopupPlatformCount; ++Index)
    {
        const NCAimTrainerLayout::FBlock Block = NCAimTrainerLayout::PopupPlatform(Index);
        Cover.Add(AddBlock(FName(*FString::Printf(TEXT("PopupPlatform%d"), Index)),
            FVector(Block.CenterX, Block.CenterY, Block.Height * 0.5f), FVector(Block.SizeX, Block.SizeY, Block.Height)));
    }
    const NCAimTrainerLayout::FBlock Ledge = NCAimTrainerLayout::AirborneFiringLedge();
    AirbornePlatforms.Add(AddBlock(TEXT("AirborneFiringLedge"),
        FVector(Ledge.CenterX, Ledge.CenterY, Ledge.Height * 0.5f), FVector(Ledge.SizeX, Ledge.SizeY, Ledge.Height)));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> JumpPadMesh(
        TEXT("/Game/RestrictedAssets/Blueprints/JumpPad/JumpPadMesh.JumpPadMesh"));
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const NCAimTrainerLayout::FBlock Pad = NCAimTrainerLayout::AirborneJumpPad(Index);
        AirbornePlatforms.Add(AddBlock(FName(*FString::Printf(TEXT("AirbornePadPlatform%d"), Index)),
            FVector(Pad.CenterX, Pad.CenterY, Pad.Height * 0.5f), FVector(Pad.SizeX, Pad.SizeY, Pad.Height)));
        UStaticMeshComponent* PadVisual = CreateDefaultSubobject<UStaticMeshComponent>(
            FName(*FString::Printf(TEXT("AirborneJumpPad%d"), Index)));
        PadVisual->SetupAttachment(RootComponent);
        PadVisual->SetRelativeLocation(FVector(Pad.CenterX, Pad.CenterY, Pad.Height + 2.f));
        PadVisual->SetStaticMesh(JumpPadMesh.Object);
        PadVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        AirbornePadVisuals.Add(PadVisual);
    }
    GooSurface = AddBlock(TEXT("AirborneGooSurface"), FVector(0.f, 0.f, NCAimTrainerLayout::AirborneHazardZ - 5.f), FVector(6400.f, 3600.f, 10.f));
    // The game retires airborne targets at this shared height without routing
    // expiry through combat damage. Cosmetic material loading waits for play.
    GooSurface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    for (int32 Index = 0; Index < 3; ++Index)
    {
        UPointLightComponent* Light = CreateDefaultSubobject<UPointLightComponent>(FName(*FString::Printf(TEXT("TrainingLight%d"), Index)));
        Light->SetupAttachment(RootComponent);
        Light->SetRelativeLocation(FVector((Index - 1) * 2000.f, 0.f, 1450.f));
        Light->SetMobility(EComponentMobility::Movable);
        Light->Intensity = 15000.f;
        Light->AttenuationRadius = 4500.f;
        Light->CastShadows = false;
    }
}

UStaticMeshComponent* ANCAimTrainerArena::AddBlock(FName Name, const FVector& Center, const FVector& Size)
{
    UStaticMeshComponent* Block = CreateDefaultSubobject<UStaticMeshComponent>(Name);
    Block->SetupAttachment(RootComponent);
    Block->SetRelativeLocation(Center);
    Block->SetRelativeScale3D(Size / 100.f);
    Block->SetStaticMesh(BlockMesh);
    if (BlockMaterial) { Block->SetMaterial(0, BlockMaterial); }
    Block->SetCollisionProfileName(TEXT("BlockAll"));
    return Block;
}

void ANCAimTrainerArena::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode() != NM_DedicatedServer)
    {
        // DM-DeckTest's goo, verified in the retail cook. Keep it optional and
        // out of CDO initialization: a stripped cosmetic asset must not open
        // a default-property error dialog during game startup.
        UMaterialInterface* GooMaterial = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Game/RestrictedAssets/Environments/Materials/SlimePit.SlimePit"),
            nullptr, LOAD_NoWarn | LOAD_Quiet);
        UStaticMesh* GooMesh = LoadObject<UStaticMesh>(nullptr,
            TEXT("/Game/RestrictedAssets/Environments/ShellResources/Meshes/Generic/SM_Sheet_500.SM_Sheet_500"),
            nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (GooMaterial && GooMesh)
        {
            // SlimePit is translucent/two-sided. Use DeckTest's single sheet
            // instead of overlapping top/bottom cube faces through the goo.
            const FBoxSphereBounds Bounds = GooMesh->GetBounds();
            if (Bounds.BoxExtent.X > 0.f && Bounds.BoxExtent.Y > 0.f)
            {
                const FVector Scale(6400.f / (2.f * Bounds.BoxExtent.X), 3600.f / (2.f * Bounds.BoxExtent.Y), 1.f);
                GooSurface->SetStaticMesh(GooMesh);
                GooSurface->SetRelativeScale3D(Scale);
                GooSurface->SetRelativeLocation(FVector(-Bounds.Origin.X * Scale.X, -Bounds.Origin.Y * Scale.Y,
                    NCAimTrainerLayout::AirborneHazardZ - Bounds.Origin.Z));
                GooSurface->SetMaterial(0, GooMaterial);
            }
        }
    }
    OnRep_Scenario();
}

bool ANCAimTrainerArena::HasArenaAssets() const { return BlockMesh && BlockMaterial; }

void ANCAimTrainerArena::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ANCAimTrainerArena, Scenario);
}

void ANCAimTrainerArena::SetScenario(uint8 NewScenario)
{
    if (Role != ROLE_Authority) { return; }
    Scenario = NewScenario;
    OnRep_Scenario();
    ForceNetUpdate();
}

void ANCAimTrainerArena::OnRep_Scenario()
{
    for (int32 Index = 0; Index < Cover.Num(); ++Index)
    {
        UStaticMeshComponent* Block = Cover[Index];
        const bool bEnabled = Index < NCAimTrainerLayout::HeadSlotCount ? Scenario == 1 : Scenario == 2;
        Block->SetHiddenInGame(!bEnabled);
        Block->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
    const bool bAirborne = Scenario == 3 || Scenario == 4;
    const bool bRockets = Scenario == 4;
    for (int32 Index = 0; Index < AirbornePlatforms.Num(); ++Index)
    {
        const NCAimTrainerLayout::FBlock Geometry = Index == 0
            ? NCAimTrainerLayout::AirborneFiringLedge(bRockets)
            : NCAimTrainerLayout::AirborneJumpPad(Index - 1, bRockets);
        UStaticMeshComponent* Block = AirbornePlatforms[Index];
        Block->SetRelativeLocation(FVector(Geometry.CenterX, Geometry.CenterY, Geometry.Height * 0.5f));
        Block->SetRelativeScale3D(FVector(Geometry.SizeX, Geometry.SizeY, Geometry.Height) / 100.f);
        Block->SetHiddenInGame(!bAirborne);
        Block->SetCollisionEnabled(bAirborne ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
    for (int32 Index = 0; Index < AirbornePadVisuals.Num(); ++Index)
    {
        const NCAimTrainerLayout::FBlock Pad = NCAimTrainerLayout::AirborneJumpPad(Index, bRockets);
        UStaticMeshComponent* Visual = AirbornePadVisuals[Index];
        Visual->SetRelativeLocation(FVector(Pad.CenterX, Pad.CenterY, Pad.Height + 2.f));
        Visual->SetRelativeScale3D(FVector(bRockets ? 0.5f : 1.f, bRockets ? 0.5f : 1.f, 1.f));
        Visual->SetHiddenInGame(!bAirborne);
    }
    if (UStaticMesh* GooMesh = GooSurface->GetStaticMesh())
    {
        // Anchor the actual top of either the fallback cube or DeckTest sheet.
        // Deriving it from the mesh also works when replication precedes play,
        // and switching scenarios never accumulates a previous height offset.
        const FBoxSphereBounds Bounds = GooMesh->GetBounds();
        const FTransform Transform = GooSurface->GetRelativeTransform();
        FVector Location = Transform.GetLocation();
        Location.Z = NCAimTrainerLayout::AirborneHazardHeight(bRockets)
            - (Bounds.Origin.Z + Bounds.BoxExtent.Z) * Transform.GetScale3D().Z;
        GooSurface->SetRelativeLocation(Location);
    }
    GooSurface->SetHiddenInGame(!bAirborne);
}
