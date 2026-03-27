#include "WipeoutGame.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTHUD_InstantReplay.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "Sound/SoundBase.h"
#include "UTPlayerController.h"
#include "UTTeamInfo.h"
#include "UTTeamPlayerStart.h"
#include "UTDroppedPickup.h"
#include "UTPickupWeapon.h"
#include "UTPickupHealth.h"
#include "UTPickupAmmo.h"
#include "UTPickupInventory.h"
#include "Engine/DemoNetDriver.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "UTCountDownMessage.h"
#include "UTGameMessage.h"
#include "WipeoutHUD.h"


// ============================================================================
// CONSTRUCTOR
// ============================================================================

AUWipeoutGame::AUWipeoutGame(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("UTGameMode", "Wipeout", "Wipeout");
	bTeamGame = true;
	HUDClass = AWipeoutHUD::StaticClass();

	// Team defaults
	NumTeams = 2;
	bBalanceTeams = true;
	bUseTeamStarts = false;
	bAnnounceTeam = true;
	bRecordReplays = true;

	// Sound defaults
	RedTeamVictorySound = nullptr;
	BlueTeamVictorySound = nullptr;
	RoundDrawSound = nullptr;
	MatchVictorySound = nullptr;
	RedTeamDominatingSound = nullptr;
	BlueTeamDominatingSound = nullptr;
	RedTeamTakesLeadSound = nullptr;
	BlueTeamTakesLeadSound = nullptr;
	OvertimeAnnouncementSound = nullptr;

	// Round defaults
	bForceRespawn = false;
	bHasRespawnChoices = false;
	ScoreLimit = 7;
	RoundTimeSeconds = 120; // Wipeout rounds run longer than elim (respawns = more time needed)
	bRoundInProgress = false;
	bAllowPlayerRespawns = false;
	LastRoundWinningTeamIndex = INDEX_NONE;
	AwardDisplayTime = 5.f;
	PreRoundCountdown = 3.f;
	SpectateDelay = 1.f;  // Shorter spectate delay since you're coming back
	TotalRoundsPlayed = 0;
	bWinByTwo = false;
	bWarmupMode = false;
	bCompetitiveAutoPause = false;
	useBPSpecFunction = false;

	// Wipeout-specific defaults: escalating respawn delays
	// Indexed by team death count. Diabotical-inspired curve.
	// Death 0 = 2s, Death 1 = 4s, Death 2 = 8s, etc.
	RespawnDelays.Add(2.0f);   // 1st team death
	RespawnDelays.Add(4.0f);   // 2nd team death
	RespawnDelays.Add(8.0f);   // 3rd team death
	RespawnDelays.Add(14.0f);  // 4th team death
	RespawnDelays.Add(22.0f);  // 5th team death
	RespawnDelays.Add(30.0f);  // 6th+ team deaths (cap)

	RespawnProtectionTime = 1.5f;
	WipeoutGracePeriod = 0.15f;
	bTeamSharedDeathCounter = true;

	// Link Gun beam teammate healing (5 HP/sec, capped at 100)
	LinkHealPerSecond = 5.0f;
	LinkHealMaxHP = 100;

	// Wipeout runtime
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	Team0RoundDamage = 0.f;
	Team1RoundDamage = 0.f;
	HighDamageCarryThreshold = 60.0f;

	// Replay
	RoundWinningKiller = nullptr;
	WinningKillerPawn = nullptr;
	RoundWinningKillTime = 0.0f;

	// Scoring — default GoalScore, can be overridden via URL (?GoalScore=X) or BP subclass
	GoalScore = 5;

	// Overtime
	bOvertimeEnabled = true;
	OvertimeStartDelay = 5.0f;
	OvertimeBaseDamage = 5.0f;
	OvertimeDamageMultiplier = 1.5f;
	OvertimeMaxDamage = 0.0f;
	bOvertimeNonLethal = false;
	OvertimeWaveInterval = 5.0f;
	OvertimeDamageType = UUTDamageType::StaticClass();
	CurrentOvertimeWave = 0;
	CurrentWaveDamage = 0.0f;

	// Spawn defaults
	MinimumEnemySpawnDistance = 2800.0f;
	MinimumEnemyHorizontalDistance = 3000.0f;
	MidRoundMinEnemyDistance = 2000.0f;

	// Score tracking
	PreviousRedScore = 0;
	PreviousBlueScore = 0;
	bHasBroadcastTeamDominating = false;

	// Last alive tracking
	bTeam0LastAliveAnnounced = false;
	bTeam1LastAliveAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;
}


// ============================================================================
// REPLAYS
// ============================================================================

bool AUWipeoutGame::SupportsInstantReplay() const
{
	return true;
}


// ============================================================================
// MATCH STATE SHORTCUTS (BP-callable)
// ============================================================================

void AUWipeoutGame::BP_SetMatchState_RoundCooldown()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("RoundCooldown")));
	}
}

void AUWipeoutGame::BP_SetMatchState_Intermission()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("Intermission")));
	}
}

void AUWipeoutGame::BP_SetMatchState_InProgress()
{
	if (HasAuthority())
	{
		SetMatchState(MatchState::InProgress);
	}
}





// ============================================================================
// INIT
// ============================================================================

void AUWipeoutGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// Super::InitGame already parses ?GoalScore=X from the URL.
	// Only override if the URL explicitly provided a GoalScore option;
	// otherwise keep the BP default (GoalScore set in constructor).
	int32 URLGoalScore = UGameplayStatics::GetIntOption(Options, TEXT("GoalScore"), -1);
	if (URLGoalScore >= 0)
	{
		GoalScore = URLGoalScore;
	}
	TimeLimit = 0; // We manage time per-round
}

void AUWipeoutGame::BeginPlay()
{
	Super::BeginPlay();

	if (!bSpawnPointsInitialized)
	{
		InitializeSpawnPointSystem();
		bSpawnPointsInitialized = true;
	}

	PrecomputeSpawnLayouts();
}

void AUWipeoutGame::InitGameState()
{
	Super::InitGameState();

	// Set GameModeClass so clients can look up the correct DisplayName.
	// Clients need the NetcodePlus plugin installed, so they can load our class.
	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		GS->GameModeClass = GetClass(); // Use actual class (or BP subclass) for correct DisplayName
	}
}

void AUWipeoutGame::HandleMatchHasStarted()
{
	UE_LOG(LogGameMode, Warning, TEXT("=== Wipeout::HandleMatchHasStarted ==="));
	Super::HandleMatchHasStarted();
	bWarmupMode = false;
}


// ============================================================================
// STATE MACHINE — CallMatchStateChangeNotify
// (Same pattern as TeamArena: intercepts state transitions)
// ============================================================================

void AUWipeoutGame::CallMatchStateChangeNotify()
{
	UE_LOG(LogGameMode, Log, TEXT("Wipeout matchstate: %s"), *GetMatchState().ToString());

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		bWarmupMode = true;
	}
	else if (GetMatchState() == MatchState::PlayerIntro)
	{
		bWarmupMode = false;
		ResetSpawnSelectionForNewRound();
	}

	if (GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		HandleMatchIntermission();
	}
	else if (GetMatchState() == MatchState::InProgress)
	{
		if (TotalRoundsPlayed == 0)
		{
			Super::CallMatchStateChangeNotify(); // Triggers HandleMatchHasStarted
		}
		bWarmupMode = false;
		StartNextRound();
	}
	else
	{
		Super::CallMatchStateChangeNotify();
	}
}


// ============================================================================
// DEFAULT TIMER — Drives intermission countdown + round clock
// ============================================================================

void AUWipeoutGame::DefaultTimer()
{
	if (GetWorld()->WorldType == EWorldType::EditorPreview) return;
	if (IsPendingKill() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS == nullptr || GS->IsPendingKill() || GetWorld()->bIsTearingDown) return;

	HandleServerManagement();

	// --- Intermission Logic ---
	if (GS->GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		if (IntermissionSecondsRemaining > 0)
		{
			--IntermissionSecondsRemaining;
			BP_OnSetIntermission(true, IntermissionSecondsRemaining);

			if (IntermissionSecondsRemaining > 0 && IntermissionSecondsRemaining <= 3)
			{
				BroadcastLocalized(this, UUTCountDownMessage::StaticClass(), IntermissionSecondsRemaining, nullptr, nullptr, nullptr);
			}
		}
		else
		{
			BP_OnSetIntermission(false, IntermissionSecondsRemaining);
			CleanupWorldForNewRound();
			SetMatchState(MatchState::InProgress);
		}
		return;
	}

	// --- Round Active Logic ---
	if (bRoundInProgress)
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);

		int32 RoundRemain = 0;
		if (RoundEndTimeSeconds > 0.f)
		{
			RoundRemain = FMath::Max(0, (int32)FMath::CeilToInt(RoundEndTimeSeconds - GetWorld()->GetTimeSeconds()));

			BP_OnSetRound(true, RoundRemain, LastRoundWinningTeamIndex,
				Alive0, Alive1, Team0DeathCount, Team1DeathCount);

			if (RoundRemain == 0)
			{
				// Time is up. Check state.
				if (Alive0 > 0 && Alive1 > 0)
				{
					if (bOvertimeEnabled)
					{
						if (!OvertimeWaveTimerHandle.IsValid())
						{
							StartOvertime();
						}
					}
					else
					{
						const int32 Winner = GetTiebreakWinnerByTeamHealth();
						FTimerDelegate TimerDelegate;
						TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), Winner, FName(TEXT("TimeTiebreak")));
						GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.1f, false);
					}
				}
				else
				{
					CheckWipeoutCondition();
				}
				return;
			}
		}

		// Normal tick: check for wipeout
		CheckWipeoutCondition();

		// Check for "last player alive" situations (clutch)
		if (Alive0 == 1 && Team0StartingSize > 1 && !bTeam0LastAliveAnnounced)
		{
			AUTPlayerState* LastPS = FindAliveOnTeamPS(0);
			if (LastPS)
			{
				bTeam0LastAliveAnnounced = true;
				int32 PendingT0 = CountPendingRespawnsOnTeam(0);
				// Only announce if no one is about to respawn imminently
				if (PendingT0 == 0)
				{
					BP_OnLastPlayerAlive(0, LastPS, Alive1);
					OnClutchSituationStarted.Broadcast(LastPS, Alive1);
				}
			}
		}
		if (Alive1 == 1 && Team1StartingSize > 1 && !bTeam1LastAliveAnnounced)
		{
			AUTPlayerState* LastPS = FindAliveOnTeamPS(1);
			if (LastPS)
			{
				bTeam1LastAliveAnnounced = true;
				int32 PendingT1 = CountPendingRespawnsOnTeam(1);
				if (PendingT1 == 0)
				{
					BP_OnLastPlayerAlive(1, LastPS, Alive0);
					OnClutchSituationStarted.Broadcast(LastPS, Alive0);
				}
			}
		}

		Super::DefaultTimer();
	}
}


