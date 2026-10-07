#include "NCAimTrainerGame.h"
#include "NCAimTrainerTarget.h"
#include "NCAimTrainerHUD.h"
#include "NCAimTrainerOnline.h"
#include "NCAimTrainerScoring.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLayout.h"
#include "NCAimTrainerCharacter.h"
#include "TeamArenaCharacter.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTWeap_LinkGun_NCP.h"
#include "UTWeaponStateFiringLinkBeam_NCP.h"
#include "UTCharacterMovement.h"
#include "UTCharacterContent.h"
#include "UTPlayerState.h"
#include "UTPickup.h"
#include "UTDroppedPickup.h"
#include "UTGameSession.h"
#include "GameFramework/WorldSettings.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimInstance.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"

ANCAimTrainerGame::ANCAimTrainerGame(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DisplayName = NSLOCTEXT("UTGameMode", "NCAimTrainer", "NetcodePlus Aim Trainer");
    PlayerControllerClass = ANCAimTrainerPlayerController::StaticClass();
    HUDClass = ANCAimTrainerHUD::StaticClass();
    DefaultPawnClass = ANCAimTrainerCharacter::StaticClass();
    PlayerPawnObject = ANCAimTrainerCharacter::StaticClass();
    PrimaryActorTick.bCanEverTick = true;
    DefaultMaxPlayers = 1;
    BotFillCount = 0;
    GoalScore = 0;
    TimeLimit = 0;
    bDelayedStart = false;
    bRequireReady = false;
    bRequireFull = false;
    bRemovePawnsAtStart = false;
    bPlayersStartWithArmor = false;
    bAmmoIsLimited = false;
    DefaultInventory.Empty();
    // Distributed in NCWepMut's pak, which may mount after native class CDOs
    // are created. Resolve only when this opt-in mode configures a trainee.
    // No asset lookup or warning is emitted while loading other game modes.
    SniperClass = nullptr;
    LightningClass = nullptr;
    InstagibClass = nullptr;
    LinkClass = nullptr;
    NextTargetTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    TargetExpiry.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextWiggleTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextCrouchTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    CrouchEndTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
}

void ANCAimTrainerGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    // UTBaseGameMode reloads DefaultPawnClass from PlayerPawnObject during
    // InitGame. A constructor-only DefaultPawnClass assignment is overwritten
    // by UT's inherited DefaultCharacter asset, including in standalone.
    PlayerPawnObject = ANCAimTrainerCharacter::StaticClass();
    Super::InitGame(MapName, Options, ErrorMessage);
    // Preserve the user's global PawnClassOverride setting. UT may save config
    // during initialization; enforce this mode's pawn without editing that setting.
    DefaultPawnClass = ANCAimTrainerCharacter::StaticClass();
    // URL/ruleset options cannot turn a ranked preset into a different test.
    DefaultMaxPlayers = 1;
    if (GameSession) { GameSession->MaxPlayers = 1; }
    BotFillCount = 0;
    GoalScore = TimeLimit = 0;
    bRequireReady = bRequireFull = bDelayedStart = false;
    bRemovePawnsAtStart = false;
    bPlayersStartWithArmor = false;
    DefaultInventory.Empty();
}

UClass* ANCAimTrainerGame::GetDefaultPawnClassForController_Implementation(AController* /*InController*/)
{
    // The opt-in trainer requires this pawn's movement component on every
    // spawn/restart. Ruleset or mutator pawn defaults cannot substitute it.
    return Progress.Scenario == 2 ? ANCAimTrainerInstagibCharacter::StaticClass() : ANCAimTrainerCharacter::StaticClass();
}

bool ANCAimTrainerGame::ReadyToStartMatch_Implementation()
{
    // UT defers a first-frame start until the next tick. Keep that guard:
    // starting synchronously can equip a weapon before its attachment's
    // BeginPlay has initialized UTOwner, which AttachToOwner requires.
    return NumPlayers > 0 && Super::ReadyToStartMatch_Implementation() && GetWorld()->HasBegunPlay();
}
bool ANCAimTrainerGame::CheckScore_Implementation(AUTPlayerState*) { return false; }
bool ANCAimTrainerGame::AllowPausing(APlayerController*) { return false; }

bool ANCAimTrainerGame::CheckRelevance_Implementation(AActor* Other)
{
    if (Other && (Other->IsA<AUTPickup>() || Other->IsA<AUTDroppedPickup>())) { return false; }
    return Super::CheckRelevance_Implementation(Other);
}

void ANCAimTrainerGame::BeginPlay()
{
    Super::BeginPlay();
    EnsureArena();
    KillBots();
    for (TActorIterator<AUTPickup> It(GetWorld()); It; ++It) { It->Destroy(); }
}

bool ANCAimTrainerGame::FailSetup(const TCHAR* Message)
{
    SetupError = Message;
    UE_LOG(LogTemp, Warning, TEXT("NCP Aim Trainer setup: %s"), Message);
    return false;
}

