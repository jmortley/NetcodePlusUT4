#include "ElimPlusGame.h"
#include "UnrealTournament.h"
#include "ElimPlusStatsReplicator.h"
#include "ElimPlusHUD.h"
#include "ElimPlusRatingSystem.h"
//#include "GSInterface.h"
//#include "ArenaPlayerCameraManager.h"
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
#include "Engine/DemoNetDriver.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "ElimPlusVictoryMessage.h"
#include "UTCountDownMessage.h" 
#include "UTGameMessage.h"

AElimPlusGame::AElimPlusGame(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("UTGameMode", "TeamArena", "Team Arena");
	bTeamGame = true;
	HUDClass = AElimPlusHUD::StaticClass();
	// TeamGameMode defaults
	NumTeams = 2;
	bBalanceTeams = true;
	bUseTeamStarts = false;
	bAnnounceTeam = true;
	RedTeamVictorySound = nullptr;
	BlueTeamDominatingSound = nullptr;
	RedTeamDominatingSound = nullptr;
	RedTeamTakesLeadSound = nullptr;
	BlueTeamTakesLeadSound = nullptr;
	BlueTeamVictorySound = nullptr;
	RoundDrawSound = nullptr;
	MatchVictorySound = nullptr;
	LastManStandingSound = nullptr;
	EnemyLastManStandingSound = nullptr;
	OvertimeAnnouncementSound = nullptr;
	bRecordReplays = true;
	// Round defaults
	bForceRespawn = false;
	bHasRespawnChoices = false;
	ScoreLimit = 10;
	RoundTimeSeconds = 90;
	RoundStartDelaySeconds = 5.f; // This is now the "AwardDisplayTime"
	bAllowRespawnMidRound = false;
	bRoundInProgress = false;
	bAllowPlayerRespawns = false;
	LastRoundWinningTeamIndex = INDEX_NONE;
	AwardDisplayTime = 5.f; // Use this for the post-round delay
	PreRoundCountdown = 3.f; // Use this for the pre-spawn countdown
	RoundWinningKiller = nullptr;
	WinningKillerPawn = nullptr;
	RoundWinningKillTime = 0.0f;
	bPendingDarkHorseReplay = false;
	bWinByTwo = false;
	//SpawnProtectionTime = 3.f;
	bWarmupMode = false;
	SpectateDelay = 2.f;
	TotalRoundsPlayed = 0;
	HighDamageCarryThreshold = 60.0f;
	//bCasterControl = true;
	// Initialize last man standing tracking
	Team0StartingSize = 0;
	Team1StartingSize = 0;
	bTeam0LastManAnnounced = false;
	bTeam1LastManAnnounced = false;
	Team0RoundDamage = 0;
	Team1RoundDamage = 0;
	bCompetitiveAutoPause = false;

	// Initialize score tracking for domination/lead detection
	PreviousRedScore = 0;
	PreviousBlueScore = 0;
	bHasBroadcastTeamDominating = false;

	// Match goals (TeamGameMode uses GoalScore)
	GoalScore = ScoreLimit;
	//TimeLimit = 20;
	//PlayerCameraManagerClass = AArenaPlayerCameraManager::StaticClass();

	// Overtime defaults...
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

	//Spawn weighting system with default values
	SpawnDistanceWeight = 0.65f;
	SpawnHeightWeight = 0.10f;
	SpawnUsageWeight = 0.10f;
	SpawnSeparationWeight = 0.15f;
	MinimumEnemySpawnDistance = 2800.0f; // Minimum distance from enemy spawns
	MinimumEnemyHorizontalDistance = 3000.0f;

}


bool AElimPlusGame::SupportsInstantReplay() const
{
	return true;
}

void AElimPlusGame::BP_SetMatchState_RoundCooldown()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("RoundCooldown")));
	}
}

void AElimPlusGame::BP_SetMatchState_Intermission()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("Intermission")));
	}
}

void AElimPlusGame::BP_SetMatchState_InProgress()
{
	if (HasAuthority())
	{
		SetMatchState(MatchState::InProgress);
	}
}




void AElimPlusGame::UpdateVictoryMessageSounds()
{
	UElimPlusVictoryMessage* VictoryMessageCDO = UElimPlusVictoryMessage::StaticClass()->GetDefaultObject<UElimPlusVictoryMessage>();
	if (VictoryMessageCDO)
	{
		VictoryMessageCDO->RedTeamVictorySound = RedTeamVictorySound;
		VictoryMessageCDO->BlueTeamVictorySound = BlueTeamVictorySound;
		VictoryMessageCDO->DrawSound = RoundDrawSound;
		VictoryMessageCDO->RedTeamDominatingSound = RedTeamDominatingSound;
		VictoryMessageCDO->BlueTeamDominatingSound = BlueTeamDominatingSound;
		VictoryMessageCDO->RedTeamTakesLeadSound = RedTeamTakesLeadSound;
		VictoryMessageCDO->BlueTeamTakesLeadSound = BlueTeamTakesLeadSound;
	}
}

void AElimPlusGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Override GoalScore with ScoreLimit, as ScoreLimit is our "rounds to win"
	GoalScore = ScoreLimit;

	// We manage time on a per-round basis, so set base TimeLimit to 0
	TimeLimit = 0;

	// Match-scoped stats reset on every map load (each map = a fresh match).
	PerPlayerMatchPPRSum.Empty();
	PerPlayerMatchPPRRoundCount.Empty();
	bRatingFlushedThisMatch = false;
}

void AElimPlusGame::BeginPlay()
{
	Super::BeginPlay();

	if (!bSpawnPointsInitialized)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Force initializing spawn system in BeginPlay"));
		InitializeSpawnPointSystem();
		bSpawnPointsInitialized = true;
	}
	// Precompute all valid spawn layouts once at map load.
	// This builds ValidLayouts_2v2 and ValidLayouts_1v1 arrays.
	// Small maps will naturally have 0 valid 2v2 layouts, causing
	// automatic fallback to 1v1 stacks � fixing the 2-vs-1 spawn bug.
	PrecomputeSpawnLayouts();

	// Server-only rating system. Initialize Mods.db schema once per map load.
	if (HasAuthority() && !RatingSystem.IsValid())
	{
		RatingSystem = MakeUnique<FElimPlusRatingSystem>();
		FElimPlusRatingSystem::InitDatabase(GetWorld());
	}
}


void AElimPlusGame::DelayedEndGame(int32 WinnerTeamIndex, FName Reason)
{
	AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
	EndGame(BestPlayer, Reason);
}


void AElimPlusGame::HandleMatchHasStarted()
{
	UE_LOG(LogGameMode, Warning, TEXT("=== TeamArena::HandleMatchHasStarted ENTER ==="));
	UE_LOG(LogGameMode, Warning, TEXT("  UTIsHandlingReplays: %s"), UTIsHandlingReplays() ? TEXT("TRUE") : TEXT("FALSE"));
	UE_LOG(LogGameMode, Warning, TEXT("  GetGameInstance: %s"), GetGameInstance() ? TEXT("VALID") : TEXT("NULL"));
	UE_LOG(LogGameMode, Warning, TEXT("  GetNetMode: %d"), (int32)GetNetMode());
	Super::HandleMatchHasStarted();

	bWarmupMode = false;

	// Defense-in-depth reset of the flush guard: InitGame already resets it on
	// map load, but a single PIE/server session can host multiple matches in a
	// row. Reset here so a subsequent match can flush its own ratings.
	bRatingFlushedThisMatch = false;

	// Spawn stats replicator now — all clients are fully loaded at this point.
	// Spawning in BeginPlay was too early and could cause client crashes.
	if (HasAuthority() && !StatsReplicator)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		StatsReplicator = GetWorld()->SpawnActor<AElimPlusStatsReplicator>(SpawnParams);
	}

	// Snapshot every loaded player's current rating as their "match-start" value.
	// Replicator's Elo + EloDeltaThisMatch stay frozen at these values until
	// HandleMatchHasEnded — players don't see ELO change round-to-round.
	if (HasAuthority() && RatingSystem.IsValid())
	{
		RatingSystem->SnapshotMatchStart();

		// Push the start-of-match Elo to replicator with delta=0 so the HUD
		// shows the player's current rating from the very first frame.
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS && StatsReplicator)
		{
			for (APlayerState* PS : GS->PlayerArray)
			{
				AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
				if (!UTPS || !UTPS->UniqueId.IsValid()) continue;
				const FString UidStr = UTPS->UniqueId.ToString();
				const int32 Elo = RatingSystem->GetCachedElo(UidStr);
				StatsReplicator->SetPlayerEloAndDelta(UidStr, Elo, 0);
			}
		}
	}
}


void AElimPlusGame::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// Server-only: pull this player's rating from Mods.db into the cache so it's
	// ready before the first round ends. Late joiners who arrive mid-match also
	// get their rating loaded, but won't have a SnapshotMatchStart entry — they
	// skip ELO for this match (no delta would be meaningful).
	if (!HasAuthority() || !RatingSystem.IsValid()) return;
	if (!NewPlayer) return;

	AUTPlayerState* UTPS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (UTPS && UTPS->UniqueId.IsValid())
	{
		const FString UidStr = UTPS->UniqueId.ToString();
		RatingSystem->LoadPlayerFromDB(GetWorld(), UidStr);

		// Push their current ELO straight to the replicator so their HUD chip
		// shows real value (not 1400 baseline) from the moment they connect.
		if (StatsReplicator)
		{
			const int32 Elo = RatingSystem->GetCachedElo(UidStr);
			StatsReplicator->SetPlayerEloAndDelta(UidStr, Elo, 0);
		}
	}
}


void AElimPlusGame::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();

	// Persist updated ratings to Mods.db and emit the final ELO + match delta
	// to the replicator. Engine routes HandleMatchHasEnded twice in some paths
	// (state-machine + derived); guard with bRatingFlushedThisMatch so we only
	// log + write once. INSERT OR REPLACE was already idempotent at the SQL level.
	if (HasAuthority() && RatingSystem.IsValid() && !bRatingFlushedThisMatch)
	{
		RatingSystem->FlushAtMatchEnd(GetWorld(), StatsReplicator);
		bRatingFlushedThisMatch = true;
	}
}


void AElimPlusGame::CallMatchStateChangeNotify()
{
	UE_LOG(LogGameMode, Log, TEXT("Current matchstate: %s"), *GetMatchState().ToString());
	// This function intercepts all SetMatchState calls
	// and routes them to our custom handlers.
	if (GetMatchState() == MatchState::WaitingToStart)
	{
		bWarmupMode = true;

	}
	else if (GetMatchState() == MatchState::PlayerIntro)
	{
		// Ensure warmup mode is disabled once match is starting
		bWarmupMode = false;
		ResetSpawnSelectionForNewRound();
	}
	if (GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		HandleMatchIntermission();
	}
	else if (GetMatchState() == MatchState::InProgress)// && GetWorld()->bMatchStarted)
	{
		// We are transitioning *to* InProgress, so start the new round
		if (TotalRoundsPlayed == 0)
		{
			Super::CallMatchStateChangeNotify();  // This triggers HandleMatchHasStarted
		}
		bWarmupMode = false;
		StartNextRound();
	}
	else
	{
		// Handle other states normally (WaitingToStart, etc)
		Super::CallMatchStateChangeNotify();
	}
}


void AElimPlusGame::DeferredHandleMatchStart()
{
	if (GetGameState<AUTGameState>())
	{
		// Call HandleMatchHasStarted, which will call StartIntermission
		// This mimics the PIE-safe flow
		HandleMatchHasStarted();
	}
	else
	{
		UE_LOG(LogGameMode, Error, TEXT("GameState still null after deferring BeginPlay!"));
	}
}

void AElimPlusGame::DeferredCheckRoundWinConditions()
{
	if (bRoundInProgress)
	{
		CheckRoundWinConditions();
	}
}


void AElimPlusGame::DefaultTimer()
{
	
	if (GetWorld()->WorldType == EWorldType::EditorPreview)
	{
		return;
	}

	
	if (IsPendingKill() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		return;



	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS == nullptr || GS->IsPendingKill() || GetWorld()->bIsTearingDown) return; // Not ready yet

	//Because we don't call super defaulttimer except when roundinprogress. Need to do server management work here
	HandleServerManagement();


	// --- Intermission Logic-- -
	if (GS->GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		if (IntermissionSecondsRemaining > 0)
		{
			--IntermissionSecondsRemaining;

			BP_OnSetIntermission(/*bInIntermission*/true, IntermissionSecondsRemaining);
			// Broadcast 3, 2, 1
			if (IntermissionSecondsRemaining > 0 && IntermissionSecondsRemaining <= 3)
			{
				BroadcastLocalized(this, UUTCountDownMessage::StaticClass(), IntermissionSecondsRemaining, nullptr, nullptr, nullptr);
			}
		}

		else // Intermission timer hit zero
		{
			// Notify BP that intermission is over (optional)
			BP_OnSetIntermission(/*bInIntermission*/false, IntermissionSecondsRemaining);
			CleanupWorldForNewRound();
			SetMatchState(MatchState::InProgress);
			//UE_LOG(LogGameMode, Log, TEXT("DefaultTimer: Intermission complete. Cleaning world and setting state to InProgress."));


		}
		return;
	}

	// --- Round Active Logic ---
	if (bRoundInProgress)
	{
		int32 RoundRemain = 0;
		if (RoundEndTimeSeconds > 0.f)
		{
			RoundRemain = FMath::Max(0, (int32)FMath::CeilToInt(RoundEndTimeSeconds - GetWorld()->GetTimeSeconds()));

			/*if (GS->GetClass()->ImplementsInterface(URoundStateProvider::StaticClass()))
				
					IRoundStateProvider::Execute_Arena_SetRound(
						GS,
						//bInProgress true,
						//RoundRemain RoundRemain,
						//LastWinnerTeamIndex INDEX_NONE  // or cached Winner if you track it
					);
			*/
			BP_OnSetRound(true, RoundRemain, LastRoundWinningTeamIndex, Team0AlivePlayers, Team1AlivePlayers);
			

			if (RoundRemain == 0)
			{
				// --- TIME IS UP ---
				// Check if both teams are still alive.
				int32 Alive0, Alive1;
				GetAliveCounts(Alive0, Alive1);

				// SCENARIO 1: BOTH teams are alive. This is a TRUE TIE.
				if (Alive0 > 0 && Alive1 > 0)
				{
					if (bOvertimeEnabled)
					{
						// Start overtime, as requested
						if (!OvertimeWaveTimerHandle.IsValid())
						{
							StartOvertime();
						}
					}
					else
					{
						// No overtime, so do a health tiebreak
						const int32 Winner = GetTiebreakWinnerByTeamHealth();
						FTimerDelegate TimerDelegate;
						TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), Winner, FName(TEXT("TimeTiebreak")));
						GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.1f, false);
					}
				}
				// SCENARIO 2: One team is already dead. This is NOT a tie.
				else
				{
					// This is an elimination win that happened as time expired.
					// Do not start overtime. Just run the elimination check one last time.
					CheckRoundWinConditions();
				}

				return; // Stop further processing this tick
			}
		}

		// Time has not run out, so check for eliminations normally.
		CheckRoundWinConditions();
		Super::DefaultTimer();
	}
}




void AElimPlusGame::HandleServerManagement()
{
	// 1. Instance cleanup logic
	HandleInstanceCleanup();

	// 2. Map voting logic
	HandleMapVoting();

	// 3. Bot management (from UTGameMode::DefaultTimer)
	CheckBotCount();

	// 4. REMOVED: Force respawn logic - not needed for round-based gameplay
	// Your spawn control in RestartPlayer() already prevents unwanted spawning
		// Update the Replay id.
	if (UTIsHandlingReplays())
	{
		UDemoNetDriver* DemoNetDriver = GetWorld()->DemoNetDriver;
		if (DemoNetDriver != nullptr && DemoNetDriver->ReplayStreamer.IsValid())
		{
			UTGameState->ReplayID = DemoNetDriver->ReplayStreamer->GetReplayID();
		}
	}
}

// NEW: Handle map voting logic (from Epic's UTGameMode::DefaultTimer)
void AElimPlusGame::HandleMapVoting()
{
	if (MatchState == MatchState::MapVoteHappening)
	{
		if (GetWorld()->GetNetMode() != NM_Standalone)
		{
			UTGameState->VoteTimer--;
			if (UTGameState->VoteTimer <= 0)
			{
				UTGameState->VoteTimer = 0;
			}
		}

		// Scan the maps and see if we have a winner
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
			if (Best[0]->VoteCount > Target)
			{
				TallyMapVotes();
			}
		}
	}
}


