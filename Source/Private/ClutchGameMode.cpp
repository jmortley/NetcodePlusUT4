#include "ClutchGameMode.h"
#include "ClutchHUD.h"
#include "ClutchRoundState.h"
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTProjectile.h"
#include "UTTeamInfo.h"
#include "UTInventory.h"
#include "UTPickup.h"
#include "UTGenericObjectivePoint.h"
#include "UTCTFFlagBase.h"
#include "UTTeamPlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "UObject/UnrealType.h"


DEFINE_LOG_CATEGORY_STATIC(LogClutch, Log, All);


namespace
{
	static float ParsePositiveFloatOption(const FString& Options, const TCHAR* Key, float Fallback)
	{
		const FString Value = UGameplayStatics::ParseOption(Options, Key);
		if (Value.IsEmpty())
		{
			return Fallback;
		}

		const float Parsed = FCString::Atof(*Value);
		return Parsed > 0.0f ? Parsed : Fallback;
	}

	static uint8 GetPlayerTeamIndex(const AUTPlayerState* PlayerState)
	{
		return PlayerState && PlayerState->Team && PlayerState->Team->TeamIndex <= 1
			? PlayerState->Team->TeamIndex
			: AClutchRoundState::NoTeam;
	}

	static bool GetClutchStartRole(const AActor* Start, EClutchRole& OutRole)
	{
		if (!Start)
		{
			return false;
		}

		// Original cooked CL maps use ClutchSpawn_C, whose only role field is
		// a Blueprint BoolProperty named "Attacker".
		if (UBoolProperty* AttackerProperty = FindField<UBoolProperty>(
			Start->GetClass(), TEXT("Attacker")))
		{
			OutRole = AttackerProperty->GetPropertyValue_InContainer(Start)
				? EClutchRole::Attacker
				: EClutchRole::Defender;
			return true;
		}

		if (Start->ActorHasTag(FName(TEXT("ClutchAttacker")))
			|| Start->ActorHasTag(FName(TEXT("Attacker"))))
		{
			OutRole = EClutchRole::Attacker;
			return true;
		}
		if (Start->ActorHasTag(FName(TEXT("ClutchDefender")))
			|| Start->ActorHasTag(FName(TEXT("Defender"))))
		{
			OutRole = EClutchRole::Defender;
			return true;
		}

		// Dumb CTF-derived CL maps use ordinary team starts. The historical
		// layout attacks from blue into the red objective, independent of which
		// player team owns the attacker role this round.
		if (const AUTTeamPlayerStart* TeamStart = Cast<AUTTeamPlayerStart>(Start))
		{
			OutRole = TeamStart->TeamNum == 1
				? EClutchRole::Attacker
				: EClutchRole::Defender;
			return true;
		}
		return false;
	}

	static void ConfigureCTFFlagBaseForClutch(AUTCTFFlagBase* FlagBase, bool bIsPole)
	{
		if (!FlagBase)
		{
			return;
		}

		// The selected red base remains as a visible, stationary pole. Every
		// other CTF base/flag is hidden and non-interactive for this mode.
		FlagBase->SetActorHiddenInGame(!bIsPole);
		FlagBase->SetActorEnableCollision(false);
		if (FlagBase->MyFlag)
		{
			AUTFlag* Flag = FlagBase->MyFlag;
			// The flag base supplies the neutral pole location. Hide the actual
			// team-colored flag so alternating attackers never appear to capture
			// their "own" CTF objective.
			Flag->SetActorHiddenInGame(true);

			// Disabling all collision makes AUTCarriedObject fall through the
			// world. FellOutOfWorld() then calls SendHome() forever, and stock UT
			// plays the flag-return message/effect before checking that it was
			// already home. Leave world collision enabled, but remove every path
			// by which a player or CTF timer can interact with this inert prop.
			Flag->SetActorEnableCollision(true);
			Flag->SetActorTickEnabled(true);
			Flag->bEnemyCanPickup = false;
			Flag->bFriendlyCanPickup = false;
			Flag->bTeamPickupSendsHome = false;
			Flag->bEnemyPickupSendsHome = false;
			Flag->GetWorldTimerManager().ClearAllTimersForObject(Flag);
			if (Flag->Collision)
			{
				Flag->Collision->bGenerateOverlapEvents = false;
				Flag->Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
			}
			if (Flag->ObjectState != CarriedObjectState::Home)
			{
				// ChangeState avoids ObjectReturnedHome(), so converting an already
				// displaced flag is silent as well.
				Flag->ChangeState(CarriedObjectState::Home);
			}
			Flag->MoveToHome();
		}
	}
}


AClutchGameMode::AClutchGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("UTGameMode", "Clutch", "Clutch");
	MapPrefix = TEXT("CL");

	// Explicitly preserve editor-safe stock classes. Clutch state is carried by
	// AClutchRoundState, never by a custom PlayerState or PlayerController.
	PlayerStateClass = AUTPlayerState::StaticClass();
	PlayerControllerClass = AUTPlayerController::StaticClass();
	HUDClass = AClutchHUD::StaticClass();

	NumTeams = 2;
	GoalScore = 9;
	TimeLimit = 0;
	DefaultMaxPlayers = 6;
	BotFillCount = 0;
	WarmupFillCount = 0;
	// The rules scale from 1v1 through 3v3. This keeps two-client PIE usable;
	// SelectRoundRoles still waits until at least one player exists on each team.
	bRequireFull = false;
	bForceNoBots = true;
	bBalanceTeams = true;
	bUseTeamStarts = true;
	bAnnounceTeam = false;
	bForceRespawn = false;
	bWeaponStayActive = false;
	bClearPlayerInventory = true;
	bAmmoIsLimited = false;
	bGameHasImpactHammer = false;
	bPlayersStartWithArmor = false;
	StartingArmorClass = nullptr;
	TeamDamagePct = 0.0f;
	DefaultInventory.Empty();

	RoundDurationSeconds = 60.0f;
	PoleUnlockDelaySeconds = 45.0f;
	PoleCaptureSeconds = 7.0f;
	PoleDecaySeconds = 7.0f;
	PoleCaptureRadius = 180.0f;
	PoleCaptureHalfHeight = 160.0f;
	PoleActorTag = FName(TEXT("ClutchPole"));
	IntermissionSeconds = 5.0f;
	MaxAttackerHits = 3;
	AttackerHealth = 300;
	DefenderDamagePerHit = 100;
	AttackerDamagePerHit = 1000;
	bWinByTwo = false;
	MinimumWinMargin = 2;

	// Resolved to concrete stock Blueprint weapons in InitGame unless a BP child
	// supplies the recovered Clutch loadouts first.
	AttackerWeaponClass = nullptr;
	DefenderWeaponClass = nullptr;

	ClutchState = nullptr;
	PoleActor = nullptr;
	NextAttackerSlot[0] = INDEX_NONE;
	NextAttackerSlot[1] = INDEX_NONE;
	NextAttackingTeamIndex = 0;
	bAllowRoundSpawns = false;
	bEndingRound = false;
	bRoundTimedOut = false;
	bPoleCaptured = false;
	bWinCheckQueued = false;

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.05f;
}


void AClutchGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Existing NC-ClutchGM Blueprint assets may have serialized the old UTHUD
	// parent default. Enforce the native Clutch HUD before players log in so PIE
	// and cooked servers both initialize the correct scoreboard/widget stack.
	HUDClass = AClutchHUD::StaticClass();
	// Likewise, older Blueprint children may have serialized the former 3v3-only
	// lobby gate. Clutch still supports six players, but two are enough to play.
	bRequireFull = false;

	GoalScore = FMath::Max(1, GoalScore);
	RoundDurationSeconds = ParsePositiveFloatOption(Options, TEXT("RoundDuration"), RoundDurationSeconds);
	PoleUnlockDelaySeconds = ParsePositiveFloatOption(Options, TEXT("PoleUnlockDelay"), PoleUnlockDelaySeconds);
	PoleCaptureSeconds = ParsePositiveFloatOption(Options, TEXT("PoleCaptureSeconds"), PoleCaptureSeconds);
	PoleDecaySeconds = ParsePositiveFloatOption(Options, TEXT("PoleDecaySeconds"), PoleDecaySeconds);
	IntermissionSeconds = ParsePositiveFloatOption(Options, TEXT("IntermissionSeconds"), IntermissionSeconds);

	MaxAttackerHits = FMath::Clamp(
		UGameplayStatics::GetIntOption(Options, TEXT("AttackerHits"), MaxAttackerHits), 1, 255);
	DefenderDamagePerHit = FMath::Max(1,
		UGameplayStatics::GetIntOption(Options, TEXT("DefenderDamage"), DefenderDamagePerHit));
	AttackerHealth = MaxAttackerHits * DefenderDamagePerHit;
	bWinByTwo = EvalBoolOptions(UGameplayStatics::ParseOption(Options, TEXT("WinByTwo")), bWinByTwo);
	MinimumWinMargin = FMath::Max(1,
		UGameplayStatics::GetIntOption(Options, TEXT("WinMargin"), MinimumWinMargin));

	PoleUnlockDelaySeconds = FMath::Clamp(PoleUnlockDelaySeconds, 0.0f, RoundDurationSeconds);
	if (!AttackerWeaponClass || AttackerWeaponClass->HasAnyClassFlags(CLASS_Abstract))
	{
		AttackerWeaponClass = LoadClass<AUTInventory>(nullptr,
			TEXT("/Game/RestrictedAssets/Weapons/Sniper/BP_Sniper.BP_Sniper_C"));
	}
	if (!DefenderWeaponClass || DefenderWeaponClass->HasAnyClassFlags(CLASS_Abstract))
	{
		DefenderWeaponClass = LoadClass<AUTInventory>(nullptr,
			TEXT("/Game/RestrictedAssets/Weapons/RocketLauncher/BP_RocketLauncher.BP_RocketLauncher_C"));
	}
	if (GameSession)
	{
		GameSession->MaxPlayers = DefaultMaxPlayers;
	}

	UE_LOG(LogClutch, Log,
		TEXT("InitGame: first-to-%d, round %.1fs, pole unlock %.1fs, capture %.1fs, hits %d"),
		GoalScore, RoundDurationSeconds, PoleUnlockDelaySeconds, PoleCaptureSeconds,
		MaxAttackerHits);
}


void AClutchGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	NCPlusVersionGate::SpawnFor(NewPlayer);

	if (!HasAuthority() || !NewPlayer)
	{
		return;
	}

	// A replicated AInfo must be spawned no earlier than PostLogin in this UT4
	// build. Earlier BeginPlay spawns have a documented stock-client crash path.
	EnsureClutchState();
	RefreshRoster();

	AUTPlayerState* PlayerState = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (PlayerState && HasMatchStarted())
	{
		const FClutchRosterEntry* Entry = ClutchState ? ClutchState->FindEntry(PlayerState) : nullptr;
		if (!Entry || Entry->PlayerStatus != EClutchStatus::Active)
		{
			EnterSpectating(NewPlayer, FindActiveTeammate(PlayerState));
		}
	}
}


void AClutchGameMode::Logout(AController* Exiting)
{
	AUTPlayerState* PlayerState = Exiting ? Cast<AUTPlayerState>(Exiting->PlayerState) : nullptr;
	if (HasAuthority() && PlayerState)
	{
		MarkDisconnected(PlayerState);
	}

	Super::Logout(Exiting);
}


void AClutchGameMode::HandleMatchHasStarted()
{
	// Super may attempt normal match-start spawns. RestartPlayer's independent
	// gate rejects them while bAllowRoundSpawns is false.
	bAllowRoundSpawns = false;
	Super::HandleMatchHasStarted();

	if (!HasAuthority())
	{
		return;
	}

	EnsureClutchState();
	RefreshRoster();
	PoleActor = ResolvePoleActor();
	NextAttackerSlot[0] = INDEX_NONE;
	NextAttackerSlot[1] = INDEX_NONE;
	NextAttackingTeamIndex = 0;
	bEndingRound = false;
	BeginNextRound();
}


void AClutchGameMode::HandleMatchHasEnded()
{
	GetWorldTimerManager().ClearAllTimersForObject(this);
	bAllowRoundSpawns = false;
	bEndingRound = true;
	if (ClutchState)
	{
		ClutchState->SetPhase(EClutchRoundPhase::MatchEnd);
	}
	Super::HandleMatchHasEnded();
}


void AClutchGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (HasAuthority() && ClutchState && ClutchState->Phase == EClutchRoundPhase::Capture)
	{
		UpdatePole(DeltaSeconds);
	}
}


void AClutchGameMode::EnsureClutchState()
{
	if (!HasAuthority() || ClutchState)
	{
		return;
	}

	for (TActorIterator<AClutchRoundState> It(GetWorld()); It; ++It)
	{
		ClutchState = *It;
		break;
	}

	bool bCreated = false;
	if (!ClutchState)
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		ClutchState = GetWorld()->SpawnActor<AClutchRoundState>(Params);
		bCreated = ClutchState != nullptr;
	}

	if (bCreated)
	{
		ClutchState->ResetForMatch(GoalScore, static_cast<uint8>(MaxAttackerHits));
	}
}