bool ANCAimTrainerGame::EnsureArena()
{
    if (Arena && Arena->IsPendingKillPending()) { Arena = nullptr; }
    UClass* TargetClass = Progress.Scenario == 2
        ? ANCAimTrainerInstagibTarget::StaticClass() : ANCAimTrainerTarget::StaticClass();
    // Replace pooled actors outside a run so native crouch and skin restoration
    // always use the correct class default, including on remote clients.
    for (ANCAimTrainerTarget* Target : Targets)
    {
        if (Target && !Target->IsPendingKillPending() && Target->GetClass() != TargetClass) { Target->Destroy(); }
    }
    Targets.RemoveAll([](ANCAimTrainerTarget* Target) { return !Target || Target->IsPendingKillPending(); });
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (!Arena)
    {
        Arena = GetWorld()->SpawnActor<ANCAimTrainerArena>(ArenaOrigin, FRotator::ZeroRotator, Params);
    }
    if (!Arena) { return FailSetup(TEXT("Cannot start: the practice room could not spawn.")); }
    if (!Arena->HasArenaAssets()) { return FailSetup(TEXT("Cannot start: the practice room mesh or material is missing from this installation.")); }
    while (Targets.Num() < NCAimTrainerLayout::TargetCount)
    {
        const NCAimTrainerLayout::FSeat Seat = Targets.Num() < NCAimTrainerLayout::HeadSlotCount
            ? NCAimTrainerLayout::HeadSeat(Targets.Num()) : NCAimTrainerLayout::PopupDodgerSeat();
        ANCAimTrainerTarget* Target = GetWorld()->SpawnActor<ANCAimTrainerTarget>(
            TargetClass, ArenaOrigin + FVector(Seat.MinX, Seat.CenterY,
                TargetClass->GetDefaultObject<ANCAimTrainerTarget>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + Seat.FloorZ),
            FRotator(0, 180, 0), Params);
        if (!Target) { return FailSetup(TEXT("Cannot start: a practice character could not spawn.")); }
        if (!Target->HasCharacterAssets())
        {
            const USkeletalMeshComponent* Mesh = Target->GetMesh();
            UE_LOG(LogTemp, Warning, TEXT("NCP Aim Trainer target assets: character=%s mesh=%s animation=%s"),
                *GetNameSafe(*Target->CharacterData), *GetNameSafe(Mesh ? Mesh->SkeletalMesh : nullptr),
                *GetNameSafe(Mesh ? *Mesh->AnimClass : nullptr));
            Target->Destroy();
            return FailSetup(TEXT("Cannot start: the practice character mesh or animation is unavailable. See the game log for the missing asset."));
        }
        Target->HideTarget();
        Targets.Add(Target);
    }
    return true;
}

bool ANCAimTrainerGame::IsTrainee(const ANCAimTrainerPlayerController* PC) const
{
    return PC && PC == Trainee && !PC->IsPendingKillPending() && PC->PlayerState && !PC->PlayerState->bOnlySpectator;
}

void ANCAimTrainerGame::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);
    ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(NewPlayer);
    if (!PC || !PC->PlayerState || PC->PlayerState->bOnlySpectator) { return; }
    if (Trainee && Trainee != PC)
    {
        PC->PlayerState->bOnlySpectator = true;
        if (PC->GetPawn()) { PC->GetPawn()->Destroy(); }
        PC->ChangeState(NAME_Spectating);
        PC->SetTrainerOnlineStatus(TEXT("The practice lane is occupied. Join as a spectator."));
        return;
    }
    Trainee = PC;
    if (!PC->GetPawn()) { RestartPlayer(PC); }
    else { ConfigurePawn(); }
    PublishProgress();
    RefreshLeaderboard();
}

void ANCAimTrainerGame::Logout(AController* Exiting)
{
    if (Exiting == Trainee)
    {
        HideAllTargets();
        RunWeapon = nullptr;
        Trainee = nullptr;
        Progress = FNCAimTrainerProgress();
        bRankedRun = false;
    }
    Super::Logout(Exiting);
}

void ANCAimTrainerGame::RestartPlayer(AController* Player)
{
    // PostLogin also reaches this path while a standalone map is initializing.
    // The normal match-start restart will retry after world BeginPlay.
    if (!GetWorld()->HasBegunPlay()) { return; }
    if (!Player || !Player->PlayerState || Player->PlayerState->bOnlySpectator) { return; }
    if (Trainee && Player != Trainee) { return; }
    // Stock restart establishes possession, PlayerState and inventory setup.
    // Move the resulting pawn to the same fixed lane on every map.
    Super::RestartPlayer(Player);
    if (APawn* Pawn = Player->GetPawn())
    {
        Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(-1800.f, 0.f, Pawn->GetSimpleCollisionHalfHeight()), FRotator::ZeroRotator,
            false, nullptr, ETeleportType::TeleportPhysics);
        Player->SetControlRotation(FRotator::ZeroRotator);
        Player->ClientSetRotation(FRotator::ZeroRotator, true);
        if (Player == Trainee)
        {
            // Deferred starts no longer have a pawn during PostLogin. Equip
            // the scenario here once possession and attachment setup are safe.
            ConfigurePawn();
            PublishProgress();
        }
    }
}

void ANCAimTrainerGame::SetPlayerDefaults(APawn* Pawn)
{
    Super::SetPlayerDefaults(Pawn);
    if (AUTCharacter* Character = Cast<AUTCharacter>(Pawn))
    {
        Character->Health = 100;
        Character->SetArmorAmount(nullptr, 0);
        Character->bCanBeDamaged = false;
        Character->GetCharacterMovement()->DisableMovement();
    }
}