void AElimPlusGame::HandleInstanceCleanup()
{
	
	// EDITOR SAFETY: Don't run server management in editor/blueprint context
	if (GetWorld() == nullptr || GetWorld()->WorldType != EWorldType::Game)
	{
		return;
	}

	// ADDITIONAL SAFETY: Don't run if we're not in a proper game context
	if (!HasAuthority() && GetNetMode() != NM_Standalone)
	{
		return;
	}

	// 1. Hub instance cleanup
	if (IsGameInstanceServer() && LobbyBeacon)
	{
		// Update lobby stats periodically
		if (GetWorld()->GetTimeSeconds() - LastLobbyUpdateTime >= 10.0f)
		{
			UpdateLobbyMatchStats();
		}

		if (!bDedicatedInstance)
		{
			// Empty server timeout
			if (!HasMatchStarted())
			{
				if (GetWorld()->GetRealTimeSeconds() > LobbyInitialTimeoutTime && NumPlayers <= 0 &&
					(GetNetDriver() == NULL || GetNetDriver()->ClientConnections.Num() == 0))
				{
					UE_LOG(LogGameMode, Warning, TEXT("Instance timeout - shutting down"));
					ShutdownGameInstance();
					return;
				}
			}
			else
			{
				if (NumPlayers <= 0)
				{
					UE_LOG(LogGameMode, Warning, TEXT("Instance empty - shutting down"));
					ShutdownGameInstance();
					return;
				}
			}

			// Idle player check
			if (!bIgnoreIdlePlayers && UTGameState && UTGameState->IsMatchInProgress())
			{
				bool bAllPlayersAreIdle = true;
				for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
				{
					AUTPlayerState* UTPlayerState = Cast<AUTPlayerState>(UTGameState->PlayerArray[i]);
					if (UTPlayerState && !IsPlayerIdle(UTPlayerState))
					{
						bAllPlayersAreIdle = false;
						break;
					}
				}

				if (bAllPlayersAreIdle)
				{
					UE_LOG(LogGameMode, Warning, TEXT("All players idle - shutting down"));
					ShutdownGameInstance();
					return;
				}
			}
		}
	}
	else
	{
		// 2. Non-hub server cleanup
		if (NumPlayers <= 0 && NumSpectators <= 0 && HasMatchStarted())
		{
			EmptyServerTime++;
			if (EmptyServerTime >= AutoRestartTime)
			{
				UE_LOG(LogGameMode, Warning, TEXT("Empty server timeout - restarting"));
				TravelToNextMap();
				return;
			}
		}
		else
		{
			EmptyServerTime = 0;
		}
	}

	// 3. Hub beacon connection monitoring
	if (LobbyBeacon && LobbyBeacon->GetNetConnection()->State == EConnectionState::USOCK_Closed)
	{
		if (!bDedicatedInstance && NumPlayers <= 0 && MatchState != MatchState::WaitingToStart)
		{
			UE_LOG(LogGameMode, Warning, TEXT("Lost hub connection and server empty - shutting down"));
			FPlatformMisc::RequestExit(false);
			return;
		}

		// Try to reconnect
		RecreateLobbyBeacon();
	}
}


/**
 * NEW: This function is called when the state *enters* MatchIntermission.
 * It's responsible for pre-round setup, hiding pawns, and forcing spectators.
 */
void AElimPlusGame::HandleMatchIntermission()
{
	UE_LOG(LogGameMode, Warning, TEXT("HandleMatchIntermission: Preparing for next round."));


	// Reset spawn points for the new round
	//ResetSpawnSelectionForNewRound();


	// Force losers to view winners (if LastRoundWinningTeamIndex is set)
	int32 WinnerIdx = LastRoundWinningTeamIndex;

	ForceLosersToViewWinners(WinnerIdx);
}


/**
 * REWRITTEN: This function just sets the state.
 * The DefaultTimer will handle the countdown.
 */
void AElimPlusGame::StartIntermission(int32 Seconds)
{
	UE_LOG(LogGameMode, Warning, TEXT("StartIntermission: Entering intermission for %d seconds."), Seconds);

	bRoundInProgress = false;
	IntermissionSecondsRemaining = FMath::Max(1, Seconds); // Ensure at least 1 second
	RoundEndTimeSeconds = 0.f;

	// Stop overtime if it's running
	StopOvertime();

	// PIE-SAFETY CHECK: Only update GameState if it's the correct class
	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		BP_OnSetIntermission(true, IntermissionSecondsRemaining);

		// Optionally push a network update to clients anyway
		GS->ForceNetUpdate();
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("StartIntermission: GameState is NULL. State will not be replicated."));
	}

	// This is the key: Set the match state.
	// This will trigger CallMatchStateChangeNotify -> HandleMatchIntermission
	SetMatchState(FName(TEXT("RoundCooldown")));//MatchIntermission);
}




void AElimPlusGame::StartNextRound()
{
	UE_LOG(LogGameMode, Warning, TEXT("StartNextRound: Spawning players and starting round."));

	if (bWarmupMode)
	{
		bRoundInProgress = false;
		return;
	}

	if (bAnnounceTeam)
	{
		bAnnounceTeam = false;
		UE_LOG(LogGameMode, Warning, TEXT("Disabled team announcements for subsequent rounds"));
	}

	// Reset per-round trackers
	CamperTracker.Empty();
	StartCampCheckTimer();
	RoundWinningKiller = nullptr;
	RoundWinningKillTime = 0.0f;
	WinningKillerPawn = nullptr;
	bPendingDarkHorseReplay = false;
	LastRoundWinningTeamIndex = INDEX_NONE;
	bTeam0LastManAnnounced = false;
	bTeam1LastManAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();
	ResetPlayersForNewRound();
	DarkHorseCandidates.Empty();
	ResetSpawnSelectionForNewRound();
	Team0AlivePlayers.Empty();
	Team1AlivePlayers.Empty();

	// --- PER-TEAM SPAWN SELECTION ---
	ScoreAllSpawnPoints();
	if (TotalRoundsPlayed % 2 == 0)
	{
		SelectOptimalSpawnPairForTeam(0);
		SelectOptimalSpawnPairForTeam(1);
	}
	else
	{
		SelectOptimalSpawnPairForTeam(1);
		SelectOptimalSpawnPairForTeam(0);
	}

	// --- BUILD SPAWN QUEUE (staggered across frames) ---
	// Spawning all players in one frame causes capsule overlap issues:
	// two teammates at the same PlayerStart get depenetration-shoved
	// into enemies, or the Z-fix can't detect the first pawn yet.
	// One spawn per frame lets each capsule register before the next.
	PendingSpawnQueue.Empty();
	bAllowPlayerRespawns = true;

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

			PendingSpawnQueue.Add(C);
		}
	}

	// Process first spawn immediately, rest on subsequent frames
	if (PendingSpawnQueue.Num() > 0)
	{
		ProcessNextSpawn();
	}
	else
	{
		// No players to spawn (empty server?) - finish immediately
		OnAllPlayersSpawned();
	}
}


void AElimPlusGame::ProcessNextSpawn()
{
	while (PendingSpawnQueue.Num() > 0)
	{
		AController* C = PendingSpawnQueue[0];
		PendingSpawnQueue.RemoveAt(0);

		if (!C || C->IsPendingKill()) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (!PS || PS->bOnlySpectator) continue;

		// Spawn this player
		RestartPlayer(C);

		// Track team sizes
		if (PS->Team)
		{
			if (PS->Team->TeamIndex == 0)
			{
				Team0StartingSize++;
				Team0AlivePlayers.Add(PS);
			}
			else if (PS->Team->TeamIndex == 1)
			{
				Team1StartingSize++;
				Team1AlivePlayers.Add(PS);
			}
		}

		// If more players remain, wait ~2 frames before spawning the next.
		// One frame wasn't enough for the capsule collision to fully register.
		// 0.05s is about 3 frames at 60fps, 2-3 frames at 120fps server tick.
		if (PendingSpawnQueue.Num() > 0)
		{
			GetWorldTimerManager().SetTimer(
				TimerHandle_StaggeredSpawn, this,
				&AElimPlusGame::ProcessNextSpawn,
				0.05f, false);
			return;
		}
		break;
	}

	// Queue is empty - all players spawned
	OnAllPlayersSpawned();
}


void AElimPlusGame::OnAllPlayersSpawned()
{
	bAllowPlayerRespawns = false;

	UE_LOG(LogGameMode, Warning, TEXT("Round starting sizes - Team0: %d, Team1: %d"), Team0StartingSize, Team1StartingSize);

	// Set round timer
	if (RoundTimeSeconds > 0)
	{
		RoundEndTimeSeconds = GetWorld()->GetTimeSeconds() + RoundTimeSeconds;
	}
	else
	{
		RoundEndTimeSeconds = 0.f;
	}

	// Set final round state
	bRoundInProgress = true;

	if (AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		BP_OnSetRound(true, RoundTimeSeconds, LastRoundWinningTeamIndex, Team0AlivePlayers, Team1AlivePlayers);
		BP_OnSetIntermission(false, 0);
		GS->ForceNetUpdate();
	}

	BroadcastLocalized(this, UUTGameMessage::StaticClass(), 0, NULL, NULL, NULL);

	WinCheckHoldUntilSeconds = GetWorld()->GetTimeSeconds() + 0.25f;
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);
	GetWorldTimerManager().SetTimer(
		InitialWinCheckHandle, this,
		&AElimPlusGame::DelayedInitialWinCheck, 0.25f, false);
}


void AElimPlusGame::EndRoundForTeam(int32 WinnerTeamIndex, FName Reason)
{
	UE_LOG(LogGameMode, Warning, TEXT("EndRoundForTeam called: Winner=%d, Reason=%s"),
		WinnerTeamIndex, *Reason.ToString());

	if (bWarmupMode || !bRoundInProgress)
	{
		// No scoring in warmup, or round already ended
		return;
	}

	// Stop the round *immediately*
	StopCampCheckTimer();
	bRoundInProgress = false;
	RoundEndTimeSeconds = 0.f;
	LastRoundWinningTeamIndex = WinnerTeamIndex;
	
	// Increment total rounds counter (includes wins and draws)
	TotalRoundsPlayed++;

	// Stop overtime if it's running
	StopOvertime();

	// Check achievements before we score (in case of end-game)
	CheckRoundAchievements(WinnerTeamIndex, Reason);

	// --- PPR accumulation ---
	// Round PPR per player = RoundKills + RoundDamage * 0.01.
	// RoundKills is engine-maintained on AUTPlayerState (reset to 0 in
	// ResetPlayersForNewRound at next round start). RoundDamage comes from
	// PlayerRoundDamage which is also cleared next round.
	// Accumulate into match-running mean and push to the stats replicator.
	{
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS && StatsReplicator)
		{
			for (APlayerState* PS : GS->PlayerArray)
			{
				AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
				if (!UTPS || UTPS->bOnlySpectator || !UTPS->UniqueId.IsValid()) continue;

				const float* DmgPtr = PlayerRoundDamage.Find(UTPS);
				const float RoundDamage = DmgPtr ? *DmgPtr : 0.f;
				const int32 RoundKills = UTPS->RoundKills;
				const float RoundPPR = static_cast<float>(RoundKills) + RoundDamage * 0.01f;

				PerPlayerMatchPPRSum.FindOrAdd(UTPS) += RoundPPR;
				PerPlayerMatchPPRRoundCount.FindOrAdd(UTPS) += 1;

				const int32 RoundCount = PerPlayerMatchPPRRoundCount[UTPS];
				const float MatchMean = (RoundCount > 0) ? (PerPlayerMatchPPRSum[UTPS] / RoundCount) : 0.f;

				const FString UidStr = UTPS->UniqueId.ToString();
				StatsReplicator->SetPlayerPPRCurrent(UidStr, MatchMean);

				// Lifetime PPR — fold this round's contribution into the rating system's
				// persistent TotalPoints/RoundsPlayed accumulators. Server-only / not
				// replicated; queryable via Mods.db (NCRatingElimPlus.TotalPoints +
				// RoundsPlayed). Gated to humans loaded by PostLogin (bots have no
				// cache entry, so RecordRoundPPR is a no-op for them).
				if (RatingSystem.IsValid())
				{
					RatingSystem->RecordRoundPPR(UidStr, RoundPPR);
				}
			}
		}
	}

	// --- Rating system: per-round ProcessMatch (in-memory only — no replicator
	// push, no DB write). HUD ELO display stays frozen until HandleMatchHasEnded.
	if (RatingSystem.IsValid())
	{
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS)
		{
			FElimPlusRoundResult RoundResult;
			RoundResult.bIsDraw = (WinnerTeamIndex == INDEX_NONE);

			auto BuildPerf = [this, GS](int32 TeamIdx) -> TArray<FElimPlusPlayerRoundPerf>
			{
				TArray<FElimPlusPlayerRoundPerf> Out;
				for (APlayerState* PS : GS->PlayerArray)
				{
					AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
					if (!UTPS || UTPS->bOnlySpectator) continue;
					if (UTPS->GetTeamNum() != TeamIdx) continue;

					FElimPlusPlayerRoundPerf P;
					// Bots have invalid UniqueIds. We still include them so the rating
					// system sees correct team sizes (e.g. 4v4 not 1v1 in a solo-vs-bots
					// match). Synthetic BOT:<name> key prevents collisions with humans
					// and is filtered out at write-back time so bot ratings never persist.
					P.UniqueId = UTPS->UniqueId.IsValid()
						? UTPS->UniqueId.ToString()
						: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
					P.Kills    = UTPS->RoundKills;
					P.Deaths   = UTPS->bOutOfLives ? 1 : 0; // exactly one death per round in elim
					P.Damage   = PlayerRoundDamage.Contains(UTPS) ? PlayerRoundDamage[UTPS] : 0.f;
					Out.Add(MoveTemp(P));
				}
				return Out;
			};

			if (RoundResult.bIsDraw)
			{
				// Draw: order doesn't matter; library treats both teams symmetrically.
				RoundResult.WinnerTeam = BuildPerf(0);
				RoundResult.LoserTeam  = BuildPerf(1);
			}
			else
			{
				const int32 LoserIdx = (WinnerTeamIndex == 0) ? 1 : 0;
				RoundResult.WinnerTeam = BuildPerf(WinnerTeamIndex);
				RoundResult.LoserTeam  = BuildPerf(LoserIdx);
			}

			RatingSystem->ProcessRound(RoundResult);
		}
	}

	// --- Score the round ---
	bool bIsDraw = (WinnerTeamIndex == INDEX_NONE);
	if (!bIsDraw)
	{
		if (Teams.IsValidIndex(WinnerTeamIndex))
		{
			Teams[WinnerTeamIndex]->Score += 1;
			Teams[WinnerTeamIndex]->ForceNetUpdate();
			UE_LOG(LogGameMode, Warning, TEXT("Team %d wins! New score: %d"), WinnerTeamIndex, Teams[WinnerTeamIndex]->Score);
		}
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("Round draw - no score change"));
	}

	/* -- - Check for Game End-- -
	if (!bIsDraw && Teams[WinnerTeamIndex]->Score >= GoalScore)
	{
		// Game Over
		bool bReplayTriggered = false;
		bool bIsMatchPoint = Teams[WinnerTeamIndex]->Score >= GoalScore;
		if (bIsMatchPoint)
		{
			BroadcastKillReplay();
			bReplayTriggered = true;
		}
		UE_LOG(LogGameMode, Warning, TEXT("Team %d has reached the score limit. Ending game."), WinnerTeamIndex);
		if (bReplayTriggered)
		{
			// DELAY EndGame so the replay can actually play.
			// 7.0 seconds gives time for the 0.5s delay + ~6s of replay footage
			FTimerHandle UnusedHandle;
			FTimerDelegate TimerDel;
			TimerDel.BindUFunction(this, FName("DelayedEndGame"), WinnerTeamIndex, FName(TEXT("ScoreLimit")));
			GetWorldTimerManager().SetTimer(UnusedHandle, TimerDel, 1.0f, false);

			return; // EXIT NOW, do not call EndGame immediately
		}
		//AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
		//EndGame(BestPlayer, FName(TEXT("ScoreLimit")));
		
		return; // Do not proceed to intermission
		
	}
	*/
	if (!bIsDraw && Teams[WinnerTeamIndex]->Score >= GoalScore)
	{
		// Check win-by-two requirement if enabled
		bool bCanEndMatch = true;
		if (bWinByTwo)
		{
			int32 OtherTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
			if (Teams.IsValidIndex(OtherTeamIndex))
			{
				int32 ScoreDifference = Teams[WinnerTeamIndex]->Score - Teams[OtherTeamIndex]->Score;
				bCanEndMatch = (ScoreDifference >= 2);

				if (!bCanEndMatch)
				{
					UE_LOG(LogGameMode, Warning, TEXT("Win-by-two not met: Team %d has %d, Team %d has %d (diff: %d)"),
						WinnerTeamIndex, Teams[WinnerTeamIndex]->Score,
						OtherTeamIndex, Teams[OtherTeamIndex]->Score,
						ScoreDifference);
				}
			}
		}

		if (bCanEndMatch)
		{
			// Game Over. Replay only fires on listen-server / dedicated; standalone
			// PIE skips it entirely (mirrors AUTFlagRunGame::GetLineUpTime pattern).
			const bool bIsReplayGoingToPlay = (GetNetMode() != NM_Standalone);
			if (bIsReplayGoingToPlay)
			{
				BroadcastKillReplay();
			}

			UE_LOG(LogGameMode, Warning, TEXT("Team %d has won the match. Ending game."), WinnerTeamIndex);

			// 7s when a replay is playing (0.5s start delay + ~6s replay + 0.5s tail);
			// short delay otherwise. Firing EndGame mid-replay destroys
			// WinningKillerPawn while the replay thread is still resolving its
			// NetworkGUID, causing EXCEPTION_ACCESS_VIOLATION in TaskGraphThreadNP /
			// CoreUObject.dll. Was unconditional 1.0f (mismatched the 7s comment).
			const float EndGameDelay = bIsReplayGoingToPlay ? 7.0f : 0.5f;
			FTimerHandle UnusedHandle;
			FTimerDelegate TimerDel;
			TimerDel.BindUFunction(this, FName("DelayedEndGame"), WinnerTeamIndex, FName(TEXT("ScoreLimit")));
			GetWorldTimerManager().SetTimer(UnusedHandle, TimerDel, EndGameDelay, false);

			return; // EXIT NOW, do not call EndGame immediately
		}
		// else: Win-by-two not satisfied, fall through to intermission
	}

	// --- Game is NOT over, proceed to intermission ---

	// Update GameState with winner for this intermission
	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		BP_OnSetRound(false, 0, LastRoundWinningTeamIndex, Team0AlivePlayers, Team1AlivePlayers);
		// Replication push � still valid on the base class
		GS->ForceNetUpdate();
	}

	// Broadcast round results (audio/visual)
	BroadcastRoundResults(WinnerTeamIndex, bIsDraw);

	// Check for domination/lead messages
	CheckForDominationAndLead(WinnerTeamIndex);

	// This is now called by HandleMatchIntermission, but we do it here
	// to immediately snap losers' cameras.
	ForceLosersToViewWinners(WinnerTeamIndex);

	// Start the intermission timer
	StartIntermission(AwardDisplayTime);
}