void AClutchGameMode::RefreshRoster()
{
	if (!HasAuthority() || !ClutchState || !UTGameState)
	{
		return;
	}

	TSet<uint8> UsedSlots[2];
	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (Entry.PlayerState && Cast<AController>(Entry.PlayerState->GetOwner())
			&& Entry.TeamIndex <= 1
			&& Entry.RosterSlot != AClutchRoundState::UnassignedSlot)
		{
			UsedSlots[Entry.TeamIndex].Add(Entry.RosterSlot);
		}
	}

	for (APlayerState* RawPlayerState : UTGameState->PlayerArray)
	{
		AUTPlayerState* PlayerState = Cast<AUTPlayerState>(RawPlayerState);
		if (!PlayerState || PlayerState->bOnlySpectator || PlayerState->bIsInactive
			|| !Cast<AController>(PlayerState->GetOwner()))
		{
			continue;
		}

		const uint8 TeamIndex = GetPlayerTeamIndex(PlayerState);
		const FClutchRosterEntry* Existing = ClutchState->FindEntry(PlayerState);
		uint8 Slot = AClutchRoundState::UnassignedSlot;
		if (Existing && Existing->TeamIndex == TeamIndex)
		{
			Slot = Existing->RosterSlot;
			if (TeamIndex <= 1 && Slot != AClutchRoundState::UnassignedSlot
				&& Existing->PlayerState != PlayerState && UsedSlots[TeamIndex].Contains(Slot))
			{
				Slot = AClutchRoundState::UnassignedSlot;
			}
		}

		if (TeamIndex <= 1 && Slot == AClutchRoundState::UnassignedSlot)
		{
			for (uint8 Candidate = 0; Candidate < 6; ++Candidate)
			{
				if (!UsedSlots[TeamIndex].Contains(Candidate))
				{
					Slot = Candidate;
					break;
				}
			}
		}

		if (TeamIndex <= 1 && Slot != AClutchRoundState::UnassignedSlot)
		{
			UsedSlots[TeamIndex].Add(Slot);
		}

		EClutchRole Role = EClutchRole::None;
		EClutchStatus Status = EClutchStatus::Queued;
		if (Existing && Existing->TeamIndex == TeamIndex
			&& Existing->PlayerStatus != EClutchStatus::Disconnected)
		{
			Role = Existing->PlayerRole;
			Status = Existing->PlayerStatus;
		}

		ClutchState->UpsertPlayer(PlayerState, TeamIndex, Slot, Role, Status);
	}
}


void AClutchGameMode::BeginRound()
{
	if (!HasAuthority() || HasMatchEnded())
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(PoleUnlockTimerHandle);
	GetWorldTimerManager().ClearTimer(RoundTimeoutTimerHandle);
	GetWorldTimerManager().ClearTimer(SpawnQueueTimerHandle);

	EnsureClutchState();
	RefreshRoster();
	bEndingRound = false;
	bRoundTimedOut = false;
	bPoleCaptured = false;
	bWinCheckQueued = false;

	if (!PoleActor)
	{
		PoleActor = ResolvePoleActor();
	}

	if (!ClutchState || !PoleActor || !SelectRoundRoles())
	{
		if (ClutchState)
		{
			ClutchState->SetPhase(EClutchRoundPhase::Waiting);
			TArray<AUTPlayerState*> ConnectedPlayers;
			for (const FClutchRosterEntry& Entry : ClutchState->Roster)
			{
				if (Entry.PlayerState && Cast<AController>(Entry.PlayerState->GetOwner()))
				{
					ConnectedPlayers.Add(Entry.PlayerState);
				}
			}
			for (AUTPlayerState* PlayerState : ConnectedPlayers)
			{
				ClutchState->SetPlayerRoundState(
					PlayerState, EClutchRole::None, EClutchStatus::Queued, 0);
			}
		}
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* Controller = It->Get();
			if (Controller && Controller->PlayerState && !Controller->PlayerState->bOnlySpectator)
			{
				// PIE and incomplete servers should remain neutral and playable while
				// waiting, rather than showing stock "You are out of lives" UI.
				EnterPlaying(Controller);
				if (!Controller->GetPawn())
				{
					Super::RestartPlayer(Controller);
				}
			}
		}
		GetWorldTimerManager().SetTimer(
			IntermissionTimerHandle, this, &AClutchGameMode::BeginNextRound, 2.0f, false);
		return;
	}

	ClearRoundPawns();
	BuildRoundSpawnQueue();
}


bool AClutchGameMode::SelectRoundRoles()
{
	if (!ClutchState)
	{
		return false;
	}

	TArray<int32> SlotsByTeam[2];
	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (Entry.PlayerState && Cast<AController>(Entry.PlayerState->GetOwner())
			&& Entry.TeamIndex <= 1
			&& Entry.RosterSlot != AClutchRoundState::UnassignedSlot)
		{
			SlotsByTeam[Entry.TeamIndex].AddUnique(static_cast<int32>(Entry.RosterSlot));
		}
	}

	if (SlotsByTeam[0].Num() == 0 || SlotsByTeam[1].Num() == 0)
	{
		return false;
	}

	const uint8 AttackingTeam = NextAttackingTeamIndex <= 1 ? NextAttackingTeamIndex : 0;
	const uint8 DefendingTeam = AClutchRoundState::GetDefendingTeam(AttackingTeam);
	const int32 SelectedSlot = AClutchRoundState::SelectNextRotationSlot(
		SlotsByTeam[AttackingTeam], NextAttackerSlot[AttackingTeam]);
	if (SelectedSlot == INDEX_NONE)
	{
		return false;
	}

	AUTPlayerState* SelectedAttacker = nullptr;
	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (Entry.PlayerState && Cast<AController>(Entry.PlayerState->GetOwner())
			&& Entry.TeamIndex == AttackingTeam
			&& Entry.RosterSlot == SelectedSlot)
		{
			SelectedAttacker = Entry.PlayerState;
			break;
		}
	}
	if (!SelectedAttacker)
	{
		return false;
	}

	NextAttackerSlot[AttackingTeam] = SelectedSlot;
	ClutchState->BeginRound(
		AttackingTeam,
		SelectedAttacker,
		ClutchState->RoundNumber + 1,
		0.0f,
		0.0f,
		0.0f);

	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		AUTPlayerState* PlayerState = Entry.PlayerState;
		if (!PlayerState || !Cast<AController>(PlayerState->GetOwner()))
		{
			continue;
		}

		if (Entry.TeamIndex == AttackingTeam)
		{
			const bool bChosen = PlayerState == SelectedAttacker;
			ClutchState->SetPlayerRoundState(
				PlayerState,
				bChosen ? EClutchRole::Attacker : EClutchRole::None,
				bChosen ? EClutchStatus::Active : EClutchStatus::Benched,
				0);
		}
		else if (Entry.TeamIndex == DefendingTeam)
		{
			ClutchState->SetPlayerRoundState(
				PlayerState, EClutchRole::Defender, EClutchStatus::Active, 0);
		}
		else
		{
			ClutchState->SetPlayerRoundState(
				PlayerState, EClutchRole::None, EClutchStatus::Queued, 0);
		}
	}

	// Inventory and health are assigned during this short spawn phase. Combat
	// begins only after every active controller has received exactly one pawn.
	ClutchState->SetPhase(EClutchRoundPhase::Intermission);
	return true;
}


