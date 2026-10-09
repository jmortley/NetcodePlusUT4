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
#include "UTPlusWeap_RocketLauncher.h"
#include "UTProjectile.h"
#include "NCAimTrainerAirbornePolicy.h"
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
    SACTFSniperClass = nullptr;
    InstagibClass = nullptr;
    LinkClass = nullptr;
    RocketClass = nullptr;
    NextTargetTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    TargetExpiry.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextWiggleTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextPopupSlideTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextPopupLongStrafeTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    NextPopupDodgeTime.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    PopupSpawnVariants.SetNumZeroed(NCAimTrainerLayout::TargetCount);
    PopupDodgeActions.SetNumZeroed(NCAimTrainerLayout::TargetCount);
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
    return NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) ? ANCAimTrainerInstagibCharacter::StaticClass()
        : NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario) ? ANCAimTrainerSACTFCharacter::StaticClass()
        : ANCAimTrainerCharacter::StaticClass();
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
    UClass* TargetClass = NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) ? ANCAimTrainerInstagibTarget::StaticClass()
        : NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario) ? ANCAimTrainerSACTFTarget::StaticClass()
        : ANCAimTrainerTarget::StaticClass();
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
    Arena->SetScenario(uint8(NCAimTrainerScenarioPolicy::ArenaScenario(Progress.Scenario)));
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
        ResetLocalSession();
        RunId.Empty();
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
        Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(PracticeLaneX(), 0.f, Pawn->GetSimpleCollisionHalfHeight() + PracticeFloorZ()), FRotator::ZeroRotator,
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
    Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(PracticeLaneX(), 0.f, StandingHeight + PracticeFloorZ()), FRotator::ZeroRotator,
        false, nullptr, ETeleportType::TeleportPhysics);
    UNCAimTrainerMovement* Movement = Cast<UNCAimTrainerMovement>(Pawn->GetCharacterMovement());
    if (!Movement)
    {
        UE_LOG(LogTemp, Warning, TEXT("NCP Aim Trainer pawn mismatch: pawn=%s movement=%s expected=%s"),
            *GetNameSafe(Pawn->GetClass()), *GetNameSafe(Pawn->GetCharacterMovement() ? Pawn->GetCharacterMovement()->GetClass() : nullptr),
            *GetNameSafe(UNCAimTrainerMovement::StaticClass()));
        return FailSetup(TEXT("Cannot start: the trainee does not have the required practice movement component."));
    }
    Movement->ResetTrainerMovement(Progress.bMovementPractice, PracticeLaneX());
    // Uncrouching can raise the capsule center. Reset after restoring posture
    // so a fixed run always starts from the original standing anchor.
    Pawn->SetActorLocation(ArenaOrigin + FVector(PracticeLaneX(), 0.f, StandingHeight + PracticeFloorZ()), false, nullptr, ETeleportType::TeleportPhysics);
    // Stock restart uses the map PlayerStart's view direction when replacing
    // the scenario's pawn. Rotating the pawn alone does not reset mouse-look.
    // Anchor both authority and the owning client's camera to the practice lane.
    Trainee->SetControlRotation(FRotator::ZeroRotator);
    Trainee->ClientSetRotation(FRotator::ZeroRotator, true);
    Pawn->bCanBeDamaged = false;
    ClearTrainerProjectiles();
    Pawn->DiscardAllInventory();
    RunWeapon = nullptr;
    // Exact shipped NCP classes only. Missing precision content blocks the
    // run instead of quietly switching to stock hit registration. A failed
    // lookup is retried on the next start, so a later pak mount can recover.
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) && !LinkClass)
    {
        LinkClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/NCPLinkGun.NCPLinkGun_C"), nullptr, LOAD_NoWarn);
    }
    else if (NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) && !InstagibClass)
    {
        InstagibClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/N+InstagibRifle.N+InstagibRifle_C"), nullptr, LOAD_NoWarn);
    }
    else if (NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) && !RocketClass)
    {
        RocketClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPRocketLauncher.UTNPRocketLauncher_C"), nullptr, LOAD_NoWarn);
    }
    else if (NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario) && !SACTFSniperClass)
    {
        SACTFSniperClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/SACTFSniper.SACTFSniper_C"), nullptr, LOAD_NoWarn);
    }
    else if (!NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario)
        && NCAimTrainerScenarioPolicy::IsSniperScenario(Progress.Scenario) && Progress.bUseLightningGun && !LightningClass)
    {
        LightningClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPLightningGun.UTNPLightningGun_C"), nullptr, LOAD_NoWarn);
    }
    else if (!NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario)
        && NCAimTrainerScenarioPolicy::IsSniperScenario(Progress.Scenario) && !Progress.bUseLightningGun && !SniperClass)
    {
        SniperClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPSniper.UTNPSniper_C"), nullptr, LOAD_NoWarn);
    }
    TSubclassOf<AUTWeapon> DesiredClass = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) ? RocketClass : NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) ? LinkClass : NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) ? InstagibClass
        : NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario) ? SACTFSniperClass
        : Progress.bUseLightningGun ? LightningClass : SniperClass;
    if (!DesiredClass || DesiredClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FailSetup(NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario)
            ? TEXT("Cannot start: the NCP Rocket Launcher is unavailable. Install the current NCWepMut content pak.")
            : NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario)
            ? TEXT("Cannot start: the NCP Link Gun is unavailable. Install the current NCWepMut content pak.")
            : NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario)
            ? TEXT("Cannot start: the SACTF sniper is unavailable. Install the MutSaCTF content pak.")
            : TEXT("Cannot start: the selected NCP rifle is unavailable. Install the NCWepMut content pak."));
    }
    if ((NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) && !DesiredClass->IsChildOf(AUTPlusWeap_RocketLauncher::StaticClass()))
        || (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) && !DesiredClass->IsChildOf(AUTWeap_LinkGun_NCP::StaticClass()))
        || (NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) && !DesiredClass->IsChildOf(AUTPlusShockRifle::StaticClass()))
        || ((NCAimTrainerScenarioPolicy::IsSniperScenario(Progress.Scenario)) && !DesiredClass->IsChildOf(AUTPlusSniper::StaticClass())))
    {
        return FailSetup(TEXT("Cannot start: the selected rifle does not use the required NetcodePlus weapon class."));
    }
    RunWeapon = Cast<AUTWeapon>(Pawn->CreateInventory(DesiredClass));
    if (RunWeapon)
    {
        // Character contact can trigger Impressive even when a practice hit
        // does not score. Suppress that reward only on trainer-owned rifles.
        if (AUTPlusSniper* Rifle = Cast<AUTPlusSniper>(RunWeapon)) { Rifle->bTrackImpressive = false; }
        if (AUTPlusShockRifle* Rifle = Cast<AUTPlusShockRifle>(RunWeapon)) { Rifle->bTrackImpressive = false; }
        RunWeapon->Ammo = RunWeapon->MaxAmmo;
        Pawn->SwitchWeapon(RunWeapon);
        if (NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario))
        {
            const AUTPlusShockRifle* Rifle = Cast<AUTPlusShockRifle>(RunWeapon);
            if (!Rifle || !Rifle->HasSharedInstagibFireModes())
            {
                return FailSetup(TEXT("Cannot start: the instagib rifle's two fire modes do not match the training preset."));
            }
        }
    }
    if (!RunWeapon) { return FailSetup(TEXT("Cannot start: the selected NCP rifle could not be equipped.")); }
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
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