bool AElimPlusGame::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1))
	{
		return false;
	}

	int32 ScoreA = Teams[0]->Score;
	int32 ScoreB = Teams[1]->Score;

	// No win-by-two fall back to normal UT logic
	if (!bWinByTwo)
	{
		return Super::CheckScore_Implementation(Scorer);
	}

	// Normal UT fraglimit start condition
	int32 LeadingScore = FMath::Max(ScoreA, ScoreB);
	int32 TrailingScore = FMath::Min(ScoreA, ScoreB);

	// Only allow victory if the leading team reached GoalScore
	if (LeadingScore >= GoalScore)
	{
		// NOW apply Win-By-2 rule
		if ((LeadingScore - TrailingScore) >= 2)
		{
			int32 WinnerTeamIndex = (ScoreA > ScoreB) ? 0 : 1;
			AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);

			EndGame(BestPlayer, TEXT("fraglimit"));
			return true;
		}
	}

	// No win yet
	return false;
}


void AElimPlusGame::BroadcastKillReplay()
{
	// Match-winning kill replay using the FlagRun-style instant replay path
	// (ClientPlayInstantReplay), NOT ClientQueueCoolMoment. CoolMoment routes
	// through UUTKillcamPlayback::CoolMomentCamStart which has been crashing
	// after MatchEnded — TaskGraphThreadNP access-violation in CoreUObject —
	// because new round actors spawn before the playback finishes cleanup.
	// ClientPlayInstantReplay uses OnKillcamStart with a NetworkGUID-resolved
	// pawn focus and a hard stop timer, which is what Wipeout/Showdown rely on.
	//
	// Only invoked from the match-end branch of EndRoundForTeam (line ~1051),
	// so this fires on the round that wins the match — never per regular round.
	if (!RoundWinningKiller || RoundWinningKillTime <= 0.f || !WinningKillerPawn)
	{
		UE_LOG(LogGameMode, Warning, TEXT("BroadcastKillReplay: skip (no winning kill captured)"));
		return;
	}

	// Rewind enough to start ~5s before the kill so the build-up plays.
	const float TimeToRewind = (GetWorld()->GetTimeSeconds() - RoundWinningKillTime) + 5.0f;
	const float StartDelay   = 0.5f; // brief pause for the kill cam to settle

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		if (PC)
		{
			PC->ClientPlayInstantReplay(WinningKillerPawn, TimeToRewind, StartDelay);
		}
	}
}


// This function is unchanged, but its role is now clear:
// It's called by the timer set in CheckRoundWinConditions.
void AElimPlusGame::DelayedEndRound(int32 WinnerTeamIndex, FName Reason)
{
	EndRoundForTeam(WinnerTeamIndex, Reason);
}


// This function is unchanged, it's the "cleanup" step
void AElimPlusGame::ResetPlayersForNewRound()
{
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();

	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		if (!C) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (PS)
		{
			// Only reset OutOfLives, other flags are handled in StartNextRound
			PS->SetOutOfLives(false);
			PS->RoundDamageDone = 0;
			PS->RoundKills = 0;
		}

		// Get and destroy the controller's current pawn (if any)
		APawn* Pawn = C->GetPawn();
		if (Pawn)
		{
			DiscardInventory(Pawn, C);
			C->UnPossess();
			Pawn->Destroy();
		}
	}
}

// This function is unchanged
void AElimPlusGame::CleanupWorldForNewRound()
{
	// Clean up any dropped items
	for (TActorIterator<AUTDroppedPickup> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}

	// Reset any placed pickups
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



void AElimPlusGame::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer) return;

	if (APlayerController* PC = Cast<APlayerController>(NewPlayer))
	{
		if (MustSpectate(PC))
		{
			return;
		}
	}

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	if (NewPlayer->GetPawn())
	{
		return;
	}

	AUTGameState* GS = GetGameState<AUTGameState>();
	bool bLineupIsActive = (GS && GS->ActiveLineUpHelper && GS->ActiveLineUpHelper->bIsPlacingPlayers);

	if (bLineupIsActive)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	const bool bShouldAllowSpawn = (bAllowRespawnMidRound || bAllowPlayerRespawns || bWarmupMode || GetMatchState() == MatchState::WaitingToStart);

	if (bShouldAllowSpawn)
	{
		AActor* ChosenStart = ChoosePlayerStart_Implementation(NewPlayer);

		OverriddenPlayerStart  = ChosenStart;
		Super::RestartPlayer(NewPlayer);
		OverriddenPlayerStart = nullptr;

		if (!NewPlayer->GetPawn())
		{
			UE_LOG(LogGameMode, Warning, TEXT("RestartPlayer: FAILED to spawn pawn for %s"),
				NewPlayer->PlayerState ? *NewPlayer->PlayerState->PlayerName : TEXT("Unknown"));
		}
	}
}


bool AElimPlusGame::ValidateSpawnLocation(const FVector& TestLocation)
{
	// UT Characters typically have a CapsuleRadius of ~34.0f and HalfHeight of ~88.0f
	const float CapsuleRadius = 40.0f;
	const float CapsuleHalfHeight = 80.0f;

	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.bTraceComplex = false;

	// We use a Sphere Sweep instead of a Line Trace. 
	// This simulates the width of the player to ensure they don't spawn clipping into a wall.
	FCollisionShape SphereShape = FCollisionShape::MakeSphere(CapsuleRadius);

	// 1. FLOOR CHECK: Sweep downwards to find solid ground
	// Start slightly above the center to avoid starting already clipped into the floor
	FVector TraceStart = TestLocation + FVector(0.f, 0.f, 10.f);
	// Sweep down just enough to find the floor, but not so far they drop off a cliff
	FVector TraceEnd = TestLocation - FVector(0.f, 0.f, CapsuleHalfHeight + 50.f);

	bool bHitGround = GetWorld()->SweepSingleByChannel(
		Hit, TraceStart, TraceEnd, FQuat::Identity, ECC_WorldStatic, SphereShape, Params);  

	if (!bHitGround)
	{
		return false; // No floor found within safe drop distance (off a cliff or into the void)
	}

	// 2. STEEPNESS CHECK: Ensure the ground is flat enough to stand on (approx 45 degrees max)
	if (Hit.ImpactNormal.Z < 0.7f)
	{
		return false; // Ground is too steep (it's a wall or steep slope)
	}

	// 3. HEADROOM CHECK: Sweep upwards to ensure the player's head won't clip the ceiling
	FVector HeadLocation = TestLocation + FVector(0.f, 0.f, CapsuleHalfHeight);
	bool bHitCeiling = GetWorld()->SweepSingleByChannel(
		Hit, TestLocation, HeadLocation, FQuat::Identity, ECC_WorldStatic, SphereShape, Params);

	if (bHitCeiling)
	{
		return false; // Hit a ceiling, overhang, or low-hanging geometry
	}

	return true;
}



APawn* AElimPlusGame::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	FRotator StartRotation(ForceInit);
	StartRotation.Yaw = StartSpot->GetActorRotation().Yaw;
	FVector StartLocation = StartSpot->GetActorLocation();

	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	if (!PawnClass)
	{
		UE_LOG(LogGameMode, Warning, TEXT("SpawnDefaultPawnFor: No PawnClass for %s"),
			*GetNameSafe(NewPlayer));
		return nullptr;
	}

	// --- ATTEMPT 1: Strict spawn at exact PlayerStart ---
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = Instigator;
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

	APawn* ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, StartLocation), SpawnInfo);

	// --- ATTEMPT 2: Cardinal offsets (shoulder-width, ~45 units) ---
	// UT4 capsule radius is ~34-40 units. 45 units places them just
	// outside the existing pawn's capsule without overshooting into walls.
	if (!ResultPawn)
	{
		const FVector Offsets[] = {
			FVector(45.f,   0.f, 0.f),
			FVector(-45.f,   0.f, 0.f),
			FVector(0.f,  45.f, 0.f),
			FVector(0.f, -45.f, 0.f),
		};

		for (const FVector& Offset : Offsets)
		{
			ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, StartLocation + Offset), SpawnInfo);
			if (ResultPawn)
			{
				UE_LOG(LogGameMode, Log, TEXT("SpawnDefaultPawnFor: Used offset spawn at %s"), *StartSpot->GetName());
				break;
			}
		}
	}

	// --- ATTEMPT 3: Force spawn with micro-jitter ---
	// AlwaysSpawn skips FindTeleportSpot entirely, so it won't sweep
	// through walls. The jitter prevents two force-spawned pawns from
	// sharing the exact same origin (which causes physics explosions).
	if (!ResultPawn)
	{
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FVector JitteredLocation = StartLocation + FVector(FMath::RandRange(-10.f, 10.f), FMath::RandRange(-10.f, 10.f), 0.f);
		ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, JitteredLocation), SpawnInfo);
		UE_LOG(LogGameMode, Warning, TEXT("SpawnDefaultPawnFor: Force-spawned at %s"), *StartSpot->GetName());
	}

	if (!ResultPawn)
	{
		UE_LOG(LogGameMode, Error, TEXT("SpawnDefaultPawnFor: COMPLETE FAILURE at %s"), *StartLocation.ToString());
		return nullptr;
	}

	// --- POST-SPAWN VALIDATION ---
	FVector PawnLocation = ResultPawn->GetActorLocation();
	bool bLocationBad = false;

	// Drift check: engine shoved pawn too far
	float Drift2D = (PawnLocation - StartLocation).Size2D();
	if (Drift2D > 300.0f || FMath::Abs(PawnLocation.Z - StartLocation.Z) > 200.0f)
	{
		bLocationBad = true;
	}

	// Floor check: no walkable ground beneath
	if (!bLocationBad)
	{
		FHitResult FloorHit;
		FCollisionQueryParams FloorParams(TEXT("SpawnFloorCheck"), false, ResultPawn);
		bool bHitFloor = GetWorld()->LineTraceSingleByChannel(
			FloorHit, PawnLocation, PawnLocation - FVector(0.f, 0.f, 300.0f),
			ECC_WorldStatic, FloorParams);

		if (!bHitFloor || FloorHit.ImpactNormal.Z < 0.7f)
		{
			bLocationBad = true;
		}
	}

	// Recovery: teleport back with jitter to prevent overlap explosions
	if (bLocationBad)
	{
		FVector SafeRecoveryLoc = StartLocation + FVector(FMath::RandRange(-10.f, 10.f), FMath::RandRange(-10.f, 10.f), 20.0f);
		ResultPawn->SetActorLocation(SafeRecoveryLoc, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogGameMode, Warning, TEXT("SpawnDefaultPawnFor: Bad location detected, reset to PlayerStart"));
	}

	return ResultPawn;
}


void AElimPlusGame::ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	// Call parent for individual player scoring only (no team score changes)
	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	if (GetMatchState() != MatchState::InProgress || !bRoundInProgress)
		return;


	AUTPlayerState* OtherPS = Other ? Cast<AUTPlayerState>(Other->PlayerState) : nullptr;
	if (!OtherPS) return;

	// Mark them as out of lives immediately
	if (!OtherPS->bOutOfLives)
	{
		OtherPS->bOutOfLives = true;
		OtherPS->ForceNetUpdate();
	}

	int32 Alive0, Alive1;
	GetAliveCounts(Alive0, Alive1);

	CheckLastManStanding(Alive0, Alive1);

	const bool Team0Eliminated = (Alive0 == 0);
	const bool Team1Eliminated = (Alive1 == 0);

	if (Team0Eliminated || Team1Eliminated)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ScoreKill: This kill ends the round (Team0=%d, Team1=%d). Deferring spectate to EndRoundForTeam."), Alive0, Alive1);
		if (Killer && Killer->PlayerState)
		{
			RoundWinningKillTime = GetWorld()->GetTimeSeconds();
			if (Killer->GetPawn())
			{
				RoundWinningKiller = Cast<AUTPlayerState>(Killer->PlayerState);
				WinningKillerPawn  = Killer->GetPawn();   // captured for ClientPlayInstantReplay focus
			}
			// Fallback to victim if killer is gone/invalid
			if (!RoundWinningKiller && OtherPS)
			{
				RoundWinningKiller = OtherPS;
				WinningKillerPawn  = OtherPS->GetUTCharacter();
				UE_LOG(LogGameMode, Warning, TEXT("ScoreKill: Killer invalid, focusing replay on Victim: %s"), *OtherPS->PlayerName);
			}
			// CHECK FOR DARK HORSE REPLAY CONDITION
			// If the killer was a tracked Dark Horse candidate, flag this for replay
			if (RoundWinningKiller && DarkHorseCandidates.Contains(RoundWinningKiller))
			{
				bPendingDarkHorseReplay = true;
			}
		}
	}
	else
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(Other);
		if (PC && !bAllowRespawnMidRound)
		{
			// --- THIS IS THE CHANGE ---
			// Don't call ForceTeamSpectate immediately.
			// Set a timer to call it after SpectateDelay.
			//UE_LOG(LogGameMode, Warning, TEXT("ScoreKill: Round continues. Setting timer for DelayedForceSpectate in %.1f sec."), SpectateDelay);
			
			FTimerHandle TimerHandle_SpectateDelay;
			FTimerDelegate SpectateDelegate;
			SpectateDelegate.BindUFunction(this, FName("DelayedForceSpectate"), OtherPS);
			GetWorldTimerManager().SetTimer(TimerHandle_SpectateDelay, SpectateDelegate, SpectateDelay, false);
		}
	}
}