void AClutchGameMode::BuildRoundSpawnQueue()
{
	PendingRoundSpawns.Reset();
	RoundSpawnAttempts.Reset();
	AUTPlayerState* ActiveAttacker = GetActiveAttacker();

	// Attacker first so benched teammates have a valid same-team view target.
	if (ActiveAttacker)
	{
		if (AController* Controller = Cast<AController>(ActiveAttacker->GetOwner()))
		{
			PendingRoundSpawns.Add(Controller);
		}
	}

	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		AController* Controller = Entry.PlayerState
			? Cast<AController>(Entry.PlayerState->GetOwner())
			: nullptr;
		if (!Controller)
		{
			continue;
		}

		if (Entry.PlayerStatus == EClutchStatus::Active)
		{
			if (Entry.PlayerState != ActiveAttacker)
			{
				PendingRoundSpawns.AddUnique(Controller);
			}
		}
		else
		{
			EnterSpectating(Controller, FindActiveTeammate(Entry.PlayerState));
		}
	}

	bAllowRoundSpawns = true;
	ProcessNextRoundSpawn();
}


void AClutchGameMode::ProcessNextRoundSpawn()
{
	if (!HasAuthority() || bEndingRound)
	{
		bAllowRoundSpawns = false;
		PendingRoundSpawns.Reset();
		return;
	}

	while (PendingRoundSpawns.Num() > 0)
	{
		TWeakObjectPtr<AController> WeakController = PendingRoundSpawns[0];
		PendingRoundSpawns.RemoveAt(0);
		AController* Controller = WeakController.Get();
		AUTPlayerState* PlayerState = Controller
			? Cast<AUTPlayerState>(Controller->PlayerState)
			: nullptr;
		if (!Controller || !IsActiveRoundPlayer(PlayerState))
		{
			continue;
		}

		EnterPlaying(Controller);
		RestartPlayer(Controller);
		if (Controller->GetPawn())
		{
			FreezeRoundPawn(Controller);
		}
		else
		{
			int32& Attempts = RoundSpawnAttempts.FindOrAdd(Controller);
			++Attempts;
			if (Attempts < 3)
			{
				PendingRoundSpawns.Add(Controller);
			}
			else
			{
				const FClutchRosterEntry* Entry = ClutchState->FindEntry(PlayerState);
				if (Entry)
				{
					ClutchState->SetPlayerRoundState(
						PlayerState, Entry->PlayerRole, EClutchStatus::Eliminated, Entry->HitsTaken);
				}
				EnterSpectating(Controller, FindActiveTeammate(PlayerState));
				UE_LOG(LogClutch, Error, TEXT("Failed to spawn active round player %s after %d attempts"),
					*PlayerState->PlayerName, Attempts);
			}
		}
		break;
	}

	if (PendingRoundSpawns.Num() > 0)
	{
		GetWorldTimerManager().SetTimer(
			SpawnQueueTimerHandle, this, &AClutchGameMode::ProcessNextRoundSpawn, 0.05f, false);
	}
	else
	{
		FinalizeRoundStart();
	}
}


void AClutchGameMode::FinalizeRoundStart()
{
	bAllowRoundSpawns = false;
	if (!ClutchState || bEndingRound)
	{
		return;
	}

	// Remove any setup-phase projectiles before releasing movement. Hitscan
	// setup shots already dealt zero damage; projectile shots must not survive
	// across the Combat boundary.
	for (TActorIterator<AUTProjectile> It(GetWorld()); It; ++It)
	{
		if (!It->IsPendingKillPending())
		{
			It->Destroy();
		}
	}
	ReleaseRoundPawns();

	const float Now = UTGameState
		? UTGameState->GetServerWorldTimeSeconds()
		: GetWorld()->GetTimeSeconds();
	ClutchState->StartRoundClock(
		Now,
		Now + PoleUnlockDelaySeconds,
		Now + RoundDurationSeconds);
	const float UnlockDelay = PoleUnlockDelaySeconds;
	const float TimeoutDelay = FMath::Max(0.01f, RoundDurationSeconds);

	if (UnlockDelay <= KINDA_SMALL_NUMBER)
	{
		ActivatePole();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			PoleUnlockTimerHandle, this, &AClutchGameMode::ActivatePole, UnlockDelay, false);
	}
	GetWorldTimerManager().SetTimer(
		RoundTimeoutTimerHandle, this, &AClutchGameMode::HandleRoundTimeout, TimeoutDelay, false);

	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (Entry.PlayerState && Entry.PlayerStatus != EClutchStatus::Active)
		{
			if (AController* Controller = Cast<AController>(Entry.PlayerState->GetOwner()))
			{
				EnterSpectating(Controller, FindActiveTeammate(Entry.PlayerState));
			}
		}
	}

	UE_LOG(LogClutch, Log, TEXT("Round %d started: team %d attacks with %s"),
		ClutchState->RoundNumber,
		ClutchState->AttackingTeamIndex,
		ClutchState->ActiveAttacker ? *ClutchState->ActiveAttacker->PlayerName : TEXT("none"));
	QueueWinCheck();
}


void AClutchGameMode::ActivatePole()
{
	if (!HasAuthority() || bEndingRound || !ClutchState
		|| ClutchState->Phase != EClutchRoundPhase::Combat)
	{
		return;
	}

	ClutchState->SetPhase(EClutchRoundPhase::Capture);
	UE_LOG(LogClutch, Verbose, TEXT("Round %d pole unlocked"), ClutchState->RoundNumber);
}


void AClutchGameMode::HandleRoundTimeout()
{
	if (!ClutchState || bEndingRound || !IsCombatPhase())
	{
		return;
	}

	bRoundTimedOut = true;
	QueueWinCheck();
}


void AClutchGameMode::QueueWinCheck()
{
	if (!HasAuthority() || bEndingRound || !IsCombatPhase())
	{
		return;
	}

	if (!bWinCheckQueued)
	{
		bWinCheckQueued = true;
		GetWorldTimerManager().SetTimerForNextTick(
			this, &AClutchGameMode::EvaluateRoundWinConditions);
	}
}