float ANCAimTrainerGame::PracticeLaneX() const
{
    return NCAimTrainerLayout::PracticeLaneX(Progress.Scenario);
}

float ANCAimTrainerGame::PracticeFloorZ() const
{
    return NCAimTrainerScenarioPolicy::IsAirborneScenario(Progress.Scenario)
        ? NCAimTrainerLayout::AirborneFiringLedge(NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario)).Height : 0.f;
}

void ANCAimTrainerGame::ClearTrainerProjectiles()
{
    if (!Trainee || !GetWorld()) { return; }
    // Destroy without exploding: a previous run must never hit a fresh target
    // or leave predicted projectile visuals flying during the menu/countdown.
    for (TActorIterator<AUTProjectile> It(GetWorld()); It; ++It)
    {
        if (It->InstigatorController == Trainee || (Trainee->GetPawn() && It->GetInstigator() == Trainee->GetPawn()))
        {
            It->Destroy();
        }
    }
}

bool ANCAimTrainerGame::IsInsidePracticeLane(const AUTCharacter* Pawn) const
{
    if (!Pawn || Pawn->IsDead()) { return false; }
    const FVector Position = Pawn->GetActorLocation() - ArenaOrigin;
    if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) || !FMath::IsFinite(Position.Z)) { return false; }
    const float StandingHeight = Pawn->GetClass()->GetDefaultObject<AUTCharacter>()->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (!Progress.bMovementPractice) { return (Position - FVector(PracticeLaneX(), 0.f, StandingHeight + PracticeFloorZ())).SizeSquared() <= 4.f; }
    // Capsule bounds accommodate standing, crouching, jumps and wall dodges.
    // The room walls stop lateral travel; the plane fixes target distance.
    const float HalfHeight = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    return FMath::IsFinite(HalfHeight) && HalfHeight > 0.f
        && FMath::Abs(Position.X - PracticeLaneX()) <= 5.f && FMath::Abs(Position.Y) <= 1800.f
        && Position.Z - HalfHeight >= PracticeFloorZ() - 10.f && Position.Z + HalfHeight <= 2010.f;
}

void ANCAimTrainerGame::SelectScenario(ANCAimTrainerPlayerController* PC, uint8 Scenario, bool bUseLightningGun)
{
    if (!IsTrainee(PC) || Scenario >= NCAimTrainerScenarioPolicy::ScenarioCount || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    ResetLocalSession();
    RunId.Empty();
    SetupError.Empty();
    const bool bMovementPractice = Progress.bMovementPractice;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bMovementPractice;
    Progress.bUseLightningGun = bUseLightningGun && !NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario);
    HideAllTargets();
    if (Arena) { Arena->SetScenario(uint8(NCAimTrainerScenarioPolicy::ArenaScenario(Scenario))); }
    if (!EnsureArena() || !ConfigurePawn()) { PC->SetTrainerOnlineStatus(SetupError); }
    PublishProgress();
    RefreshLeaderboard();
}

void ANCAimTrainerGame::SetMovementPractice(ANCAimTrainerPlayerController* PC, bool bEnabled)
{
    if (!IsTrainee(PC) || Progress.Phase == 1 || Progress.Phase == 2 || Progress.bMovementPractice == bEnabled) { return; }
    ResetLocalSession();
    const uint8 Scenario = Progress.Scenario;
    const bool bUseLightningGun = Progress.bUseLightningGun;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bEnabled;
    Progress.bUseLightningGun = bUseLightningGun && !NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario);
    bRankedRun = false;
    RunId.Empty();
    SetupError.Empty();
    HideAllTargets();
    const bool bReady = ConfigurePawn();
    PublishProgress();
    PC->SetTrainerOnlineStatus(!bReady ? SetupError : (bEnabled
        ? TEXT("Movement mode: strafe, dodge, jump and crouch. Complete runs use the movement leaderboard.")
        : TEXT("Fixed-position practice selected. Choose a scenario and start a run.")));
}

void ANCAimTrainerGame::StartTraining(ANCAimTrainerPlayerController* PC, bool bUseLightningGun)
{
    if (!IsTrainee(PC) || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    ResetLocalSession();
    Progress.bUseLightningGun = bUseLightningGun && !NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario);
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
    Progress.bUseLightningGun = bUseLightningGun && !NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario);
    Progress.RemainingSeconds = 3.f;
    PhaseStartedAt = GetWorld()->GetTimeSeconds();
    TrackedSeconds = 0.0;
    FiredSeconds = 0.0;
    bPreviousContact = false;
    bPreviousFiring = false;
    RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
    Schedule.Initialize(int32(GetTypeHash(RunId)));
    bRankedRun = GetNetMode() != NM_Standalone && BaseMutator == nullptr && FMath::IsNearlyEqual(GetWorldSettings()->GetEffectiveTimeDilation(), 1.f)
        && GetClass() == StaticClass();
    UnrankedReason = bRankedRun ? FString() : (GetNetMode() == NM_Standalone
        ? TEXT("Offline practice: scores are shown here but are not submitted to the shared leaderboard.")
        : TEXT("Practice only: mutators or altered game speed change the preset."));
    HideAllTargets();
    Arena->SetScenario(uint8(NCAimTrainerScenarioPolicy::ArenaScenario(Scenario)));
    PublishProgress();
    PC->SetTrainerOnlineStatus(bRankedRun ? TEXT("Complete all 60 seconds to submit to UT4Stats.") : UnrankedReason);
    StartLocalSession();
}