/** NEW: Implementation for the delayed spectator function */
void AElimPlusGame::DelayedForceSpectate(AUTPlayerState* DeadPS)
{
	// --- SAFETY CHECKS ---
	// 1. Check if the PlayerState is still valid
	if (DeadPS == nullptr || DeadPS->IsPendingKill())
	{
		UE_LOG(LogGameMode, Log, TEXT("DelayedForceSpectate: DeadPS is null or pending kill. Aborting."));
		return;
	}

	// 2. Check if the PlayerController is still valid
	AUTPlayerController* PC = Cast<AUTPlayerController>(DeadPS->GetOwner());
	if (PC == nullptr)
	{
		UE_LOG(LogGameMode, Log, TEXT("DelayedForceSpectate: PlayerController for %s is null. Aborting."), *DeadPS->PlayerName);
		return;
	}

	// 3. Check if the round ended *during* our delay
	if (!bRoundInProgress)
	{
		//UE_LOG(LogGameMode, Log, TEXT("DelayedForceSpectate: Round ended during delay. Aborting spectate."), *DeadPS->PlayerName);
		// EndRoundForTeam will handle their camera now
		return;
	}

	// --- ALL CHECKS PASSED ---
	//UE_LOG(LogGameMode, Log, TEXT("DelayedForceSpectate: Delay complete. Forcing %s to spectate."), *DeadPS->PlayerName);
	ForceTeamSpectate(DeadPS);
}



AActor* AElimPlusGame::ChoosePlayerStart_Implementation(AController* Player)
{


	// Get player's team
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ChoosePlayerStart: PlayerState is NULL, using fallback"));
		//return Super::ChoosePlayerStart_Implementation(Player);
		return nullptr;
	}

	if (!PS->Team)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ChoosePlayerStart: Player %s has no team, using fallback"), *PS->PlayerName);
		//return Super::ChoosePlayerStart_Implementation(Player);
		return nullptr;
	}

	const int32 TeamIndex = PS->Team->TeamIndex;
	TArray<APlayerStart*>& SelectedSpawns = (TeamIndex == 0) ? Team0SelectedSpawns : Team1SelectedSpawns;


	if (SelectedSpawns.Num() == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ChoosePlayerStart: No spawns selected for team %d, using fallback"), TeamIndex);
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	APlayerStart* ChosenSpawn = nullptr;

	if (SelectedSpawns.Num() >= 2)
	{
		APlayerStart* SpawnA = SelectedSpawns[0];
		APlayerStart* SpawnB = SelectedSpawns[1];

		int32 CountAtSpawnA = 0;
		int32 CountAtSpawnB = 0;
		float SpawnCheckRadiusSq = 150.f * 150.f;

		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (Pawn && Pawn->PlayerState)
			{
				AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
				if (OtherPS && OtherPS != PS && OtherPS->Team && OtherPS->Team->TeamIndex == TeamIndex)
				{
					FVector PawnLocation = Pawn->GetActorLocation();
					if (SpawnA && (FVector::DistSquared(PawnLocation, SpawnA->GetActorLocation()) < SpawnCheckRadiusSq))
					{
						CountAtSpawnA++;
					}
					else if (SpawnB && (FVector::DistSquared(PawnLocation, SpawnB->GetActorLocation()) < SpawnCheckRadiusSq))
					{
						CountAtSpawnB++;
					}
				}
			}
		}

		if (CountAtSpawnA < CountAtSpawnB)
		{
			ChosenSpawn = SpawnA;
		}
		else if (CountAtSpawnB < CountAtSpawnA)
		{
			ChosenSpawn = SpawnB;
		}
		else
		{
			// Counts are equal, so randomly pick one instead of forcing constant spawn pairs
			ChosenSpawn = FMath::RandBool() ? SpawnA : SpawnB;
		}
	}
	else if (SelectedSpawns.Num() > 0)
	{
		ChosenSpawn = SelectedSpawns[0];
	}

	if (!ChosenSpawn)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ChoosePlayerStart: Failed to choose spawn for team %d, using fallback"), TeamIndex);
		return Super::ChoosePlayerStart_Implementation(Player);
	}


	UE_LOG(LogGameMode, Log, TEXT("Selected spawn %s for team %d player %s"),
		*ChosenSpawn->GetName(), TeamIndex, *PS->PlayerName);

	return ChosenSpawn;
}


AActor* AElimPlusGame::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	// If we have an overridden player start (from RestartPlayer), use it
	if (OverriddenPlayerStart)
	{
		//UE_LOG(LogGameMode, Warning, TEXT("Using overriden playerstart"));
		return OverriddenPlayerStart;
	}

	UE_LOG(LogGameMode, Warning, TEXT("Calling parent findplayerstart"));
	return Super::FindPlayerStart_Implementation(Player, IncomingName);
}



void AElimPlusGame::PrecomputeSpawnLayouts()
{
	ValidLayouts_2v2.Empty();
	ValidLayouts_1v1.Empty();

	// Use AllSpawnPoints (already filtered by InitializeSpawnPointSystem)
	// instead of re-scanning the world with TActorIterator, which would
	// re-include spawns we intentionally removed (e.g., highest spawn).
	TArray<APlayerStart*> AllSpawns;
	for (const FSpawnPointDataElimPlus& SPD : AllSpawnPoints)
	{
		if (SPD.PlayerStart && !SPD.PlayerStart->IsPendingKill())
		{
			// Filter out spawns that are flagged for other game modes
			AUTPlayerStart* UTPS = Cast<AUTPlayerStart>(SPD.PlayerStart);
			if (UTPS && UTPS->bIgnoreInShowdown)
				continue;

			// Filter out team-tagged spawns (UTTeamPlayerStart with a TeamNum).
			// These are placed for CTF/Blitz flag bases and will be inside geometry
			// or right next to enemy objectives.
			AUTTeamPlayerStart* TeamStart = Cast<AUTTeamPlayerStart>(SPD.PlayerStart);
			if (TeamStart)
				continue;

			AllSpawns.Add(SPD.PlayerStart);
		}
	}

	const int32 N = AllSpawns.Num();
	if (N < 2)
	{
		UE_LOG(LogGameMode, Error, TEXT("PrecomputeSpawnLayouts: Only %d usable spawns on map!"), N);
		return;
	}

	const float SafetyThreshold = FMath::Min(MinimumEnemyHorizontalDistance, 2900.0f);

	// -------------------------------------------------------
	// MULTI-AXIS CLUSTERING (ported from AUWipeoutGame::PrecomputeSpawnLayouts).
	// Old code brute-forced O(N^4) enumeration over ALL (a,b,c,d) with no
	// requirement that teammates come from the same spatial side. That meant
	// "Team 0 pair" could span the entire map as long as their pairwise
	// separation was within MinTeammateSeparation/MaxTeammateSeparation.
	//
	// New approach: project spawns onto 5 candidate axes (E-W, N-S, NE-SW,
	// NW-SE, principal/PCA), median-split each, pick the axis with the largest
	// MinCrossDistance between sides. Then enumerate 2v2 pairs only WITHIN
	// each side, guaranteeing teammates spawn near each other and far from the
	// enemy. Same goal as Wipeout: maximise enemy distance.
	// -------------------------------------------------------

	// Centroid for axis projection origin
	FVector Centroid = FVector::ZeroVector;
	for (APlayerStart* S : AllSpawns) Centroid += S->GetActorLocation();
	Centroid /= N;

	// Principal axis via 2D covariance (largest eigenvector of 2x2 cov matrix)
	float Sxx = 0.f, Sxy = 0.f, Syy = 0.f;
	for (APlayerStart* S : AllSpawns)
	{
		FVector D = S->GetActorLocation() - Centroid;
		Sxx += D.X * D.X;
		Sxy += D.X * D.Y;
		Syy += D.Y * D.Y;
	}
	const float PrincipalAngle = 0.5f * FMath::Atan2(2.f * Sxy, Sxx - Syy);
	const FVector2D PrincipalAxis(FMath::Cos(PrincipalAngle), FMath::Sin(PrincipalAngle));

	struct FAxisCandidate { FString Name; FVector2D Dir; };
	TArray<FAxisCandidate> Candidates = {
		{ TEXT("E-W"),       FVector2D(1.f, 0.f) },
		{ TEXT("N-S"),       FVector2D(0.f, 1.f) },
		{ TEXT("NE-SW"),     FVector2D(0.707f,  0.707f) },
		{ TEXT("NW-SE"),     FVector2D(0.707f, -0.707f) },
		{ TEXT("Principal"), PrincipalAxis }
	};

	TArray<APlayerStart*> SideA, SideB;
	float BestMinCross = -1.f;
	FString BestAxisName;

	for (const FAxisCandidate& Axis : Candidates)
	{
		struct FSpawnProj { float Proj; APlayerStart* Spawn; };
		TArray<FSpawnProj> Projections;
		Projections.Reserve(N);
		for (APlayerStart* S : AllSpawns)
		{
			FVector D = S->GetActorLocation() - Centroid;
			FSpawnProj Item;
			Item.Proj = D.X * Axis.Dir.X + D.Y * Axis.Dir.Y;
			Item.Spawn = S;
			Projections.Add(Item);
		}
		Projections.Sort([](const FSpawnProj& A, const FSpawnProj& B) { return A.Proj < B.Proj; });

		const int32 SplitIdx = N / 2;
		TArray<APlayerStart*> CandA, CandB;
		for (int32 i = 0; i < SplitIdx; ++i) CandA.Add(Projections[i].Spawn);
		for (int32 i = SplitIdx; i < N; ++i) CandB.Add(Projections[i].Spawn);

		float MinCross = FLT_MAX;
		for (APlayerStart* SA : CandA)
			for (APlayerStart* SB : CandB)
				MinCross = FMath::Min(MinCross, (SA->GetActorLocation() - SB->GetActorLocation()).Size2D());

		UE_LOG(LogGameMode, Log, TEXT("  ElimPlus axis %-10s: A=%d B=%d MinCross=%.0f"),
			*Axis.Name, CandA.Num(), CandB.Num(), MinCross);

		if (MinCross > BestMinCross)
		{
			BestMinCross = MinCross;
			BestAxisName = Axis.Name;
			SideA = CandA;
			SideB = CandB;
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("ElimPlus selected axis '%s' (MinCross=%.0f, A=%d, B=%d)"),
		*BestAxisName, BestMinCross, SideA.Num(), SideB.Num());

	// -------------------------------------------------------
	// PHASE 1: 2v2 layouts — pairs come from a single side.
	// -------------------------------------------------------
	for (int32 a = 0; a < SideA.Num(); ++a)
	{
		for (int32 b = a + 1; b < SideA.Num(); ++b)
		{
			const float T0Sep = (SideA[a]->GetActorLocation() - SideA[b]->GetActorLocation()).Size2D();
			if (T0Sep < MinTeammateSeparation2D || T0Sep > MaxTeammateSeparation2D)
				continue;

			for (int32 c = 0; c < SideB.Num(); ++c)
			{
				for (int32 d = c + 1; d < SideB.Num(); ++d)
				{
					const float T1Sep = (SideB[c]->GetActorLocation() - SideB[d]->GetActorLocation()).Size2D();
					if (T1Sep < MinTeammateSeparation2D || T1Sep > MaxTeammateSeparation2D)
						continue;

					const float D_ac = (SideA[a]->GetActorLocation() - SideB[c]->GetActorLocation()).Size2D();
					const float D_ad = (SideA[a]->GetActorLocation() - SideB[d]->GetActorLocation()).Size2D();
					const float D_bc = (SideA[b]->GetActorLocation() - SideB[c]->GetActorLocation()).Size2D();
					const float D_bd = (SideA[b]->GetActorLocation() - SideB[d]->GetActorLocation()).Size2D();
					const float MinCross = FMath::Min(FMath::Min(D_ac, D_ad), FMath::Min(D_bc, D_bd));
					if (MinCross < SafetyThreshold)
						continue;

					FSpawnLayoutElimPlus Layout;
					Layout.T0_Primary   = SideA[a];
					Layout.T0_Secondary = SideA[b];
					Layout.T1_Primary   = SideB[c];
					Layout.T1_Secondary = SideB[d];
					Layout.MinCrossDistance2D = MinCross;
					Layout.T0Separation = T0Sep;
					Layout.T1Separation = T1Sep;
					Layout.UsageCount = 0;
					Layout.QualityScore = MinCross
						+ FMath::Min(T0Sep, 1500.0f) * 0.3f
						+ FMath::Min(T1Sep, 1500.0f) * 0.3f;

					ValidLayouts_2v2.Add(Layout);
				}
			}
		}
	}

	// -------------------------------------------------------
	// PHASE 2: 1v1 (stack) layouts — one anchor per side, max distance.
	// -------------------------------------------------------
	const float StackSafetyThreshold = FMath::Max(SafetyThreshold, MinimumStackSpawnDistance2D);
	for (int32 a = 0; a < SideA.Num(); ++a)
	{
		for (int32 b = 0; b < SideB.Num(); ++b)
		{
			const float Dist2D = (SideA[a]->GetActorLocation() - SideB[b]->GetActorLocation()).Size2D();
			if (Dist2D < StackSafetyThreshold) continue;

			FSpawnLayoutElimPlus Layout;
			Layout.T0_Primary = SideA[a];  Layout.T0_Secondary = nullptr;
			Layout.T1_Primary = SideB[b];  Layout.T1_Secondary = nullptr;
			Layout.MinCrossDistance2D = Dist2D;
			Layout.T0Separation = 0.f;
			Layout.T1Separation = 0.f;
			Layout.UsageCount = 0;
			Layout.QualityScore = Dist2D;

			ValidLayouts_1v1.Add(Layout);
		}
	}

	// Sort both arrays by quality (best first)
	ValidLayouts_2v2.Sort([](const FSpawnLayoutElimPlus& A, const FSpawnLayoutElimPlus& B)
		{
			return A.QualityScore > B.QualityScore;
		});

	ValidLayouts_1v1.Sort([](const FSpawnLayoutElimPlus& A, const FSpawnLayoutElimPlus& B)
		{
			return A.QualityScore > B.QualityScore;
		});

	UE_LOG(LogGameMode, Log, TEXT("=== SPAWN PRECOMPUTE COMPLETE ==="));
	UE_LOG(LogGameMode, Log, TEXT("  Usable spawns: %d"), N);
	UE_LOG(LogGameMode, Log, TEXT("  Valid 2v2 layouts: %d"), ValidLayouts_2v2.Num());
	UE_LOG(LogGameMode, Log, TEXT("  Valid 1v1 layouts: %d"), ValidLayouts_1v1.Num());

	if (ValidLayouts_2v2.Num() > 0)
	{
		UE_LOG(LogGameMode, Log, TEXT("  Best 2v2 cross distance: %.0f"), ValidLayouts_2v2[0].MinCrossDistance2D);
		UE_LOG(LogGameMode, Log, TEXT("  Worst 2v2 cross distance: %.0f"), ValidLayouts_2v2.Last().MinCrossDistance2D);
	}

	if (ValidLayouts_2v2.Num() == 0 && ValidLayouts_1v1.Num() == 0)
	{
		UE_LOG(LogGameMode, Error, TEXT("  !!! NO VALID SPAWN LAYOUTS - MAP IS BROKEN OR THRESHOLD TOO HIGH !!!"));
		UE_LOG(LogGameMode, Error, TEXT("  SafetyThreshold=%.0f  StackSafetyThreshold=%.0f"), SafetyThreshold, StackSafetyThreshold);
	}
	else if (ValidLayouts_2v2.Num() == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("  No valid 2v2 layouts - all rounds will use 1v1 stack spawns on this map"));
	}
}