bool ANCAimTrainerGame::ConfigurePawn()
{
    // This also protects scenario selection and existing-pawn login paths.
    if (!GetWorld()->HasBegunPlay()) { return FailSetup(TEXT("Practice is still initializing. Try starting again in a moment.")); }
    AUTCharacter* Pawn = Trainee ? Cast<AUTCharacter>(Trainee->GetPawn()) : nullptr;
    if (!Pawn && Trainee)
    {
        // A failed profile replacement must remain retryable from the menu.
        Super::RestartPlayer(Trainee);
        Pawn = Cast<AUTCharacter>(Trainee->GetPawn());
    }
    if (!Pawn || Pawn->IsDead()) { return FailSetup(TEXT("Cannot start: your practice character is not ready.")); }
    UClass* PawnClass = GetDefaultPawnClassForController_Implementation(Trainee);
    if (Pawn->GetClass() != PawnClass)
    {
        // Scenario changes are admitted only outside active/countdown phases.
        // A new class is replicated normally instead of patching live capsule
        // defaults that UnCrouch would later undo on either side.
        Trainee->UnPossess();
        Pawn->Destroy();
        RunWeapon = nullptr;
        Super::RestartPlayer(Trainee);
        Pawn = Cast<AUTCharacter>(Trainee->GetPawn());
        if (!Pawn || Pawn->GetClass() != PawnClass) { return FailSetup(TEXT("Cannot start: the selected practice character could not spawn.")); }
    }
    const float StandingHeight = PawnClass->GetDefaultObject<AUTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(-1800.f, 0.f, StandingHeight), FRotator::ZeroRotator,
        false, nullptr, ETeleportType::TeleportPhysics);
    UNCAimTrainerMovement* Movement = Cast<UNCAimTrainerMovement>(Pawn->GetCharacterMovement());
    if (!Movement)
    {
        UE_LOG(LogTemp, Warning, TEXT("NCP Aim Trainer pawn mismatch: pawn=%s movement=%s expected=%s"),
            *GetNameSafe(Pawn->GetClass()), *GetNameSafe(Pawn->GetCharacterMovement() ? Pawn->GetCharacterMovement()->GetClass() : nullptr),
            *GetNameSafe(UNCAimTrainerMovement::StaticClass()));
        return FailSetup(TEXT("Cannot start: the trainee does not have the required practice movement component."));
    }
    Movement->ResetTrainerMovement(Progress.bMovementPractice);
    // Uncrouching can raise the capsule center. Reset after restoring posture
    // so a fixed run always starts from the original standing anchor.
    Pawn->SetActorLocation(ArenaOrigin + FVector(-1800.f, 0.f, StandingHeight), false, nullptr, ETeleportType::TeleportPhysics);
    // Stock restart uses the map PlayerStart's view direction when replacing
    // the scenario's pawn. Rotating the pawn alone does not reset mouse-look.
    // Anchor both authority and the owning client's camera to the practice lane.
    Trainee->SetControlRotation(FRotator::ZeroRotator);
    Trainee->ClientSetRotation(FRotator::ZeroRotator, true);
    Pawn->bCanBeDamaged = false;
    Pawn->DiscardAllInventory();
    RunWeapon = nullptr;
    // Exact shipped NCP classes only. Missing precision content blocks the
    // run instead of quietly switching to stock hit registration. A failed
    // lookup is retried on the next start, so a later pak mount can recover.
    if (Progress.Scenario == 0 && !LinkClass)
    {
        LinkClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/NCPLinkGun.NCPLinkGun_C"), nullptr, LOAD_NoWarn);
    }
    else if (Progress.Scenario == 2 && !InstagibClass)
    {
        InstagibClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/N+InstagibRifle.N+InstagibRifle_C"), nullptr, LOAD_NoWarn);
    }
    else if (Progress.Scenario == 1 && Progress.bUseLightningGun && !LightningClass)
    {
        LightningClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPLightningGun.UTNPLightningGun_C"), nullptr, LOAD_NoWarn);
    }
    else if (Progress.Scenario == 1 && !Progress.bUseLightningGun && !SniperClass)
    {
        SniperClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPSniper.UTNPSniper_C"), nullptr, LOAD_NoWarn);
    }
    TSubclassOf<AUTWeapon> DesiredClass = Progress.Scenario == 0 ? LinkClass : Progress.Scenario == 2 ? InstagibClass
        : Progress.bUseLightningGun ? LightningClass : SniperClass;
    if (!DesiredClass || DesiredClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FailSetup(Progress.Scenario == 0
            ? TEXT("Cannot start: the NCP Link Gun is unavailable. Install the current NCWepMut content pak.")
            : TEXT("Cannot start: the selected NCP rifle is unavailable. Install the NCWepMut content pak."));
    }
    if ((Progress.Scenario == 0 && !DesiredClass->IsChildOf(AUTWeap_LinkGun_NCP::StaticClass()))
        || (Progress.Scenario == 2 && !DesiredClass->IsChildOf(AUTPlusShockRifle::StaticClass()))
        || (Progress.Scenario == 1 && !DesiredClass->IsChildOf(AUTPlusSniper::StaticClass())))
    {
        return FailSetup(TEXT("Cannot start: the selected rifle does not use the required NetcodePlus weapon class."));
    }
    RunWeapon = Cast<AUTWeapon>(Pawn->CreateInventory(DesiredClass));
    if (RunWeapon)
    {
        RunWeapon->Ammo = RunWeapon->MaxAmmo;
        Pawn->SwitchWeapon(RunWeapon);
        if (Progress.Scenario == 2)
        {
            const AUTPlusShockRifle* Rifle = Cast<AUTPlusShockRifle>(RunWeapon);
            if (!Rifle || !Rifle->HasSharedInstagibFireModes())
            {
                return FailSetup(TEXT("Cannot start: the instagib rifle's two fire modes do not match the training preset."));
            }
        }
    }
    if (!RunWeapon) { return FailSetup(TEXT("Cannot start: the selected NCP rifle could not be equipped.")); }
    if (Progress.Scenario == 0)
    {
        const AUTWeap_LinkGun_NCP* Link = Cast<AUTWeap_LinkGun_NCP>(RunWeapon);
        const float BeamRefire = RunWeapon->GetRefireTime(1);
        if (!Link || !Link->InstantHitInfo.IsValidIndex(1) || !Link->FiringState.IsValidIndex(1)
            || !Link->FiringState[1] || !Link->FiringState[1]->IsA(UUTWeaponStateFiringLinkBeam_NCP::StaticClass())
            || Link->InstantHitInfo[1].Damage <= 0 || !Link->InstantHitInfo[1].DamageType
            || !FMath::IsFinite(BeamRefire) || BeamRefire <= 0.f
            || !FMath::IsFinite(Link->InstantHitInfo[1].TraceRange) || Link->InstantHitInfo[1].TraceRange < 1800.f)
        {
            return FailSetup(TEXT("Cannot start: the Link Gun content lacks the required NCP beam state or range. Update the NCWepMut content pak."));
        }
        return true;
    }
    if (RunWeapon->ShotsStatsName == NAME_None) { return FailSetup(TEXT("Cannot start: the selected rifle has no shot counter for scoring.")); }
    return true;
}

bool ANCAimTrainerGame::IsInsidePracticeLane(const AUTCharacter* Pawn) const
{
    if (!Pawn || Pawn->IsDead()) { return false; }
    const FVector Position = Pawn->GetActorLocation() - ArenaOrigin;
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z)) { return false; }
    const float StandingHeight = Pawn->GetClass()->GetDefaultObject<AUTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (!Progress.bMovementPractice) { return (Position - FVector(-1800.f, 0.f, StandingHeight)).SizeSquared() <= 4.f; }
    // Capsule bounds accommodate standing, crouching, jumps and wall dodges.
    // The room walls stop lateral travel; the plane fixes target distance.
    const float HalfHeight = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    return FMath::IsFinite(HalfHeight) && HalfHeight > 0.f
        && FMath::Abs(Position.X + 1800.f) <= 5.f && FMath::Abs(Position.Y) <= 1800.f
        && Position.Z - HalfHeight >= -10.f && Position.Z + HalfHeight <= 2010.f;
}