void ANCAimTrainerGame::AbortTraining(ANCAimTrainerPlayerController* PC)
{
    if (!IsTrainee(PC)) { return; }
    ResetLocalSession();
    HideAllTargets();
    const uint8 Scenario = Progress.Scenario;
    const bool bMovementPractice = Progress.bMovementPractice;
    const bool bUseLightningGun = Progress.bUseLightningGun;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.bMovementPractice = bMovementPractice;
    Progress.bUseLightningGun = bUseLightningGun && !NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario);
    bRankedRun = false;
    RunId.Empty();
    if (RunWeapon) { RunWeapon->StopFire(0); RunWeapon->StopFire(1); }
    ClearTrainerProjectiles(); // A loaded volley can release when StopFire runs.
    PublishProgress();
    PC->SetTrainerOnlineStatus(TEXT("Choose a scenario. Incomplete runs are not submitted."));
}

void ANCAimTrainerGame::BeginActiveRun()
{
    if (!PrepareLocalRecording()) { return; }
    Progress.Phase = 2;
    Progress.RemainingSeconds = 60.f;
    PhaseStartedAt = LastTraceTime = GetWorld()->GetTimeSeconds();
    NextDirectionTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::StrafeHoldSeconds(Schedule.FRand(), Schedule.FRand());
    NextDodgeTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::DodgeDelaySeconds(Schedule.FRand());
    NextTrackingSlideTime = PhaseStartedAt + NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(Schedule.FRand());
    NextPopupTime = PhaseStartedAt;
    const float Refire = RunWeapon ? RunWeapon->GetRefireTime(0) : 1.f;
    const float MinimumRefire = NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario) ? 0.7f : 1.f;
    PopupRefireSeconds = FMath::IsFinite(Refire) ? FMath::Max(MinimumRefire, Refire) : MinimumRefire;
    if (NCAimTrainerScenarioPolicy::IsInstagibScenario(Progress.Scenario) && (!FMath::IsFinite(Refire) || !FMath::IsNearlyEqual(Refire, 1.f)))
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: the instagib rifle's refire interval differs from the one-second preset.");
        InvalidateLocalRun();
    }
    if ((Progress.Scenario == 3 || Progress.Scenario == 8) && (!FMath::IsFinite(Refire) || !FMath::IsNearlyEqual(Refire, 1.3f)))
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: the Sniper/Lightning refire interval differs from the 1.3-second preset.");
        InvalidateLocalRun();
    }
    if (NCAimTrainerScenarioPolicy::IsSACTFScenario(Progress.Scenario)
        && (!FMath::IsFinite(Refire) || !FMath::IsNearlyEqual(Refire, 0.7f)))
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: the SACTF sniper's refire interval differs from the 0.7-second preset.");
        InvalidateLocalRun();
    }
    AUTPlayerState* PS = Trainee ? Cast<AUTPlayerState>(Trainee->PlayerState) : nullptr;
    ShotStatBaseline = RunWeapon ? RunWeapon->GetWeaponShotsStats(PS) : 0.f;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        NextTargetTime[Index] = PhaseStartedAt + (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) ? 0.f : Index * 0.25f);
        TargetExpiry[Index] = 0.f;
        NextWiggleTime[Index] = PhaseStartedAt;
        NextPopupSlideTime[Index] = NextPopupLongStrafeTime[Index] = NextPopupDodgeTime[Index] = 0.f;
        PopupSpawnVariants[Index] = PopupDodgeActions[Index] = 0;
        NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
    }
    if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario)) { UpdatePopupDodger(PhaseStartedAt); }
    PublishProgress();
}

void ANCAimTrainerGame::HideAllTargets()
{
    NextTrackingSlideTime = 0.f;
    ClearTrainerProjectiles();
    for (int32 Index = 0; Index < NextCrouchTime.Num(); ++Index)
    {
        NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
        NextPopupSlideTime[Index] = NextPopupLongStrafeTime[Index] = NextPopupDodgeTime[Index] = 0.f;
        PopupSpawnVariants[Index] = PopupDodgeActions[Index] = 0;
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
    const bool bPopupDodger = NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) && Index == NCAimTrainerLayout::PopupDodgerSlot;
    const bool bTimedPopup = NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) && !bPopupDodger;
    PopupSpawnVariants[Index] = bTimedPopup ? NCAimTrainerScenarioPolicy::PopupSpawnVariant(Index, Schedule.FRand()) : 0;
    FVector Position;
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
    {
        if (Index != 0) { return; }
        // Stay inside the actual Link beam range across the target's strafe
        // and dodge lane. The old sniper display target was 2500 units away.
        Position = FVector(-800.f, 0.f, StandingHeight);
        TargetExpiry[Index] = PhaseStartedAt + 60.f;
    }
    else if (NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario))
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
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::PopupSeat(Index, PopupSpawnVariants[Index]);
        Position = FVector(Schedule.FRandRange(Seat.MinX, Seat.MaxX),
            Seat.CenterY + Schedule.FRandRange(-Seat.SpawnJitterY, Seat.SpawnJitterY), StandingHeight + Seat.FloorZ);
        TargetExpiry[Index] = Now + NCAimTrainerScenarioPolicy::PopupExposure(PopupRefireSeconds, Schedule.FRand());
    }
    Targets[Index]->ActivateTarget(ArenaOrigin + Position, NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) || bPopupDodger);
    Targets[Index]->SetTrainerSpeedScale(Progress.Scenario == 6 ? 1.3f : 1.f);
    Targets[Index]->SetTrainerHeadshotScale(NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario) ? 1.15f : 1.f);
    ++LocalAppearances[Index];
    NextCrouchTime[Index] = CrouchEndTime[Index] = 0.f;
    NextPopupSlideTime[Index] = NextPopupLongStrafeTime[Index] = NextPopupDodgeTime[Index] = 0.f;
    PopupDodgeActions[Index] = NCAimTrainerScenarioPolicy::PopupStrafe;
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
    {
        NextCrouchTime[Index] = Now + NCAimTrainerScenarioPolicy::TrackingCrouchDelaySeconds(Schedule.FRand());
    }
    if (!NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) && !bPopupDodger)
    {
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario)
            ? NCAimTrainerLayout::HeadSeat(Index) : NCAimTrainerLayout::PopupSeat(Index, PopupSpawnVariants[Index]);
        Targets[Index]->StartWiggle(Seat.WiggleRange);
        if (bTimedPopup && NCAimTrainerScenarioPolicy::HasVariedPopupMovement(Index) && Schedule.FRand() < 0.5f)
        {
            Targets[Index]->ReverseStrafe();
        }
        NextWiggleTime[Index] = Now + (bTimedPopup
            ? NCAimTrainerScenarioPolicy::PopupStrafeHoldSeconds(Index, Schedule.FRand(), Schedule.FRand())
            : NCAimTrainerScenarioPolicy::WiggleHoldSeconds(Schedule.FRand()));
        if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario))
        {
            // Pick one special movement per appearance. Dodges do not repeat
            // on the permanent foreground target's timer or compete with slides.
            const int32 Action = NCAimTrainerScenarioPolicy::PopupAction(Index, PopupSpawnVariants[Index], Schedule.FRand());
            if (Action == NCAimTrainerScenarioPolicy::PopupSlide)
            {
                NextPopupSlideTime[Index] = Now + NCAimTrainerScenarioPolicy::PopupSlideDelaySeconds(Schedule.FRand());
            }
            if (Action == NCAimTrainerScenarioPolicy::PopupLongStrafe)
            {
                NextPopupLongStrafeTime[Index] = Now + NCAimTrainerScenarioPolicy::PopupLongStrafeDelaySeconds(Schedule.FRand());
            }
            if (Action >= NCAimTrainerScenarioPolicy::PopupForwardDodge)
            {
                PopupDodgeActions[Index] = Action;
                NextPopupDodgeTime[Index] = Now + NCAimTrainerScenarioPolicy::PopupDodgeDelaySecondsForAppearance(Schedule.FRand());
            }
            if (Action == NCAimTrainerScenarioPolicy::PopupStrafe
                && NCAimTrainerScenarioPolicy::ShouldCrouch(Schedule.FRand()))
            {
                NextCrouchTime[Index] = Now + NCAimTrainerScenarioPolicy::CrouchDelaySeconds(Schedule.FRand());
            }
        }
    }
}