void AElimPlusGame::SelectSpawnLayoutForRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();

	// Decide which pool to use
	//TArray<FSpawnLayoutElimPlus>& Pool = (ValidLayouts_2v2.Num() > 0) ? ValidLayouts_2v2 : ValidLayouts_1v1;
	bool bForce1v1 = (TotalRoundsPlayed % 3 == 0) && (ValidLayouts_1v1.Num() > 0);
	TArray<FSpawnLayoutElimPlus>& Pool = (!bForce1v1 && ValidLayouts_2v2.Num() > 0) ? ValidLayouts_2v2 : ValidLayouts_1v1;


	if (Pool.Num() == 0)
	{
		UE_LOG(LogGameMode, Error, TEXT("SelectSpawnLayoutForRound: No valid layouts available!"));
		return;
	}

	// -------------------------------------------------------
	// SELECTION STRATEGY:
	// Pick from the top-quality layouts, weighted against usage.
	// This gives you variety without ever picking a bad layout.
	// -------------------------------------------------------

	// Consider the top N layouts (or all if fewer exist)
	const int32 PoolSize = FMath::Min(Pool.Num(), 30);

	// Score each candidate: quality minus usage penalty
	int32 BestIndex = 0;
	float BestSelectionScore = -FLT_MAX;

	for (int32 i = 0; i < PoolSize; ++i)
	{
		// Penalize repeated use. Each prior use reduces score.
		// The penalty scales so even the best layout gets rotated out.
		float UsagePenalty = Pool[i].UsageCount * 3000.0f;

		// Small random jitter for variety when scores are close
		float Jitter = FMath::FRandRange(0.0f, 500.0f);

		float SelectionScore = Pool[i].QualityScore - UsagePenalty + Jitter;

		if (SelectionScore > BestSelectionScore)
		{
			BestSelectionScore = SelectionScore;
			BestIndex = i;
		}
	}

	FSpawnLayoutElimPlus& Chosen = Pool[BestIndex];
	Chosen.UsageCount++;

	// -------------------------------------------------------
	// TEAM ASSIGNMENT:
	// Alternate which team gets which side each round.
	// On even rounds, T0 gets the T0 slots. On odd rounds, swap.
	// This replaces your entire team-axis rotation system.
	// -------------------------------------------------------
	bool bSwapTeams = (TotalRoundsPlayed % 2 == 1);

	APlayerStart* FinalT0_Primary = bSwapTeams ? Chosen.T1_Primary : Chosen.T0_Primary;
	APlayerStart* FinalT0_Secondary = bSwapTeams ? Chosen.T1_Secondary : Chosen.T0_Secondary;
	APlayerStart* FinalT1_Primary = bSwapTeams ? Chosen.T0_Primary : Chosen.T1_Primary;
	APlayerStart* FinalT1_Secondary = bSwapTeams ? Chosen.T0_Secondary : Chosen.T1_Secondary;

	// Assign Team 0
	Team0SelectedSpawns.Add(FinalT0_Primary);
	if (FinalT0_Secondary)
	{
		Team0SelectedSpawns.Add(FinalT0_Secondary);
	}

	// Assign Team 1
	Team1SelectedSpawns.Add(FinalT1_Primary);
	if (FinalT1_Secondary)
	{
		Team1SelectedSpawns.Add(FinalT1_Secondary);
	}

	UE_LOG(LogGameMode, Log, TEXT("=== ROUND %d SPAWN LAYOUT ==="), TotalRoundsPlayed);
	UE_LOG(LogGameMode, Log, TEXT("  Layout index: %d (Usage: %d)  CrossDist: %.0f"),
		BestIndex, Chosen.UsageCount, Chosen.MinCrossDistance2D);
	UE_LOG(LogGameMode, Log, TEXT("  Team 0: %s + %s"),
		FinalT0_Primary ? *FinalT0_Primary->GetName() : TEXT("NULL"),
		FinalT0_Secondary ? *FinalT0_Secondary->GetName() : TEXT("STACK"));
	UE_LOG(LogGameMode, Log, TEXT("  Team 1: %s + %s"),
		FinalT1_Primary ? *FinalT1_Primary->GetName() : TEXT("NULL"),
		FinalT1_Secondary ? *FinalT1_Secondary->GetName() : TEXT("STACK"));
}


void AElimPlusGame::InitializeSpawnPointSystem()
{
	UE_LOG(LogGameMode, Log, TEXT("InitializeSpawnPointSystem: Starting comprehensive spawn analysis"));
	AllSpawnPoints.Empty();
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (APlayerStart* Spawn = *It)
		{
			// Skip spawns meant for other game modes
			AUTPlayerStart* UTPS = Cast<AUTPlayerStart>(Spawn);
			if (UTPS && UTPS->bIgnoreInShowdown)
				continue;

			// Skip team-tagged spawns (CTF/Blitz flag-base spawns)
			if (Cast<AUTTeamPlayerStart>(Spawn))
				continue;

			AllSpawnPoints.Add(FSpawnPointDataElimPlus(Spawn));
		}
	}
	if (AllSpawnPoints.Num() < 2)
	{
		UE_LOG(LogGameMode, Error, TEXT("InitializeSpawnPointSystem: Insufficient spawn points (%d), need at least 2"), AllSpawnPoints.Num());
		return;
	}

	// Score FIRST so HeightScore is populated before we sort by it
	ScoreAllSpawnPoints();

	// Only run this if we have enough spawns to safely remove some
	if (AllSpawnPoints.Num() > 4)
	{
		AllSpawnPoints.Sort([](const FSpawnPointDataElimPlus& A, const FSpawnPointDataElimPlus& B)
			{
				return A.HeightScore > B.HeightScore;
			});

		UE_LOG(LogGameMode, Warning, TEXT("InitializeSpawnPointSystem: Removing top spawn for fairness."));

		if (AllSpawnPoints[0].PlayerStart)
		{
			UE_LOG(LogGameMode, Warning, TEXT("  - Removing: %s (HeightScore: %f)"), *AllSpawnPoints[0].PlayerStart->GetName(), AllSpawnPoints[0].HeightScore);
		}

		AllSpawnPoints.RemoveAt(0);
	}
	UE_LOG(LogGameMode, Log, TEXT("InitializeSpawnPointSystem: Analyzed %d spawn points"), AllSpawnPoints.Num());
}

void AElimPlusGame::ScoreAllSpawnPoints()
{
	if (AllSpawnPoints.Num() == 0) return;

	// 1. Calculate Bounds (AABB) 
	FVector MapMin = AllSpawnPoints[0].PlayerStart->GetActorLocation();
	FVector MapMax = MapMin;

	for (const FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints)
	{
		if (!SpawnData.PlayerStart) continue;

		const FVector& Location = SpawnData.PlayerStart->GetActorLocation();
		MapMin.X = FMath::Min(MapMin.X, Location.X);
		MapMin.Y = FMath::Min(MapMin.Y, Location.Y);
		MapMin.Z = FMath::Min(MapMin.Z, Location.Z);
		MapMax.X = FMath::Max(MapMax.X, Location.X);
		MapMax.Y = FMath::Max(MapMax.Y, Location.Y);
		MapMax.Z = FMath::Max(MapMax.Z, Location.Z);
	}

	// 2. Use Geometric Center
	const FVector MapCenter = (MapMin + MapMax) * 0.5f;
	const FVector MapExtents = (MapMax - MapMin) * 0.5f;
	const FVector MapSize = MapMax - MapMin;

	// 3. Determine the "Team Axis" based on Round Rotation
	FVector TeamAxis = FVector(1.f, 0.f, 0.f);

	int32 CycleStep = CurrentRoundNumber % 4;

	switch (CycleStep)
	{
	case 1: // Round 1, 5, 9...
		// Axis: Standard X (East)
		TeamAxis = FVector(1.f, 0.f, 0.f);
		break;

	case 2: // Round 2, 6, 10...
		// Axis: Inverted Y (South)
		TeamAxis = FVector(0.f, -1.f, 0.f);
		break;

	case 3: // Round 3, 7, 11...
		// Axis: Inverted X (West)
		TeamAxis = FVector(-1.f, 0.f, 0.f);
		break;

	case 0: // Round 4, 8, 12...
		// Axis: Standard Y (North)
		TeamAxis = FVector(0.f, 1.f, 0.f);
		break;
	}

	// !!! IMPORTANT: I REMOVED THE DIAGONAL CHECK HERE !!!
	// If you leave the diagonal check, it will overwrite your TeamAxis 
	// on square maps like DM-Pending.

	// Pre-calculate the maximum projection distance
	const float MaxProjectionDist = FMath::Abs(FVector::DotProduct(MapExtents, TeamAxis));
	const bool bHasValidProjection = MaxProjectionDist > 1.0f;
	const bool bHasValidHeight = MapSize.Z > 100.f;

	// 4. Score all spawn points 
	for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints)
	{
		if (!SpawnData.PlayerStart) continue;

		const FVector& Location = SpawnData.PlayerStart->GetActorLocation();

		// --- Calculate Height Score (Z-Relative) --- 
		SpawnData.HeightScore = bHasValidHeight
			? ((Location.Z - MapMin.Z) / MapSize.Z)
			: 0.5f;

		// --- Calculate TeamSideScore via Projection ---
		if (bHasValidProjection)
		{
			const FVector RelativePos = Location - MapCenter;
			const float ProjectedDist = FVector::DotProduct(RelativePos, TeamAxis);
			SpawnData.TeamSideScore = FMath::Clamp(ProjectedDist / MaxProjectionDist, -1.0f, 1.0f);
		}
		else
		{
			SpawnData.TeamSideScore = 0.0f;
		}
	}
}



/*
void AElimPlusGame::SelectOptimalSpawnPairForTeam(int32 TeamIndex)
{
	TArray<APlayerStart*>& SelectedSpawns = (TeamIndex == 0) ? Team0SelectedSpawns : Team1SelectedSpawns;
	const TArray<APlayerStart*>& EnemySpawns = (TeamIndex == 0) ? Team1SelectedSpawns : Team0SelectedSpawns;
	SelectedSpawns.Empty();

	TArray<FSpawnPointDataElimPlus*> Candidates = GetSpawnCandidatesForTeam(TeamIndex);
	if (Candidates.Num() < 2)
	{
		UE_LOG(LogGameMode, Warning, TEXT("SelectOptimalSpawnPairForTeam: Insufficient candidates for team %d (%d available)"), TeamIndex, Candidates.Num());
		for (FSpawnPointDataElimPlus* Candidate : Candidates)
		{
			if (Candidate && Candidate->PlayerStart)
			{
				SelectedSpawns.Add(Candidate->PlayerStart);
				Candidate->IncrementUsageForTeam(TeamIndex, CurrentRoundNumber);
			}
		}
		return;
	}

	// Shuffle candidates to introduce variety
	for (int32 i = Candidates.Num() - 1; i > 0; i--)
	{
		int32 j = FMath::RandRange(0, i);
		if (i != j)
		{
			Candidates.Swap(i, j);
		}
	}

	APlayerStart* PrimarySpawn = nullptr;
	APlayerStart* SecondarySpawn = nullptr;

	FindMaxDistanceSpawnPair(Candidates, EnemySpawns, TeamIndex,PrimarySpawn, SecondarySpawn);

	if (PrimarySpawn)
	{
		SelectedSpawns.Add(PrimarySpawn);
		for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints)
		{
			if (SpawnData.PlayerStart == PrimarySpawn)
			{
				SpawnData.IncrementUsageForTeam(TeamIndex, CurrentRoundNumber);
				break;
			}
		}
	}

	// IMPORTANT: Check if SecondarySpawn is valid before adding it
	// The fallback logic may return nullptr here to force a stack spawn.
	if (SecondarySpawn)
	{
		SelectedSpawns.Add(SecondarySpawn);
		for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints)
		{
			if (SpawnData.PlayerStart == SecondarySpawn)
			{
				SpawnData.IncrementUsageForTeam(TeamIndex, CurrentRoundNumber);
				break;
			}
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("SelectOptimalSpawnPairForTeam: Team %d selected %s and %s"),
		TeamIndex,
		PrimarySpawn ? *PrimarySpawn->GetName() : TEXT("None"),
		SecondarySpawn ? *SecondarySpawn->GetName() : TEXT("None (Fallback/Stack)"));
}
*/


void AElimPlusGame::SelectOptimalSpawnPairForTeam(int32 TeamIndex)
{
	TArray<APlayerStart*>& SelectedSpawns = (TeamIndex == 0) ? Team0SelectedSpawns : Team1SelectedSpawns;
	const TArray<APlayerStart*>& EnemySpawns = (TeamIndex == 0) ? Team1SelectedSpawns : Team0SelectedSpawns;
	SelectedSpawns.Empty();

	TArray<FSpawnPointDataElimPlus*> AllCandidates = GetSpawnCandidatesForTeam(TeamIndex);

	// --- STEP 1: HARD SAFETY FILTER ---
	// We create a new list containing ONLY spawns that are strictly safe.
	// We do not care about "average" distance. If a spawn is close to an enemy, it is dead to us.

	TArray<FSpawnPointDataElimPlus*> SafeCandidates;
	//const float HardSafetyThreshold = 2700.0f; // Minimum safe distance (Teleport range is ~1500)
	//const float HardSafetyThreshold = MinimumEnemyHorizontalDistance;
	const float SafetyThreshold = FMath::Min(MinimumEnemyHorizontalDistance, 4500.0f);

	if (EnemySpawns.Num() > 0)
	{
		for (FSpawnPointDataElimPlus* Cand : AllCandidates)
		{
			if (!Cand || !Cand->PlayerStart) continue;

			bool bIsStrictlySafe = true;
			for (APlayerStart* EnemySpawn : EnemySpawns)
			{
				float Dist3D = FVector::Dist(Cand->PlayerStart->GetActorLocation(), EnemySpawn->GetActorLocation());
				float Dist2D = (Cand->PlayerStart->GetActorLocation() - EnemySpawn->GetActorLocation()).Size2D();

				if (Dist3D < MinimumEnemySpawnDistance || Dist2D < MinimumEnemyHorizontalDistance)
				{
					bIsStrictlySafe = false;
					break;
				}
			}

			if (bIsStrictlySafe)
			{
				SafeCandidates.Add(Cand);
			}
		}
	}
	else
	{
		// No enemies yet (First Pick), so all candidates are technically safe
		SafeCandidates = AllCandidates;
	}

	APlayerStart* PrimarySpawn = nullptr;
	APlayerStart* SecondarySpawn = nullptr;

	// --- STEP 2: SUFFICIENCY CHECK ---

	if (SafeCandidates.Num() >= 2)
	{
		// SCENARIO A: We have at least 2 safe spots. 
		// Now we can use your scoring logic to find the best PAIR among these safe ones.
		FindMaxDistanceSpawnPair(SafeCandidates, EnemySpawns, TeamIndex, PrimarySpawn, SecondarySpawn);
	}
	else if (SafeCandidates.Num() == 1)
	{
		// SCENARIO B: We only have 1 safe spot.
		// Forcing a stack spawn to ensure safety.
		UE_LOG(LogGameMode, Warning, TEXT("Spawn System: Only 1 safe spawn found for Team %d. Forcing stack spawn at %s."), TeamIndex, *SafeCandidates[0]->PlayerStart->GetName());
		PrimarySpawn = SafeCandidates[0]->PlayerStart;
		SecondarySpawn = nullptr; // Explicitly null to force stack
	}
	else
	{
		// SCENARIO C: ZERO safe spots (Emergency).
		// The enemy has total map control. Find the absolute furthest single point.
		UE_LOG(LogGameMode, Warning, TEXT("Spawn System: CRITICAL - No safe spawns found. Finding absolute furthest single point."));

		float BestDist = -1.0f;
		for (FSpawnPointDataElimPlus* Cand : AllCandidates)
		{
			float Dist = CalculateMinDistanceToEnemySpawns(Cand->PlayerStart, EnemySpawns);
			if (Dist > BestDist)
			{
				BestDist = Dist;
				PrimarySpawn = Cand->PlayerStart;
			}
		}
		SecondarySpawn = nullptr; // Do not risk a second spawn
	}

	// --- ASSIGNMENT ---
	if (PrimarySpawn)
	{
		SelectedSpawns.Add(PrimarySpawn);
		for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints) {
			if (SpawnData.PlayerStart == PrimarySpawn) {
				SpawnData.IncrementUsageForTeam(TeamIndex, CurrentRoundNumber);
				break;
			}
		}
	}

	if (SecondarySpawn)
	{
		SelectedSpawns.Add(SecondarySpawn);
		for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints) {
			if (SpawnData.PlayerStart == SecondarySpawn) {
				SpawnData.IncrementUsageForTeam(TeamIndex, CurrentRoundNumber);
				break;
			}
		}
	}
}