void ANCAimTrainerGame::SelectScenario(ANCAimTrainerPlayerController* PC, uint8 Scenario, bool bUseLightningGun)
{
    if (!IsTrainee(PC) || Scenario > 2 || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    SetupError.Empty();
    const bool bMovementPractice = Progress.bMovementPractice;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bMovementPractice;
    Progress.bUseLightningGun = bUseLightningGun;
    HideAllTargets();
    if (Arena) { Arena->SetScenario(Scenario); }
    if (!EnsureArena() || !ConfigurePawn()) { PC->SetTrainerOnlineStatus(SetupError); }
    PublishProgress();
    RefreshLeaderboard();
}

void ANCAimTrainerGame::SetMovementPractice(ANCAimTrainerPlayerController* PC, bool bEnabled)
{
    if (!IsTrainee(PC) || Progress.Phase == 1 || Progress.Phase == 2 || Progress.bMovementPractice == bEnabled) { return; }
    const uint8 Scenario = Progress.Scenario;
    const bool bUseLightningGun = Progress.bUseLightningGun;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bEnabled;
    Progress.bUseLightningGun = bUseLightningGun;
    bRankedRun = false;
    RunId.Empty();
    SetupError.Empty();
    HideAllTargets();
    const bool bReady = ConfigurePawn();
    PublishProgress();
    PC->SetTrainerOnlineStatus(!bReady ? SetupError : (bEnabled
        ? TEXT("Movement practice: strafe, dodge, jump and crouch. Results stay local and are not submitted.")
        : TEXT("Fixed-position practice selected. Choose a scenario and start a run.")));
}

void ANCAimTrainerGame::StartTraining(ANCAimTrainerPlayerController* PC, bool bUseLightningGun)
{
    if (!IsTrainee(PC) || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    Progress.bUseLightningGun = bUseLightningGun;
    SetupError.Empty();
    if (!EnsureArena() || !ConfigurePawn())
    {
        PC->SetTrainerOnlineStatus(SetupError);
        return;
    }
    const uint8 Scenario = Progress.Scenario;
    const bool bMovementPractice = Progress.bMovementPractice;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bMovementPractice;
    Progress.Phase = 1;
    Progress.bUseLightningGun = bUseLightningGun;
    Progress.RemainingSeconds = 3.f;
    PhaseStartedAt = GetWorld()->GetTimeSeconds();
    TrackedSeconds = 0.0;
    FiredSeconds = 0.0;
    bPreviousContact = false;
    bPreviousFiring = false;
    RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Schedule.Initialize(int32(GetTypeHash(RunId)));
    bRankedRun = !Progress.bMovementPractice && GetNetMode() != NM_Standalone && BaseMutator == nullptr && FMath::IsNearlyEqual(GetWorldSettings()->GetEffectiveTimeDilation(), 1.f)
        && GetClass() == StaticClass();
    UnrankedReason = bRankedRun ? FString() : (Progress.bMovementPractice
        ? TEXT("Movement practice: scores are shown here but are not submitted to the shared leaderboard.")
        : (GetNetMode() == NM_Standalone
        ? TEXT("Offline practice: scores are shown here but are not submitted to the shared leaderboard.")
        : TEXT("Practice only: mutators or altered game speed change the preset.")));
    HideAllTargets();
    Arena->SetScenario(Scenario);
    PublishProgress();
    PC->SetTrainerOnlineStatus(bRankedRun ? TEXT("Complete all 60 seconds to submit to UT4Stats.") : UnrankedReason);
}

void ANCAimTrainerGame::AbortTraining(ANCAimTrainerPlayerController* PC)
{
    if (!IsTrainee(PC)) { return; }
    HideAllTargets();
    const uint8 Scenario = Progress.Scenario;
    const bool bMovementPractice = Progress.bMovementPractice;
    const bool bUseLightningGun = Progress.bUseLightningGun;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bMovementPractice;
    Progress.bUseLightningGun = bUseLightningGun;
    bRankedRun = false;
    RunId.Empty();
    if (RunWeapon) { RunWeapon->StopFire(0); RunWeapon->StopFire(1); }
    PublishProgress();
    PC->SetTrainerOnlineStatus(TEXT("Choose a scenario. Incomplete runs are not submitted."));
}

void ANCAimTrainerGame::BeginActiveRun()
{
    Progress.Phase = 2;
    Progress.RemainingSeconds = 60.f;
    PhaseStartedAt = LastTraceTime = GetWorld()->GetTimeSeconds();
    NextDirectionTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::StrafeHoldSeconds(Schedule.FRand(), Schedule.FRand());
    NextDodgeTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::DodgeDelaySeconds(Schedule.FRand());
    NextTrackingSlideTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(Schedule.FRand());
    NextPopupTime = PhaseStartedAt;
    NextPopupSlideTime = 0.f;
    const float Refire = Progress.Scenario == 2 && RunWeapon ? RunWeapon->GetRefireTime(0) : 1.f;
    PopupRefireSeconds = FMath::IsFinite(Refire) ? FMath::Max(1.f, Refire) : 1.f;
    if (Progress.Scenario == 2 && (!FMath::IsFinite(Refire) || !FMath::IsNearlyEqual(Refire, 1.f)))
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: the instagib rifle's refire interval differs from the one-second preset.");
    }
    AUTPlayerState* PS = Trainee ? Cast<AUTPlayerState>(Trainee->PlayerState) : nullptr;
    ShotStatBaseline = RunWeapon ? RunWeapon->GetWeaponShotsStats(PS) : 0.f;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        NextTargetTime[Index] = PhaseStartedAt + (Progress.Scenario == 2 ? 0.f : Index * 0.25f);
        TargetExpiry[Index] = 0.f;
        NextWiggleTime[Index] = PhaseStartedAt;
        NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
    }
    if (Progress.Scenario == 2) { UpdatePopupDodger(PhaseStartedAt); }
    PublishProgress();
}