// ============================================================================
// WIPEOUT RESPAWN SYSTEM — The core new mechanic
// ============================================================================

float AUWipeoutGame::ComputeRespawnDelay(int32 TeamIndex, AUTPlayerState* PS) const
{
	int32 DeathIndex = 0;

	if (bTeamSharedDeathCounter)
	{
		// Team-wide escalation (Diabotical default)
		DeathIndex = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;
	}
	else
	{
		// Per-player escalation (alternative mode)
		const int32* PlayerDeaths = PlayerDeathCounts.Find(PS);
		DeathIndex = PlayerDeaths ? *PlayerDeaths : 0;
	}

	return GetRespawnDelayForDeathIndex(DeathIndex);
}


float AUWipeoutGame::GetRespawnDelayForDeathIndex(int32 DeathIndex) const
{
	if (RespawnDelays.Num() == 0)
	{
		return 5.0f; // Fallback
	}

	// Clamp to last element (the cap)
	int32 Index = FMath::Clamp(DeathIndex, 0, RespawnDelays.Num() - 1);
	return RespawnDelays[Index];
}


float AUWipeoutGame::GetCurrentRespawnDelay(int32 TeamIndex) const
{
	int32 DeathCount = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;
	return GetRespawnDelayForDeathIndex(DeathCount);
}


float AUWipeoutGame::GetPlayerRespawnTimeRemaining(AUTPlayerState* PS) const
{
	if (!PS) return 0.f;

	const FPendingRespawn* Pending = PendingRespawns.Find(PS);
	if (!Pending) return 0.f;

	float Elapsed = GetWorld()->GetTimeSeconds() - Pending->RespawnStartTime;
	return FMath::Max(0.f, Pending->TotalRespawnTime - Elapsed);
}


bool AUWipeoutGame::IsPlayerWaitingToRespawn(AUTPlayerState* PS) const
{
	if (!PS) return false;
	return PendingRespawns.Contains(PS);
}


int32 AUWipeoutGame::CountPendingRespawnsOnTeam(int32 TeamIndex) const
{
	int32 Count = 0;
	for (auto& Pair : PendingRespawns)
	{
		AUTPlayerState* PS = Pair.Key.Get();
		if (PS && PS->Team && PS->Team->TeamIndex == TeamIndex)
		{
			Count++;
		}
	}
	return Count;
}


void AUWipeoutGame::StartRespawnTimer(AUTPlayerState* DeadPS)
{
	if (!DeadPS || !DeadPS->Team) return;

	const int32 TeamIndex = DeadPS->Team->TeamIndex;

	// Increment death counter BEFORE computing delay
	// (so death 0 uses index 0, death 1 uses index 1, etc.)
	if (TeamIndex == 0) { Team0DeathCount++; }
	else                { Team1DeathCount++; }

	// Track per-player deaths too
	int32& PlayerDeaths = PlayerDeathCounts.FindOrAdd(DeadPS);
	PlayerDeaths++;

	// Compute delay based on whichever mode we're in
	float RespawnDelay = ComputeRespawnDelay(TeamIndex, DeadPS);

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: %s died (Team %d, death #%d). Respawn in %.1fs"),
		*DeadPS->PlayerName, TeamIndex,
		(TeamIndex == 0) ? Team0DeathCount : Team1DeathCount,
		RespawnDelay);

	// Create the pending respawn entry
	FPendingRespawn& Pending = PendingRespawns.FindOrAdd(DeadPS);
	Pending.TotalRespawnTime = RespawnDelay;
	Pending.RespawnStartTime = GetWorld()->GetTimeSeconds();
	Pending.DeathIndex = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;

	// Clear any existing timer for this player (safety)
	if (Pending.RespawnTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(Pending.RespawnTimerHandle);
	}

	// Set the respawn timer
	FTimerDelegate TimerDelegate;
	TimerDelegate.BindUFunction(this, FName("OnRespawnTimerFired"), DeadPS);
	GetWorldTimerManager().SetTimer(Pending.RespawnTimerHandle, TimerDelegate, RespawnDelay, false);

	// Update replicated PlayerState fields so HUD/Scoreboard can display
	DeadPS->RespawnWaitTime = RespawnDelay;
	DeadPS->RespawnTime = RespawnDelay;
	DeadPS->ForceNetUpdate();

	// Notify Blueprint
	BP_OnPlayerWaitingRespawn(DeadPS, RespawnDelay,
		(TeamIndex == 0) ? Team0DeathCount : Team1DeathCount);

	// Force spectate during respawn wait
	// Small delay to let death cam play
	FTimerHandle SpectateDelayHandle;
	FTimerDelegate SpectateDelegate;
	SpectateDelegate.BindLambda([this, DeadPS]()
	{
		if (DeadPS && !DeadPS->IsPendingKill() && bRoundInProgress && IsPlayerWaitingToRespawn(DeadPS))
		{
			ForceTeamSpectate(DeadPS);
		}
	});
	GetWorldTimerManager().SetTimer(SpectateDelayHandle, SpectateDelegate, SpectateDelay, false);
}


void AUWipeoutGame::OnRespawnTimerFired(AUTPlayerState* PS)
{
	if (!PS || PS->IsPendingKill()) return;

	// Safety: round might have ended during our wait
	if (!bRoundInProgress)
	{
		PendingRespawns.Remove(PS);
		return;
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Respawn timer fired for %s"), *PS->PlayerName);

	// Remove from pending list
	PendingRespawns.Remove(PS);

	// Clear respawn state so HUD shows alive
	PS->bOutOfLives = false;
	PS->RespawnTime = 0.f;
	PS->RespawnWaitTime = 0.f;
	PS->ForceNetUpdate();

	// Get the controller
	AController* C = Cast<AController>(PS->GetOwner());
	if (!C)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: No controller for %s, cannot respawn"), *PS->PlayerName);
		return;
	}

	// Transition from spectating to playing
	if (AUTPlayerController* PC = Cast<AUTPlayerController>(C))
	{
		PC->ChangeState(NAME_Playing);
		PC->ClientGotoState(NAME_Playing);
	}

	// Actually respawn them (mid-round)
	bAllowPlayerRespawns = true;
	RestartPlayer(C);
	bAllowPlayerRespawns = false;

	if (C->GetPawn())
	{
		// Apply spawn protection
		if (RespawnProtectionTime > 0.f)
		{
			SpawnProtectedUntil.FindOrAdd(PS) = GetWorld()->GetTimeSeconds() + RespawnProtectionTime;
		}

		// Notify Blueprint
		BP_OnPlayerRespawnedMidRound(PS);

		UE_LOG(LogGameMode, Log, TEXT("Wipeout: %s respawned successfully"), *PS->PlayerName);
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Failed to spawn pawn for %s"), *PS->PlayerName);
	}
}


void AUWipeoutGame::CancelAllPendingRespawns()
{
	for (auto& Pair : PendingRespawns)
	{
		if (Pair.Value.RespawnTimerHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(Pair.Value.RespawnTimerHandle);
		}
	}
	PendingRespawns.Empty();

	if (RespawnCountdownTickHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(RespawnCountdownTickHandle);
	}
}


void AUWipeoutGame::CancelPendingRespawn(AUTPlayerState* PS)
{
	if (!PS) return;

	FPendingRespawn* Pending = PendingRespawns.Find(PS);
	if (Pending)
	{
		if (Pending->RespawnTimerHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(Pending->RespawnTimerHandle);
		}
		PendingRespawns.Remove(PS);
	}
}


void AUWipeoutGame::TickRespawnCountdowns()
{
	if (!bRoundInProgress) return;

	for (auto& Pair : PendingRespawns)
	{
		AUTPlayerState* PS = Pair.Key.Get();
		if (!PS) continue;

		float Remaining = GetPlayerRespawnTimeRemaining(PS);
		int32 SecondsRemaining = FMath::CeilToInt(Remaining);

		// Update replicated PlayerState so HUD shows countdown
		PS->RespawnTime = Remaining;

		if (SecondsRemaining > 0)
		{
			BP_OnPlayerRespawnCountdown(PS, SecondsRemaining);
		}
	}
}


// ============================================================================
// SCORE KILL — The key Wipeout change
// In elimination: death = permanent. In Wipeout: death = respawn timer.
// ============================================================================