void AElimPlusGame::FindMaxDistanceSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, const TArray<APlayerStart*>& EnemySpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary)
{
	OutPrimary = nullptr;
	OutSecondary = nullptr;

	if (CandidateSpawns.Num() < 2)
	{
		if (CandidateSpawns.Num() == 1)
		{
			OutPrimary = CandidateSpawns[0]->PlayerStart;
		}
		return;
	}

	// Heuristic: If we have no enemies (First Pick), we MUST rely on TeamSideScore.
	bool bBlindPick = (EnemySpawns.Num() == 0);

	float CurrentDistanceWeight = bBlindPick ? 0.0f : SpawnDistanceWeight;
	float CurrentHeightWeight = SpawnHeightWeight;
	float CurrentUsageWeight = SpawnUsageWeight;
	float CurrentSeparationWeight = SpawnSeparationWeight;

	// DRASTICALLY increase Side Bias during blind picks
	float SideBiasWeight = bBlindPick ? 5.0f : 0.0f;

	// Normalize
	float TotalWeight = CurrentDistanceWeight + CurrentHeightWeight + CurrentUsageWeight + CurrentSeparationWeight + SideBiasWeight;
	if (TotalWeight > KINDA_SMALL_NUMBER)
	{
		CurrentDistanceWeight /= TotalWeight;
		CurrentHeightWeight /= TotalWeight;
		CurrentUsageWeight /= TotalWeight;
		CurrentSeparationWeight /= TotalWeight;
		SideBiasWeight /= TotalWeight;
	}

	struct FSpawnPairScore
	{
		float Score;
		int32 Index1;
		int32 Index2;
		FSpawnPairScore(float InScore, int32 InIndex1, int32 InIndex2) : Score(InScore), Index1(InIndex1), Index2(InIndex2) {}
	};

	TArray<FSpawnPairScore> ScoredPairs;
	ScoredPairs.Reserve(CandidateSpawns.Num() * 2);

	for (int32 i = 0; i < CandidateSpawns.Num(); ++i)
	{
		for (int32 j = i + 1; j < CandidateSpawns.Num(); ++j)
		{
			APlayerStart* Spawn1 = CandidateSpawns[i]->PlayerStart;
			APlayerStart* Spawn2 = CandidateSpawns[j]->PlayerStart;
			if (!Spawn1 || !Spawn2) continue;

			// 1. Enemy Distance Score
			float MinDist1 = CalculateMinDistanceToEnemySpawns(Spawn1, EnemySpawns);
			float MinDist2 = CalculateMinDistanceToEnemySpawns(Spawn2, EnemySpawns);

			// Normalize distance score (clamp at sensible max map size, e.g., 5000 units)
			float DistScore1 = FMath::Clamp(MinDist1 / 5000.0f, 0.0f, 1.0f);
			float DistScore2 = FMath::Clamp(MinDist2 / 5000.0f, 0.0f, 1.0f);

			// 2. Vertical Stacking Check (The Bio/Mini Fix) & Proximity
			// Note: We don't need heavy penalties here anymore because SelectOptimalSpawnPairForTeam
			// has already filtered out the truly dangerous ones.

			float HeightScore1 = CandidateSpawns[i]->HeightScore;
			float HeightScore2 = CandidateSpawns[j]->HeightScore;

			float UsageScore1 = 1.0f / (1.0f + CandidateSpawns[i]->GetUsageCountForTeam(0) + CandidateSpawns[i]->GetUsageCountForTeam(1));
			float UsageScore2 = 1.0f / (1.0f + CandidateSpawns[j]->GetUsageCountForTeam(0) + CandidateSpawns[j]->GetUsageCountForTeam(1));

			// Teammate Separation Score (Prefer spreading out)
			float SpawnSeparation = FVector::Dist(Spawn1->GetActorLocation(), Spawn2->GetActorLocation());
			float SeparationScore = FMath::Min(SpawnSeparation / 1500.0f, 1.0f);

			// SIDE BIAS CALCULATION (Crucial for Round 1 Blind Picks)
			float SideScore1 = IsSpawnOnHomeSide(*CandidateSpawns[i], TeamIndex) ? 1.0f : 0.0f;
			float SideScore2 = IsSpawnOnHomeSide(*CandidateSpawns[j], TeamIndex) ? 1.0f : 0.0f;

			float CombinedScore =
				(DistScore1 + DistScore2) * CurrentDistanceWeight +
				(HeightScore1 + HeightScore2) * CurrentHeightWeight +
				(UsageScore1 + UsageScore2) * CurrentUsageWeight +
				SeparationScore * CurrentSeparationWeight +
				(SideScore1 + SideScore2) * SideBiasWeight;

			CombinedScore += FMath::FRandRange(-0.05f, 0.05f); // Slight jitter
			ScoredPairs.Add(FSpawnPairScore(CombinedScore, i, j));
		}
	}

	if (ScoredPairs.Num() > 0)
	{
		ScoredPairs.Sort([](const FSpawnPairScore& A, const FSpawnPairScore& B) { return A.Score > B.Score; });
		OutPrimary = CandidateSpawns[ScoredPairs[0].Index1]->PlayerStart;
		OutSecondary = CandidateSpawns[ScoredPairs[0].Index2]->PlayerStart;
	}
}



void AElimPlusGame::FindLeastUsedSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary)
{
	OutPrimary = nullptr;
	OutSecondary = nullptr;

	if (CandidateSpawns.Num() < 2)
	{
		if (CandidateSpawns.Num() == 1)
		{
			OutPrimary = CandidateSpawns[0]->PlayerStart;
		}
		return;
	}

	// Sort by usage count (ascending) with some randomization
	TArray<FSpawnPointDataElimPlus*> SortedCandidates = CandidateSpawns;
	SortedCandidates.Sort([TeamIndex](const FSpawnPointDataElimPlus& A, const FSpawnPointDataElimPlus& B)
		{
			int32 UsageA = A.GetUsageCountForTeam(TeamIndex);
			int32 UsageB = B.GetUsageCountForTeam(TeamIndex);

			// If usage counts are equal, randomize
			if (UsageA == UsageB)
			{
				return FMath::RandBool();
			}
			return UsageA < UsageB;
		});

	// Take the two least used spawns
	OutPrimary = SortedCandidates[0]->PlayerStart;
	if (SortedCandidates.Num() > 1)
	{
		OutSecondary = SortedCandidates[1]->PlayerStart;
	}
}



// NEW: Balanced selection with randomization
void AElimPlusGame::FindBalancedRandomSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, const TArray<APlayerStart*>& EnemySpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary)
{
	OutPrimary = nullptr;
	OutSecondary = nullptr;

	if (CandidateSpawns.Num() < 2)
	{
		if (CandidateSpawns.Num() == 1)
		{
			OutPrimary = CandidateSpawns[0]->PlayerStart;
		}
		return;
	}

	// Create a weighted selection pool
	TArray<float> SpawnScores;
	SpawnScores.Reserve(CandidateSpawns.Num());

	// Calculate scores for all candidates
	for (const FSpawnPointDataElimPlus* SpawnData : CandidateSpawns)
	{
		if (!SpawnData || !SpawnData->PlayerStart) continue;

		float MinDist = CalculateMinDistanceToEnemySpawns(SpawnData->PlayerStart, EnemySpawns);
		float UsageScore = 1.0f / (1.0f + SpawnData->GetUsageCountForTeam(TeamIndex) + 1.0f); // +1 to avoid division issues
		float HeightScore = SpawnData->HeightScore;

		// Add some randomness to the score
		float RandomFactor = FMath::FRandRange(0.8f, 1.2f);

		float CombinedScore = (MinDist * 0.4f + UsageScore * 0.4f + HeightScore * 0.2f) * RandomFactor;
		SpawnScores.Add(CombinedScore);
	}

	// Select top candidates with some randomness
	int32 TopCandidatesCount = FMath::Min(6, CandidateSpawns.Num()); // Consider top 6 candidates

	// Sort indices by score (descending)
	TArray<int32> SortedIndices;
	for (int32 i = 0; i < CandidateSpawns.Num(); i++)
	{
		SortedIndices.Add(i);
	}

	SortedIndices.Sort([&SpawnScores](const int32& A, const int32& B)
		{
			return SpawnScores[A] > SpawnScores[B];
		});

	// Randomly select from top candidates
	int32 PrimaryIndex = SortedIndices[FMath::RandRange(0, FMath::Min(2, TopCandidatesCount - 1))]; // Top 3
	OutPrimary = CandidateSpawns[PrimaryIndex]->PlayerStart;

	// Select secondary from remaining top candidates
	if (TopCandidatesCount > 1)
	{
		TArray<int32> RemainingIndices;
		for (int32 i = 0; i < TopCandidatesCount; i++)
		{
			if (SortedIndices[i] != PrimaryIndex)
			{
				RemainingIndices.Add(SortedIndices[i]);
			}
		}

		if (RemainingIndices.Num() > 0)
		{
			int32 SecondaryIndex = RemainingIndices[FMath::RandRange(0, FMath::Min(2, RemainingIndices.Num() - 1))];
			OutSecondary = CandidateSpawns[SecondaryIndex]->PlayerStart;
		}
	}
}





float AElimPlusGame::CalculateMinDistanceToEnemySpawns(APlayerStart* SpawnPoint, const TArray<APlayerStart*>& EnemySpawns)
{
	if (!SpawnPoint || EnemySpawns.Num() == 0)
	{
		return 10000.0f;
	}
	float MinDistance = FLT_MAX;
	FVector SpawnLocation = SpawnPoint->GetActorLocation();
	for (APlayerStart* EnemySpawn : EnemySpawns)
	{
		if (EnemySpawn)
		{
			float Distance = FVector::Dist(SpawnLocation, EnemySpawn->GetActorLocation());
			MinDistance = FMath::Min(MinDistance, Distance);
		}
	}
	return (MinDistance == FLT_MAX) ? 10000.0f : MinDistance;
}


FVector AElimPlusGame::FindSafeSpawnOffset(APlayerStart* BaseSpawn, int32 AttemptIndex)
{
	if (!BaseSpawn) return FVector::ZeroVector;
	FVector BaseLocation = BaseSpawn->GetActorLocation();
	float Angle = (AttemptIndex * 45.0f) * PI / 180.0f;
	float Distance = SpawnOffsetDistance * (1.0f + AttemptIndex * 0.5f);
	FVector Offset = FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, 0.0f);
	FVector TestLocation = BaseLocation + Offset;
	return TestLocation;
}




// IMPROVED: Add randomization to candidate selection
TArray<FSpawnPointDataElimPlus*> AElimPlusGame::GetSpawnCandidatesForTeam(int32 TeamIndex)
{
	TArray<FSpawnPointDataElimPlus*> Candidates;

	// SOFT-FILTER: Include ALL spawns - safety (distance from enemies) will determine
	// which spawns are viable. Home-side preference is applied as a BONUS in scoring,
	// not as a hard filter that could trap a team near enemies.
	for (FSpawnPointDataElimPlus& SpawnData : AllSpawnPoints)
	{
		if (!SpawnData.PlayerStart) continue;
		Candidates.Add(&SpawnData);
	}

	// Shuffle to add variety when scores are similar
	for (int32 i = Candidates.Num() - 1; i > 0; i--)
	{
		int32 j = FMath::RandRange(0, i);
		if (i != j)
		{
			Candidates.Swap(i, j);
		}
	}

	return Candidates;
}

// Helper function to check if a spawn is on the team's "home side" for this round
bool AElimPlusGame::IsSpawnOnHomeSide(const FSpawnPointDataElimPlus& SpawnData, int32 TeamIndex) const
{
	bool bSwapSides = (CurrentRoundNumber % 2 == 1);

	if (bSwapSides)
	{
		// ODD round: Team 0 is Positive, Team 1 is Negative
		return (TeamIndex == 0 && SpawnData.TeamSideScore >= 0.0f) ||
			(TeamIndex == 1 && SpawnData.TeamSideScore <= 0.0f);
	}
	else
	{
		// EVEN round: Team 0 is Negative, Team 1 is Positive
		return (TeamIndex == 0 && SpawnData.TeamSideScore <= 0.0f) ||
			(TeamIndex == 1 && SpawnData.TeamSideScore >= 0.0f);
	}
}


bool AElimPlusGame::IsLocationClearOfPlayers(const FVector& Location, float CheckRadius)
{
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (APawn* Pawn = It->Get())
		{
			if (Pawn->IsValidLowLevel() && !Pawn->IsPendingKill())
			{
				float Distance = FVector::Dist(Location, Pawn->GetActorLocation());
				if (Distance < CheckRadius)
				{
					return false;
				}
			}
		}
	}
	return true;
}

void AElimPlusGame::ResetSpawnSelectionForNewRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();
	++CurrentRoundNumber;
	//UE_LOG(LogGameMode, Log, TEXT("ResetSpawnSelectionForNewRound: Starting round %d"), CurrentRoundNumber);
	FMath::SRandInit(static_cast<int32>(FPlatformTime::Cycles()));
}

void AElimPlusGame::ForceTeamSpectate(AUTPlayerState* DeadPS)
{
	if (DeadPS == nullptr) return;

	if (useBPSpecFunction)
	{
		// Call the Blueprint-implemented function if the flag is set
		BP_SpectatePSImplementation(DeadPS);
		return;
	}

	if (!DeadPS->bOutOfLives)
	{
		DeadPS->bOutOfLives = true;
		DeadPS->ForceNetUpdate();
	}
	AUTPlayerController* PC = Cast<AUTPlayerController>(DeadPS->GetOwner());
	if (!PC) return;
	PC->ChangeState(NAME_Spectating);
	PC->ClientGotoState(NAME_Spectating);
	if (AUTPlayerState* TeamTarget = FindAliveTeammate(DeadPS))
	{
		PC->SetViewTarget(TeamTarget->GetUTCharacter());
		//PC->ServerViewPlayerState(TeamTarget);
		PC->bSpectateBehindView = false;  // Force first person view
		PC->BehindView(false);
		return;
	}
	if (AUTPlayerState* EnemyTarget = FindAliveEnemy(DeadPS))
	{
		PC->SetViewTarget(EnemyTarget->GetUTCharacter());
		//PC->ServerViewPlayerState(EnemyTarget);
		PC->bSpectateBehindView = false;  // Force first person view
		PC->BehindView(false);
		return;
	}
	PC->ServerViewSelf();
}

AUTPlayerState* AElimPlusGame::FindAliveEnemy(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;

	const int32 MyTeam = PS->Team->TeamIndex;

	// Use team members to include both human players and bots
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
			if (P && (!UTC || !UTC->IsDead()))
			{
				return OtherPS;
			}
		}
	}

	return nullptr;
}


AUTPlayerState* AElimPlusGame::FindAliveTeammate(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;

	const int32 TeamIdx = PS->Team->TeamIndex;
	//if (!Teams.IsValidIndex(TeamIdx)) return nullptr;
	if (!Teams.IsValidIndex(TeamIdx) || !Teams[TeamIdx]) return nullptr;

	TArray<AController*> Members = Teams[TeamIdx]->GetTeamMembers();
	for (AController* C : Members)
	{
		if (!C) continue;

		AUTPlayerState* OtherPS = Cast<AUTPlayerState>(C->PlayerState);
		if (!OtherPS || OtherPS == PS || OtherPS->bOnlySpectator) continue;

		APawn* P = C->GetPawn();
		AUTCharacter* UTC = Cast<AUTCharacter>(P);
		if (P && (!UTC || !UTC->IsDead()))
		{
			return OtherPS;
		}
	}

	return nullptr;
}




int32 AElimPlusGame::CountAliveOnTeam(int32 TeamIndex) const
{
	int32 Alive = 0;

	// Use team members instead of just player controllers to include bots
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
			if (P && (!UTC || !UTC->IsDead()))
			{
				++Alive;
			}
		}
	}

	return Alive;
}


void AElimPlusGame::ForceLosersToViewWinners(int32 WinnerTeamIndex)
{
	//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Called with WinnerTeamIndex: %d"), WinnerTeamIndex);
	if (WinnerTeamIndex < 0)
	{
		//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Round was a draw. No view change."));
		return;
	}
	const int32 LoserTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
	//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: LoserTeamIndex is: %d"), LoserTeamIndex);
	AUTPlayerState* TargetPS = FindAliveOnTeamPS(WinnerTeamIndex);
	if (!TargetPS)
	{
		//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: No alive winner found. Finding any winner..."));
		TargetPS = FindAnyOnTeamPS(WinnerTeamIndex);
		if (!TargetPS)
		{
			//UE_LOG(LogGameMode, Error, TEXT("ForceLosersToViewWinners: FAILED. No winner PlayerState found to spectate."));
			return;
		}
	}
	AUTCharacter* TargetCharacter = TargetPS->GetUTCharacter();
	if (!TargetCharacter)
	{
		//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Target PlayerState %s has no character!"), *TargetPS->PlayerName);
		return;
	}
	//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Found target character to spectate: %s (owned by %s)"),
	//	*TargetCharacter->GetName(), *TargetPS->PlayerName);
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!PC || !PS || !PS->Team) continue;
		if (PS->Team->TeamIndex == LoserTeamIndex)
		{
			//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Processing player %s. Current state: %s"),
			//	*PS->PlayerName, PC->GetStateName().IsValid() ? *PC->GetStateName().ToString() : TEXT("Unknown"));
			if (!PC->IsInState(NAME_Spectating))
			{
				PC->ChangeState(NAME_Spectating);
				PC->ClientGotoState(NAME_Spectating);
				//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Changed %s to spectating state"), *PS->PlayerName);
			}
			if (!PS->bOutOfLives)
			{
				PS->bOutOfLives = true;
				PS->ForceNetUpdate();
			}
			PC->SetViewTarget(TargetCharacter);
			PC->bSpectateBehindView = false;  // Force first person view
			PC->BehindView(false);
			//PC->SetFocusToGameViewport();// Apply the camera mode
			//UE_LOG(LogGameMode, Warning, TEXT("ForceLosersToViewWinners: Set %s to directly view character %s"),
			//	*PS->PlayerName, *TargetCharacter->GetName());
		}
	}
}

