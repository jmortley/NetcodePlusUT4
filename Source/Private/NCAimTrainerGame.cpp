#include "NCAimTrainerGame.h"
#include "NCAimTrainerTarget.h"
#include "NCAimTrainerHUD.h"
#include "NCAimTrainerOnline.h"
#include "NCAimTrainerScoring.h"
#include "TeamArenaCharacter.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTCharacterMovement.h"
#include "UTPlayerState.h"
#include "UTPickup.h"
#include "UTDroppedPickup.h"
#include "UTGameSession.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"

ANCAimTrainerGame::ANCAimTrainerGame(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DisplayName = NSLOCTEXT("UTGameMode", "NCAimTrainer", "NetcodePlus Aim Trainer");
    PlayerControllerClass = ANCAimTrainerPlayerController::StaticClass();
    HUDClass = ANCAimTrainerHUD::StaticClass();
    DefaultPawnClass = ATeamArenaCharacter::StaticClass();
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
    InstagibClass = nullptr;
}

void ANCAimTrainerGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);
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

bool ANCAimTrainerGame::ReadyToStartMatch_Implementation() { return NumPlayers > 0; }
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

bool ANCAimTrainerGame::EnsureArena()
{
    if (Arena && Arena->IsPendingKillPending()) { Arena = nullptr; }
    Targets.RemoveAll([](ANCAimTrainerTarget* Target) { return !Target || Target->IsPendingKillPending(); });
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (!Arena)
    {
        Arena = GetWorld()->SpawnActor<ANCAimTrainerArena>(ArenaOrigin, FRotator::ZeroRotator, Params);
    }
    if (!Arena || !Arena->HasArenaAssets()) { return false; }
    while (Targets.Num() < 3)
    {
        ANCAimTrainerTarget* Target = GetWorld()->SpawnActor<ANCAimTrainerTarget>(
            ArenaOrigin + FVector(900.f, (Targets.Num() - 1) * 650.f, 108.f), FRotator(0, 180, 0), Params);
        if (!Target || !Target->HasCharacterAssets())
        {
            if (Target) { Target->Destroy(); }
            return false;
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
    ConfigurePawn();
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
    if (!Player || !Player->PlayerState || Player->PlayerState->bOnlySpectator) { return; }
    if (Trainee && Player != Trainee) { return; }
    // Stock restart establishes possession, PlayerState and inventory setup.
    // Move the resulting pawn to the same fixed lane on every map.
    Super::RestartPlayer(Player);
    if (APawn* Pawn = Player->GetPawn())
    {
        Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(-1800.f, 0.f, 108.f), FRotator::ZeroRotator,
            false, nullptr, ETeleportType::TeleportPhysics);
        Player->SetControlRotation(FRotator::ZeroRotator);
        Player->ClientSetRotation(FRotator::ZeroRotator, true);
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
    AUTCharacter* Pawn = Trainee ? Cast<AUTCharacter>(Trainee->GetPawn()) : nullptr;
    if (!Pawn || Pawn->IsDead()) { return false; }
    Pawn->SetActorLocationAndRotation(ArenaOrigin + FVector(-1800.f, 0.f, 108.f), FRotator::ZeroRotator,
        false, nullptr, ETeleportType::TeleportPhysics);
    Pawn->GetCharacterMovement()->StopMovementImmediately();
    Pawn->GetCharacterMovement()->DisableMovement();
    Pawn->bCanBeDamaged = false;
    Pawn->DiscardAllInventory();
    RunWeapon = nullptr;
    // Exact shipped NCP classes only. Missing precision content blocks the
    // run instead of quietly switching to stock hit registration. A failed
    // lookup is retried on the next start, so a later pak mount can recover.
    if (Progress.Scenario == 2 && !InstagibClass)
    {
        InstagibClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/N+InstagibRifle.N+InstagibRifle_C"), nullptr, LOAD_NoWarn);
    }
    else if (Progress.Scenario != 2 && !SniperClass)
    {
        SniperClass = LoadClass<AUTWeapon>(nullptr,
            TEXT("/Game/Blueprints/Netcode/UTNPSniper.UTNPSniper_C"), nullptr, LOAD_NoWarn);
    }
    TSubclassOf<AUTWeapon> DesiredClass = Progress.Scenario == 2 ? InstagibClass : SniperClass;
    if (!DesiredClass || DesiredClass->HasAnyClassFlags(CLASS_Abstract))
    {
        // Unarmed tracking remains usable; precision modes require the actual
        // shipped NCP weapon and never quietly fall back to different hit tests.
        return Progress.Scenario == 0;
    }
    if ((Progress.Scenario == 2 && !DesiredClass->IsChildOf(AUTPlusShockRifle::StaticClass()))
        || (Progress.Scenario != 2 && !DesiredClass->IsChildOf(AUTPlusSniper::StaticClass()))) { return false; }
    RunWeapon = Cast<AUTWeapon>(Pawn->CreateInventory(DesiredClass));
    if (RunWeapon)
    {
        RunWeapon->Ammo = RunWeapon->MaxAmmo;
        Pawn->SwitchWeapon(RunWeapon);
        if (Progress.Scenario == 2)
        {
            const AUTPlusShockRifle* Rifle = Cast<AUTPlusShockRifle>(RunWeapon);
            if (!Rifle || !Rifle->HasSharedInstagibFireModes()) { return false; }
        }
    }
    return Progress.Scenario == 0 || (RunWeapon && RunWeapon->ShotsStatsName != NAME_None);
}

void ANCAimTrainerGame::SelectScenario(ANCAimTrainerPlayerController* PC, uint8 Scenario)
{
    if (!IsTrainee(PC) || Scenario > 2 || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    HideAllTargets();
    if (Arena) { Arena->SetScenario(Scenario); }
    ConfigurePawn();
    PublishProgress();
    RefreshLeaderboard();
}

void ANCAimTrainerGame::StartTraining(ANCAimTrainerPlayerController* PC)
{
    if (!IsTrainee(PC) || Progress.Phase == 1 || Progress.Phase == 2) { return; }
    if (!EnsureArena() || !ConfigurePawn())
    {
        PC->SetTrainerOnlineStatus(TEXT("Cannot start: trainer character/arena or NCP weapon assets are missing. Check the server cook."));
        return;
    }
    const uint8 Scenario = Progress.Scenario;
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
    Progress.Phase = 1;
    Progress.RemainingSeconds = 3.f;
    PhaseStartedAt = GetWorld()->GetTimeSeconds();
    TrackedSeconds = 0.0;
    bPreviousContact = false;
    RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    Schedule.Initialize(int32(GetTypeHash(RunId)));
    bRankedRun = BaseMutator == nullptr && FMath::IsNearlyEqual(GetWorldSettings()->GetEffectiveTimeDilation(), 1.f)
        && GetClass() == StaticClass();
    UnrankedReason = bRankedRun ? FString() : TEXT("Practice only: mutators or altered game speed change the preset.");
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
    Progress = FNCAimTrainerProgress();
    Progress.Scenario = Scenario;
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
    NextDirectionTime = PhaseStartedAt + 0.8f;
    AUTPlayerState* PS = Trainee ? Cast<AUTPlayerState>(Trainee->PlayerState) : nullptr;
    ShotStatBaseline = RunWeapon ? RunWeapon->GetWeaponShotsStats(PS) : 0.f;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        NextTargetTime[Index] = PhaseStartedAt + Index * 0.25f;
        TargetExpiry[Index] = 0.f;
    }
    PublishProgress();
}

void ANCAimTrainerGame::HideAllTargets()
{
    for (ANCAimTrainerTarget* Target : Targets)
    {
        if (Target && !Target->IsPendingKillPending()) { Target->HideTarget(); }
    }
}

void ANCAimTrainerGame::ActivateSlot(int32 Index, float Now)
{
    if (!Targets.IsValidIndex(Index)) { return; }
    FVector Position;
    if (Progress.Scenario == 0)
    {
        if (Index != 0) { return; }
        Position = FVector(700.f, 0.f, 108.f);
        TargetExpiry[Index] = PhaseStartedAt + 60.f;
    }
    else if (Progress.Scenario == 1)
    {
        Position = FVector(900.f, (Index - 1) * 650.f, 108.f);
        TargetExpiry[Index] = Now + 4.5f;
    }
    else
    {
        // Disjoint lateral lanes avoid overlapping targets. Distance, side,
        // lifetime and cadence vary, unlike a fixed flat grid of dots.
        Position = FVector(Schedule.FRandRange(-100.f, 2300.f),
            (Index - 1) * 850.f + Schedule.FRandRange(-240.f, 240.f), 108.f + FMath::Max(1.f, Index * 160.f));
        TargetExpiry[Index] = Now + Schedule.FRandRange(2.0f, 4.0f);
    }
    Targets[Index]->ActivateTarget(ArenaOrigin + Position, Progress.Scenario == 0);
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
    Progress.Score = NCAimTrainerScoring::PrecisionScore(Progress.Hits, Progress.Shots, Progress.TargetsExpired);
    Progress.Accuracy = Progress.Shots > 0 ? 100.f * Progress.Hits / Progress.Shots : 0.f;
}

float ANCAimTrainerGame::RecordTargetHit(ANCAimTrainerTarget* Target, float Damage,
    const FDamageEvent& Event, AController* Instigator, AActor* Causer)
{
    if (Progress.Phase != 2 || Progress.Scenario == 0 || !IsTrainee(Trainee) || Instigator != Trainee
        || !RunWeapon || Causer != RunWeapon || !FMath::IsFinite(Damage) || Damage <= 0.f) { return 0.f; }
    const int32 Slot = Targets.IndexOfByKey(Target);
    const float Now = GetWorld()->GetTimeSeconds();
    if (Slot == INDEX_NONE || !Target->IsAvailable() || Now >= PhaseStartedAt + 60.f || Now >= TargetExpiry[Slot]) { return 0.f; }
    const AUTWeaponFix* FixedWeapon = Cast<AUTWeaponFix>(RunWeapon);
    const float Rewind = FixedWeapon ? FixedWeapon->GetHitValidationPredictionTime() : 0.f;
    if (!FMath::IsFinite(Rewind) || Rewind < 0.f || Now - Rewind < Target->GetAppearanceTime()) { return 0.f; }
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
    NextTargetTime[Slot] = Now + (Progress.Scenario == 2 ? Schedule.FRandRange(0.20f, 0.65f) : 0.35f);
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
        if (!Pawn || Pawn->IsDead() || (Pawn->GetActorLocation() - (ArenaOrigin + FVector(-1800, 0, 108))).SizeSquared() > 4.f)
        {
            AbortTraining(Trainee);
            Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because the trainee left the fixed practice lane."));
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
        const int32 ActiveSlots = Progress.Scenario == 0 ? 1 : 3;
        if (Targets.Num() != 3 || !Arena || Arena->IsPendingKillPending())
        {
            AbortTraining(Trainee);
            Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because the practice arena was removed."));
            return;
        }
        for (int32 Index = 0; Index < ActiveSlots; ++Index)
        {
            if (!Targets[Index] || Targets[Index]->IsPendingKillPending())
            {
                AbortTraining(Trainee);
                Trainee->SetTrainerOnlineStatus(TEXT("Run stopped because a practice target was removed."));
                return;
            }
            if (Targets[Index]->IsAvailable() && Now >= TargetExpiry[Index])
            {
                Targets[Index]->HideTarget();
                ++Progress.TargetsExpired;
                NextTargetTime[Index] = Now + Schedule.FRandRange(0.25f, 0.65f);
            }
            else if (!Targets[Index]->IsAvailable() && Now >= NextTargetTime[Index]) { ActivateSlot(Index, Now); }
        }
        if (Progress.Scenario == 0)
        {
            if (Now >= NextDirectionTime)
            {
                Targets[0]->SetStrafeDirection(Schedule.RandRange(0, 1) == 0 ? -1.f : 1.f);
                NextDirectionTime = Now + Schedule.FRandRange(0.45f, 1.15f);
            }
            if (Now - LastTraceTime >= 1.f / 30.f)
            {
                // PlayerCameraManager updates after actor ticks, so its cached
                // viewpoint would introduce a frame of unrelated aim delay.
                const FVector ViewLocation = Pawn->GetPawnViewLocation();
                const FRotator ViewRotation = Trainee->GetControlRotation();
                FHitResult Hit;
                FCollisionQueryParams Params(FName(TEXT("TrainerTracking")), false, Pawn);
                const bool bContact = GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation,
                    ViewLocation + ViewRotation.Vector() * 10000.f, COLLISION_TRACE_WEAPON, Params)
                    && Hit.GetActor() == Targets[0] && Targets[0]->IsAvailable();
                TrackedSeconds += NCAimTrainerScoring::TrackingCredit(Now - LastTraceTime, bPreviousContact, bContact);
                bPreviousContact = bContact;
                LastTraceTime = Now;
                Progress.Score = NCAimTrainerScoring::TrackingMilliseconds(TrackedSeconds);
                Progress.TrackingSeconds = float(TrackedSeconds);
                Progress.Accuracy = Now > PhaseStartedAt ? 100.f * float(TrackedSeconds) / (Now - PhaseStartedAt) : 0.f;
            }
        }
        if (RunWeapon) { RunWeapon->Ammo = RunWeapon->MaxAmmo; }
    }
    if (Now >= NextStatusTime) { PublishProgress(); NextStatusTime = Now + 0.1f; }
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
    if (Progress.Scenario == 0) { Progress.Accuracy = 100.f * Progress.Score / 60000.f; }
    if (RunWeapon) { RunWeapon->StopFire(0); RunWeapon->StopFire(1); }
    PublishProgress();
    if (!bRankedRun || Progress.Hits > Progress.Shots)
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
        else if (Game->Progress.Phase == 0) { WeakPC->SetTrainerOnlineStatus(TEXT("UT4Stats leaderboard unavailable. Practice is still available.")); }
    });
}
