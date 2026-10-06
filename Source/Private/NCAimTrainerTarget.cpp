#include "NCAimTrainerTarget.h"
#include "NCAimTrainerGame.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "UTCharacterMovement.h"
#include "UTCharacterContent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
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
    GetCharacterMovement()->MaxWalkSpeed = 500.f;
    GetCharacterMovement()->MaxAcceleration = 7000.f;
    // Head position and animation must update even on a dedicated server.
    GetMesh()->MeshComponentUpdateFlag = EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones;
    GetMesh()->bEnableUpdateRateOptimizations = false;
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
}

void ANCAimTrainerTarget::ActivateTarget(const FVector& Location, bool bStrafe)
{
    if (Role != ROLE_Authority) { return; }
    bTrainerStrafe = bStrafe;
    StrafeCenter = Location;
    StrafeDirection = 1.f;
    ResetTargetMovement();
    SetActorLocationAndRotation(Location, FRotator(0.f, 180.f, 0.f), false, nullptr, ETeleportType::TeleportPhysics);
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
    ResetTargetMovement();
    GetCharacterMovement()->DisableMovement();
    SavedPositions.Reset();
    SavedCapsulePostures.Reset();
    OnRep_TrainerVisible();
    ForceNetUpdate();
}

void ANCAimTrainerTarget::ResetTargetMovement()
{
    GetCharacterMovement()->StopMovementImmediately();
    ConsumeMovementInputVector();
    if (UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement()))
    {
        Movement->ClearDodgeInput();
        Movement->ClearFallingStateFlags();
        Movement->bIsDodgeLanding = false;
        Movement->DodgeResetTime = 0.f;
    }
}

void ANCAimTrainerTarget::ReverseStrafe()
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || !GetCharacterMovement()->IsMovingOnGround()) { return; }
    const float Offset = GetActorLocation().Y - StrafeCenter.Y;
    StrafeDirection = Offset >= 800.f ? -1.f : Offset <= -800.f ? 1.f : -StrafeDirection;
}

bool ANCAimTrainerTarget::TryTrainerDodge(float DirectionRoll)
{
    if (Role != ROLE_Authority || !bTrainerVisible || !bTrainerStrafe || !GetCharacterMovement()->IsMovingOnGround()) { return false; }
    const float Direction = NCAimTrainerScenarioPolicy::DodgeDirection(GetActorLocation().Y - StrafeCenter.Y, DirectionRoll);
    // Use UT's normal impulse, cooldown, landing and replicated movement event.
    // Never simulate a dodge by teleporting or assigning horizontal velocity.
    if (!Dodge(FVector(0.f, Direction, 0.f), FVector(1.f, 0.f, 0.f))) { return false; }
    StrafeDirection = Direction;
    return true;
}

void ANCAimTrainerTarget::Tick(float DeltaSeconds)
{
    if (Role == ROLE_Authority)
    {
        // Stock CheckJumpInput retires this flag only on locally controlled
        // pawns. These targets have no controller or client saved moves.
        UUTCharacterMovement* Movement = Cast<UUTCharacterMovement>(GetCharacterMovement());
        if (Movement && Movement->bIsDodgeLanding
            && Movement->GetCurrentMovementTime() >= Movement->DodgeResetTime + Movement->DodgeLandingTimeAdjust)
        {
            Movement->bIsDodgeLanding = false;
        }
    }
    if (Role == ROLE_Authority && bTrainerVisible && bTrainerStrafe && GetCharacterMovement()->IsMovingOnGround())
    {
        const float Offset = GetActorLocation().Y - StrafeCenter.Y;
        if (Offset >= 800.f) { StrafeDirection = -1.f; }
        else if (Offset <= -800.f) { StrafeDirection = 1.f; }
        AddMovementInput(FVector(0.f, StrafeDirection, 0.f), 1.f, true);
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
    return Game && bTrainerVisible ? Game->RecordTargetHit(this, Damage, Event, Instigator, Causer) : 0.f;
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
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Cover.Add(AddBlock(FName(*FString::Printf(TEXT("HeadCover%d"), Index)),
            FVector(600.f, (Index - 1) * 650.f, 88.f), FVector(180.f, 460.f, 176.f)));
    }
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const float Height = FMath::Max(1.f, Index * 160.f);
        Cover.Add(AddBlock(FName(*FString::Printf(TEXT("PopupPlatform%d"), Index)),
            FVector(1100.f, (Index - 1) * 850.f, Height * 0.5f), FVector(2600.f, 580.f, Height)));
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
        const bool bEnabled = Index < 3 ? Scenario == 1 : Scenario == 2;
        Block->SetHiddenInGame(!bEnabled);
        Block->SetCollisionEnabled(bEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
}