void AUWipeoutGame::ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	// Call parent for individual player scoring (kills/deaths stats)
	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	if (GetMatchState() != MatchState::InProgress || !bRoundInProgress)
		return;

	AUTPlayerState* OtherPS = Other ? Cast<AUTPlayerState>(Other->PlayerState) : nullptr;
	if (!OtherPS) return;

	// Track the killing blow for potential replay
	if (Killer && Killer->PlayerState)
	{
		RoundWinningKillTime = GetWorld()->GetTimeSeconds();
		RoundWinningKiller = Cast<AUTPlayerState>(Killer->PlayerState);
		if (!RoundWinningKiller)
		{
			RoundWinningKiller = OtherPS;
		}
	}

	// Send death recap to victim (private message showing mutual damage)
	AUTPlayerState* KillerPS = Killer ? Cast<AUTPlayerState>(Killer->PlayerState) : nullptr;
	SendDeathRecap(OtherPS, KillerPS);

	// In Wipeout, death doesn't mean permanently out.
	// Mark as bOutOfLives temporarily (so GetAliveCounts returns correctly)
	// but start a respawn timer to bring them back.
	if (!OtherPS->bOutOfLives)
	{
		OtherPS->bOutOfLives = true;
		OtherPS->ForceNetUpdate();
	}

	// Clear life-damage tracking for the dead player
	ClearLifeDamageFor(OtherPS);

	// Start the escalating respawn timer
	StartRespawnTimer(OtherPS);

	// Check for wipeout with a brief grace period
	// (prevents false triggers from simultaneous kills)
	if (!GetWorldTimerManager().IsTimerActive(TH_RoundEndDelay))
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DeferredCheckWipeout"));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, WipeoutGracePeriod, false);
	}
}


// ============================================================================
// WIPEOUT WIN CONDITION CHECK
// A team loses when ALL players are dead simultaneously.
// Players with pending respawn timers count as dead.
// ============================================================================

void AUWipeoutGame::CheckWipeoutCondition()
{
	if (bWarmupMode) return;
	if (!bRoundInProgress || GetWorld()->GetTimeSeconds() < WinCheckHoldUntilSeconds) return;

	int32 Alive0, Alive1;
	GetAliveCounts(Alive0, Alive1);

	const bool Team0Wiped = (Alive0 == 0);
	const bool Team1Wiped = (Alive1 == 0);

	if (!Team0Wiped && !Team1Wiped)
	{
		return; // No wipeout
	}

	// A team is wiped if ALL players are dead AND none have pending respawn timers.
	// Wait... In Diabotical, the wipeout happens when all are dead simultaneously,
	// even if someone has a pending respawn. The respawn timer is the risk —
	// if your whole team dies before anyone respawns, you lose.
	//
	// So the check is simply: are all players on a team lacking a living pawn?
	// This is exactly what GetAliveCounts gives us.

	if (Team0Wiped && !Team1Wiped)
	{
		// Team 0 wiped out — Team 1 wins
		BP_OnTeamWipeout(0);
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team 0 WIPED OUT! Team 1 wins the round."));

		// Cancel Team 0's pending respawns (round is over)
		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 1, FName(TEXT("Wipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team1Wiped && !Team0Wiped)
	{
		BP_OnTeamWipeout(1);
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team 1 WIPED OUT! Team 0 wins the round."));

		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 0, FName(TEXT("Wipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team0Wiped && Team1Wiped)
	{
		// Both teams wiped simultaneously — draw
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: DOUBLE WIPEOUT — Round is a draw."));

		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), (int32)INDEX_NONE, FName(TEXT("DoubleWipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
}


void AUWipeoutGame::DeferredCheckWipeout()
{
	if (bRoundInProgress)
	{
		CheckWipeoutCondition();
	}
}


// ============================================================================
// INTERMISSION & ROUND FLOW
// ============================================================================

void AUWipeoutGame::HandleMatchIntermission()
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::HandleMatchIntermission"));
	ForceLosersToViewWinners(LastRoundWinningTeamIndex);
}


void AUWipeoutGame::StartIntermission(int32 Seconds)
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::StartIntermission: %d seconds"), Seconds);

	bRoundInProgress = false;
	IntermissionSecondsRemaining = FMath::Max(1, Seconds);
	RoundEndTimeSeconds = 0.f;

	// Stop overtime and cancel pending respawns
	StopOvertime();
	CancelAllPendingRespawns();

	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		BP_OnSetIntermission(true, IntermissionSecondsRemaining);
		GS->ForceNetUpdate();
	}

	SetMatchState(FName(TEXT("RoundCooldown")));
}


void AUWipeoutGame::StartNextRound()
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::StartNextRound"));

	if (bWarmupMode)
	{
		bRoundInProgress = false;
		return;
	}

	if (bAnnounceTeam)
	{
		bAnnounceTeam = false;
	}

	// Reset all Wipeout state for the new round
	RoundWinningKiller = nullptr;
	RoundWinningKillTime = 0.0f;
	LastRoundWinningTeamIndex = INDEX_NONE;
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	PlayerDeathCounts.Empty();
	CancelAllPendingRespawns();
	SpawnProtectedUntil.Empty();
	LinkHealAccumulator.Empty();
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();
	bTeam0LastAliveAnnounced = false;
	bTeam1LastAliveAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;

	ResetPlayersForNewRound();
	ResetSpawnSelectionForNewRound();
	SelectSpawnLayoutForRound();

	// Fallback if layout selection failed
	if (Team0SelectedSpawns.Num() == 0 || Team1SelectedSpawns.Num() == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Layout selection failed — insufficient spawn layouts on this map"));
	}

	// Spawn all players
	bAllowPlayerRespawns = true;
	int32 PlayersSpawned = 0;

	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		if (!C) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (PS && !PS->bOnlySpectator)
		{
			PS->bOutOfLives = false;
			PS->ForceNetUpdate();

			if (AUTPlayerController* PC = Cast<AUTPlayerController>(C))
			{
				PC->ChangeState(NAME_Playing);
				PC->ClientGotoState(NAME_Playing);
			}

			RestartPlayer(C);
			PlayersSpawned++;

			if (PS->Team)
			{
				if (PS->Team->TeamIndex == 0) { Team0StartingSize++; }
				else if (PS->Team->TeamIndex == 1) { Team1StartingSize++; }
			}
		}
	}

	bAllowPlayerRespawns = false;

	UE_LOG(LogGameMode, Warning, TEXT("Wipeout round starting: Team0=%d, Team1=%d"), Team0StartingSize, Team1StartingSize);

	// Set round timer
	if (RoundTimeSeconds > 0)
	{
		RoundEndTimeSeconds = GetWorld()->GetTimeSeconds() + RoundTimeSeconds;
	}
	else
	{
		RoundEndTimeSeconds = 0.f;
	}

	bRoundInProgress = true;

	if (AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);
		BP_OnSetRound(true, RoundTimeSeconds, LastRoundWinningTeamIndex,
			Alive0, Alive1, Team0DeathCount, Team1DeathCount);
		BP_OnSetIntermission(false, 0);
		GS->ForceNetUpdate();
	}

	BroadcastLocalized(this, UUTGameMessage::StaticClass(), 0, NULL, NULL, NULL);

	// Start the respawn countdown ticker (fires every 1s for BP_OnPlayerRespawnCountdown)
	GetWorldTimerManager().SetTimer(RespawnCountdownTickHandle, this,
		&AUWipeoutGame::TickRespawnCountdowns, 1.0f, true);

	WinCheckHoldUntilSeconds = GetWorld()->GetTimeSeconds() + 0.25f;
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);
	GetWorldTimerManager().SetTimer(
		InitialWinCheckHandle, this,
		&AUWipeoutGame::DelayedInitialWinCheck, 0.25f, false);
}


// ============================================================================
// END ROUND
// ============================================================================

void AUWipeoutGame::EndRoundForTeam(int32 WinnerTeamIndex, FName Reason)
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::EndRoundForTeam: Winner=%d, Reason=%s"),
		WinnerTeamIndex, *Reason.ToString());

	if (bWarmupMode || !bRoundInProgress) return;

	bRoundInProgress = false;
	RoundEndTimeSeconds = 0.f;
	LastRoundWinningTeamIndex = WinnerTeamIndex;
	TotalRoundsPlayed++;

	StopOvertime();
	CancelAllPendingRespawns();

	// Score the round
	bool bIsDraw = (WinnerTeamIndex == INDEX_NONE);
	if (!bIsDraw)
	{
		if (Teams.IsValidIndex(WinnerTeamIndex))
		{
			Teams[WinnerTeamIndex]->Score += 1;
			Teams[WinnerTeamIndex]->ForceNetUpdate();
			UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team %d wins round! Score: %d"), WinnerTeamIndex, Teams[WinnerTeamIndex]->Score);
		}
	}

	// Check for high damage carry
	CheckForHighDamageCarry(WinnerTeamIndex);

	// Check for match end
	if (!bIsDraw && Teams[WinnerTeamIndex]->Score >= GoalScore)
	{
		bool bCanEndMatch = true;
		if (bWinByTwo)
		{
			int32 OtherTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
			if (Teams.IsValidIndex(OtherTeamIndex))
			{
				int32 ScoreDifference = Teams[WinnerTeamIndex]->Score - Teams[OtherTeamIndex]->Score;
				bCanEndMatch = (ScoreDifference >= 2);
			}
		}

		if (bCanEndMatch)
		{
			BroadcastKillReplay();
			UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team %d wins the match!"), WinnerTeamIndex);

			FTimerHandle UnusedHandle;
			FTimerDelegate TimerDel;
			TimerDel.BindUFunction(this, FName("DelayedEndGame"), WinnerTeamIndex, FName(TEXT("ScoreLimit")));
			GetWorldTimerManager().SetTimer(UnusedHandle, TimerDel, 1.0f, false);
			return;
		}
	}

	// Game not over — proceed to intermission
	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);
		BP_OnSetRound(false, 0, LastRoundWinningTeamIndex,
			Alive0, Alive1, Team0DeathCount, Team1DeathCount);
		GS->ForceNetUpdate();
	}

	BroadcastRoundResults(WinnerTeamIndex, bIsDraw);
	CheckForDominationAndLead(WinnerTeamIndex);
	ForceLosersToViewWinners(WinnerTeamIndex);
	StartIntermission(AwardDisplayTime);
}