void ANCAimTrainerGame::UpdateShotCount()
{
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario)) { return; }
    AUTPlayerState* PS = Trainee ? Cast<AUTPlayerState>(Trainee->PlayerState) : nullptr;
    const float RawShots = RunWeapon ? RunWeapon->GetWeaponShotsStats(PS) - ShotStatBaseline : -1.f;
    const float MaxShots = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) ? 150.f : 200.f;
    if (!FMath::IsFinite(RawShots) || RawShots < 0.f || RawShots > MaxShots)
    {
        bRankedRun = false;
        UnrankedReason = TEXT("Practice only: weapon shot accounting changed during the run.");
        InvalidateLocalRun();
        return;
    }
    Progress.Shots = FMath::RoundToInt(RawShots);
    RecordLocalShotCount();
    if (NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario))
    {
        Progress.Score = NCAimTrainerScoring::RocketScore(Progress.Hits, Progress.TargetsExpired);
        Progress.Accuracy = NCAimTrainerScoring::RocketAccuracy(Progress.Hits, Progress.TargetsExpired);
        return;
    }
    Progress.Score = NCAimTrainerScenarioPolicy::IsAirborneScenario(Progress.Scenario)
        ? NCAimTrainerScoring::AirborneScore(Progress.Hits, Progress.Shots,
            NCAimTrainerScenarioPolicy::HasHeadshotBonus(Progress.Scenario) ? Progress.Headshots : 0)
        : NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario)
        ? NCAimTrainerScoring::HeadshotScore(Progress.Hits, Progress.Shots)
        : NCAimTrainerScoring::PrecisionScore(Progress.Hits, Progress.Shots, Progress.TargetsExpired,
            NCAimTrainerScenarioPolicy::HasHeadshotBonus(Progress.Scenario) ? Progress.Headshots : 0);
    Progress.Accuracy = Progress.Shots > 0 ? 100.f * Progress.Hits / Progress.Shots : 0.f;
}

float ANCAimTrainerGame::RecordTargetHit(ANCAimTrainerTarget* Target, float Damage,
    const FDamageEvent& Event, AController* Instigator, AActor* Causer)
{
    if (Progress.Phase != 2 || !IsTrainee(Trainee) || Instigator != Trainee
        || !RunWeapon || !FMath::IsFinite(Damage) || Damage <= 0.f) { return 0.f; }
    const int32 Slot = Targets.IndexOfByKey(Target);
    const float Now = GetWorld()->GetTimeSeconds();
    if (Slot == INDEX_NONE || !Target->IsAvailable() || Now >= PhaseStartedAt + 60.f || Now >= TargetExpiry[Slot]) { return 0.f; }
    if (NCAimTrainerScenarioPolicy::IsAirborneScenario(Progress.Scenario) && IsAtAirborneHazard(Target)) { return 0.f; }
    const bool bRocket = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario);
    if (bRocket ? !IsCurrentRocketDamage(Target, Event, Causer) : Causer != RunWeapon) { return 0.f; }
    const AUTWeaponFix* FixedWeapon = Cast<AUTWeaponFix>(RunWeapon);
    const float Rewind = !bRocket && FixedWeapon ? FixedWeapon->GetHitValidationPredictionTime() : 0.f;
    if (!FMath::IsFinite(Rewind) || Rewind < 0.f || Now - Rewind < Target->GetAppearanceTime()) { return 0.f; }
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
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
    bool bHeadshot = false;
    if (NCAimTrainerScenarioPolicy::IsSniperScenario(Progress.Scenario))
    {
        const AUTPlusSniper* Sniper = Cast<AUTPlusSniper>(RunWeapon);
        if (!Sniper || !Sniper->HeadshotDamageType) { return 0.f; }
        bHeadshot = Event.DamageTypeClass == Sniper->HeadshotDamageType;
        if (NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario) && !bHeadshot) { return 0.f; }
        if (bHeadshot) { ++Progress.Headshots; }
    }
    ++Progress.Hits;
    // Native weapons increment their shot stat before applying damage. Capture
    // it here too, so a same-tick hit never arrives before its shot event.
    UpdateShotCount();
    RecordLocalTarget(Slot, true, bHeadshot);
    Target->HideTarget();
    NextTargetTime[Slot] = Now + (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) ? 0.f : 0.35f);
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
        if (bLocalAuthPending) { return; }
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
            InvalidateLocalRun();
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
        if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
        {
            UpdateTrackingMovement(Now);
            if (Now - LastTraceTime >= 1.f / 30.f)
            {
                UpdateTrackingSample(Now);
            }
        }
        if (RunWeapon) { RunWeapon->Ammo = RunWeapon->MaxAmmo; }
        FlushLocalCheckpoint();
    }
    if (Now >= NextStatusTime) { PublishProgress(); NextStatusTime = Now + 0.1f; }
}