void AClutchGameMode::EvaluateRoundWinConditions()
{
	bWinCheckQueued = false;
	if (!ClutchState || bEndingRound || !IsCombatPhase())
	{
		return;
	}

	AUTPlayerState* Attacker = GetActiveAttacker();
	AController* AttackerController = Attacker ? Cast<AController>(Attacker->GetOwner()) : nullptr;
	AUTCharacter* AttackerCharacter = AttackerController
		? Cast<AUTCharacter>(AttackerController->GetPawn())
		: nullptr;
	bool bAttackerAlive = AttackerCharacter && !AttackerCharacter->IsDead();
	if (const FClutchRosterEntry* Entry = Attacker ? ClutchState->FindEntry(Attacker) : nullptr)
	{
		bAttackerAlive = Entry->PlayerStatus == EClutchStatus::Active
			&& Entry->HitsTaken < ClutchState->MaxAttackerHits;
	}

	const uint8 Winner = AClutchRoundState::ResolveRoundWinner(
		ClutchState->AttackingTeamIndex,
		bRoundTimedOut,
		CountActiveDefenders(),
		bAttackerAlive,
		bPoleCaptured);

	if (Winner != AClutchRoundState::NoTeam)
	{
		FName Reason(TEXT("AttackerEliminated"));
		if (bRoundTimedOut)
		{
			Reason = FName(TEXT("Timeout"));
		}
		else if (Winner == ClutchState->AttackingTeamIndex
			&& CountActiveDefenders() <= 0)
		{
			Reason = FName(TEXT("Elimination"));
		}
		else if (Winner == ClutchState->AttackingTeamIndex && bPoleCaptured)
		{
			Reason = FName(TEXT("Capture"));
		}
		EndRound(Winner, Reason);
	}
}


void AClutchGameMode::EndRound(uint8 WinningTeamIndex, FName Reason)
{
	if (!HasAuthority() || bEndingRound || !ClutchState || WinningTeamIndex > 1
		|| !Teams.IsValidIndex(WinningTeamIndex) || !Teams[WinningTeamIndex])
	{
		return;
	}

	bEndingRound = true;
	bAllowRoundSpawns = false;
	PendingRoundSpawns.Reset();
	GetWorldTimerManager().ClearTimer(PoleUnlockTimerHandle);
	GetWorldTimerManager().ClearTimer(RoundTimeoutTimerHandle);
	GetWorldTimerManager().ClearTimer(SpawnQueueTimerHandle);

	ClutchState->FinishRound(WinningTeamIndex, Reason, false);
	const int32 OldScore = Teams[WinningTeamIndex]->Score;
	Teams[WinningTeamIndex]->Score = OldScore + 1;
	Teams[WinningTeamIndex]->ForceNetUpdate();
	BroadcastScoreUpdate(nullptr, Teams[WinningTeamIndex], OldScore);

	const int32 LosingTeamIndex = 1 - WinningTeamIndex;
	const int32 LosingScore = Teams.IsValidIndex(LosingTeamIndex) && Teams[LosingTeamIndex]
		? Teams[LosingTeamIndex]->Score
		: 0;
	const bool bMatchWon = AClutchRoundState::HasWonMatch(
		Teams[WinningTeamIndex]->Score,
		LosingScore,
		GoalScore,
		bWinByTwo,
		MinimumWinMargin);

	AUTPlayerState* PreferredTarget = FindBestPlayerOnTeam(WinningTeamIndex);
	AController* PreferredController = PreferredTarget
		? Cast<AController>(PreferredTarget->GetOwner())
		: nullptr;
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* Controller = It->Get();
		if (Controller && Controller != PreferredController
			&& Controller->PlayerState && !Controller->PlayerState->bOnlySpectator)
		{
			EnterSpectating(Controller, PreferredTarget);
		}
	}
	// Switch the view-target owner last so every other client captures a stable
	// pawn target before that controller enters Spectating.
	if (PreferredController && PreferredController->PlayerState
		&& !PreferredController->PlayerState->bOnlySpectator)
	{
		EnterSpectating(PreferredController, PreferredTarget);
	}

	UE_LOG(LogClutch, Log, TEXT("Round %d ended: team %d (%s), score %d-%d"),
		ClutchState->RoundNumber, WinningTeamIndex, *Reason.ToString(),
		Teams[WinningTeamIndex]->Score, LosingScore);

	if (bMatchWon)
	{
		ClutchState->FinishRound(WinningTeamIndex, Reason, true);
		EndGame(PreferredTarget, FName(TEXT("scorelimit")));
		return;
	}

	NextAttackingTeamIndex = static_cast<uint8>(1 - ClutchState->AttackingTeamIndex);
	GetWorldTimerManager().SetTimer(
		IntermissionTimerHandle, this, &AClutchGameMode::BeginNextRound,
		FMath::Max(0.01f, IntermissionSeconds), false);
}


void AClutchGameMode::BeginNextRound()
{
	if (!HasAuthority() || HasMatchEnded())
	{
		return;
	}

	bEndingRound = false;
	BeginRound();
}


bool AClutchGameMode::PlayerCanRestart_Implementation(APlayerController* Player)
{
	if (!HasMatchStarted())
	{
		return Super::PlayerCanRestart_Implementation(Player);
	}

	AUTPlayerState* PlayerState = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	return bAllowRoundSpawns && Player && !Player->GetPawn()
		&& IsActiveRoundPlayer(PlayerState) && !PlayerState->bOutOfLives
		&& Super::PlayerCanRestart_Implementation(Player);
}


void AClutchGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->GetPawn())
	{
		return;
	}

	if (!HasMatchStarted())
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	if (HasMatchEnded() && UTGameState && UTGameState->IsLineUpActive())
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	AUTPlayerState* PlayerState = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (!bAllowRoundSpawns || !IsActiveRoundPlayer(PlayerState) || PlayerState->bOutOfLives)
	{
		return;
	}

	Super::RestartPlayer(NewPlayer);
}


AActor* AClutchGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	AUTPlayerState* PlayerState = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	const FClutchRosterEntry* Entry = ClutchState && PlayerState
		? ClutchState->FindEntry(PlayerState)
		: nullptr;
	if (!Entry || Entry->PlayerStatus != EClutchStatus::Active
		|| Entry->PlayerRole == EClutchRole::None)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	APlayerStart* BestStart = nullptr;
	float BestRating = -MAX_FLT;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		APlayerStart* Candidate = *It;
		EClutchRole StartRole = EClutchRole::None;
		if (!GetClutchStartRole(Candidate, StartRole) || StartRole != Entry->PlayerRole)
		{
			continue;
		}

		// Role starts are independent of red/blue ownership because teams rotate
		// attack every round. Bypass AUTTeamGameMode's team-number rejection while
		// retaining stock occupancy/proximity scoring.
		const float Rating = AUTGameMode::RatePlayerStart(Candidate, Player);
		if (!BestStart || Rating > BestRating)
		{
			BestStart = Candidate;
			BestRating = Rating;
		}
	}

	return BestStart ? BestStart : Super::ChoosePlayerStart_Implementation(Player);
}