void AElimPlusGame::DebugPlayerStates()
{
	UE_LOG(LogGameMode, Warning, TEXT("=== DEBUG PLAYER STATES ==="));
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!PC || !PS) continue;
		FName StateName = PC->GetStateName();
		AActor* ViewTarget = PC->GetViewTarget();
		FString ViewTargetName = ViewTarget ? ViewTarget->GetName() : TEXT("None");
		/*UE_LOG(LogGameMode, Warning, TEXT("Player: %s, Team: %d, State: %s, ViewTarget: %s, OutOfLives: %s, HasPawn: %s"),
			*PS->PlayerName,
			PS->Team ? PS->Team->TeamIndex : -1,
			StateName.IsValid() ? *StateName.ToString() : TEXT("Unknown"),
			*ViewTargetName,
			PS->bOutOfLives ? TEXT("Yes") : TEXT("No"),
			PC->GetPawn() ? TEXT("Yes") : TEXT("No")); */
	}
	//UE_LOG(LogGameMode, Warning, TEXT("=== END DEBUG ==="));
}

bool AElimPlusGame::CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget)
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
	if (bRoundInProgress)
	{
		const int32 MyTeamAlive = CountAliveOnTeam(ViewerPS->Team->TeamIndex);
		if (MyTeamAlive > 0)
		{
			if (ViewerPS->Team != TargetPS->Team)
			{
				return false;
			}
		}
	}
	const AController* TargetPC = Cast<AController>(TargetPS->GetOwner());
	const APawn* P = TargetPC ? TargetPC->GetPawn() : nullptr;
	const AUTCharacter* C = Cast<AUTCharacter>(P);
	const bool bTargetAlive = P && (!C || !C->IsDead());
	if (!bTargetAlive && bRoundInProgress)
	{
		return false;
	}
	return Super::CanSpectate_Implementation(Viewer, ViewTarget);
}




AUTPlayerState* AElimPlusGame::FindAliveOnTeamPS(int32 TeamIndex) const
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
		if (P && (!UTC || !UTC->IsDead()))
		{
			return PS;
		}
	}

	return nullptr;
}


AUTPlayerState* AElimPlusGame::FindAnyOnTeamPS(int32 TeamIndex) const
{
	if (const AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		for (APlayerState* APS : GS->PlayerArray)
		{
			if (AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS))
			{
				if (UTPS->Team && UTPS->Team->TeamIndex == TeamIndex)
				{
					return UTPS;
				}
			}
		}
	}
	return nullptr;
}

bool AElimPlusGame::GetAliveCounts(int32& OutAliveTeam0, int32& OutAliveTeam1) const
{
	OutAliveTeam0 = 0;
	OutAliveTeam1 = 0;
	int32 TotalPlayerStates = 0;
	int32 ValidCharacters = 0;
	int32 DeadOrNoPawn = 0;
	int32 NoTeam = 0;
	int32 Spectators = 0;
	int32 Inactive = 0;
	AUTGameState* GS = GetGameState<AUTGameState>();
	if (GS == nullptr || GS->IsPendingKill())
	{
		// Don't log an error, this is normal during shutdown
		return false;
	}
	if (!GS)
	{
		UE_LOG(LogGameMode, Error, TEXT("GetAliveCounts: Could not find GameState!"));
		return false;
	}
	TotalPlayerStates = GS->PlayerArray.Num();
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
		if (!PS) continue;
		if (PS->bOnlySpectator)
		{
			Spectators++;
			continue;
		}
		if (PS->bIsInactive)
		{
			Inactive++;
			continue;
		}
		if (!PS->Team)
		{
			NoTeam++;
			continue;
		}
		AUTCharacter* Pawn = Cast<AUTCharacter>(PS->GetUTCharacter());
		if (!Pawn || Pawn->IsDead() || Pawn->Health <= 0)
		{
			DeadOrNoPawn++;
			continue;
		}
		ValidCharacters++;
		const int32 TeamIndex = PS->Team->TeamIndex;
		if (TeamIndex == 0)
		{
			OutAliveTeam0++;
		}
		else if (TeamIndex == 1)
		{
			OutAliveTeam1++;
		}
	}
	return true;
}

void AElimPlusGame::CheckLastManStanding(int32 Alive0, int32 Alive1)
{
	if (Team0StartingSize > 1 && Alive0 == 1 && !bTeam0LastManAnnounced)
	{
		AUTPlayerState* ClutchPlayer = FindAliveOnTeamPS(0);
		BroadcastLastManStanding(0, ClutchPlayer);
		bTeam0LastManAnnounced = true;
		// NEW: Track dark horse potential - Team0 player is now 1 vs Alive1 enemies
		if (ClutchPlayer && Alive1 >= 3)
		{
			// This player is now in a 1v3+ situation - mark them as a dark horse candidate
			if (!DarkHorseCandidates.Contains(ClutchPlayer))
			{
				DarkHorseCandidates.Add(ClutchPlayer, Alive1); // Store how many enemies they're facing
				//UE_LOG(LogGameMode, Log, TEXT("Dark Horse Candidate: %s (Team 0) is now 1v%d"), *ClutchPlayer->PlayerName, Alive1);
			}
		}
		if (ClutchPlayer)
		{	// Broadcast the "clutch attempt" event
			// Pass the player and the number of enemies they are facing (Alive1)
			OnClutchSituationStarted.Broadcast(ClutchPlayer, Alive1);
			UE_LOG(LogGameMode, Log, TEXT("Clutch Situation Started: %s (Team 0) vs %d enemies."), *ClutchPlayer->PlayerName, Alive1);
		}
	}
	if (Team1StartingSize > 1 && Alive1 == 1 && !bTeam1LastManAnnounced)
	{
		AUTPlayerState* ClutchPlayer = FindAliveOnTeamPS(1);
		BroadcastLastManStanding(1, ClutchPlayer);
		bTeam1LastManAnnounced = true;
		// NEW: Track dark horse potential - Team1 player is now 1 vs Alive0 enemies
		if (ClutchPlayer && Alive0 >= 3)
		{
			// This player is now in a 1v3+ situation - mark them as a dark horse candidate
			if (!DarkHorseCandidates.Contains(ClutchPlayer))
			{
				DarkHorseCandidates.Add(ClutchPlayer, Alive0); // Store how many enemies they're facing
				//UE_LOG(LogGameMode, Log, TEXT("Dark Horse Candidate: %s (Team 1) is now 1v%d"), *ClutchPlayer->PlayerName, Alive0);
			}
		}
		if (ClutchPlayer)
		{
			// Broadcast the "clutch attempt" event
			// Pass the player and the number of enemies they are facing (Alive0)
			OnClutchSituationStarted.Broadcast(ClutchPlayer, Alive0);
			UE_LOG(LogGameMode, Log, TEXT("Clutch Situation Started: %s (Team 1) vs %d enemies."), *ClutchPlayer->PlayerName, Alive0);
		}
	}
}

void AElimPlusGame::BroadcastLastManStanding(int32 LastManTeamIndex, AUTPlayerState* LastManPlayerState)
{
	// Call Blueprint implementation first (for players without C++ plugin)
	BP_OnLastManStanding(LastManTeamIndex, LastManPlayerState);

}

void AElimPlusGame::CheckRoundWinConditions()
{
	if (bWarmupMode) return;
	if (!bRoundInProgress || GetWorld()->GetTimeSeconds() < WinCheckHoldUntilSeconds) return;
	if (GetWorldTimerManager().IsTimerActive(TH_RoundEndDelay))
	{
		return;
	}
	int32 Alive0, Alive1;
	GetAliveCounts(Alive0, Alive1);
	/*if (Team0StartingSize == 0 || Team1StartingSize == 0)
	{
		// One team is empty - let the timer run out naturally
		return;
	}*/
	const bool Team0Eliminated = (Alive0 == 0);
	const bool Team1Eliminated = (Alive1 == 0);
	if (Team0Eliminated && !Team1Eliminated)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 1, FName(TEXT("Elimination")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team1Eliminated && !Team0Eliminated)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 0, FName(TEXT("Elimination")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team0Eliminated && Team1Eliminated)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), INDEX_NONE, FName(TEXT("Draw")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
}

void AElimPlusGame::BroadcastDomination(int32 DominatingTeamIndex)
{
	// Pure Blueprint implementation - let BP handle all audio/UI
	BP_OnDomination(DominatingTeamIndex);
}

void AElimPlusGame::BroadcastTakesLead(int32 LeadingTeamIndex)
{
	// Pure Blueprint implementation - let BP handle all audio/UI
	BP_OnTakesLead(LeadingTeamIndex);
}

void AElimPlusGame::BroadcastRoundResults(int32 WinnerTeamIndex, bool bIsDraw)
{
	// Call Blueprint implementation first (for players without C++ plugin)
	bool bAllWinnersAlive = false;
	if (!bIsDraw && WinnerTeamIndex >= 0)
	{
		// Get total team size and alive count for the winning team
		int32 TotalWinningTeamSize = 0;
		int32 AliveWinningTeamSize = 0;

		if (Teams.IsValidIndex(WinnerTeamIndex))
		{
			TArray<AController*> WinnerMembers = Teams[WinnerTeamIndex]->GetTeamMembers();
			for (AController* C : WinnerMembers)
			{
				AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
				if (PS && !PS->bOnlySpectator)
				{
					TotalWinningTeamSize++;

					// Check if this player is alive
					APawn* P = C->GetPawn();
					AUTCharacter* UTC = Cast<AUTCharacter>(P);
					if (P && (!UTC || !UTC->IsDead()))
					{
						AliveWinningTeamSize++;
					}
				}
			}
		}

		// All winners are alive if alive count equals total count and team isn't empty
		bAllWinnersAlive = (TotalWinningTeamSize > 0 && AliveWinningTeamSize == TotalWinningTeamSize);
	}
	BP_OnRoundResults(WinnerTeamIndex, bIsDraw, bAllWinnersAlive);

}

void AElimPlusGame::CheckRoundAchievements(int32 WinnerTeamIndex, FName Reason)
{
	if (WinnerTeamIndex == INDEX_NONE || Reason != FName(TEXT("Elimination")))
	{
		return;
	}
	CheckForACE(WinnerTeamIndex);
	CheckForDarkHorse(WinnerTeamIndex);
	CheckForHighDamageCarry(WinnerTeamIndex);
}



void AElimPlusGame::CheckForDarkHorse(int32 WinnerTeamIndex)
{
	int32 AliveWinners = CountAliveOnTeam(WinnerTeamIndex);
	if (AliveWinners == 1)
	{
		// Check for dark horse using team members to include bots
		if (Teams.IsValidIndex(WinnerTeamIndex))
		{
			TArray<AController*> WinnerMembers = Teams[WinnerTeamIndex]->GetTeamMembers();
			for (AController* C : WinnerMembers)
			{
				if (!C) continue;

				AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
				if (!PS || PS->bOnlySpectator) continue;

				AUTCharacter* Character = PS->GetUTCharacter();
				if (Character && !Character->IsDead() && PS->RoundKills >= 3)
				{
					// NEW: Only award dark horse if this player was marked as a candidate
					// (meaning they were in a 1v2+ situation at some point)
					if (DarkHorseCandidates.Contains(PS))
					{
						int32 EnemiesTheyFaced = DarkHorseCandidates[PS];
						RecordDarkHorse(PS, EnemiesTheyFaced);
					}
					break;
				}
			}
		}
	}
}


// For CheckForACE:
void AElimPlusGame::CheckForACE(int32 WinnerTeamIndex)
{
	int32 EnemyTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;

	// Count enemy team size using team members
	int32 EnemyTeamSize = 0;
	if (Teams.IsValidIndex(EnemyTeamIndex))
	{
		TArray<AController*> EnemyMembers = Teams[EnemyTeamIndex]->GetTeamMembers();
		for (AController* C : EnemyMembers)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
			if (PS && !PS->bOnlySpectator)
			{
				EnemyTeamSize++;
			}
		}
	}

	// Check for ACE using team members
	if (Teams.IsValidIndex(WinnerTeamIndex))
	{
		TArray<AController*> WinnerMembers = Teams[WinnerTeamIndex]->GetTeamMembers();
		for (AController* C : WinnerMembers)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
			if (PS && PS->RoundKills >= EnemyTeamSize && EnemyTeamSize >= 2)
			{
				RecordACE(PS);
				break;
			}
		}
	}
}



void AElimPlusGame::CheckForHighDamageCarry(int32 WinnerTeamIndex)
{
	float TeamTotalDamage = (WinnerTeamIndex == 0) ? Team0RoundDamage : Team1RoundDamage;
	if (TeamTotalDamage > 0)
	{
		for (auto& DamagePair : PlayerRoundDamage)
		{
			AUTPlayerState* PS = DamagePair.Key.Get();
			float PlayerDamage = DamagePair.Value;
			if (PS && PS->Team && PS->Team->TeamIndex == WinnerTeamIndex && PlayerDamage > 440.f)
			{
				float DamagePercentage = (PlayerDamage / TeamTotalDamage) * 100.0f;
				if (DamagePercentage >= HighDamageCarryThreshold)
				{
					RecordHighDamageCarry(PS, DamagePercentage);
				}
			}
		}
	}
}

void AElimPlusGame::RecordACE(AUTPlayerState* PlayerState)
{
	if (!PlayerState) return;
	//UE_LOG(LogGameMode, Log, TEXT("ACE Achievement: %s"), *PlayerState->PlayerName);
	OnPlayerACE.Broadcast(PlayerState);
}

void AElimPlusGame::RecordDarkHorse(AUTPlayerState* PlayerState, int32 EnemiesKilled)
{
	if (!PlayerState) return;
	//UE_LOG(LogGameMode, Log, TEXT("Dark Horse Achievement: %s (1v%d)"), *PlayerState->PlayerName, EnemiesKilled);
	OnPlayerDarkHorse.Broadcast(PlayerState, EnemiesKilled);
}

void AElimPlusGame::RecordHighDamageCarry(AUTPlayerState* PlayerState, float DamagePercentage)
{
	if (!PlayerState) return;
	UE_LOG(LogGameMode, Log, TEXT("High Damage Carry Achievement: %s (%.1f%%)"), *PlayerState->PlayerName, DamagePercentage);
	OnPlayerHighDamageCarry.Broadcast(PlayerState, DamagePercentage);
}

void AElimPlusGame::ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);
	// Add the same safety checks as parent implementations
	if (!Victim || !Attacker || !UTGameState)
	{
		return;
	}

	// Use the same team check pattern as parent implementations
	if (UTGameState->OnSameTeam(Victim, Attacker))
	{
		// This is team damage, don't count it for achievements
		return;
	}

	// Now safely proceed with damage tracking logic
	if (!bRoundInProgress || !Attacker->Team || DamageAmount <= 0)
	{
		return;
	}


		// **NEW**: Calculate actual damage dealt (not overkill)
		int32 ActualDamageDealt = DamageAmount;

		if (Victim && Victim->GetUTCharacter())
		{
			AUTCharacter* VictimChar = Victim->GetUTCharacter();
			int32 VictimHealth = VictimChar->Health;
			float VictimArmor = VictimChar->GetArmorAmount();
			int32 TotalVictimHP = VictimHealth + FMath::FloorToInt(VictimArmor);

			// Cap damage at victim's actual health + armor
			ActualDamageDealt = FMath::Min(DamageAmount, TotalVictimHP);
		}

		if (!PlayerRoundDamage.Contains(Attacker))
		{
			PlayerRoundDamage.Add(Attacker, 0.0f);
		}
		PlayerRoundDamage[Attacker] += ActualDamageDealt; // Use actual damage, not overkill
		if (Attacker->Team->TeamIndex == 0)
		{
			Team0RoundDamage += ActualDamageDealt;
		}
		else if (Attacker->Team->TeamIndex == 1)
		{
			Team1RoundDamage += ActualDamageDealt;
		}

}

bool AElimPlusGame::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	// Check for invulnerability during intermission
	if (GetMatchState() == FName(TEXT("RoundCooldown")) && Injured && Injured->PlayerState)
	{
		AUTPlayerState* InjuredPS = Cast<AUTPlayerState>(Injured->PlayerState);
		if (InjuredPS && InjuredPS->Team && InjuredPS->Team->TeamIndex == LastRoundWinningTeamIndex)
		{
			Damage = 0;
			return true; // Damage modified (to zero)
		}
	}

	return Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
}