void AUWipeoutGame::DelayedEndRound(int32 WinnerTeamIndex, FName Reason)
{
	EndRoundForTeam(WinnerTeamIndex, Reason);
}


void AUWipeoutGame::DelayedEndGame(int32 WinnerTeamIndex, FName Reason)
{
	AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
	EndGame(BestPlayer, Reason);
}


void AUWipeoutGame::EndGame(AUTPlayerState* Winner, FName Reason)
{
	// Safety: only access replay data if demo recording is actually running.
	// Standalone PIE and servers without demo recording will crash in
	// PickMostCoolMoments if DemoNetDriver is null.
	if (GetWorld()->DemoNetDriver != nullptr)
	{
		PickMostCoolMoments();
	}

	Super::EndGame(Winner, Reason);
}


void AUWipeoutGame::DeferredHandleMatchStart()
{
	if (GetGameState<AUTGameState>())
	{
		HandleMatchHasStarted();
	}
}


// ============================================================================
// CHECK SCORE (win-by-two support)
// ============================================================================

bool AUWipeoutGame::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return false;

	int32 ScoreA = Teams[0]->Score;
	int32 ScoreB = Teams[1]->Score;

	if (!bWinByTwo)
	{
		return Super::CheckScore_Implementation(Scorer);
	}

	int32 LeadingScore = FMath::Max(ScoreA, ScoreB);
	int32 TrailingScore = FMath::Min(ScoreA, ScoreB);

	if (LeadingScore >= GoalScore && (LeadingScore - TrailingScore) >= 2)
	{
		int32 WinnerTeamIndex = (ScoreA > ScoreB) ? 0 : 1;
		AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
		EndGame(BestPlayer, TEXT("fraglimit"));
		return true;
	}

	return false;
}


// ============================================================================
// RESTART PLAYER — Allows mid-round spawns when timer fires
// ============================================================================

void AUWipeoutGame::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer) return;

	if (APlayerController* PC = Cast<APlayerController>(NewPlayer))
	{
		if (MustSpectate(PC)) return;
	}

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	if (NewPlayer->GetPawn()) return;

	AUTGameState* GS = GetGameState<AUTGameState>();
	bool bLineupIsActive = (GS && GS->ActiveLineUpHelper && GS->ActiveLineUpHelper->bIsPlacingPlayers);
	if (bLineupIsActive)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	const bool bShouldAllowSpawn = (bAllowPlayerRespawns || bWarmupMode || GetMatchState() == MatchState::WaitingToStart);

	if (bShouldAllowSpawn)
	{
		AActor* ChosenStart = nullptr;

		// Determine if this is a round-start spawn or mid-round respawn
		if (bRoundInProgress && Team0StartingSize > 0)
		{
			// Mid-round respawn — use dynamic spawn selection away from enemies
			ChosenStart = ChooseMidRoundSpawn(NewPlayer);
		}

		if (!ChosenStart)
		{
			// Round-start or fallback — use precomputed layout
			ChosenStart = ChoosePlayerStart_Implementation(NewPlayer);
		}

		OverriddenPlayerStart = ChosenStart;
		Super::RestartPlayer(NewPlayer);
		OverriddenPlayerStart = nullptr;

		if (!NewPlayer->GetPawn())
		{
			UE_LOG(LogGameMode, Warning, TEXT("Wipeout::RestartPlayer: FAILED to spawn pawn for %s"),
				NewPlayer->PlayerState ? *NewPlayer->PlayerState->PlayerName : TEXT("Unknown"));
		}
	}
}


// ============================================================================
// MID-ROUND SPAWN SELECTION
// Picks the spawn point furthest from all living enemies.
// ============================================================================

AActor* AUWipeoutGame::ChooseMidRoundSpawn(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team) return nullptr;

	const int32 MyTeam = PS->Team->TeamIndex;

	// Gather all living enemy positions
	TArray<FVector> EnemyPositions;
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		APawn* Pawn = It->Get();
		if (!Pawn) continue;

		AUTCharacter* UTC = Cast<AUTCharacter>(Pawn);
		if (UTC && UTC->IsDead()) continue;

		AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
		if (OtherPS && OtherPS->Team && OtherPS->Team->TeamIndex != MyTeam)
		{
			EnemyPositions.Add(Pawn->GetActorLocation());
		}
	}

	// If no living enemies, just use normal spawn
	if (EnemyPositions.Num() == 0)
	{
		return ChoosePlayerStart_Implementation(Player);
	}

	// Score all spawn points by minimum distance to any enemy
	APlayerStart* BestSpawn = nullptr;
	float BestMinDist = -1.0f;

	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		APlayerStart* Spawn = *It;
		if (!Spawn) continue;

		FVector SpawnLoc = Spawn->GetActorLocation();
		float MinDist = FLT_MAX;

		for (const FVector& EnemyLoc : EnemyPositions)
		{
			float Dist = (SpawnLoc - EnemyLoc).Size2D();
			MinDist = FMath::Min(MinDist, Dist);
		}

		// Reject spawns too close to enemies
		if (MinDist < MidRoundMinEnemyDistance)
			continue;

		// Add slight randomness to prevent always picking the exact same spot
		float Score = MinDist + FMath::FRandRange(0.f, 500.f);

		if (Score > BestMinDist)
		{
			BestMinDist = Score;
			BestSpawn = Spawn;
		}
	}

	if (BestSpawn)
	{
		UE_LOG(LogGameMode, Log, TEXT("Wipeout: Mid-round spawn for %s at %s (dist from enemy: %.0f)"),
			*PS->PlayerName, *BestSpawn->GetName(), BestMinDist);
		return BestSpawn;
	}

	// Fallback to round-start spawn
	return ChoosePlayerStart_Implementation(Player);
}


// ============================================================================
// CHOOSE PLAYER START (round-start — uses precomputed layouts)
// ============================================================================

AActor* AUWipeoutGame::ChoosePlayerStart_Implementation(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team) return nullptr;

	const int32 TeamIndex = PS->Team->TeamIndex;
	TArray<APlayerStart*>& SelectedSpawns = (TeamIndex == 0) ? Team0SelectedSpawns : Team1SelectedSpawns;

	if (SelectedSpawns.Num() == 0)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	APlayerStart* ChosenSpawn = nullptr;

	if (SelectedSpawns.Num() >= 2)
	{
		APlayerStart* SpawnA = SelectedSpawns[0];
		APlayerStart* SpawnB = SelectedSpawns[1];

		int32 CountAtA = 0, CountAtB = 0;
		float CheckRadiusSq = 150.f * 150.f;

		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (Pawn && Pawn->PlayerState)
			{
				AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
				if (OtherPS && OtherPS != PS && OtherPS->Team && OtherPS->Team->TeamIndex == TeamIndex)
				{
					FVector PawnLoc = Pawn->GetActorLocation();
					if (SpawnA && FVector::DistSquared(PawnLoc, SpawnA->GetActorLocation()) < CheckRadiusSq) CountAtA++;
					else if (SpawnB && FVector::DistSquared(PawnLoc, SpawnB->GetActorLocation()) < CheckRadiusSq) CountAtB++;
				}
			}
		}

		ChosenSpawn = (CountAtA <= CountAtB) ? SpawnA : SpawnB;
	}
	else
	{
		ChosenSpawn = SelectedSpawns[0];
	}

	return ChosenSpawn ? ChosenSpawn : Super::ChoosePlayerStart_Implementation(Player);
}


AActor* AUWipeoutGame::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	if (OverriddenPlayerStart)
	{
		return OverriddenPlayerStart;
	}
	return Super::FindPlayerStart_Implementation(Player, IncomingName);
}


// ============================================================================
// SPAWN PAWN (AlwaysSpawn to prevent collisions on stack spawns)
// ============================================================================

APawn* AUWipeoutGame::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	FRotator StartRotation(ForceInit);
	StartRotation.Yaw = StartSpot->GetActorRotation().Yaw;
	FVector StartLocation = StartSpot->GetActorLocation();

	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = Instigator;
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	APawn* ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, StartLocation), SpawnInfo);

	if (!ResultPawn)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout::SpawnDefaultPawnFor: Failed at %s"), *StartLocation.ToString());
	}

	return ResultPawn;
}


// ============================================================================
// MODIFY DAMAGE — Spawn protection for mid-round respawns + intermission invuln
// ============================================================================