void ANCAimTrainerGame::UpdateTrackingMovement(float Now)
{
    if (Progress.Phase != 2 || !NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) || Now >= PhaseStartedAt + 60.f
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

bool ANCAimTrainerGame::IsCurrentRocketDamage(const ANCAimTrainerTarget* Target,
    const FDamageEvent& Event, AActor* Causer) const
{
    const AUTPlusWeap_RocketLauncher* Launcher = Cast<AUTPlusWeap_RocketLauncher>(RunWeapon);
    const AUTProjectile* Projectile = Cast<AUTProjectile>(Causer);
    if (!Launcher || !Projectile || !Target || Projectile->Role != ROLE_Authority
        || Projectile->InstigatorController != Trainee || Projectile->GetInstigator() != Trainee->GetPawn()
        || !Projectile->MyDamageType || Event.DamageTypeClass != Projectile->MyDamageType
        || !NCAimTrainerAirbornePolicy::ValidRocketAge(GetWorld()->GetTimeSeconds(), Projectile->CreationTime,
            PhaseStartedAt, Target->GetAppearanceTime())) { return false; }
    // Accept only projectiles actually configured by this NCP launcher. Stock
    // explosion/LOS and NCP projectile validation have already run upstream.
    bool bAllowed = (Launcher->SeekingRocketClass && Projectile->GetClass() == Launcher->SeekingRocketClass)
        || (Launcher->SpiralRocketClass && Projectile->GetClass() == Launcher->SpiralRocketClass);
    for (const FPlusRocketFireMode& Mode : Launcher->RocketFireModes)
    {
        bAllowed |= Mode.ProjClass && Projectile->GetClass() == Mode.ProjClass;
    }
    return bAllowed;
}

bool ANCAimTrainerGame::IsAtAirborneHazard(const ANCAimTrainerTarget* Target) const
{
    if (!Target || !Target->GetCapsuleComponent()) { return true; }
    const FVector Position = Target->GetActorLocation() - ArenaOrigin;
    const float Feet = Position.Z - Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    return !FMath::IsFinite(Feet) || NCAimTrainerAirbornePolicy::AtHazard(Feet,
        NCAimTrainerLayout::AirborneHazardHeight(NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario)))
        || FMath::Abs(Position.X) > 3200.f || FMath::Abs(Position.Y) > 1800.f;
}

bool ANCAimTrainerGame::LaunchAirborneJumper()
{
    if (!Targets.IsValidIndex(0) || !Targets[0] || !Targets[0]->IsAvailable()) { return false; }
    ANCAimTrainerTarget* Target = Targets[0];
    const FVector Position = Target->GetActorLocation() - ArenaOrigin;
    const bool bRockets = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario);
    const NCAimTrainerLayout::FBlock Pad = NCAimTrainerLayout::AirborneJumpPad(Position.Y < 0.f ? 1 : 0, bRockets);
    const float EndZ = Pad.Height + Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f;
    float UpSpeed = 0.f, FlightSeconds = 0.f;
    if (!NCAimTrainerAirbornePolicy::JumpArc(Target->GetCharacterMovement()->GetGravityZ(),
        Position.Z, EndZ, NCAimTrainerLayout::AirborneJumpApex(bRockets), UpSpeed, FlightSeconds)) { return false; }
    return Target->LaunchAirborneTarget(FVector((Pad.CenterX - Position.X) / FlightSeconds,
        (Pad.CenterY - Position.Y) / FlightSeconds, UpSpeed));
}

void ANCAimTrainerGame::ActivateAirborneSlot(int32 Index, float Now)
{
    if (Progress.Phase != 2 || Now >= PhaseStartedAt + 60.f
        || !Targets.IsValidIndex(Index) || !Targets[Index]) { return; }
    ANCAimTrainerTarget* Target = Targets[Index];
    const float HalfHeight = Target->GetClass()->GetDefaultObject<ANCAimTrainerTarget>()
        ->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    FVector Position, Velocity = FVector::ZeroVector;
    const bool bRockets = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario);
    if (Index == 0)
    {
        const NCAimTrainerLayout::FBlock Pad = NCAimTrainerLayout::AirborneJumpPad(Schedule.RandRange(0, 1), bRockets);
        Position = FVector(Pad.CenterX, Pad.CenterY, Pad.Height + HalfHeight + 2.f);
    }
    else
    {
        const bool bSideWall = NCAimTrainerAirbornePolicy::UseSideWallSeat(Index - 1, Schedule.FRand());
        const NCAimTrainerLayout::FSeat Seat = NCAimTrainerLayout::AirborneDropSeat(Index - 1, bSideWall, bRockets);
        Position = FVector(Schedule.FRandRange(Seat.MinX, Seat.MaxX),
            Seat.CenterY + Schedule.FRandRange(-Seat.SpawnJitterY, Seat.SpawnJitterY),
            NCAimTrainerLayout::AirborneTargetHeight(
                Schedule.FRandRange(NCAimTrainerLayout::AirborneDropMinZ, NCAimTrainerLayout::AirborneDropMaxZ), bRockets));
        // Small varied drift gives each fall a readable arc. Bounds leave room
        // for the entire capsule at the maximum drift until it reaches goo.
        // Side-wall drops drift inward, never out through the adjacent wall.
        const float DriftY = bSideWall ? (Index == 1 ? 1.f : -1.f) * Schedule.FRandRange(20.f, 65.f)
            : Schedule.FRandRange(-80.f, 80.f);
        const float LaunchScale = NCAimTrainerLayout::AirborneLaunchScale(bRockets);
        Velocity = FVector(0.f, DriftY * LaunchScale, Schedule.FRandRange(150.f, 300.f) * LaunchScale);
    }
    Target->ActivateAirborneTarget(ArenaOrigin + Position, Velocity,
        Index == 0 ? 1.f : NCAimTrainerAirbornePolicy::FlightRate(Progress.Scenario));
    Target->SetTrainerHeadshotScale(1.f);
    TargetExpiry[Index] = PhaseStartedAt + 60.f;
    ++LocalAppearances[Index];
    if (Index == 0) { LaunchAirborneJumper(); }
}