int32 AElimPlusGame::GetTiebreakWinnerByTeamHealth() const
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
				const int32 T = PS->Team->TeamIndex % 2;
				const float Health = FMath::Max(0.f, (float)C->Health);
				const float Armor = C->GetArmorAmount();
				(T == 0 ? Sum0 : Sum1) += (Health + Armor);
			}
		}
	}
	if (Sum0 > Sum1) return 0;
	if (Sum1 > Sum0) return 1;
	return INDEX_NONE;
}

void AElimPlusGame::DelayedInitialWinCheck()
{
	if (bRoundInProgress)
	{
		WinCheckHoldUntilSeconds = 0.f;
		UE_LOG(LogGameMode, Warning, TEXT("Performing delayed initial win check"));
		CheckRoundWinConditions();
	}
}

void AElimPlusGame::StopOvertime()
{
	if (OvertimeTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(OvertimeTimerHandle);
	}
	if (OvertimeWaveTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(OvertimeWaveTimerHandle);
	}
	CurrentOvertimeWave = 0;
	CurrentWaveDamage = 0.0f;
}

void AElimPlusGame::StartOvertime()
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
			CountdownStart, false
		);
	}
	GetWorldTimerManager().SetTimer(
		OvertimeWaveTimerHandle,
		this,
		&AElimPlusGame::ExecuteOvertimeWave,
		FMath::Max(0.1f, OvertimeStartDelay),
		false
	);
	BP_OnOvertimeStarted();
	UE_LOG(LogGameMode, Warning, TEXT("Overtime has started! First wave in %.1f seconds with %.1f damage"),
		OvertimeStartDelay, OvertimeBaseDamage);
}

void AElimPlusGame::BroadcastOvertimeCountdown(int32 CountdownValue)
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
			1.0f, false
		);
	}
	else if (CountdownValue == 1)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this]() { BroadcastOvertimeAnnouncement(); },
			1.0f, false
		);
	}
}

void AElimPlusGame::BroadcastOvertimeAnnouncement()
{
	if (!OvertimeAnnouncementSound) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->UTClientPlaySound(OvertimeAnnouncementSound);
		}
	}
	//UE_LOG(LogGameMode, Warning, TEXT("Overtime announcement sound played to all players"));
}

void AElimPlusGame::ExecuteOvertimeWave()
{
	if (GetNetMode() == NM_Client || !bRoundInProgress)
	{
		StopOvertime();
		return;
	}
	CurrentOvertimeWave++;
	if (CurrentOvertimeWave == 1)
	{
		CurrentWaveDamage = OvertimeBaseDamage;
	}
	else
	{
		CurrentWaveDamage *= OvertimeDamageMultiplier;
	}
	if (OvertimeMaxDamage > 0.0f)
	{
		CurrentWaveDamage = FMath::Min(CurrentWaveDamage, OvertimeMaxDamage);
	}
	UE_LOG(LogGameMode, Warning, TEXT("Overtime Wave %d: %.1f damage to %d players"),
		CurrentOvertimeWave, CurrentWaveDamage, GetWorld()->GetNumPawns());
	BP_OnOvertimeWave(CurrentWaveDamage, CurrentOvertimeWave);
	int32 DamageCount = 0;
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (!bRoundInProgress)
		{
			UE_LOG(LogGameMode, Warning, TEXT("Round ended during overtime wave - stopping damage application"));
			break;
		}
		AUTCharacter* C = Cast<AUTCharacter>(*It);
		if (!C || C->IsDead()) continue;
		float DamageToApply = CurrentWaveDamage;
		if (bOvertimeNonLethal && C->Health <= 1.f) continue;
		if (bOvertimeNonLethal && (C->Health - DamageToApply) < 1.f)
		{
			DamageToApply = FMath::Max(0.f, C->Health - 1.f);
			if (DamageToApply <= 0.f) continue;
		}
		UE_LOG(LogGameMode, Log, TEXT("Applying %.1f damage to %s (Health: %d, Armor: %.1f)"),
			DamageToApply, *C->GetName(), C->Health, C->GetArmorAmount());
		bool bWillKillPlayer = (C->Health - DamageToApply) <= 0;
		FHitResult Hit;
		Hit.bBlockingHit = true;
		Hit.Location = C->GetActorLocation();
		Hit.ImpactPoint = C->GetActorLocation();
		Hit.Normal = FVector(0, 0, 1);
		Hit.ImpactNormal = FVector(0, 0, 1);
		Hit.Actor = C;
		Hit.Component = Cast<UPrimitiveComponent>(C->GetRootComponent());
		FUTPointDamageEvent DamageEvent(
			DamageToApply,
			Hit,
			FVector(0, 0, -1),
			OvertimeDamageType ? *OvertimeDamageType : UUTDamageType::StaticClass(),
			FVector::ZeroVector
		);
		C->TakeDamage(
			DamageToApply,
			DamageEvent,
			nullptr,
			this
		);
		DamageCount++;
		if (bWillKillPlayer && bRoundInProgress)
		{
			//GetWorldTimerManager().SetTimerForNextTick([this]()
			GetWorldTimerManager().SetTimerForNextTick(this, &AElimPlusGame::DeferredCheckRoundWinConditions);

		}
	}
	UE_LOG(LogGameMode, Warning, TEXT("Applied damage to %d players"), DamageCount);
	CheckRoundWinConditions();
	if (bRoundInProgress)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeWaveTimerHandle,
			this,
			&AElimPlusGame::ExecuteOvertimeWave,
			OvertimeWaveInterval,
			false
		);
	}
}

void AElimPlusGame::CheckForDominationAndLead(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex == INDEX_NONE || !Teams.IsValidIndex(0) || !Teams.IsValidIndex(1))
	{
		return;
	}
	const int32 RedScore = Teams[0]->Score;
	const int32 BlueScore = Teams[1]->Score;
	const int32 ScoreDifference = FMath::Abs(RedScore - BlueScore);
	const bool bWasTied = (PreviousRedScore == PreviousBlueScore);
	const bool bNowHasLead = (RedScore != BlueScore);
	if (bWasTied && bNowHasLead)
	{
		BroadcastTakesLead(WinnerTeamIndex);
	}
	else if (!bWasTied && bNowHasLead)
	{
		const bool bRedWasWinning = (PreviousRedScore > PreviousBlueScore);
		const bool bBlueWasWinning = (PreviousBlueScore > PreviousRedScore);
		const bool bRedNowWinning = (RedScore > BlueScore);
		const bool bBlueNowWinning = (BlueScore > RedScore);
		if ((bBlueWasWinning && bRedNowWinning) || (bRedWasWinning && bBlueNowWinning))
		{
			BroadcastTakesLead(WinnerTeamIndex);
		}
	}
	if (ScoreDifference >= 5)
	{
		const int32 DominatingTeam = (RedScore > BlueScore) ? 0 : 1;
		if (!bHasBroadcastTeamDominating)
		{
			BroadcastDomination(DominatingTeam);
			bHasBroadcastTeamDominating = true;
		}
		else
		{
			const int32 PreviousDominatingTeam = (PreviousRedScore > PreviousBlueScore) ? 0 : 1;
			if (DominatingTeam != PreviousDominatingTeam)
			{
				BroadcastDomination(DominatingTeam);
			}
		}
	}
	else
	{
		bHasBroadcastTeamDominating = false;
	}
	PreviousRedScore = RedScore;
	PreviousBlueScore = BlueScore;
}

void AElimPlusGame::BP_RestartCurrentRound()
{
	if (!HasAuthority())
	{
		UE_LOG(LogGameMode, Warning, TEXT("BP_RestartCurrentRound: Only server can restart rounds"));
		return;
	}

	if (bWarmupMode || !HasMatchStarted())
	{
		UE_LOG(LogGameMode, Warning, TEXT("BP_RestartCurrentRound: Cannot restart round - match not in progress"));
		return;
	}

	UE_LOG(LogGameMode, Warning, TEXT("BP_RestartCurrentRound: Restarting current round due to admin request"));

	// Stop any active overtime
	StopOvertime();

	// Clear any pending timers that might interfere
	GetWorldTimerManager().ClearTimer(TH_RoundEndDelay);
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);
	GetWorldTimerManager().ClearTimer(TimerHandle_StaggeredSpawn);

	// Cancel any in-progress staggered spawns
	PendingSpawnQueue.Empty();

	// Force end current round state immediately
	bRoundInProgress = false;
	RoundEndTimeSeconds = 0.f;

	// Reset round-specific tracking without incrementing TotalRoundsPlayed
	LastRoundWinningTeamIndex = INDEX_NONE;
	bTeam0LastManAnnounced = false;
	bTeam1LastManAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();

	// Clean up the world and reset players for the new round
	ResetPlayersForNewRound();
	CleanupWorldForNewRound();

	// Reset spawn selection (increments CurrentRoundNumber for axis rotation)
	ResetSpawnSelectionForNewRound();

	// Start a brief intermission before the new round
	// When intermission ends, DefaultTimer sets InProgress -> CallMatchStateChangeNotify -> StartNextRound
	StartIntermission(4);
}



void AElimPlusGame::BP_SetTeamScores(int32 RedScore, int32 BlueScore)
{
	if (!HasAuthority())
	{
		UE_LOG(LogGameMode, Warning, TEXT("BP_SetTeamScores: Only server can set scores"));
		return;
	}

	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1))
	{
		UE_LOG(LogGameMode, Warning, TEXT("BP_SetTeamScores: Teams not initialized"));
		return;
	}

	const int32 OldRed = Teams[0]->Score;
	const int32 OldBlue = Teams[1]->Score;

	Teams[0]->Score = FMath::Max(0, RedScore);
	Teams[1]->Score = FMath::Max(0, BlueScore);

	Teams[0]->ForceNetUpdate();
	Teams[1]->ForceNetUpdate();

	// Keep lead/domination tracking in sync
	PreviousRedScore = Teams[0]->Score;
	PreviousBlueScore = Teams[1]->Score;
	bHasBroadcastTeamDominating = false;

	UE_LOG(LogGameMode, Warning, TEXT("BP_SetTeamScores: Red %d->%d, Blue %d->%d"),
		OldRed, Teams[0]->Score, OldBlue, Teams[1]->Score);
}



void AElimPlusGame::Logout(AController* Exiting)
{
	if (bCompetitiveAutoPause && IsMatchInProgress() && !HasMatchEnded() && !GetWorld()->IsPaused())
	{
		if (Exiting)
		{
			AUTPlayerState* ExitingPS = Cast<AUTPlayerState>(Exiting->PlayerState);

			// Only pause for actual human players (ignore bots and spectators)
			if (ExitingPS && !ExitingPS->bIsABot && !ExitingPS->bOnlySpectator)
			{
				UE_LOG(LogGameMode, Warning, TEXT("Competitive Auto-Pause: Player %s disconnected. Pausing match."), *ExitingPS->PlayerName);

				// Passing nullptr to SetPause acts as a "System/Admin" pause
				SetPause(nullptr);

				// Optional: Broadcast a message to chat so players know why it paused
				// BroadcastLocalized(this, UUTGameMessage::StaticClass(), 0, nullptr, nullptr, nullptr); 
			}
		}
	}
	
	if (Exiting)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (PS)
		{
			// --- THIS IS THE FIX ---
			// The PlayerState is about to be destroyed.
			// We MUST remove it from our TMap to prevent
			// stale pointer access.
			PlayerRoundDamage.Remove(PS);
			Team0AlivePlayers.Remove(PS);
			Team1AlivePlayers.Remove(PS);
			DarkHorseCandidates.Remove(PS);
			PerPlayerMatchPPRSum.Remove(PS);
			PerPlayerMatchPPRRoundCount.Remove(PS);

		}
	}

	Super::Logout(Exiting);
}


void AElimPlusGame::InitGameState()
{
	Super::InitGameState();
	// GameModeClass is left as-is — clients have NetcodePlus installed,
	// so the base class sets it correctly and the scoreboard reads
	// DisplayName from the actual game mode class (or BP subclass).
}

// 4. Timer Management
void AElimPlusGame::StartCampCheckTimer()
{
	if (bEnableAntiCamp && CampCheckInterval > 0.0f)
	{
		GetWorldTimerManager().SetTimer(TimerHandle_CampCheck, this, &AElimPlusGame::CheckForCampers, CampCheckInterval, true);
	}
}

void AElimPlusGame::StopCampCheckTimer()
{
	GetWorldTimerManager().ClearTimer(TimerHandle_CampCheck);
}

// 5. The Core Logic
void AElimPlusGame::CheckForCampers()
{
	if (!bRoundInProgress || bWarmupMode) return;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		if (!PC) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
		if (!PS || PS->bOnlySpectator || PS->bOutOfLives) continue;

		APawn* Pawn = PC->GetPawn();
		if (!Pawn || Pawn->IsPendingKill()) continue;

		// Get or Create entry in the map
		FCamperDataElimPlus& Data = CamperTracker.FindOrAdd(PS);

		// Update Circular Buffer
		Data.LocationHistory[Data.NextSlot] = Pawn->GetActorLocation();
		Data.NextSlot++;

		if (Data.NextSlot >= 10)
		{
			Data.NextSlot = 0;
			Data.bHasFullHistory = true;
		}

		// Only calculate if we have enough samples (Warmed Up)
		if (Data.bHasFullHistory)
		{
			// Calculate Bounding Box of history
			FBox HistoryBox(ForceInit);
			for (int32 i = 0; i < 10; i++)
			{
				HistoryBox += Data.LocationHistory[i];
			}

			// Get Max Dimension (Extent is half-size, so multiply by 2 for full width/height)
			FVector Size = HistoryBox.GetSize();
			float MaxDim = FMath::Max3(Size.X, Size.Y, Size.Z);

			// Check vs Threshold
			if (MaxDim < CampThreshold)
			{
				// Is it time to punish/warn again?
				float TimeSinceLast = GetWorld()->GetTimeSeconds() - Data.LastPunishTime;

				if (TimeSinceLast > CampWarnCooldown)
				{
					Data.ConsecutiveCampCount++;
					Data.bWarned = true;
					Data.LastPunishTime = GetWorld()->GetTimeSeconds();

					// TRIGGER BLUEPRINT EVENT
					BP_OnCamperDetected(PS, Data.ConsecutiveCampCount);

					UE_LOG(LogGameMode, Log, TEXT("Camper Detected: %s (Dim: %.2f, Count: %d)"), *PS->PlayerName, MaxDim, Data.ConsecutiveCampCount);
				}
			}
			else
			{
				// Player is moving enough
				if (Data.bWarned || Data.ConsecutiveCampCount > 0)
				{
					Data.bWarned = false;
					Data.ConsecutiveCampCount = 0;

					// TRIGGER CLEAR EVENT
					BP_OnCamperClear(PS);
				}
			}
		}
	}
}




#pragma endregion


uint8 AElimPlusGame::PickBalancedTeam(AUTPlayerState* PS, uint8 RequestedTeam)
{
	// Mid-round paranoia + safe-fallback gate. The base impl picks smallest team
	// (or honors RequestedTeam) and is correct in all non-tie cases.
	if (bRoundInProgress || !RatingSystem.IsValid() || !PS || Teams.Num() < 2)
	{
		return Super::PickBalancedTeam(PS, RequestedTeam);
	}

	int32 MinSize = INT_MAX;
	for (AUTTeamInfo* T : Teams)
	{
		if (T) MinSize = FMath::Min(MinSize, int32(T->GetSize()));
	}

	int32 BestTeamIdx = INDEX_NONE;
	int64 BestStrength = MAX_int64;
	for (int32 i = 0; i < Teams.Num(); ++i)
	{
		if (!Teams[i] || int32(Teams[i]->GetSize()) != MinSize) continue;

		int64 TeamStrength = 0;
		TArray<AController*> Members = Teams[i]->GetTeamMembers();
		for (AController* C : Members)
		{
			AUTPlayerState* MemberPS = C ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
			if (MemberPS && MemberPS->UniqueId.IsValid())
			{
				TeamStrength += RatingSystem->GetCachedElo(MemberPS->UniqueId.ToString());
			}
			else
			{
				TeamStrength += 1400;  // bot or unrated, matches kDefaultRating
			}
		}
		if (TeamStrength < BestStrength)
		{
			BestStrength = TeamStrength;
			BestTeamIdx = i;
		}
	}

	if (BestTeamIdx == INDEX_NONE) return Super::PickBalancedTeam(PS, RequestedTeam);

	UE_LOG(LogGameMode, Verbose, TEXT("ElimPlus PickBalancedTeam: %s -> team %d (lowest cached-Elo sum %lld)"),
		*PS->PlayerName, BestTeamIdx, BestStrength);
	return static_cast<uint8>(BestTeamIdx);
}