// ---------------------------------------------------------------------------
// CheckRelevance — Strip pickups not appropriate for Wipeout:
//   - Remove Redeemer weapon base (and its weapon)
//   - Remove ALL health pickups and vials
//   - Remove armor EXCEPT ShieldBelt
//   - Remove powerups EXCEPT UDamage/Amp
//   - Keep all other weapon bases (ammo refill encourages movement)
// ---------------------------------------------------------------------------
bool AUWipeoutGame::CheckRelevance_Implementation(AActor* Other)
{
	if (!Other)
	{
		return Super::CheckRelevance_Implementation(Other);
	}

	// --- Weapon bases: only remove Redeemer ---
	AUTPickupWeapon* WeaponPickup = Cast<AUTPickupWeapon>(Other);
	if (WeaponPickup)
	{
		if (WeaponPickup->WeaponType)
		{
			FString WeaponName = WeaponPickup->WeaponType->GetName();
			if (WeaponName.Contains(TEXT("Redeemer")))
			{
				return false;
			}
		}
		return Super::CheckRelevance_Implementation(Other);
	}

	// --- Health pickups: remove all (health vials, health packs, etc.) ---
	if (Other->IsA(AUTPickupHealth::StaticClass()))
	{
		return false;
	}

	// --- Standalone ammo pickups: remove (weapon bases handle ammo refill) ---
	if (Other->IsA(AUTPickupAmmo::StaticClass()))
	{
		return false;
	}

	// --- Inventory pickups (armor + powerups): selective removal ---
	AUTPickupInventory* InvPickup = Cast<AUTPickupInventory>(Other);
	if (InvPickup && InvPickup->GetInventoryType())
	{
		FString InvName = InvPickup->GetInventoryType()->GetName();

		// Keep: ShieldBelt
		if (InvName.Contains(TEXT("ShieldBelt")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Keep: UDamage / Amp
		if (InvName.Contains(TEXT("UDamage")) || InvName.Contains(TEXT("Amp")) || InvName.Contains(TEXT("Berserk")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Remove everything else (Thighpads, Chest, Helmet, Jumpboots, Invisibility, etc.)
		return false;
	}

	return Super::CheckRelevance_Implementation(Other);
}

bool AUWipeoutGame::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
	AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	// Intermission invulnerability for winners
	if (GetMatchState() == FName(TEXT("RoundCooldown")) && Injured && Injured->PlayerState)
	{
		AUTPlayerState* InjuredPS = Cast<AUTPlayerState>(Injured->PlayerState);
		if (InjuredPS && InjuredPS->Team && InjuredPS->Team->TeamIndex == LastRoundWinningTeamIndex)
		{
			Damage = 0;
			return true;
		}
	}

	// Mid-round spawn protection
	if (bRoundInProgress && Injured && Injured->PlayerState && RespawnProtectionTime > 0.f)
	{
		AUTPlayerState* InjuredPS = Cast<AUTPlayerState>(Injured->PlayerState);
		if (InjuredPS)
		{
			float* ProtectedUntil = SpawnProtectedUntil.Find(InjuredPS);
			if (ProtectedUntil && GetWorld()->GetTimeSeconds() < *ProtectedUntil)
			{
				// Player is still under spawn protection
				// Allow self-damage to break protection (prevents abuse)
				if (InstigatedBy && InstigatedBy->PlayerState == Injured->PlayerState)
				{
					SpawnProtectedUntil.Remove(InjuredPS);
				}
				else
				{
					Damage = 0;
					return true;
				}
			}
			else if (ProtectedUntil)
			{
				// Protection expired, clean up
				SpawnProtectedUntil.Remove(InjuredPS);
			}
		}
	}

	// -----------------------------------------------------------------------
	// Link Gun beam teammate healing
	// When the beam (UTDMG_Link_Alt) hits a same-team player, heal them
	// instead of dealing damage. Rate: ~5 HP/sec (beam ticks ~8x/sec at
	// 0.12s interval, so we accumulate fractional healing per tick).
	// Capped at 100 HP (normal max). Works with stock Link Gun.
	// -----------------------------------------------------------------------
	if (LinkHealPerSecond > 0.f && DamageType && Injured && InstigatedBy)
	{
		FString DTName = DamageType->GetName();
		if (DTName.Contains(TEXT("Link_Alt")) || DTName.Contains(TEXT("LinkBeam")))
		{
			AUTCharacter* InjuredChar = Cast<AUTCharacter>(Injured);
			AUTPlayerState* InjuredPS = InjuredChar ? Cast<AUTPlayerState>(InjuredChar->PlayerState) : nullptr;
			AUTPlayerState* InstigatorPS = InstigatedBy ? Cast<AUTPlayerState>(InstigatedBy->PlayerState) : nullptr;

			if (InjuredPS && InstigatorPS && InjuredPS != InstigatorPS
				&& InjuredPS->Team && InstigatorPS->Team
				&& InjuredPS->Team->TeamIndex == InstigatorPS->Team->TeamIndex)
			{
				// Same team — heal instead of damage
				if (InjuredChar->Health < LinkHealMaxHP)
				{
					// Accumulate fractional healing per beam tick
					// Beam fires every ~0.12s, so per-tick heal = HealPerSecond * 0.12
					float HealPerTick = LinkHealPerSecond * 0.12f;
					float& Accumulator = LinkHealAccumulator.FindOrAdd(InjuredPS);
					Accumulator += HealPerTick;

					int32 WholeHeal = FMath::FloorToInt(Accumulator);
					if (WholeHeal > 0)
					{
						Accumulator -= (float)WholeHeal;
						int32 ActualHeal = FMath::Min(WholeHeal, LinkHealMaxHP - InjuredChar->Health);
						if (ActualHeal > 0)
						{
							InjuredChar->Health += ActualHeal;
						}
					}
				}

				// Block damage to teammate
				Damage = 0;
				return true;
			}
		}
	}

	return Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
}


// ============================================================================
// SCORE DAMAGE (damage tracking for achievements)
// ============================================================================

void AUWipeoutGame::ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);

	if (!Victim || !Attacker || !UTGameState) return;
	if (UTGameState->OnSameTeam(Victim, Attacker)) return;
	if (!bRoundInProgress || !Attacker->Team || DamageAmount <= 0) return;

	// Calculate actual damage dealt (no overkill)
	int32 ActualDamage = DamageAmount;
	if (Victim && Victim->GetUTCharacter())
	{
		AUTCharacter* VictimChar = Victim->GetUTCharacter();
		int32 TotalHP = VictimChar->Health + FMath::FloorToInt(VictimChar->GetArmorAmount());
		ActualDamage = FMath::Min(DamageAmount, TotalHP);
	}

	if (!PlayerRoundDamage.Contains(Attacker))
	{
		PlayerRoundDamage.Add(Attacker, 0.0f);
	}
	PlayerRoundDamage[Attacker] += ActualDamage;

	if (Attacker->Team->TeamIndex == 0)      Team0RoundDamage += ActualDamage;
	else if (Attacker->Team->TeamIndex == 1)  Team1RoundDamage += ActualDamage;

	// Track per-life mutual damage for death recap
	uint64 Key = MakeDamagePairKey(Attacker, Victim);
	int32& PairDmg = LifeDamageMap.FindOrAdd(Key);
	PairDmg += ActualDamage;
}


// ============================================================================
// LIFE DAMAGE TRACKING (for death recap messages)
// ============================================================================

uint64 AUWipeoutGame::MakeDamagePairKey(const AUTPlayerState* From, const AUTPlayerState* To)
{
	// Pack two 32-bit IDs into one 64-bit key
	uint32 FromID = From ? From->GetUniqueID() : 0;
	uint32 ToID = To ? To->GetUniqueID() : 0;
	return (uint64(FromID) << 32) | uint64(ToID);
}

int32 AUWipeoutGame::GetLifeDamage(const AUTPlayerState* From, const AUTPlayerState* To) const
{
	uint64 Key = MakeDamagePairKey(From, To);
	const int32* Val = LifeDamageMap.Find(Key);
	return Val ? *Val : 0;
}

void AUWipeoutGame::ClearLifeDamageFor(AUTPlayerState* PS)
{
	if (!PS) return;
	uint32 PSID = PS->GetUniqueID();

	TArray<uint64> KeysToRemove;
	for (auto& Pair : LifeDamageMap)
	{
		uint32 FromID = uint32(Pair.Key >> 32);
		uint32 ToID = uint32(Pair.Key & 0xFFFFFFFF);
		if (FromID == PSID || ToID == PSID)
		{
			KeysToRemove.Add(Pair.Key);
		}
	}
	for (uint64 Key : KeysToRemove)
	{
		LifeDamageMap.Remove(Key);
	}
}

void AUWipeoutGame::SendDeathRecap(AUTPlayerState* Victim, AUTPlayerState* Killer)
{
	if (!Victim || !Killer || Victim == Killer) return;

	AUTPlayerController* VictimPC = Cast<AUTPlayerController>(Cast<AController>(Victim->GetOwner()));
	if (!VictimPC) return;

	int32 DmgToKiller = GetLifeDamage(Victim, Killer);
	int32 DmgFromKiller = GetLifeDamage(Killer, Victim);

	FString Msg = FString::Printf(TEXT("You dealt %d to %s | They dealt %d to you"),
		DmgToKiller, *Killer->PlayerName, DmgFromKiller);

	VictimPC->ClientSay(nullptr, Msg, ChatDestinations::System);
}


// ============================================================================
// RESET / CLEANUP
// ============================================================================

void AUWipeoutGame::ResetPlayersForNewRound()
{
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();
	LifeDamageMap.Empty();

	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		if (!C) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (PS)
		{
			PS->SetOutOfLives(false);
			PS->RoundDamageDone = 0;
			PS->RoundKills = 0;
		}

		APawn* Pawn = C->GetPawn();
		if (Pawn)
		{
			DiscardInventory(Pawn, C);
			C->UnPossess();
			Pawn->Destroy();
		}
	}
}


void AUWipeoutGame::CleanupWorldForNewRound()
{
	for (TActorIterator<AUTDroppedPickup> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}

	for (FActorIterator It(GetWorld()); It; ++It)
	{
		if (It->GetClass()->ImplementsInterface(UUTResetInterface::StaticClass()))
		{
			IUTResetInterface::Execute_Reset(*It);
		}
	}

	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (AUTCharacter* UTC = Cast<AUTCharacter>(It->Get()))
		{
			if (UTC->IsDead() && !UTC->IsPendingKill())
			{
				UTC->Destroy();
			}
		}
	}
}


// ============================================================================
// ALIVE COUNTS & HELPERS
// ============================================================================