void ANCAimTrainerGame::HideAllTargets()
{
    NextPopupSlideTime = 0.f;
    NextTrackingSlideTime = 0.f;
    for (int32 Index = 0; Index < NextCrouchTime.Num(); ++Index)
    {
        NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
    }
    for (ANCAimTrainerTarget* Target : Targets)
    {
        if (Target && !Target->IsPendingKillPending()) { Target->HideTarget(); }
    }
}

void ANCAimTrainerGame::ActivateSlot(int32 Index, float Now)
{
    if (!Targets.IsValidIndex(Index)) { return; }
    // Current capsules may still be crouched from an earlier appearance. The
    // class default gives the standing seat before ActivateTarget resets it.
    const float StandingHeight = Targets[Index]->GetClass()->GetDefaultObject<ANCAimTrainerTarget>()
        ->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const bool bPopupDodger = Progress.Scenario == 2 && Index == NCAimTrainerLayout::PopupDodgerSlot;
    FVector Position;
    if (Progress.Scenario == 0)
    {
        if (Index != 0) { return; }
        // Stay inside the actual Link beam range across the target's strafe
        // and dodge lane. The old sniper display target was 2500 units away.
        Position = FVector(-800.f, 0.f, StandingHeight);
        TargetExpiry[Index] = PhaseStartedAt + 60.f;
    }
    else if (Progress.Scenario == 1)
    {
        if (Index >= NCAimTrainerLayout::HeadSlotCount) { return; }
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::HeadSeat(Index);
        Position = FVector(Seat.MinX, Seat.CenterY, StandingHeight + Seat.FloorZ);
        TargetExpiry[Index] = Now + 6.5f;
    }
    else if (bPopupDodger)
    {
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::PopupDodgerSeat();
        Position = FVector(Seat.MinX, Seat.CenterY, StandingHeight + Seat.FloorZ);
        TargetExpiry[Index] = PhaseStartedAt + 60.f;
        NextDodgeTime = Now + NCAimTrainerScenarioPolicy::PopupFirstDodgeDelaySeconds(Schedule.FRand());
        NextDirectionTime = Now + NCAimTrainerScenarioPolicy::StrafeHoldSeconds(Schedule.FRand(), Schedule.FRand());
    }
    else
    {
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::PopupSeat(Index);
        Position = FVector(Schedule.FRandRange(Seat.MinX, Seat.MaxX),
            Seat.CenterY + Schedule.FRandRange(-Seat.SpawnJitterY, Seat.SpawnJitterY), StandingHeight + Seat.FloorZ);
        TargetExpiry[Index] = Now + NCAimTrainerScenarioPolicy::PopupExposure(PopupRefireSeconds, Schedule.FRand());
    }
    Targets[Index]->ActivateTarget(ArenaOrigin + Position, Progress.Scenario == 0 || bPopupDodger);
    NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
    if (Progress.Scenario == 0)
    {
        NextCrouchTime[Index] = Now + NCAimTrainerScenarioPolicy::TrackingCrouchDelaySeconds(Schedule.FRand());
    }
    if (Progress.Scenario != 0 && !bPopupDodger)
    {
        const NCAimTrainerLayout::FSeat Seat = Progress.Scenario == 1
            ? NCAimTrainerLayout::HeadSeat(Index) : NCAimTrainerLayout::PopupSeat(Index);
        Targets[Index]->StartWiggle(Seat.WiggleRange);
        NextWiggleTime[Index] = Now + NCAimTrainerScenarioPolicy::WiggleHoldSeconds(Schedule.FRand());
        if (Progress.Scenario == 2 && Index == NCAimTrainerLayout::PopupSliderSlot)
        {
            // The high platform target slides toward the trainee once per
            // appearance, with no separate crouch competing for its posture.
            NextPopupSlideTime = Now + NCAimTrainerScenarioPolicy::PopupSlideDelaySeconds(Schedule.FRand());
        }
        else if (Progress.Scenario == 2 && NCAimTrainerScenarioPolicy::ShouldCrouch(Schedule.FRand()))
        {
            NextCrouchTime[Index] = Now + NCAimTrainerScenarioPolicy::CrouchDelaySeconds(Schedule.FRand());
        }
    }
}

void ANCAimTrainerGame::UpdateShotCount()
{
    if (Progress.Scenario == 0) { return; }
    AUTPlayerState* PS = Trainee ? Cast<AUTPlayerState>(Trainee->PlayerState) : nullptr;
    const float RawShots = RunWeapon ? RunWeapon->GetWeaponShotsStats(PS) - ShotStatBaseline : -1.f;
    if (!FMath::IsFinite(RawShots) || RawShots < 0.f || RawShots > 200.f)
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: weapon shot accounting changed during the run.");
        return;
    }
    Progress.Shots = FMath::RoundToInt(RawShots);
    Progress.Score = Progress.Scenario == 1
        ? NCAimTrainerScoring::HeadshotScore(Progress.Hits, Progress.Shots)
        : NCAimTrainerScoring::PrecisionScore(Progress.Hits, Progress.Shots, Progress.TargetsExpired);
    Progress.Accuracy = Progress.Shots > 0 ? 100.f * Progress.Hits / Progress.Shots : 0.f;
}