void ANCAimTrainerGame::UpdateAirborneTargets(float Now)
{
    if (Progress.Phase != 2 || Now >= PhaseStartedAt + 60.f) { return; }
    TArray<int32> EligibleSlots;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        ANCAimTrainerTarget* Target = Targets[Index];
        if (Target->IsAvailable())
        {
            if (IsAtAirborneHazard(Target)
                || (Index != 0 && Target->GetCharacterMovement()->IsMovingOnGround()))
            {
                ++Progress.TargetsExpired;
                RecordLocalTarget(Index, false);
                Target->HideTarget();
                NextTargetTime[Index] = Now + 0.2f;
                UpdateShotCount();
            }
            else if (Index == 0 && Target->GetCharacterMovement()->IsMovingOnGround())
            {
                LaunchAirborneJumper();
            }
        }
        if (!Target->IsAvailable() && Now >= NextTargetTime[Index])
        {
            if (Index == 0) { ActivateAirborneSlot(Index, Now); }
            else { EligibleSlots.Add(Index); }
        }
    }
    // Independent falls fill the room gradually. Never catch up in a burst
    // after a slow frame, and never wait for every target to be cleared.
    if (Now >= NextPopupTime && EligibleSlots.Num() > 0)
    {
        ActivateAirborneSlot(EligibleSlots[Schedule.RandRange(0, EligibleSlots.Num() - 1)], Now);
        NextPopupTime = Now + NCAimTrainerAirbornePolicy::SpawnDelay(Schedule.FRand());
    }
}

void ANCAimTrainerGame::UpdateTargets(float Now)
{
    if (NCAimTrainerScenarioPolicy::IsAirborneScenario(Progress.Scenario)) { UpdateAirborneTargets(Now); return; }
    if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario)) { UpdatePopupDodger(Now); }
    const int32 ActiveSlots = NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) ? 1 : NCAimTrainerScenarioPolicy::IsHeadshotScenario(Progress.Scenario)
        ? NCAimTrainerLayout::HeadSlotCount : NCAimTrainerLayout::PopupSlotCount;
    TArray<int32> EligibleSlots;
    for (int32 Index = 0; Index < ActiveSlots; ++Index)
    {
        if (Targets[Index]->IsAvailable() && Now >= TargetExpiry[Index])
        {
            RecordLocalTarget(Index, false);
            Targets[Index]->HideTarget();
            ++Progress.TargetsExpired;
            NextTargetTime[Index] = Now + (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) ? 0.f : Schedule.FRandRange(0.25f, 0.65f));
        }
        if (!Targets[Index]->IsAvailable() && Now >= NextTargetTime[Index])
        {
            if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario)) { EligibleSlots.Add(Index); }
            else { ActivateSlot(Index, Now); }
        }
        if (!NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) && Targets[Index]->IsAvailable() && Now >= NextWiggleTime[Index])
        {
            Targets[Index]->ReverseStrafe();
            NextWiggleTime[Index] = Now + (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario)
                ? NCAimTrainerScenarioPolicy::PopupStrafeHoldSeconds(Index, Schedule.FRand(), Schedule.FRand())
                : NCAimTrainerScenarioPolicy::WiggleHoldSeconds(Schedule.FRand()));
        }
        if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) && Targets[Index]->IsAvailable())
        {
            if (NextPopupDodgeTime[Index] > 0.f && Now >= NextPopupDodgeTime[Index])
            {
                NextPopupDodgeTime[Index] = 0.f;
                const int32 Action = PopupDodgeActions[Index];
                const bool bSlideOnLanding = Action == NCAimTrainerScenarioPolicy::PopupDodgeSlide;
                // Allow the native jump/landing (and optional slide), then a
                // full legal rifle shot. A missed opportunity never extends life.
                const float RequiredTime = (bSlideOnLanding ? 2.05f : 1.05f) + PopupRefireSeconds;
                if (TargetExpiry[Index] - Now >= RequiredTime && PhaseStartedAt + 60.f - Now >= RequiredTime)
                {
                    const float Angle = NCAimTrainerScenarioPolicy::PopupDodgeAngleDegrees(Schedule.FRand()) * (PI / 180.f);
                    const float XSign = Action == NCAimTrainerScenarioPolicy::PopupForwardDodge ? -1.f : 1.f;
                    const float YSign = Schedule.FRand() < 0.5f ? -1.f : 1.f;
                    const FVector Direction(XSign * FMath::Cos(Angle), YSign * FMath::Sin(Angle), 0.f);
                    Targets[Index]->TryTrainerPopupDodge(Index, Direction, ArenaOrigin, bSlideOnLanding);
                }
            }
            if (NextPopupSlideTime[Index] > 0.f && Now >= NextPopupSlideTime[Index])
            {
                NextPopupSlideTime[Index] = 0.f; // One attempt per appearance, never a catch-up burst.
                // Allow the native 0.7-second slide, a posture transition, and
                // a full rifle refire interval before either expiry or run end.
                const float RequiredTime = 1.f + PopupRefireSeconds;
                if (TargetExpiry[Index] - Now >= RequiredTime && PhaseStartedAt + 60.f - Now >= RequiredTime)
                {
                    Targets[Index]->TryTrainerPopupSlide(Index, PopupSpawnVariants[Index]);
                }
            }
            if (NextPopupLongStrafeTime[Index] > 0.f && Now >= NextPopupLongStrafeTime[Index])
            {
                NextPopupLongStrafeTime[Index] = 0.f;
                const float Hold = NCAimTrainerScenarioPolicy::PopupLongStrafeHoldSeconds(Schedule.FRand());
                const float RequiredTime = Hold + PopupRefireSeconds;
                if (TargetExpiry[Index] - Now >= RequiredTime && PhaseStartedAt + 60.f - Now >= RequiredTime)
                {
                    if (Targets[Index]->IsTrainerSliding()) { NextPopupLongStrafeTime[Index] = Now + 0.15f; }
                    else
                    {
                        Targets[Index]->StartPopupLongStrafe(NCAimTrainerLayout::PopupLongStrafeRange, Hold, Schedule.FRand());
                    }
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
    // several targets at once or catch up after a stall: respect the selected
    // rifle's refire interval. Randomize the next available height/lane.
    if (NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) && Now >= NextPopupTime && EligibleSlots.Num() > 0)
    {
        ActivateSlot(EligibleSlots[Schedule.RandRange(0, EligibleSlots.Num() - 1)], Now);
        NextPopupTime = Now + NCAimTrainerScenarioPolicy::PopupSpawnDelay(PopupRefireSeconds, Schedule.FRand());
    }
}

void ANCAimTrainerGame::UpdatePopupDodger(float Now)
{
    if (Progress.Phase != 2 || !NCAimTrainerScenarioPolicy::IsPopupScenario(Progress.Scenario) || Now >= PhaseStartedAt + 60.f) { return; }
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
    return Progress.Phase == 2 && NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) && Link && Link->IsFiring()
        && Link->GetCurrentFireMode() == 1 && !Link->IsLinkPulsing();
}