void AClutchGameMode::SetPlayerDefaults(APawn* PlayerPawn)
{
	AUTCharacter* Character = Cast<AUTCharacter>(PlayerPawn);
	AUTPlayerState* PlayerState = Character ? Cast<AUTPlayerState>(Character->PlayerState) : nullptr;
	const bool bAttacker = Character && IsActiveRole(PlayerState, EClutchRole::Attacker);

	if (bAttacker)
	{
		Character->HealthMax = AttackerHealth;
		Character->SuperHealthMax = AttackerHealth;
	}

	Super::SetPlayerDefaults(PlayerPawn);

	if (bAttacker)
	{
		Character->HealthMax = AttackerHealth;
		Character->SuperHealthMax = AttackerHealth;
		Character->Health = AttackerHealth;
		Character->OnHealthUpdated();
	}

	// Every Clutch pawn is released at the same round boundary. Stock UT spawn
	// protection would otherwise discard a legitimate opening hit until the
	// victim fires or its protection timer expires.
	if (Character)
	{
		Character->bSpawnProtectionEligible = false;
	}
}


void AClutchGameMode::GiveDefaultInventory(APawn* PlayerPawn)
{
	if (!HasMatchStarted())
	{
		Super::GiveDefaultInventory(PlayerPawn);
		return;
	}

	AUTCharacter* Character = Cast<AUTCharacter>(PlayerPawn);
	AUTPlayerState* PlayerState = Character ? Cast<AUTPlayerState>(Character->PlayerState) : nullptr;
	if (!Character || !PlayerState)
	{
		return;
	}

	Character->DefaultCharacterInventory.Empty();
	Character->DiscardAllInventory();
	for (AUTInventory* Preserved : PlayerState->PreservedKeepOnDeathInventoryList)
	{
		if (Preserved && !Preserved->IsPendingKillPending())
		{
			Preserved->Destroy();
		}
	}
	PlayerState->PreservedKeepOnDeathInventoryList.Empty();

	TSubclassOf<AUTInventory> RoleInventory;
	if (IsActiveRole(PlayerState, EClutchRole::Attacker) && AttackerWeaponClass)
	{
		RoleInventory = AttackerWeaponClass;
	}
	else if (IsActiveRole(PlayerState, EClutchRole::Defender) && DefenderWeaponClass)
	{
		RoleInventory = DefenderWeaponClass;
	}
	if (RoleInventory)
	{
		Character->K2_CreateInventory(RoleInventory, true);
	}
}


void AClutchGameMode::DiscardInventory(APawn* Other, AController* Killer)
{
	if (AUTCharacter* Character = Cast<AUTCharacter>(Other))
	{
		Character->DiscardAllInventory();
	}
}


bool AClutchGameMode::CheckRelevance_Implementation(AActor* Other)
{
	// Clutch maps are arenas; role loadouts are the only allowed inventory.
	if (Other && Other->IsA(AUTPickup::StaticClass()))
	{
		return false;
	}
	return Super::CheckRelevance_Implementation(Other);
}


bool AClutchGameMode::ModifyDamage_Implementation(int32& Damage, FVector& Momentum,
	APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo,
	AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	Super::ModifyDamage_Implementation(
		Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);

	if (Damage <= 0 || !IsCombatPhase())
	{
		if (!IsCombatPhase())
		{
			Damage = 0;
			Momentum = FVector::ZeroVector;
		}
		return true;
	}

	AUTPlayerState* VictimState = Injured ? Cast<AUTPlayerState>(Injured->PlayerState) : nullptr;
	AUTPlayerState* AttackerState = InstigatedBy
		? Cast<AUTPlayerState>(InstigatedBy->PlayerState)
		: nullptr;
	if (!VictimState || !AttackerState)
	{
		return true; // preserve environmental damage
	}

	if (!IsActiveRoundPlayer(VictimState) || !IsActiveRoundPlayer(AttackerState))
	{
		Damage = 0;
		Momentum = FVector::ZeroVector;
		return true;
	}

	if (IsActiveRole(AttackerState, EClutchRole::Defender)
		&& IsActiveRole(VictimState, EClutchRole::Attacker))
	{
		Damage = DefenderDamagePerHit;
	}
	else if (IsActiveRole(AttackerState, EClutchRole::Attacker)
		&& IsActiveRole(VictimState, EClutchRole::Defender))
	{
		Damage = AttackerDamagePerHit;
	}
	else
	{
		Damage = 0;
		Momentum = FVector::ZeroVector;
	}
	return true;
}


void AClutchGameMode::ScoreDamage_Implementation(
	int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);

	if (!ClutchState || DamageAmount <= 0 || !IsCombatPhase())
	{
		return;
	}
	if (IsActiveRole(Attacker, EClutchRole::Defender)
		&& IsActiveRole(Victim, EClutchRole::Attacker))
	{
		ClutchState->AddAttackerHit(Victim);
		QueueWinCheck();
	}
}


void AClutchGameMode::ScoreKill_Implementation(AController* Killer, AController* Other,
	APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	if (!IsCombatPhase())
	{
		return;
	}

	AUTPlayerState* VictimState = Other ? Cast<AUTPlayerState>(Other->PlayerState) : nullptr;
	const FClutchRosterEntry* VictimEntry = ClutchState && VictimState
		? ClutchState->FindEntry(VictimState)
		: nullptr;
	const bool bWasActive = VictimEntry && VictimEntry->PlayerStatus == EClutchStatus::Active;
	const EClutchRole VictimRole = VictimEntry ? VictimEntry->PlayerRole : EClutchRole::None;
	const uint8 VictimHits = VictimEntry ? VictimEntry->HitsTaken : 0;

	if (bWasActive)
	{
		ClutchState->SetPlayerRoundState(
			VictimState, VictimRole, EClutchStatus::Eliminated, VictimHits);
		VictimState->SetOutOfLives(true);
		VictimState->ForceNetUpdate();
	}

	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	if (bWasActive)
	{
		QueueWinCheck();
		GetWorldTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateUObject(
				this, &AClutchGameMode::DeferredEnterSpectating, VictimState));
	}
}


bool AClutchGameMode::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	// Team score changes occur atomically in EndRound, never per frag.
	return false;
}


bool AClutchGameMode::CanSpectate_Implementation(
	APlayerController* Viewer, APlayerState* ViewTarget)
{
	if (!Viewer || !ViewTarget)
	{
		return false;
	}

	AUTPlayerState* ViewerState = Cast<AUTPlayerState>(Viewer->PlayerState);
	AUTPlayerState* TargetState = Cast<AUTPlayerState>(ViewTarget);
	if (!ViewerState || ViewerState->bOnlySpectator || !ClutchState || !IsCombatPhase())
	{
		return Super::CanSpectate_Implementation(Viewer, ViewTarget);
	}

	const FClutchRosterEntry* TargetEntry = TargetState ? ClutchState->FindEntry(TargetState) : nullptr;
	if (!TargetEntry || TargetEntry->PlayerStatus != EClutchStatus::Active
		|| GetPlayerTeamIndex(ViewerState) != TargetEntry->TeamIndex)
	{
		return false;
	}

	AController* TargetController = Cast<AController>(TargetState->GetOwner());
	AUTCharacter* TargetCharacter = TargetController
		? Cast<AUTCharacter>(TargetController->GetPawn())
		: nullptr;
	return TargetCharacter && !TargetCharacter->IsDead();
}