float ANCAimTrainerGame::RecordTargetHit(ANCAimTrainerTarget* Target, float Damage,
    const FDamageEvent& Event, AController* Instigator, AActor* Causer)
{
    if (Progress.Phase != 2 || !IsTrainee(Trainee) || Instigator != Trainee
        || !RunWeapon || Causer != RunWeapon || !FMath::IsFinite(Damage) || Damage <= 0.f) { return 0.f; }
    const int32 Slot = Targets.IndexOfByKey(Target);
    const float Now = GetWorld()->GetTimeSeconds();
    if (Slot == INDEX_NONE || !Target->IsAvailable() || Now >= PhaseStartedAt + 60.f || Now >= TargetExpiry[Slot]) { return 0.f; }
    const AUTWeaponFix* FixedWeapon = Cast<AUTWeaponFix>(RunWeapon);
    const float Rewind = FixedWeapon ? FixedWeapon->GetHitValidationPredictionTime() : 0.f;
    if (!FMath::IsFinite(Rewind) || Rewind < 0.f || Now - Rewind < Target->GetAppearanceTime()) { return 0.f; }
    if (Progress.Scenario == 0)
    {
        AUTWeap_LinkGun_NCP* Link = Cast<AUTWeap_LinkGun_NCP>(RunWeapon);
        if (Slot != 0 || !Link || !Link->IsFiring() || Link->GetCurrentFireMode() != 1
            || !Link->InstantHitInfo.IsValidIndex(1) || Event.DamageTypeClass != Link->InstantHitInfo[1].DamageType) { return 0.f; }
        // Beam contact supplies time-based credit in Tick. Keep the target
        // alive and avoid precision counters. The native beam already batches
        // damage; confirm every accepted batch just like ordinary NCP combat.
        Trainee->NotifyTrainerHit(Damage);
        return Damage;
    }
    // A late request from a previous appearance must not score the reused pawn,
    // even when the next headshot target occupies the same physical station.
    if (Progress.Scenario == 1)
    {
        const AUTPlusSniper* Sniper = Cast<AUTPlusSniper>(RunWeapon);
        if (!Sniper || !Sniper->HeadshotDamageType || Event.DamageTypeClass != Sniper->HeadshotDamageType) { return 0.f; }
        ++Progress.Headshots;
    }
    ++Progress.Hits;
    Target->HideTarget();
    NextTargetTime[Slot] = Now + (Progress.Scenario == 2 ? 0.f : 0.35f);
    Trainee->NotifyTrainerHit(Damage);
    // Keep the pawn alive and bypass ordinary frag/drop/scoring paths. One
    // appearance can score exactly once, even if a repeated RPC reaches it.
    return Damage;
}

void ANCAimTrainerGame::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!IsTrainee(Trainee)) { return; }
    const float Now = GetWorld()->GetTimeSeconds();
    if (Progress.Phase == 1)
    {
        Progress.RemainingSeconds = FMath::Max(0.f, 3.f - (Now - PhaseStartedAt));
        if (Progress.RemainingSeconds <= 0.f) { BeginActiveRun(); }
    }
    else if (Progress.Phase == 2)
    {
        AUTCharacter* Pawn = Cast<AUTCharacter>(Trainee->GetPawn());
        if (!IsInsidePracticeLane(Pawn))
        {
            AbortTraining(Trainee);
            Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because the trainee left the practice lane."));
            return;
        }
        if (!FMath::IsNearlyEqual(GetWorldSettings()->GetEffectiveTimeDilation(), 1.f) || DeltaSeconds > 0.25f)
        {
            bRankedRun = false;
            UnrankedReason = TEXT("Practice only: changed game speed or a long server stall interrupted this run.");
        }
        Progress.RemainingSeconds = FMath::Max(0.f, 60.f - (Now - PhaseStartedAt));
        UpdateShotCount();
        if (Progress.RemainingSeconds <= 0.f) { FinishRun(); return; }
        if (Targets.Num() != NCAimTrainerLayout::TargetCount || !Arena || Arena->IsPendingKillPending())
        {
            AbortTraining(Trainee);
            Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because the practice arena was removed."));
            return;
        }
        for (ANCAimTrainerTarget* Target : Targets)
        {
            if (!Target || Target->IsPendingKillPending())
            {
                AbortTraining(Trainee);
                Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because a practice target was removed."));
                return;
            }
        }
        UpdateTargets(Now);
        if (Progress.Scenario == 0)
        {
            UpdateTrackingMovement(Now);
            if (Now - LastTraceTime >= 1.f / 30.f)
            {
                UpdateTrackingSample(Now);
            }
        }
        if (RunWeapon) { RunWeapon->Ammo = RunWeapon->MaxAmmo; }
    }
    if (Now >= NextStatusTime) { PublishProgress(); NextStatusTime = Now + 0.1f; }
}

void ANCAimTrainerGame::UpdateTrackingMovement(float Now)
{
    if (Progress.Phase != 2 || Progress.Scenario != 0 || Now >= PhaseStartedAt + 60.f
        || !Targets.IsValidIndex(0) || !Targets[0] || !Targets[0]->IsAvailable()) { return; }
    if (CrouchEndTime[0] > 0.f && Now >= CrouchEndTime[0])
    {
        // Clearance may defer standing. Keep the crouch active until native
        // uncrouching succeeds, then wait a fresh interval before the next dip.
        if (Targets[0]->SetTrainerCrouched(false))
        {
            CrouchEndTime[0] = 0.f;
            NextCrouchTime[0] = Now + NCAimTrainerScenarioPolicy::TrackingCrouchDelaySeconds(Schedule.FRand());
        }
    }
    else if (CrouchEndTime[0] == 0.f && NextCrouchTime[0] > 0.f && Now >= NextCrouchTime[0])
    {
        NextCrouchTime[0] = 0.f;
        const float Hold = NCAimTrainerScenarioPolicy::TrackingCrouchHoldSeconds(Schedule.FRand());
        if (PhaseStartedAt + 60.f - Now >= Hold + 0.1f)
        {
            if (Targets[0]->SetTrainerCrouched(true)) { CrouchEndTime[0] = Now + Hold; }
            else { NextCrouchTime[0] = Now + 0.2f; } // Wait for a dodge/slide to finish.
        }
    }
    // A/D input continues at native crouched speed; don't replace this brief
    // posture change with a simultaneous slide or dodge.
    if (CrouchEndTime[0] == 0.f && Now >= NextTrackingSlideTime)
    {
        // Let the native slide finish before the run ends. Grounding and UT's
        // shared dodge/slide cooldown decide when an occasional slide can start.
        if (PhaseStartedAt + 60.f - Now >= 1.f)
        {
            const bool bSlid = Targets[0]->TryTrainerTrackingSlide(Schedule.FRand());
            NextTrackingSlideTime = Now + (bSlid ? NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(Schedule.FRand()) : 0.2f);
        }
        else { NextTrackingSlideTime = PhaseStartedAt + 60.f; }
    }
    if (CrouchEndTime[0] == 0.f && Now >= NextDodgeTime)
    {
        Targets[0]->TryTrainerDodge(Schedule.FRand());
        NextDodgeTime = Now + NCAimTrainerScenarioPolicy::DodgeDelaySeconds(Schedule.FRand());
    }
    if (Now >= NextDirectionTime)
    {
        Targets[0]->ReverseStrafe();
        NextDirectionTime = Now + NCAimTrainerScenarioPolicy::StrafeHoldSeconds(Schedule.FRand(), Schedule.FRand());
    }
}