bool AUWipeoutGame::GetAliveCounts(int32& OutAliveTeam0, int32& OutAliveTeam1) const
{
	OutAliveTeam0 = 0;
	OutAliveTeam1 = 0;

	AUTGameState* GS = GetGameState<AUTGameState>();
	if (!GS || GS->IsPendingKill()) return false;

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
		if (!PS || PS->bOnlySpectator || PS->bIsInactive || !PS->Team) continue;

		AUTCharacter* Pawn = Cast<AUTCharacter>(PS->GetUTCharacter());
		if (!Pawn || Pawn->IsDead() || Pawn->Health <= 0) continue;

		if (PS->Team->TeamIndex == 0) OutAliveTeam0++;
		else if (PS->Team->TeamIndex == 1) OutAliveTeam1++;
	}

	return true;
}


int32 AUWipeoutGame::CountAliveOnTeam(int32 TeamIndex) const
{
	int32 Alive = 0;
	if (Teams.IsValidIndex(TeamIndex) && Teams[TeamIndex])
	{
		TArray<AController*> Members = Teams[TeamIndex]->GetTeamMembers();
		for (AController* C : Members)
		{
			if (!C) continue;
			AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
			if (!PS || PS->bOnlySpectator) continue;
			const APawn* P = C->GetPawn();
			const AUTCharacter* UTC = Cast<AUTCharacter>(P);
			if (P && (!UTC || !UTC->IsDead())) ++Alive;
		}
	}
	return Alive;
}


int32 AUWipeoutGame::GetTiebreakWinnerByTeamHealth() const
{
	float Sum0 = 0.f, Sum1 = 0.f;
	for (TActorIterator<AUTCharacter> It(GetWorld()); It; ++It)
	{
		AUTCharacter* C = *It;
		if (!C || C->IsDead()) continue;
		if (AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState))
		{
			if (PS->Team)
			{
				const float HP = FMath::Max(0.f, (float)C->Health) + C->GetArmorAmount();
				(PS->Team->TeamIndex == 0 ? Sum0 : Sum1) += HP;
			}
		}
	}
	if (Sum0 > Sum1) return 0;
	if (Sum1 > Sum0) return 1;
	return INDEX_NONE;
}


void AUWipeoutGame::DelayedInitialWinCheck()
{
	if (bRoundInProgress)
	{
		WinCheckHoldUntilSeconds = 0.f;
		CheckWipeoutCondition();
	}
}


// ============================================================================
// SPECTATING
// ============================================================================

void AUWipeoutGame::ForceTeamSpectate(AUTPlayerState* DeadPS)
{
	if (!DeadPS) return;

	if (useBPSpecFunction)
	{
		BP_SpectatePSImplementation(DeadPS);
		return;
	}

	AUTPlayerController* PC = Cast<AUTPlayerController>(DeadPS->GetOwner());
	if (!PC) return;

	PC->ChangeState(NAME_Spectating);
	PC->ClientGotoState(NAME_Spectating);

	if (AUTPlayerState* TeamTarget = FindAliveTeammate(DeadPS))
	{
		PC->SetViewTarget(TeamTarget->GetUTCharacter());
		PC->bSpectateBehindView = false;
		PC->BehindView(false);
		return;
	}
	if (AUTPlayerState* EnemyTarget = FindAliveEnemy(DeadPS))
	{
		PC->SetViewTarget(EnemyTarget->GetUTCharacter());
		PC->bSpectateBehindView = false;
		PC->BehindView(false);
		return;
	}
	PC->ServerViewSelf();
}


AUTPlayerState* AUWipeoutGame::FindAliveTeammate(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;
	const int32 TeamIdx = PS->Team->TeamIndex;
	if (!Teams.IsValidIndex(TeamIdx) || !Teams[TeamIdx]) return nullptr;

	TArray<AController*> Members = Teams[TeamIdx]->GetTeamMembers();
	for (AController* C : Members)
	{
		if (!C) continue;
		AUTPlayerState* OtherPS = Cast<AUTPlayerState>(C->PlayerState);
		if (!OtherPS || OtherPS == PS || OtherPS->bOnlySpectator) continue;
		APawn* P = C->GetPawn();
		AUTCharacter* UTC = Cast<AUTCharacter>(P);
		if (P && (!UTC || !UTC->IsDead())) return OtherPS;
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAliveEnemy(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;
	const int32 MyTeam = PS->Team->TeamIndex;

	for (int32 TeamIdx = 0; TeamIdx < Teams.Num(); TeamIdx++)
	{
		if (TeamIdx == MyTeam || !Teams.IsValidIndex(TeamIdx)) continue;
		TArray<AController*> Members = Teams[TeamIdx]->GetTeamMembers();
		for (AController* C : Members)
		{
			if (!C) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(C->PlayerState);
			if (!OtherPS || OtherPS->bOnlySpectator) continue;
			APawn* P = C->GetPawn();
			AUTCharacter* UTC = Cast<AUTCharacter>(P);
			if (P && (!UTC || !UTC->IsDead())) return OtherPS;
		}
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAliveOnTeamPS(int32 TeamIndex) const
{
	if (!Teams.IsValidIndex(TeamIndex)) return nullptr;
	TArray<AController*> Members = Teams[TeamIndex]->GetTeamMembers();
	for (AController* C : Members)
	{
		if (!C) continue;
		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (!PS || PS->bOnlySpectator) continue;
		APawn* P = C->GetPawn();
		const AUTCharacter* UTC = Cast<AUTCharacter>(P);
		if (P && (!UTC || !UTC->IsDead())) return PS;
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAnyOnTeamPS(int32 TeamIndex) const
{
	if (const AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		for (APlayerState* APS : GS->PlayerArray)
		{
			if (AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS))
			{
				if (UTPS->Team && UTPS->Team->TeamIndex == TeamIndex) return UTPS;
			}
		}
	}
	return nullptr;
}


void AUWipeoutGame::ForceLosersToViewWinners(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex < 0) return;

	const int32 LoserTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
	AUTPlayerState* TargetPS = FindAliveOnTeamPS(WinnerTeamIndex);
	if (!TargetPS) TargetPS = FindAnyOnTeamPS(WinnerTeamIndex);
	if (!TargetPS) return;

	AUTCharacter* TargetCharacter = TargetPS->GetUTCharacter();
	if (!TargetCharacter) return;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!PC || !PS || !PS->Team) continue;

		if (PS->Team->TeamIndex == LoserTeamIndex)
		{
			if (!PC->IsInState(NAME_Spectating))
			{
				PC->ChangeState(NAME_Spectating);
				PC->ClientGotoState(NAME_Spectating);
			}
			if (!PS->bOutOfLives)
			{
				PS->bOutOfLives = true;
				PS->ForceNetUpdate();
			}
			PC->SetViewTarget(TargetCharacter);
			PC->bSpectateBehindView = false;
			PC->BehindView(false);
		}
	}
}


bool AUWipeoutGame::CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget)
{
	if (!Viewer || !ViewTarget) return false;

	const AUTPlayerState* ViewerPS = Cast<AUTPlayerState>(Viewer->PlayerState);
	const AUTPlayerState* TargetPS = Cast<AUTPlayerState>(ViewTarget);
	if (!ViewerPS || !TargetPS || !ViewerPS->Team || !TargetPS->Team)
	{
		return Super::CanSpectate_Implementation(Viewer, ViewTarget);
	}

	if (!bRoundInProgress)
	{
		return Super::CanSpectate_Implementation(Viewer, ViewTarget);
	}

	// During round: dead players can only spectate teammates if teammates are alive
	if (bRoundInProgress)
	{
		const int32 MyTeamAlive = CountAliveOnTeam(ViewerPS->Team->TeamIndex);
		if (MyTeamAlive > 0 && ViewerPS->Team != TargetPS->Team)
		{
			return false; // Can't spectate enemies while teammates are alive
		}
	}

	const AController* TargetPC = Cast<AController>(TargetPS->GetOwner());
	const APawn* P = TargetPC ? TargetPC->GetPawn() : nullptr;
	const AUTCharacter* C = Cast<AUTCharacter>(P);
	const bool bTargetAlive = P && (!C || !C->IsDead());
	if (!bTargetAlive && bRoundInProgress) return false;

	return Super::CanSpectate_Implementation(Viewer, ViewTarget);
}


// ============================================================================
// SPAWN LAYOUT PRECOMPUTATION (from TeamArena, for round-start spawns)
// ============================================================================

void AUWipeoutGame::InitializeSpawnPointSystem()
{
	AllSpawnPointsList.Empty();
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (APlayerStart* Spawn = *It)
		{
			AllSpawnPointsList.Add(Spawn);
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Found %d spawn points"), AllSpawnPointsList.Num());
}


void AUWipeoutGame::PrecomputeSpawnLayouts()
{
	ValidLayouts_2v2.Empty();
	ValidLayouts_1v1.Empty();

	const int32 N = AllSpawnPointsList.Num();
	if (N < 2)
	{
		UE_LOG(LogGameMode, Error, TEXT("Wipeout::PrecomputeSpawnLayouts: Only %d spawns!"), N);
		return;
	}

	const float SafetyThreshold = FMath::Min(MinimumEnemyHorizontalDistance, 2900.0f);

	// 2v2 layouts
	for (int32 a = 0; a < N; ++a)
	{
		for (int32 b = a + 1; b < N; ++b)
		{
			float T0Sep = (AllSpawnPointsList[a]->GetActorLocation() - AllSpawnPointsList[b]->GetActorLocation()).Size2D();
			if (T0Sep < MinTeammateSeparation2D || T0Sep > MaxTeammateSeparation2D) continue;

			for (int32 c = 0; c < N; ++c)
			{
				if (c == a || c == b) continue;
				for (int32 d = c + 1; d < N; ++d)
				{
					if (d == a || d == b) continue;

					float T1Sep = (AllSpawnPointsList[c]->GetActorLocation() - AllSpawnPointsList[d]->GetActorLocation()).Size2D();
					if (T1Sep < MinTeammateSeparation2D || T1Sep > MaxTeammateSeparation2D) continue;

					float MinCross = FLT_MAX;
					MinCross = FMath::Min(MinCross, (AllSpawnPointsList[a]->GetActorLocation() - AllSpawnPointsList[c]->GetActorLocation()).Size2D());
					MinCross = FMath::Min(MinCross, (AllSpawnPointsList[a]->GetActorLocation() - AllSpawnPointsList[d]->GetActorLocation()).Size2D());
					MinCross = FMath::Min(MinCross, (AllSpawnPointsList[b]->GetActorLocation() - AllSpawnPointsList[c]->GetActorLocation()).Size2D());
					MinCross = FMath::Min(MinCross, (AllSpawnPointsList[b]->GetActorLocation() - AllSpawnPointsList[d]->GetActorLocation()).Size2D());

					if (MinCross < SafetyThreshold) continue;

					FWipeoutSpawnLayout Layout;
					Layout.T0_Primary = AllSpawnPointsList[a];
					Layout.T0_Secondary = AllSpawnPointsList[b];
					Layout.T1_Primary = AllSpawnPointsList[c];
					Layout.T1_Secondary = AllSpawnPointsList[d];
					Layout.MinCrossDistance2D = MinCross;
					Layout.T0Separation = T0Sep;
					Layout.T1Separation = T1Sep;
					Layout.UsageCount = 0;
					Layout.QualityScore = MinCross + FMath::Min(T0Sep, 1500.0f) * 0.3f + FMath::Min(T1Sep, 1500.0f) * 0.3f;

					ValidLayouts_2v2.Add(Layout);
				}
			}
		}
	}

	// 1v1 (stack) layouts
	const float StackSafetyThreshold = FMath::Max(SafetyThreshold, MinimumStackSpawnDistance2D);
	for (int32 a = 0; a < N; ++a)
	{
		for (int32 b = 0; b < N; ++b)
		{
			if (b == a) continue;
			float Dist = (AllSpawnPointsList[a]->GetActorLocation() - AllSpawnPointsList[b]->GetActorLocation()).Size2D();
			if (Dist < StackSafetyThreshold) continue;

			FWipeoutSpawnLayout Layout;
			Layout.T0_Primary = AllSpawnPointsList[a];
			Layout.T0_Secondary = nullptr;
			Layout.T1_Primary = AllSpawnPointsList[b];
			Layout.T1_Secondary = nullptr;
			Layout.MinCrossDistance2D = Dist;
			Layout.T0Separation = 0.f;
			Layout.T1Separation = 0.f;
			Layout.UsageCount = 0;
			Layout.QualityScore = Dist;

			ValidLayouts_1v1.Add(Layout);
		}
	}

	ValidLayouts_2v2.Sort([](const FWipeoutSpawnLayout& A, const FWipeoutSpawnLayout& B) { return A.QualityScore > B.QualityScore; });
	ValidLayouts_1v1.Sort([](const FWipeoutSpawnLayout& A, const FWipeoutSpawnLayout& B) { return A.QualityScore > B.QualityScore; });

	UE_LOG(LogGameMode, Log, TEXT("Wipeout spawn precompute: %d 2v2, %d 1v1 layouts"), ValidLayouts_2v2.Num(), ValidLayouts_1v1.Num());
}


void AUWipeoutGame::SelectSpawnLayoutForRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();

	bool bForce1v1 = (TotalRoundsPlayed % 3 == 0) && (ValidLayouts_1v1.Num() > 0);
	TArray<FWipeoutSpawnLayout>& Pool = (!bForce1v1 && ValidLayouts_2v2.Num() > 0) ? ValidLayouts_2v2 : ValidLayouts_1v1;

	if (Pool.Num() == 0)
	{
		UE_LOG(LogGameMode, Error, TEXT("Wipeout: No valid spawn layouts!"));
		return;
	}

	const int32 PoolSize = FMath::Min(Pool.Num(), 30);
	int32 BestIndex = 0;
	float BestScore = -FLT_MAX;

	for (int32 i = 0; i < PoolSize; ++i)
	{
		float UsagePenalty = Pool[i].UsageCount * 3000.0f;
		float Jitter = FMath::FRandRange(0.0f, 500.0f);
		float Score = Pool[i].QualityScore - UsagePenalty + Jitter;
		if (Score > BestScore)
		{
			BestScore = Score;
			BestIndex = i;
		}
	}

	FWipeoutSpawnLayout& Chosen = Pool[BestIndex];
	Chosen.UsageCount++;

	bool bSwapTeams = (TotalRoundsPlayed % 2 == 1);

	APlayerStart* FinalT0_P = bSwapTeams ? Chosen.T1_Primary   : Chosen.T0_Primary;
	APlayerStart* FinalT0_S = bSwapTeams ? Chosen.T1_Secondary : Chosen.T0_Secondary;
	APlayerStart* FinalT1_P = bSwapTeams ? Chosen.T0_Primary   : Chosen.T1_Primary;
	APlayerStart* FinalT1_S = bSwapTeams ? Chosen.T0_Secondary : Chosen.T1_Secondary;

	Team0SelectedSpawns.Add(FinalT0_P);
	if (FinalT0_S) Team0SelectedSpawns.Add(FinalT0_S);

	Team1SelectedSpawns.Add(FinalT1_P);
	if (FinalT1_S) Team1SelectedSpawns.Add(FinalT1_S);

	UE_LOG(LogGameMode, Log, TEXT("Wipeout round %d: Layout idx %d, CrossDist %.0f"),
		TotalRoundsPlayed, BestIndex, Chosen.MinCrossDistance2D);
}


void AUWipeoutGame::ResetSpawnSelectionForNewRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();
	++CurrentRoundNumber;
	FMath::SRandInit(static_cast<int32>(FPlatformTime::Cycles()));
}


