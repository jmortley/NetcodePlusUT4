#include "NCAimTrainerTarget.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLayout.h"
#include "NCAimTrainerCharacterProfile.h"
#include "UTCharacterMovement.h"
#include "UTCharacterContent.h"
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

void ANCAimTrainerTarget::BeginPlay()
{
    Super::BeginPlay();
    OnRep_TrainerVisible();
}

bool ANCAimTrainerTarget::HasCharacterAssets() const
{
    return CharacterData && GetMesh() && GetMesh()->SkeletalMesh && GetMesh()->AnimClass;
}

void ANCAimTrainerTarget::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ANCAimTrainerTarget, bTrainerVisible);
}

void ANCAimTrainerTarget::OnRep_TrainerVisible()
{
    SetActorHiddenInGame(!bTrainerVisible);
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
    bTrainerStrafe = false;
    bTrainerWiggle = false;
    ResetTargetMovement();
    GetCharacterMovement()->DisableMovement();
    SavedPositions.Reset();
    SavedCapsulePostures.Reset();
    OnRep_TrainerVisible();
    ForceNetUpdate();
}

void ANCAimTrainerTarget::StartWiggle(float HalfWidth)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !FMath::IsFinite(HalfWidth) || HalfWidth <= 0.f) { return; }
    bTrainerStrafe = true;
    bTrainerWiggle = true;
    StrafeRange = FMath::Clamp(HalfWidth, 20.f, 140.f);
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
    PopupLongStrafeEndTime = GetWorld()->GetTimeSeconds() + FMath::Clamp(HoldSeconds, 0.4f, 0.8f);
    return true;
}

void ANCAimTrainerTarget::ResetTargetMovement()
{
    GetCharacterMovement()->StopMovementImmediately();
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
    const float Offset = GetActorLocation().Y - StrafeCenter.Y;
    StrafeDirection = Offset >= StrafeRange ? -1.f : Offset <= -StrafeRange ? 1.f : -StrafeDirection;
}

bool ANCAimTrainerTarget::TryTrainerDodge(float DirectionRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || bTrainerWiggle || !GetCharacterMovement()->IsMovingOnGround()) { return false; }
    const float Direction = NCAimTrainerScenarioPolicy::DodgeDirection(GetActorLocation().Y - StrafeCenter.Y, DirectionRoll);
    // Use UT's normal impulse, cooldown, landing and replicated movement event.
    // Never simulate a dodge by teleporting or assigning horizontal velocity.
    if (!Dodge(FVector(0.f, Direction, 0.f), FVector(1.f, 0.f, 0.f))) { return false; }
    StrafeDirection = Direction;
    return true;
}

bool ANCAimTrainerTarget::TryTrainerPopupDodge(int32 Slot, const FVector& Direction, const FVector& ArenaOrigin, bool bSlideOnLanding)
{
    UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
    const float HorizontalSizeSquared = Direction.X * Direction.X + Direction.Y * Direction.Y;
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerWiggle || IsDead()
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
        || !NCAimTrainerLayout::CanPopupDodgePath(Slot, Start.X, Start.Y, End.X, End.Y, Radius, WiggleRange))
    {
        return false;
    }
    if (!Dodge(DodgeDirection, DodgeCross)) { return false; }
    // Native UT owns impulse, airborne motion, cooldown and its movement event.
    // Resume short strafes at the landing position instead of rushing back to
    // the old spawn anchor or countersteering the diagonal while it lands.
    ConsumeMovementInputVector();
    bRecenterWiggleAfterDodge = true;
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
    const float Direction = NCAimTrainerScenarioPolicy::TrackingSlideDirection(
        GetActorLocation().Y - StrafeCenter.Y, DirectionRoll);
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
        else
        {
            const float Offset = GetActorLocation().Y - StrafeCenter.Y;
            // Drive A/D input with the real gameplay acceleration/speed. Turn
            // early enough to brake inside the seat instead of slowing the pawn.
            StrafeDirection = NCAimTrainerScenarioPolicy::BoundedStrafeDirection(Offset,
                GetVelocity().Y, GetCharacterMovement()->MaxAcceleration,
                StrafeRange, StrafeDirection, DeltaSeconds);
            AddMovementInput(FVector(0.f, StrafeDirection, 0.f), 1.f, true);
        }
    }
    Super::Tick(DeltaSeconds);
}

FVector ANCAimTrainerTarget::GetHeadLocation(float PredictionTime)
{
    // These fixed-model targets use their visible head pose, including offline
    // where there is no client head-offset claim. Keep NCP's normal sniper
    // radius and obstruction tests; only the trainer's head center changes.
    return AUTCharacter::GetHeadLocation(PredictionTime);
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
        const bool bEnabled = Index < NCAimTrainerLayout::HeadSlotCount ? Scenario == 1 : (Scenario == 2 || Scenario == 3);
        Block->SetHiddenInGame(!bEnabled);
        Block->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
}