void AClutchGameMode::ClearRoundPawns()
{
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* Controller = It->Get();
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (!Controller || !Pawn)
		{
			continue;
		}

		DiscardInventory(Pawn, Controller);
		Controller->UnPossess();
		Pawn->Destroy();
	}

	// A pawn can already be unpossessed by the death pipeline and therefore be
	// invisible to the controller pass. Remove those too; otherwise they survive
	// into the next round and appear as frozen TacCom/X-ray ghosts.
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		AUTCharacter* Character = Cast<AUTCharacter>(It->Get());
		if (Character && !Character->IsPendingKillPending())
		{
			Character->DiscardAllInventory();
			Character->Destroy();
		}
	}
}


void AClutchGameMode::FreezeRoundPawn(AController* Controller)
{
	AUTCharacter* Character = Controller ? Cast<AUTCharacter>(Controller->GetPawn()) : nullptr;
	if (Character && Character->GetCharacterMovement())
	{
		Character->StopFire(0);
		Character->StopFire(1);
		Character->GetCharacterMovement()->DisableMovement();
	}
	if (AUTPlayerController* PlayerController = Cast<AUTPlayerController>(Controller))
	{
		PlayerController->ClientIgnoreMoveInput(true);
	}
}


void AClutchGameMode::ReleaseRoundPawns()
{
	if (!ClutchState)
	{
		return;
	}
	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (!Entry.PlayerState || Entry.PlayerStatus != EClutchStatus::Active)
		{
			continue;
		}
		AController* Controller = Cast<AController>(Entry.PlayerState->GetOwner());
		AUTCharacter* Character = Controller ? Cast<AUTCharacter>(Controller->GetPawn()) : nullptr;
		if (Character && Character->GetCharacterMovement())
		{
			Character->GetCharacterMovement()->SetDefaultMovementMode();
		}
		if (AUTPlayerController* PlayerController = Cast<AUTPlayerController>(Controller))
		{
			PlayerController->ClientIgnoreMoveInput(false);
		}
	}
}


void AClutchGameMode::EnterPlaying(AController* Controller)
{
	AUTPlayerState* PlayerState = Controller ? Cast<AUTPlayerState>(Controller->PlayerState) : nullptr;
	if (!Controller || !PlayerState)
	{
		return;
	}

	PlayerState->SetOutOfLives(false);
	PlayerState->ForceNetUpdate();
	if (AUTPlayerController* PlayerController = Cast<AUTPlayerController>(Controller))
	{
		PlayerController->ChangeState(NAME_Playing);
		PlayerController->ClientGotoState(NAME_Playing);
	}
}


void AClutchGameMode::EnterSpectating(
	AController* Controller, AUTPlayerState* PreferredTarget)
{
	AUTPlayerState* PlayerState = Controller ? Cast<AUTPlayerState>(Controller->PlayerState) : nullptr;
	if (!Controller || !PlayerState || PlayerState->bOnlySpectator)
	{
		return;
	}

	PlayerState->SetOutOfLives(true);
	PlayerState->ForceNetUpdate();
	AUTPlayerController* PlayerController = Cast<AUTPlayerController>(Controller);
	if (!PlayerController)
	{
		return;
	}

	if (!PreferredTarget)
	{
		PreferredTarget = FindActiveTeammate(PlayerState);
	}

	AController* TargetController = PreferredTarget
		? Cast<AController>(PreferredTarget->GetOwner())
		: nullptr;
	AActor* StableViewTarget = TargetController ? TargetController->GetPawn() : nullptr;
	PlayerController->ChangeState(NAME_Spectating);
	PlayerController->ClientGotoState(NAME_Spectating);
	if (StableViewTarget)
	{
		PlayerController->SetViewTarget(StableViewTarget);
		PlayerController->bSpectateBehindView = false;
		PlayerController->BehindView(false);
	}
	else
	{
		PlayerController->ServerViewSelf();
	}
}


void AClutchGameMode::DeferredEnterSpectating(AUTPlayerState* PlayerState)
{
	if (!PlayerState || bEndingRound || !ClutchState)
	{
		return;
	}
	const FClutchRosterEntry* Entry = ClutchState->FindEntry(PlayerState);
	if (!Entry || Entry->PlayerStatus != EClutchStatus::Eliminated)
	{
		return;
	}
	if (AController* Controller = Cast<AController>(PlayerState->GetOwner()))
	{
		EnterSpectating(Controller, FindActiveTeammate(PlayerState));
	}
}


AUTPlayerState* AClutchGameMode::FindActiveTeammate(
	const AUTPlayerState* PlayerState) const
{
	if (!ClutchState || !PlayerState)
	{
		return nullptr;
	}
	const uint8 TeamIndex = GetPlayerTeamIndex(PlayerState);
	for (const FClutchRosterEntry& Entry : ClutchState->Roster)
	{
		if (Entry.PlayerState && Entry.PlayerState != PlayerState
			&& Entry.TeamIndex == TeamIndex
			&& Entry.PlayerStatus == EClutchStatus::Active)
		{
			AController* Controller = Cast<AController>(Entry.PlayerState->GetOwner());
			AUTCharacter* Character = Controller
				? Cast<AUTCharacter>(Controller->GetPawn())
				: nullptr;
			if (Character && !Character->IsDead())
			{
				return Entry.PlayerState;
			}
		}
	}
	return nullptr;
}