// ============================================================================
// OVERTIME (wave-based damage — same system as TeamArena)
// ============================================================================

void AUWipeoutGame::StartOvertime()
{
	if (!bOvertimeEnabled || !bRoundInProgress) return;

	CurrentOvertimeWave = 0;
	CurrentWaveDamage = OvertimeBaseDamage;

	if (OvertimeStartDelay >= 3.0f)
	{
		float CountdownStart = OvertimeStartDelay - 3.0f;
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this]() { BroadcastOvertimeCountdown(3); },
			CountdownStart, false);
	}

	GetWorldTimerManager().SetTimer(
		OvertimeWaveTimerHandle, this,
		&AUWipeoutGame::ExecuteOvertimeWave,
		FMath::Max(0.1f, OvertimeStartDelay), false);

	BP_OnOvertimeStarted();
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout Overtime started! First wave in %.1fs with %.1f dmg"),
		OvertimeStartDelay, OvertimeBaseDamage);
}


void AUWipeoutGame::StopOvertime()
{
	if (OvertimeTimerHandle.IsValid())      GetWorldTimerManager().ClearTimer(OvertimeTimerHandle);
	if (OvertimeWaveTimerHandle.IsValid())   GetWorldTimerManager().ClearTimer(OvertimeWaveTimerHandle);
	CurrentOvertimeWave = 0;
	CurrentWaveDamage = 0.0f;
}


void AUWipeoutGame::ExecuteOvertimeWave()
{
	if (GetNetMode() == NM_Client || !bRoundInProgress)
	{
		StopOvertime();
		return;
	}

	CurrentOvertimeWave++;
	CurrentWaveDamage = (CurrentOvertimeWave == 1) ? OvertimeBaseDamage : CurrentWaveDamage * OvertimeDamageMultiplier;
	if (OvertimeMaxDamage > 0.0f) CurrentWaveDamage = FMath::Min(CurrentWaveDamage, OvertimeMaxDamage);

	BP_OnOvertimeWave(CurrentWaveDamage, CurrentOvertimeWave);

	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (!bRoundInProgress) break;

		AUTCharacter* C = Cast<AUTCharacter>(*It);
		if (!C || C->IsDead()) continue;

		float DamageToApply = CurrentWaveDamage;
		if (bOvertimeNonLethal && C->Health <= 1.f) continue;
		if (bOvertimeNonLethal && (C->Health - DamageToApply) < 1.f)
		{
			DamageToApply = FMath::Max(0.f, C->Health - 1.f);
			if (DamageToApply <= 0.f) continue;
		}

		FHitResult Hit;
		Hit.bBlockingHit = true;
		Hit.Location = C->GetActorLocation();
		Hit.ImpactPoint = C->GetActorLocation();
		Hit.Normal = FVector(0, 0, 1);
		Hit.ImpactNormal = FVector(0, 0, 1);
		Hit.Actor = C;
		Hit.Component = Cast<UPrimitiveComponent>(C->GetRootComponent());

		FUTPointDamageEvent DamageEvent(
			DamageToApply, Hit, FVector(0, 0, -1),
			OvertimeDamageType ? *OvertimeDamageType : UUTDamageType::StaticClass(),
			FVector::ZeroVector);

		// Pass the victim's own controller as instigator for overtime environment damage.
		// Passing nullptr crashes Epic's damage pipeline which dereferences InstigatedBy
		// without null checks in kill credit / death message code paths.
		AController* VictimController = C->GetController();
		C->TakeDamage(DamageToApply, DamageEvent, VictimController, this);
	}

	CheckWipeoutCondition();

	if (bRoundInProgress)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeWaveTimerHandle, this,
			&AUWipeoutGame::ExecuteOvertimeWave,
			OvertimeWaveInterval, false);
	}
}


void AUWipeoutGame::BroadcastOvertimeCountdown(int32 CountdownValue)
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->ClientReceiveLocalizedMessage(UUTCountDownMessage::StaticClass(), CountdownValue);
		}
	}

	if (CountdownValue > 1)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this, CountdownValue]() { BroadcastOvertimeCountdown(CountdownValue - 1); },
			1.0f, false);
	}
	else if (CountdownValue == 1)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this]() { BroadcastOvertimeAnnouncement(); },
			1.0f, false);
	}
}