void ANCAimTrainerGame::UpdateTargets(float Now)
{
    if (Progress.Scenario == 2) { UpdatePopupDodger(Now); }
    const int32 ActiveSlots = Progress.Scenario == 0 ? 1 : Progress.Scenario == 1
        ? NCAimTrainerLayout::HeadSlotCount : NCAimTrainerLayout::PopupSlotCount;
    TArray<int32> EligibleSlots;
    for (int32 Index = 0; Index < ActiveSlots; ++Index)
    {
        if (Targets[Index]->IsAvailable() && Now >= TargetExpiry[Index])
        {
            Targets[Index]->HideTarget();
            ++Progress.TargetsExpired;
            NextTargetTime[Index] = Now + (Progress.Scenario == 2 ? 0.f : Schedule.FRandRange(0.25f, 0.65f));
        }
        if (!Targets[Index]->IsAvailable() && Now >= NextTargetTime[Index])
        {
            if (Progress.Scenario == 2) { EligibleSlots.Add(Index); }
            else { ActivateSlot(Index, Now); }
        }
        if (Progress.Scenario != 0 && Targets[Index]->IsAvailable() && Now >= NextWiggleTime[Index])
        {
            Targets[Index]->ReverseStrafe();
            NextWiggleTime[Index] = Now + NCAimTrainerScenarioPolicy::WiggleHoldSeconds(Schedule.FRand());
        }
        if (Progress.Scenario == 2 && Targets[Index]->IsAvailable())
        {
            if (Index == NCAimTrainerLayout::PopupSliderSlot && NextPopupSlideTime > 0.f && Now >= NextPopupSlideTime)
            {
                NextPopupSlideTime = 0.f; // One attempt per appearance, never a catch-up burst.
                // Allow the native 0.7-second slide, a posture transition, and
                // a full rifle refire interval before either expiry or run end.
                const float RequiredTime = 1.f + PopupRefireSeconds;
                if (TargetExpiry[Index] - Now >= RequiredTime && PhaseStartedAt + 60.f - Now >= RequiredTime)
                {
                    Targets[Index]->TryTrainerSlideForward();
                }
            }
            if (CrouchEndTime[Index] > 0.f && Now >= CrouchEndTime[Index])
            {
                // Native clearance can temporarily prevent standing. Retry
                // without leaving a queued crouch request behind.
                if (Targets[Index]->SetTrainerCrouched(false)) { CrouchEndTime[Index] = 0.f; }
            }
            else if (NextCrouchTime[Index] > 0.f && Now >= NextCrouchTime[Index])
            {
                NextCrouchTime[Index] = 0.f; // At most one brief dip per appearance.
                const float Hold = NCAimTrainerScenarioPolicy::CrouchHoldSeconds(Schedule.FRand());
                // Keep a full rifle refire interval to shoot after it stands,
                // especially where the rear target dips completely behind cover.
                if (TargetExpiry[Index] - Now >= Hold + PopupRefireSeconds + 0.1f
                    && Targets[Index]->SetTrainerCrouched(true))
                {
                    CrouchEndTime[Index] = Now + Hold;
                }
            }
        }
    }
    // Initial targets, hits and expiries all share this deadline. Never replace
    // several targets at once or catch up after a stall: one rifle, one second
    // per shot. Randomize which available height/lane gets the next appearance.
    if (Progress.Scenario == 2 && Now >= NextPopupTime && EligibleSlots.Num() > 0)
    {
        ActivateSlot(EligibleSlots[Schedule.RandRange(0, EligibleSlots.Num() - 1)], Now);
        NextPopupTime = Now + NCAimTrainerScenarioPolicy::PopupSpawnDelay(PopupRefireSeconds, Schedule.FRand());
    }
}

void ANCAimTrainerGame::UpdatePopupDodger(float Now)
{
    if (Progress.Phase != 2 || Progress.Scenario != 2 || Now >= PhaseStartedAt + 60.f) { return; }
    const int32 Slot = NCAimTrainerLayout::PopupDodgerSlot;
    if (!Targets.IsValidIndex(Slot) || !Targets[Slot]) { return; }
    // This sixth target has a clear floor lane and never times out. Refill on
    // the next game tick after a hit, independently of the five popup seats.
    if (!Targets[Slot]->IsAvailable()) { ActivateSlot(Slot, Now); }
    if (!Targets[Slot]->IsAvailable()) { return; }
    if (Now >= NextDodgeTime)
    {
        const bool bDodged = Targets[Slot]->TryTrainerDodge(Schedule.FRand());
        // A landing or native cooldown can reject an attempt. Retry shortly;
        // never bypass UT's grounded/cooldown checks or queue several dodges.
        NextDodgeTime = Now + (bDodged ? NCAimTrainerScenarioPolicy::PopupDodgeDelaySeconds(Schedule.FRand()) : 0.2f);
    }
    if (Now >= NextDirectionTime)
    {
        Targets[Slot]->ReverseStrafe();
        NextDirectionTime = Now + NCAimTrainerScenarioPolicy::StrafeHoldSeconds(Schedule.FRand(), Schedule.FRand());
    }
}

bool ANCAimTrainerGame::HasTrackingContact() const
{
    AUTWeap_LinkGun_NCP* Link = Cast<AUTWeap_LinkGun_NCP>(RunWeapon);
    return IsTrackingBeamFiring() && Targets.IsValidIndex(0)
        && Targets[0] && Targets[0]->IsAvailable() && Link
        && Link->CurrentLinkedTarget == Targets[0];
}

bool ANCAimTrainerGame::IsTrackingBeamFiring() const
{
    AUTWeap_LinkGun_NCP* Link = Cast<AUTWeap_LinkGun_NCP>(RunWeapon);
    return Progress.Phase == 2 && Progress.Scenario == 0 && Link && Link->IsFiring()
        && Link->GetCurrentFireMode() == 1 && !Link->IsLinkPulsing();
}