void ANCAimTrainerGame::UpdateTrackingSample(float Now)
{
    if (!FMath::IsFinite(Now) || Now <= LastTraceTime) { return; }
    const bool bFiring = IsTrackingBeamFiring();
    const bool bContact = bFiring && HasTrackingContact();
    RecordLocalSample(bFiring, bContact);
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
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario))
    {
        Progress.Accuracy = NCAimTrainerScoring::TrackingAccuracy(Progress.Score,
            NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds));
    }
    if (RunWeapon) { RunWeapon->StopFire(0); RunWeapon->StopFire(1); }
    ClearTrainerProjectiles(); // Include any loaded volley released by StopFire.
    PublishProgress();
    // Include standalone results so a score complaint can be distinguished
    // from a rejected hit or a weapon shot-counter problem in the game log.
    UE_LOG(LogTemp, Log, TEXT("NCP Aim Trainer result: scenario=%d revision=%d score=%d hits=%d shots=%d headshots=%d expired=%d tracked_ms=%d fired_ms=%d ranked=%d"),
        int32(Progress.Scenario), FNCAimTrainerOnline::PresetRevisionForScenario(Progress.Scenario), Progress.Score,
        Progress.Hits, Progress.Shots, Progress.Headshots, Progress.TargetsExpired,
        NCAimTrainerScoring::TrackingMilliseconds(TrackedSeconds), NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds),
        int32(bRankedRun && (NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) || Progress.Hits <= Progress.Shots)));
    if (LocalSession.IsValid() && bLocalRecording)
    {
        Trainee->SetTrainerOnlineStatus(TEXT("Checking final checkpoints with UT4Stats..."));
        FlushLocalCheckpoint(true);
        return;
    }
    if (!bRankedRun || (!NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) && Progress.Hits > Progress.Shots))
    {
        Trainee->SetTrainerOnlineStatus(UnrankedReason.IsEmpty() ? TEXT("Practice only: incomplete shot accounting.") : UnrankedReason);
        return;
    }
    FNCAimTrainerResult Result;
    Result.Scenario = Progress.Scenario;
    Result.bMovementPractice = Progress.bMovementPractice;
    Result.RunId = RunId;
    AUTPlayerState* PS = Cast<AUTPlayerState>(Trainee->PlayerState);
    Result.PlayerId = PS ? PS->StatsID : FString();
    Result.DisplayName = PS ? PS->PlayerName : FString();
    Result.Score = Progress.Score;
    Result.Shots = Progress.Shots;
    Result.Hits = Progress.Hits;
    Result.Headshots = Progress.Headshots;
    Result.TargetsExpired = Progress.TargetsExpired;
    Result.TrackedMilliseconds = NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) ? Progress.Score : 0;
    Result.FiredMilliseconds = NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) ? NCAimTrainerScoring::TrackingMilliseconds(FiredSeconds) : 0;
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
    // Menu clients own public reads and their scenario/source/movement caches. Only a
    // confirmed submission needs a server notification; no duplicate hub fetch.
    if (bAfterSubmit && IsTrainee(Trainee))
    {
        Trainee->NotifyTrainerLeaderboardSubmission(Progress.Scenario, GetNetMode() == NM_Standalone, Progress.bMovementPractice);
    }
}

void ANCAimTrainerGame::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ResetLocalSession();
    RunId.Empty();
    Super::EndPlay(EndPlayReason);
}

bool ANCAimTrainerGame::IsStandardPreset() const
{
    return BaseMutator == nullptr && GetClass() == StaticClass()
        && FMath::IsNearlyEqual(GetWorldSettings()->GetEffectiveTimeDilation(), 1.f);
}

void ANCAimTrainerGame::ResetLocalSession()
{
    if (LocalSession.IsValid()) { LocalSession->Cancel(); }
    LocalSession.Reset();
    LocalEvents.Empty();
    bLocalAuthPending = bLocalRecording = false;
    LocalNextCheckpoint = 1;
    LocalLastShotCount = 0;
    for (int32& Appearance : LocalAppearances) { Appearance = 0; }
}

void ANCAimTrainerGame::StartLocalSession()
{
    if (GetNetMode() != NM_Standalone || !IsStandardPreset()) { return; }
    bLocalAuthPending = true;
    Trainee->SetTrainerOnlineStatus(TEXT("Verifying your signed-in UT account for the local-run leaderboard..."));
    const FString RequestedId = RunId;
    TWeakObjectPtr<ANCAimTrainerGame> WeakGame(this);
    LocalSession = FNCAimTrainerLocalSession::Start(GetWorld(), Progress.Scenario, RequestedId,
        [WeakGame, RequestedId](TSharedPtr<FNCAimTrainerLocalSession> Session, const FString& Message)
    {
        ANCAimTrainerGame* Game = WeakGame.Get();
        if (!Game || Game->RunId != RequestedId || Game->Progress.Phase != 1 || !Game->IsTrainee(Game->Trainee))
        {
            if (Session.IsValid()) { Session->Cancel(); }
            return;
        }
        Game->bLocalAuthPending = false;
        Game->LocalSession = Session;
        // Account verification happens before the countdown, never during the
        // timed challenge. An unavailable service still permits local practice.
        Game->PhaseStartedAt = Game->GetWorld()->GetTimeSeconds();
        if (!Session.IsValid()) { Game->UnrankedReason = Message; }
        Game->Trainee->SetTrainerOnlineStatus(Session.IsValid()
            ? TEXT("Local-run leaderboard: complete 60 seconds while connected. Five-second checkpoints enabled.") : Message);
    }, Progress.bMovementPractice);
}