AActor* AClutchGameMode::ResolvePoleActor()
{
	AActor* NameFallback = nullptr;
	for (FActorIterator It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor == this)
		{
			continue;
		}
		if (!PoleActorTag.IsNone() && Actor->ActorHasTag(PoleActorTag))
		{
			return Actor;
		}

		const FString ActorName = Actor->GetName();
		if (!NameFallback && (ActorName.Contains(TEXT("ClutchPole"), ESearchCase::IgnoreCase)
			|| ActorName.Contains(TEXT("CapturePole"), ESearchCase::IgnoreCase)))
		{
			NameFallback = Actor;
		}
	}
	if (NameFallback)
	{
		return NameFallback;
	}

	// Compatibility with dumb CTF-derived CL maps. Their red flag base is the
	// historical defender objective; teams still alternate the attacker role.
	TArray<AUTCTFFlagBase*> FlagBases;
	AUTCTFFlagBase* SelectedFlagBase = nullptr;
	for (TActorIterator<AUTCTFFlagBase> It(GetWorld()); It; ++It)
	{
		AUTCTFFlagBase* FlagBase = *It;
		FlagBases.Add(FlagBase);
		if (!SelectedFlagBase || FlagBase->TeamNum == 0)
		{
			SelectedFlagBase = FlagBase;
		}
	}
	if (SelectedFlagBase)
	{
		for (AUTCTFFlagBase* FlagBase : FlagBases)
		{
			ConfigureCTFFlagBaseForClutch(
				FlagBase, FlagBase == SelectedFlagBase);
		}
		UE_LOG(LogClutch, Log,
			TEXT("Using CTF flag base '%s' as the single Clutch pole"),
			*SelectedFlagBase->GetName());
		return SelectedFlagBase;
	}

	TArray<AUTGenericObjectivePoint*> ObjectivePoints;
	for (TActorIterator<AUTGenericObjectivePoint> It(GetWorld()); It; ++It)
	{
		ObjectivePoints.Add(*It);
	}
	if (ObjectivePoints.Num() == 1)
	{
		return ObjectivePoints[0];
	}

	// Last-resort compatibility for the old dumb CL maps: their custom
	// ClutchSpawn actors identify attacker/defender sides even when no pole
	// marker survived. The midpoint of the two spawn centroids is deterministic
	// and gives the map author a functional capture objective immediately.
	FVector RoleLocationSums[2] = { FVector::ZeroVector, FVector::ZeroVector };
	int32 RoleStartCounts[2] = { 0, 0 };
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		EClutchRole StartRole = EClutchRole::None;
		if (GetClutchStartRole(*It, StartRole))
		{
			const int32 RoleIndex = StartRole == EClutchRole::Attacker ? 0
				: (StartRole == EClutchRole::Defender ? 1 : INDEX_NONE);
			if (RoleIndex != INDEX_NONE)
			{
				RoleLocationSums[RoleIndex] += It->GetActorLocation();
				++RoleStartCounts[RoleIndex];
			}
		}
	}
	if (RoleStartCounts[0] > 0 && RoleStartCounts[1] > 0)
	{
		const FVector AttackerCentroid = RoleLocationSums[0] / RoleStartCounts[0];
		const FVector DefenderCentroid = RoleLocationSums[1] / RoleStartCounts[1];
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.Name = FName(TEXT("ClutchPoleFallback"));
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Fallback = GetWorld()->SpawnActor<AActor>(
			AActor::StaticClass(),
			(AttackerCentroid + DefenderCentroid) * 0.5f,
			FRotator::ZeroRotator,
			Params);
		if (Fallback)
		{
			UE_LOG(LogClutch, Warning,
				TEXT("No explicit pole marker found; using role-spawn midpoint at %s"),
				*Fallback->GetActorLocation().ToString());
			return Fallback;
		}
	}

	UE_LOG(LogClutch, Warning,
		TEXT("No pole marker found. Tag a map actor '%s' or place exactly one AUTGenericObjectivePoint."),
		*PoleActorTag.ToString());
	return nullptr;
}


void AClutchGameMode::UpdatePole(float DeltaSeconds)
{
	if (!ClutchState || !PoleActor || DeltaSeconds <= 0.0f)
	{
		return;
	}

	bool bAttackerPresent = false;
	bool bDefenderPresent = false;
	const FVector PoleLocation = PoleActor->GetActorLocation();
	const float RadiusSq = FMath::Square(PoleCaptureRadius);

	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		AUTCharacter* Character = Cast<AUTCharacter>(It->Get());
		if (!Character || Character->IsDead())
		{
			continue;
		}
		const FVector Offset = Character->GetActorLocation() - PoleLocation;
		if (FVector2D(Offset.X, Offset.Y).SizeSquared() > RadiusSq
			|| FMath::Abs(Offset.Z) > PoleCaptureHalfHeight)
		{
			continue;
		}

		AUTPlayerState* PlayerState = Cast<AUTPlayerState>(Character->PlayerState);
		bAttackerPresent |= IsActiveRole(PlayerState, EClutchRole::Attacker);
		bDefenderPresent |= IsActiveRole(PlayerState, EClutchRole::Defender);
	}

	float NewProgress = ClutchState->PoleProgress;
	if (bAttackerPresent && !bDefenderPresent)
	{
		NewProgress += DeltaSeconds * (100.0f / FMath::Max(0.1f, PoleCaptureSeconds));
	}
	else if (!bAttackerPresent)
	{
		NewProgress -= DeltaSeconds * (100.0f / FMath::Max(0.1f, PoleDecaySeconds));
	}
	// Attacker + defender is contested and deliberately freezes progress.

	ClutchState->SetPoleProgress(NewProgress);
	if (NewProgress >= 100.0f)
	{
		bPoleCaptured = true;
		QueueWinCheck();
	}
}


bool AClutchGameMode::IsCombatPhase() const
{
	return ClutchState && ClutchState->IsGameplayPhase() && !bEndingRound;
}


bool AClutchGameMode::IsActiveRoundPlayer(const AUTPlayerState* PlayerState) const
{
	if (!ClutchState || !PlayerState)
	{
		return false;
	}
	const FClutchRosterEntry* Entry = ClutchState->FindEntry(PlayerState);
	return Entry && Entry->PlayerStatus == EClutchStatus::Active;
}


bool AClutchGameMode::IsActiveRole(
	const AUTPlayerState* PlayerState, EClutchRole Role) const
{
	if (!ClutchState || !PlayerState)
	{
		return false;
	}
	const FClutchRosterEntry* Entry = ClutchState->FindEntry(PlayerState);
	return Entry && Entry->PlayerStatus == EClutchStatus::Active
		&& Entry->PlayerRole == Role;
}


int32 AClutchGameMode::CountActiveDefenders() const
{
	int32 Count = 0;
	if (ClutchState)
	{
		for (const FClutchRosterEntry& Entry : ClutchState->Roster)
		{
			if (Entry.PlayerState && Entry.PlayerRole == EClutchRole::Defender
				&& Entry.PlayerStatus == EClutchStatus::Active)
			{
				AController* Controller = Cast<AController>(Entry.PlayerState->GetOwner());
				AUTCharacter* Character = Controller
					? Cast<AUTCharacter>(Controller->GetPawn())
					: nullptr;
				if (Character && !Character->IsDead())
				{
					++Count;
				}
			}
		}
	}
	return Count;
}


AUTPlayerState* AClutchGameMode::GetActiveAttacker() const
{
	if (!ClutchState || !ClutchState->ActiveAttacker)
	{
		return nullptr;
	}
	const FClutchRosterEntry* Entry = ClutchState->FindEntry(ClutchState->ActiveAttacker);
	return Entry && Entry->PlayerRole == EClutchRole::Attacker
		&& Entry->PlayerStatus == EClutchStatus::Active
		? ClutchState->ActiveAttacker
		: nullptr;
}


void AClutchGameMode::MarkDisconnected(AUTPlayerState* PlayerState)
{
	if (!ClutchState || !PlayerState)
	{
		return;
	}
	const FClutchRosterEntry* Entry = ClutchState->FindEntry(PlayerState);
	const bool bWasActive = Entry && Entry->PlayerStatus == EClutchStatus::Active;
	ClutchState->DetachPlayer(PlayerState);
	if (bWasActive)
	{
		QueueWinCheck();
	}
}