void AUWipeoutGame::BroadcastOvertimeAnnouncement()
{
	if (!OvertimeAnnouncementSound) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->UTClientPlaySound(OvertimeAnnouncementSound);
		}
	}
}


// ============================================================================
// BROADCAST RESULTS
// ============================================================================

void AUWipeoutGame::BroadcastRoundResults(int32 WinnerTeamIndex, bool bIsDraw)
{
	BP_OnRoundResults(WinnerTeamIndex, bIsDraw);
}


void AUWipeoutGame::BroadcastKillReplay()
{
	if (RoundWinningKiller && RoundWinningKillTime > 0.f)
	{
		float ReplayOffset = (GetWorld()->GetTimeSeconds() - RoundWinningKillTime) + 5.0f;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
			{
				PC->ClientQueueCoolMoment(RoundWinningKiller->UniqueId, ReplayOffset);
			}
		}
	}
}


// ============================================================================
// DOMINATION / LEAD TRACKING
// ============================================================================

void AUWipeoutGame::CheckForDominationAndLead(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex == INDEX_NONE || !Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return;

	const int32 RedScore = Teams[0]->Score;
	const int32 BlueScore = Teams[1]->Score;
	const int32 ScoreDiff = FMath::Abs(RedScore - BlueScore);
	const bool bWasTied = (PreviousRedScore == PreviousBlueScore);
	const bool bNowHasLead = (RedScore != BlueScore);

	if (bWasTied && bNowHasLead)
	{
		BroadcastTakesLead(WinnerTeamIndex);
	}
	else if (!bWasTied && bNowHasLead)
	{
		const bool bRedWasWinning = (PreviousRedScore > PreviousBlueScore);
		const bool bRedNowWinning = (RedScore > BlueScore);
		if (bRedWasWinning != bRedNowWinning)
		{
			BroadcastTakesLead(WinnerTeamIndex);
		}
	}

	if (ScoreDiff >= 5)
	{
		const int32 DominatingTeam = (RedScore > BlueScore) ? 0 : 1;
		if (!bHasBroadcastTeamDominating)
		{
			BroadcastDomination(DominatingTeam);
			bHasBroadcastTeamDominating = true;
		}
	}
	else
	{
		bHasBroadcastTeamDominating = false;
	}

	PreviousRedScore = RedScore;
	PreviousBlueScore = BlueScore;
}


void AUWipeoutGame::BroadcastDomination(int32 DominatingTeamIndex) { BP_OnDomination(DominatingTeamIndex); }
void AUWipeoutGame::BroadcastTakesLead(int32 LeadingTeamIndex)     { BP_OnTakesLead(LeadingTeamIndex); }


// ============================================================================
// ACHIEVEMENTS
// ============================================================================

void AUWipeoutGame::CheckForHighDamageCarry(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex == INDEX_NONE) return;

	float TeamTotalDamage = (WinnerTeamIndex == 0) ? Team0RoundDamage : Team1RoundDamage;
	if (TeamTotalDamage <= 0) return;

	for (auto& DamagePair : PlayerRoundDamage)
	{
		AUTPlayerState* PS = DamagePair.Key.Get();
		float PlayerDamage = DamagePair.Value;
		if (PS && PS->Team && PS->Team->TeamIndex == WinnerTeamIndex && PlayerDamage > 440.f)
		{
			float DamagePct = (PlayerDamage / TeamTotalDamage) * 100.0f;
			if (DamagePct >= HighDamageCarryThreshold)
			{
				RecordHighDamageCarry(PS, DamagePct);
			}
		}
	}
}


void AUWipeoutGame::RecordHighDamageCarry(AUTPlayerState* PlayerState, float DamagePercentage)
{
	if (!PlayerState) return;
	UE_LOG(LogGameMode, Log, TEXT("Wipeout High Damage Carry: %s (%.1f%%)"), *PlayerState->PlayerName, DamagePercentage);
	OnPlayerHighDamageCarry.Broadcast(PlayerState, DamagePercentage);
}


// ============================================================================
// SERVER MANAGEMENT (from TeamArena)
// ============================================================================

void AUWipeoutGame::HandleServerManagement()
{
	HandleInstanceCleanup();
	HandleMapVoting();
	CheckBotCount();

	if (UTIsHandlingReplays())
	{
		UDemoNetDriver* DemoNetDriver = GetWorld()->DemoNetDriver;
		if (DemoNetDriver != nullptr && DemoNetDriver->ReplayStreamer.IsValid())
		{
			UTGameState->ReplayID = DemoNetDriver->ReplayStreamer->GetReplayID();
		}
	}
}


void AUWipeoutGame::HandleMapVoting()
{
	if (MatchState == MatchState::MapVoteHappening)
	{
		if (GetWorld()->GetNetMode() != NM_Standalone)
		{
			UTGameState->VoteTimer--;
			if (UTGameState->VoteTimer <= 0) UTGameState->VoteTimer = 0;
		}

		TArray<AUTReplicatedMapInfo*> Best;
		for (int32 i = 0; i < UTGameState->MapVoteList.Num(); i++)
		{
			if (UTGameState->MapVoteList[i]->VoteCount > 0)
			{
				if (Best.Num() == 0 || Best[0]->VoteCount < UTGameState->MapVoteList[i]->VoteCount)
				{
					Best.Empty();
					Best.Add(UTGameState->MapVoteList[i]);
				}
			}
		}

		if (Best.Num() > 0)
		{
			int32 Target = int32(float(GetNumPlayers()) * 0.5);
			if (Best[0]->VoteCount > Target) TallyMapVotes();
		}
	}
}


void AUWipeoutGame::HandleInstanceCleanup()
{
	if (GetWorld() == nullptr || GetWorld()->WorldType != EWorldType::Game) return;
	if (!HasAuthority() && GetNetMode() != NM_Standalone) return;

	if (IsGameInstanceServer() && LobbyBeacon)
	{
		if (GetWorld()->GetTimeSeconds() - LastLobbyUpdateTime >= 10.0f)
		{
			UpdateLobbyMatchStats();
		}

		if (!bDedicatedInstance)
		{
			if (!HasMatchStarted())
			{
				if (GetWorld()->GetRealTimeSeconds() > LobbyInitialTimeoutTime && NumPlayers <= 0 &&
					(GetNetDriver() == NULL || GetNetDriver()->ClientConnections.Num() == 0))
				{
					ShutdownGameInstance();
					return;
				}
			}
			else if (NumPlayers <= 0)
			{
				ShutdownGameInstance();
				return;
			}
		}
	}
	else
	{
		if (NumPlayers <= 0 && NumSpectators <= 0 && HasMatchStarted())
		{
			EmptyServerTime++;
			if (EmptyServerTime >= AutoRestartTime)
			{
				TravelToNextMap();
				return;
			}
		}
		else
		{
			EmptyServerTime = 0;
		}
	}

	if (LobbyBeacon && LobbyBeacon->GetNetConnection()->State == EConnectionState::USOCK_Closed)
	{
		if (!bDedicatedInstance && NumPlayers <= 0 && MatchState != MatchState::WaitingToStart)
		{
			FPlatformMisc::RequestExit(false);
			return;
		}
		RecreateLobbyBeacon();
	}
}


// ============================================================================
// LOGOUT
// ============================================================================

void AUWipeoutGame::Logout(AController* Exiting)
{
	if (bCompetitiveAutoPause && IsMatchInProgress() && !HasMatchEnded() && !GetWorld()->IsPaused())
	{
		if (Exiting)
		{
			AUTPlayerState* ExitingPS = Cast<AUTPlayerState>(Exiting->PlayerState);
			if (ExitingPS && !ExitingPS->bIsABot && !ExitingPS->bOnlySpectator)
			{
				UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Player %s disconnected. Pausing match."), *ExitingPS->PlayerName);
				SetPause(nullptr);
			}
		}
	}

	if (Exiting)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (PS)
		{
			// Clean up all tracking maps to prevent stale pointer access
			PlayerRoundDamage.Remove(PS);
			PlayerDeathCounts.Remove(PS);
			CancelPendingRespawn(PS);
			SpawnProtectedUntil.Remove(PS);
		}
	}

	Super::Logout(Exiting);
}


// ============================================================================
// BP_RESTART ROUND (admin tool)
// ============================================================================

void AUWipeoutGame::BP_RestartCurrentRound()
{
	if (!HasAuthority()) return;
	if (bWarmupMode || !HasMatchStarted()) return;

	UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Admin restarting current round"));

	StopOvertime();
	CancelAllPendingRespawns();
	GetWorldTimerManager().ClearTimer(TH_RoundEndDelay);
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);

	bRoundInProgress = false;
	RoundEndTimeSeconds = 0.f;
	LastRoundWinningTeamIndex = INDEX_NONE;
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	PlayerDeathCounts.Empty();
	SpawnProtectedUntil.Empty();

	ResetPlayersForNewRound();
	CleanupWorldForNewRound();
	ResetSpawnSelectionForNewRound();
	SelectSpawnLayoutForRound();

	StartIntermission(4);
}


void AUWipeoutGame::BP_SetTeamScores(int32 RedScore, int32 BlueScore)
{
	if (!HasAuthority()) return;
	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return;

	Teams[0]->Score = FMath::Max(0, RedScore);
	Teams[1]->Score = FMath::Max(0, BlueScore);
	Teams[0]->ForceNetUpdate();
	Teams[1]->ForceNetUpdate();

	PreviousRedScore = Teams[0]->Score;
	PreviousBlueScore = Teams[1]->Score;
	bHasBroadcastTeamDominating = false;
}