void ANCAimTrainerGame::UpdateTrackingSample(float Now)
{
    if (!FMath::IsFinite(Now) || Now <= LastTraceTime) { return; }
    const bool bFiring = IsTrackingBeamFiring();
    const bool bContact = bFiring && HasTrackingContact();
    const double Elapsed = double(Now) - double(LastTraceTime);
    // Apply the same continuity and stall rules to both clocks. Idle time
    // changes neither accuracy nor score; firing off-target lowers accuracy.
    FiredSeconds += NCAimTrainerScoring::TrackingCredit(Elapsed, bPreviousFiring, bFiring);
    TrackedSeconds += NCAimTrainerScoring::TrackingCredit(Elapsed, bPreviousFiring && bPreviousContact, bContact);
    bPreviousFiring = bFiring;
    bPreviousContact = bContact;
    LastTraceTime = Now;
    Progress.Score = NCAimTrainerScoring::TrackingMilliseconds(TrackedSeconds);
    Progress.TrackingSeconds = float(TrackedSeconds);
    Progress.FiringSeconds = float(FiredSeconds);
    Progress.Accuracy = NCAimTrainerScoring::TrackingAccuracy(Progress.Score,
        NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds));
}

void ANCAimTrainerGame::PublishProgress()
{
    if (Trainee) { Trainee->SetTrainerProgress(Progress); }
}

void ANCAimTrainerGame::FinishRun()
{
    UpdateShotCount();
    HideAllTargets();
    Progress.Phase = 3;
    Progress.RemainingSeconds = 0.f;
    if (Progress.Scenario == 0)
    {
        Progress.Accuracy = NCAimTrainerScoring::TrackingAccuracy(Progress.Score,
            NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds));
    }
    if (RunWeapon) { RunWeapon->StopFire(0); RunWeapon->StopFire(1); }
    PublishProgress();
    // Include standalone results so a score complaint can be distinguished
    // from a rejected hit or a weapon shot-counter problem in the game log.
    UE_LOG(LogTemp, Log, TEXT("NCP Aim Trainer result: scenario=%d revision=%d score=%d hits=%d shots=%d headshots=%d expired=%d tracked_ms=%d fired_ms=%d ranked=%d"),
        int32(Progress.Scenario), int32(FNCAimTrainerOnline::PresetRevision), Progress.Score,
        Progress.Hits, Progress.Shots, Progress.Headshots, Progress.TargetsExpired,
        NCAimTrainerScoring::TrackingMilliseconds(TrackedSeconds), NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds),
        int32(bRankedRun && !Progress.bMovementPractice && Progress.Hits <= Progress.Shots));
    if (!bRankedRun || Progress.bMovementPractice || Progress.Hits > Progress.Shots)
    {
        Trainee->SetTrainerOnlineStatus(UnrankedReason.IsEmpty() ? TEXT("Practice only: incomplete shot accounting.") : UnrankedReason);
        return;
    }
    FNCAimTrainerResult Result;
    Result.Scenario = Progress.Scenario;
    Result.RunId = RunId;
    AUTPlayerState* PS = Cast<AUTPlayerState>(Trainee->PlayerState);
    Result.PlayerId = PS ? PS->StatsID : FString();
    Result.DisplayName = PS ? PS->PlayerName : FString();
    Result.Score = Progress.Score;
    Result.Shots = Progress.Shots;
    Result.Hits = Progress.Hits;
    Result.Headshots = Progress.Headshots;
    Result.TargetsExpired = Progress.TargetsExpired;
    Result.TrackedMilliseconds = Progress.Scenario == 0 ? Progress.Score : 0;
    Result.FiredMilliseconds = Progress.Scenario == 0 ? NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds) : 0;
    Trainee->SetTrainerOnlineStatus(TEXT("Submitting completed run to UT4Stats..."));
    TWeakObjectPtr<ANCAimTrainerGame> WeakGame(this);
    TWeakObjectPtr<ANCAimTrainerPlayerController> WeakPC(Trainee);
    const FString CompletedId = RunId;
    FNCAimTrainerOnline::Submit(GetWorld(), Result, [WeakGame, WeakPC, CompletedId](bool bSuccess, const FString& Message)
    {
        ANCAimTrainerGame* Game = WeakGame.Get();
        if (!Game || !WeakPC.IsValid() || Game->Trainee != WeakPC.Get() || Game->RunId != CompletedId || Game->Progress.Phase != 3) { return; }
        WeakPC->SetTrainerOnlineStatus(Message);
        if (bSuccess) { Game->RefreshLeaderboard(true); }
    });
}

void ANCAimTrainerGame::RefreshLeaderboard(bool bAfterSubmit)
{
    if (!IsTrainee(Trainee)) { return; }
    const int32 Scenario = Progress.Scenario;
    Trainee->SetTrainerLeaderboard(LeaderboardCache[Scenario]);
    const double Now = FPlatformTime::Seconds();
    if (LeaderboardInFlight[Scenario] || (!bAfterSubmit && Now < NextLeaderboardFetch[Scenario])) { return; }
    LeaderboardInFlight[Scenario] = true;
    NextLeaderboardFetch[Scenario] = Now + 60.0;
    TWeakObjectPtr<ANCAimTrainerGame> WeakGame(this);
    TWeakObjectPtr<ANCAimTrainerPlayerController> WeakPC(Trainee);
    FNCAimTrainerOnline::Fetch(GetWorld(), Scenario, [WeakGame, WeakPC, Scenario](bool bSuccess, const TArray<FNCAimTrainerLeaderboardRow>& Rows)
    {
        ANCAimTrainerGame* Game = WeakGame.Get();
        if (Game)
        {
            Game->LeaderboardInFlight[Scenario] = false;
            if (bSuccess) { Game->LeaderboardCache[Scenario] = Rows; }
        }
        if (!Game || !WeakPC.IsValid() || Game->Trainee != WeakPC.Get() || Game->Progress.Scenario != Scenario) { return; }
        if (bSuccess) { WeakPC->SetTrainerLeaderboard(Rows); }
        else if (Game->Progress.Phase == 0 && Game->SetupError.IsEmpty())
        {
            WeakPC->SetTrainerOnlineStatus(TEXT("UT4Stats leaderboard unavailable. Practice is still available."));
        }
    });
}