void ANCAimTrainerGame::BeginLocalRecording()
{
    if (!LocalSession.IsValid()) { return; }
    if (!IsStandardPreset()) { InvalidateLocalRun(); return; }
    bLocalRecording = true;
    const FString RecordingId = RunId;
    TWeakObjectPtr<ANCAimTrainerGame> WeakGame(this);
    LocalSession->SetCompletion([WeakGame, RecordingId](bool bSuccess, const FString& Message, int32 Score)
    {
        ANCAimTrainerGame* Game = WeakGame.Get();
        if (!Game || Game->RunId != RecordingId || !Game->IsTrainee(Game->Trainee)) { return; }
        if (!bSuccess)
        {
            Game->bLocalRecording = false;
            Game->UnrankedReason = Message;
            Game->LocalEvents.Empty();
        }
        if (Game->Progress.Phase >= 1 && Game->Progress.Phase <= 3) { Game->Trainee->SetTrainerOnlineStatus(Message); }
        if (bSuccess && Game->Progress.Phase == 3)
        {
            // UT4Stats computes points from the accepted event sequence.
            Game->Progress.Score = Score;
            Game->PublishProgress();
            Game->RefreshLeaderboard(true);
        }
    });
    LocalSession->BeginRecording();
}

bool ANCAimTrainerGame::PrepareLocalRecording()
{
    if (!LocalSession.IsValid() || !LocalSession->IsHealthy()) { return true; }
    if (!bLocalRecording)
    {
        Trainee->SetTrainerOnlineStatus(TEXT("Confirming the run clock with UT4Stats..."));
        BeginLocalRecording();
    }
    // Start the timed challenge only after checkpoint zero is acknowledged.
    // Otherwise loss of its first request moves the server clock behind the
    // running client and valid later checkpoints look impossibly early.
    const bool bReady = !bLocalRecording || !LocalSession.IsValid() || LocalSession->IsRecordingReady();
    if (bReady && bLocalRecording)
    {
        Trainee->SetTrainerOnlineStatus(TEXT("Five-second checkpoints active. Complete 60 seconds to save this local run."));
    }
    return bReady;
}

void ANCAimTrainerGame::InvalidateLocalRun()
{
    if (!LocalSession.IsValid()) { return; }
    bLocalRecording = false;
    LocalSession->Cancel();
    LocalSession.Reset();
    LocalEvents.Empty();
    if (UnrankedReason.IsEmpty()) { UnrankedReason = TEXT("Practice only: local checkpoint recording was interrupted."); }
    if (IsTrainee(Trainee)) { Trainee->SetTrainerOnlineStatus(UnrankedReason); }
}

void ANCAimTrainerGame::AddLocalEvent(FNCAimTrainerLocalEvent Event)
{
    if (!bLocalRecording || !LocalSession.IsValid() || !LocalSession->IsHealthy()) { return; }
    const double Elapsed = double(GetWorld()->GetTimeSeconds()) - double(PhaseStartedAt);
    if (!FMath::IsFinite(Elapsed) || Elapsed < 0.0 || Elapsed >= 60.0) { return; }
    if (LocalEvents.Num() >= 256)
    {
        UnrankedReason = TEXT("Practice only: too many events between checkpoints.");
        InvalidateLocalRun();
        return;
    }
    Event.TimeUs = int32(Elapsed * 1000000.0);
    LocalEvents.Add(Event);
}

void ANCAimTrainerGame::RecordLocalShotCount()
{
    if (!bLocalRecording) { return; }
    const int32 Added = Progress.Shots - LocalLastShotCount;
    const int32 MaxBatch = NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) ? 3 : 1;
    if (Added < 0 || Added > MaxBatch)
    {
        UnrankedReason = TEXT("Practice only: shot timing could not be recorded reliably.");
        InvalidateLocalRun();
        return;
    }
    // Loaded rockets increment their projectile counter together before launch.
    // Preserve that observed batch time; do not manufacture spaced shot times.
    for (int32 Index = 0; Index < Added; ++Index)
    {
        FNCAimTrainerLocalEvent Event;
        Event.Type = FNCAimTrainerLocalEvent::Shot;
        AddLocalEvent(Event);
    }
    LocalLastShotCount = Progress.Shots;
}

void ANCAimTrainerGame::RecordLocalTarget(int32 Slot, bool bHit, bool bHeadshot)
{
    if (NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario) || Slot < 0 || Slot >= 6) { return; }
    FNCAimTrainerLocalEvent Event;
    Event.Type = bHit ? FNCAimTrainerLocalEvent::Hit : FNCAimTrainerLocalEvent::Expire;
    Event.Target = Slot;
    Event.Appearance = LocalAppearances[Slot];
    Event.bHead = bHit && bHeadshot;
    AddLocalEvent(Event);
}

void ANCAimTrainerGame::RecordLocalSample(bool bFiring, bool bContact)
{
    FNCAimTrainerLocalEvent Event;
    Event.Type = FNCAimTrainerLocalEvent::Sample;
    Event.bFiring = bFiring;
    Event.bContact = bContact;
    AddLocalEvent(Event);
}

void ANCAimTrainerGame::FlushLocalCheckpoint(bool bFinal)
{
    if (!bLocalRecording || !LocalSession.IsValid()) { return; }
    if (!IsStandardPreset() || (!NCAimTrainerScenarioPolicy::IsTrackingScenario(Progress.Scenario)
        && !NCAimTrainerScenarioPolicy::IsRocketScenario(Progress.Scenario) && Progress.Hits > Progress.Shots))
    {
        UnrankedReason = TEXT("Practice only: the preset or shot accounting changed during the run.");
        InvalidateLocalRun();
        return;
    }
    const int32 ElapsedUs = int32(FMath::Clamp(double(GetWorld()->GetTimeSeconds()) - double(PhaseStartedAt), 0.0, 60.0) * 1000000.0);
    if (!bFinal && ElapsedUs < LocalNextCheckpoint * 5000000) { return; }
    // Never manufacture missing checkpoints after a stall or at run completion.
    if ((bFinal && LocalNextCheckpoint != 12) || (!bFinal && ElapsedUs > LocalNextCheckpoint * 5000000 + 250000))
    {
        UnrankedReason = TEXT("Practice only: a scheduled checkpoint was missed.");
        InvalidateLocalRun();
        return;
    }
    if (!LocalSession->QueueCheckpoint(bFinal ? 60000000 : ElapsedUs, LocalEvents, bFinal))
    {
        UnrankedReason = LocalSession->GetFailureReason();
        InvalidateLocalRun();
        return;
    }
    LocalEvents.Empty();
    ++LocalNextCheckpoint;
}
