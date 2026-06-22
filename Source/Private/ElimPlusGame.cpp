#include "ElimPlusGame.h"
#include "NCFireValCollector.h"
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "ElimPlusStatsReplicator.h"
#include "NCAccuracyStatsReplicator.h"
#include "ElimPlusHUD.h"
#include "ElimPlusRatingSystem.h"
#include "NCEloUploader.h"
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

bool AElimPlusGame::ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass)
{
	// Unlock entitlement-gated cosmetics (boxhat etc.): force the chosen hat as an OverrideHatClass (which
	// the engine does NOT entitlement-check) on the next tick — after ServerReceiveHatClass's
	// ValidateEntitlements strip — so the community master's withheld cosmetic entitlements can't remove it.
	// Server-side; never kicks. Mirrors ANCPlusCTFGameMode::ValidateHat. NULL in the log = class didn't load.
	if (HatOwner && !HatClass.IsEmpty())
	{
		TWeakObjectPtr<AUTPlayerState> WeakPS(HatOwner);
		const FString Path = HatClass;
		GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakPS, Path]()
		{
			if (AUTPlayerState* PS = WeakPS.Get())
			{
				PS->SetOverrideHatClass(Path);
				UE_LOG(LogTemp, Warning, TEXT("[Cosmetics] ForceOverrideHat '%s' -> %s"), *Path,
					PS->OverrideHatClass ? *PS->OverrideHatClass->GetName() : TEXT("NULL (class did not load)"));
			}
		}));
	}
	return Super::ValidateHat(HatOwner, HatClass);
}

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

	// MinimumEnemySpawnDistance default is in the header (3600.0f, matching
	// AUWipeoutGame). All other Wipeout-style spawn knobs (per-spawn weights,
	// teammate separation tunings, layout offsets) were removed when the system
	// was unified — see the spawn block declarations in ElimPlusGame.h.
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
	bDidPreMatchRebalance = false;
}

void AElimPlusGame::BeginPlay()
{
	Super::BeginPlay();

	if (!bSpawnPointsInitialized)
	{
		InitializeSpawnPointSystem();
		bSpawnPointsInitialized = true;
	}
	// Precompute all valid spawn layouts once at map load.
	// Builds ValidLayouts_4v4 (top N spawns per side, multi-axis side detection)
	// and ValidLayouts_1v1 (single-spawn-per-team for tighter rounds).
	// SelectSpawnLayoutForRound picks one each round and populates
	// Team0/Team1SelectedSpawns; ChoosePlayerStart_Implementation does
	// per-player scoring within those pools.
	PrecomputeSpawnLayouts();

	// Server-only rating system. Initialize Mods.db schema once per map load.
	if (HasAuthority() && !RatingSystem.IsValid())
	{
		RatingSystem = MakeUnique<FElimPlusRatingSystem>();
		FElimPlusRatingSystem::InitDatabase(GetWorld());

		// Push the testing-config bot ELO range into the rating system so both
		// PickBalancedTeam and the per-round Glicko placeholder use the same
		// numbers. No-op when bRandomizeBotElo is false (production default).
		RatingSystem->SetBotRandomEloRange(bRandomizeBotElo, BotEloMin, BotEloMax);
	}
}


void AElimPlusGame::DelayedEndGame(int32 WinnerTeamIndex, FName Reason)
{
	AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
	EndGame(BestPlayer, Reason);
}


void AElimPlusGame::HandleMatchHasStarted()
{
	// Build marker — change the tag string below whenever you want to verify
	// the deployed binary contains a specific commit's changes.
	UE_LOG(LogGameMode, Warning, TEXT("ElimPlus build marker: rebalance+warnings+broadcast (post-ca60db0)"));
	Super::HandleMatchHasStarted();

	FNCFireValCollector::Get().Reset();   // fresh sample table + CSV id for this match

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

	// Per-weapon hits/shots replicator for the accuracy HUD widget.
	ANCAccuracyStatsReplicator::EnsureSpawned(this);

	// Replicate the bBalanceTeams URL flag to clients so the HUD's pre-match
	// team-balance preview overlay can hide when the admin disabled balancing.
	if (HasAuthority() && StatsReplicator)
	{
		StatsReplicator->SetBalanceTeamsActive(bBalanceTeams);
	}

	// Full ELO-based team rebalance now that all bots have joined (Super has
	// completed any AddBots fills). Runs BEFORE StartNextRound spawns the
	// player pawns, so the round begins with already-balanced teams. Players
	// never see a mid-match suicide-respawn — the move is silent because
	// pawns haven't been created yet for this round.
	if (HasAuthority() && !bDidPreMatchRebalance)
	{
		RebalanceTeamsForMatchStart();
		bDidPreMatchRebalance = true;
	}

	// Snapshot every loaded player's current rating as their "match-start" value.
	// Replicator's Elo + EloDeltaThisMatch stay frozen at these values until
	// HandleMatchHasEnded — players don't see ELO change round-to-round.
	if (HasAuthority() && RatingSystem.IsValid())
	{
		RatingSystem->SnapshotMatchStart();

		// Push the start-of-match Elo to replicator with delta=0 so the HUD
		// shows the player's current rating from the very first frame. Bots
		// also get pushed when bRandomizeBotElo is on, so the scoreboard can
		// display their assigned random ELO (otherwise GetOrAssignBotElo
		// returns 1400 and the row reads as default anyway — same as before).
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS && StatsReplicator)
		{
			for (APlayerState* PS : GS->PlayerArray)
			{
				AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
				if (!UTPS || UTPS->bOnlySpectator) continue;

				if (UTPS->UniqueId.IsValid())
				{
					const FString UidStr = UTPS->UniqueId.ToString();
					const int32 Elo = RatingSystem->GetCachedElo(UidStr);
					StatsReplicator->SetPlayerEloAndDelta(UidStr, Elo, 0);
					// Global leaderboard rank (1-based, frozen for the match like ELO).
					StatsReplicator->SetPlayerGlobalRank(UidStr, RatingSystem->GetPlayerGlobalRank(GetWorld(), UidStr));
				}
				else
				{
					// Bot — synthetic key. GetOrAssignBotElo is sticky-random
					// when randomization is on, else returns 1400.
					const FString BotKey = FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
					const int32 BotElo = RatingSystem->GetOrAssignBotElo(BotKey);
					StatsReplicator->SetPlayerEloAndDelta(BotKey, BotElo, 0);
				}
			}
		}
	}
}


void AElimPlusGame::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// Early plugin-version check — kicks mismatched clients within 10s of join.
	NCPlusVersionGate::SpawnFor(NewPlayer);

	// Server-only: pull this player's rating from Mods.db into the cache so it's
	// ready before the first round ends. Late joiners who arrive mid-match also
	// get their rating loaded, but won't have a SnapshotMatchStart entry — they
	// skip ELO for this match (no delta would be meaningful).
	if (!HasAuthority() || !RatingSystem.IsValid()) return;
	if (!NewPlayer) return;

	// Eagerly spawn the stats replicator on first PostLogin if it doesn't exist
	// yet. Originally we spawned it in HandleMatchHasStarted, but that's too
	// late — the pre-match team preview overlay (PlayerIntro/CountdownToBegin)
	// queries it for ELOs, so the replicator + EloCache need to be populated
	// before then. PostLogin is the earliest point where we have an actual
	// player to push, and clients are connected enough to receive replicated
	// data without crashing (the BeginPlay-spawn issue from earlier sessions).
	if (!StatsReplicator)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		StatsReplicator = GetWorld()->SpawnActor<AElimPlusStatsReplicator>(SpawnParams);
	}

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
			StatsReplicator->SetPlayerGlobalRank(UidStr, RatingSystem->GetPlayerGlobalRank(GetWorld(), UidStr));
		}
	}

	// Also push any bots already in PlayerArray. Bots don't trigger PostLogin
	// themselves but they may have been added before the human connected (lobby
	// fill). Without this they show 1400 in the pre-match preview until
	// HandleMatchHasStarted runs the full push.
	if (StatsReplicator)
	{
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS)
		{
			for (APlayerState* PS : GS->PlayerArray)
			{
				AUTPlayerState* OtherPS = Cast<AUTPlayerState>(PS);
				if (!OtherPS || OtherPS->bOnlySpectator) continue;
				if (OtherPS->UniqueId.IsValid()) continue;  // human, handled by their own PostLogin
				const FString BotKey = FString::Printf(TEXT("BOT:%s"), *OtherPS->PlayerName);
				const int32 BotElo = RatingSystem->GetOrAssignBotElo(BotKey);
				StatsReplicator->SetPlayerEloAndDelta(BotKey, BotElo, 0);
			}
		}
	}
}


void AElimPlusGame::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();

	FNCFireValCollector::Get().ReportOnce(GetWorld());   // emit [FireVal] + CSV (guards double-route)

	// Persist updated ratings to Mods.db and emit the final ELO + match delta
	// to the replicator. Engine routes HandleMatchHasEnded twice in some paths
	// (state-machine + derived); guard with bRatingFlushedThisMatch so we only
	// log + write once. INSERT OR REPLACE was already idempotent at the SQL level.
	if (HasAuthority() && RatingSystem.IsValid() && !bRatingFlushedThisMatch)
	{
		RatingSystem->FlushAtMatchEnd(GetWorld(), StatsReplicator);
		bRatingFlushedThisMatch = true;

		// Build + push the global-ELO payload to ut4stats.com. Walk the player
		// array once, gather per-player audit data, hand off to the rating
		// system to fill in pre/post/RD/sigma from its caches.
		AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
		if (GS)
		{
			FNCElimPlusMatchInput UploadIn;

			// Winner team from Teams[].Score; -1 on draw.
			int32 RedScore  = 0;
			int32 BlueScore = 0;
			if (GS->Teams.Num() >= 2 && GS->Teams[0] && GS->Teams[1])
			{
				RedScore  = static_cast<int32>(GS->Teams[0]->Score);
				BlueScore = static_cast<int32>(GS->Teams[1]->Score);
			}
			UploadIn.RedScore  = RedScore;
			UploadIn.BlueScore = BlueScore;
			if (RedScore > BlueScore)      UploadIn.WinnerTeamIndex = 0;
			else if (BlueScore > RedScore) UploadIn.WinnerTeamIndex = 1;
			else                            UploadIn.WinnerTeamIndex = -1;

			for (APlayerState* APS : GS->PlayerArray)
			{
				AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
				if (!UTPS || UTPS->bOnlySpectator) continue;
				// Bots have invalid UniqueId — rating system filter drops them, but
				// we can short-circuit here to avoid bloating the payload.
				if (!UTPS->UniqueId.IsValid()) continue;

				FNCElimPlusPlayerInput P;
				// ElimPlus rating system keys on UniqueId.ToString() (see PostLogin
				// at LoadPlayerFromDB call). Must match exactly or the cache lookup
				// in BuildResultPayload misses.
				P.UniqueId   = UTPS->UniqueId.ToString();
				P.PlayerName = UTPS->PlayerName;
				P.TeamIndex  = UTPS->GetTeamNum();
				P.Kills      = UTPS->Kills;
				P.Deaths     = UTPS->Deaths;
				P.Damage     = static_cast<int32>(UTPS->DamageDone);
				UploadIn.Players.Add(MoveTemp(P));
			}

			const FString Json = RatingSystem->BuildResultPayload(GetWorld(), UploadIn);
			if (!Json.IsEmpty())
			{
				FNCEloUploader::PostMatchResult(GetWorld(), Json);
			}
		}
	}
}


void AElimPlusGame::CallMatchStateChangeNotify()
{
	// This function intercepts all SetMatchState calls
	// and routes them to our custom handlers.
	if (GetMatchState() == MatchState::WaitingToStart)
	{
		bWarmupMode = true;

	}
	else if (GetMatchState() == MatchState::PlayerIntro
	      || GetMatchState() == MatchState::CountdownToBegin)
	{
		// Ensure warmup mode is disabled once match is starting
		bWarmupMode = false;
		ResetSpawnSelectionForNewRound();
		// Note: rebalance moved to HandleMatchHasStarted — at PlayerIntro and
		// CountdownToBegin the bot roster is still filling (Super::AddBots
		// happens inside the InProgress transition), so a rebalance here would
		// only see partial players and miss late bots. Scoreboard is still
		// auto-shown for these states by AElimPlusHUD::NotifyMatchStateChange.
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
	UE_LOG(LogGameMode, Verbose, TEXT("HandleMatchIntermission: Preparing for next round."));


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
	UE_LOG(LogGameMode, Verbose, TEXT("StartIntermission: Entering intermission for %d seconds."), Seconds);

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
	UE_LOG(LogGameMode, Verbose, TEXT("StartNextRound: Spawning players and starting round."));

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
	// Sweep AFTER the reset, on the canonical round-start path. CleanupWorldForNewRound
	// also runs in DefaultTimer at intermission-end, but that fires BEFORE StartNextRound
	// (and a BP-driven state transition can bypass it), so any pickup still on the floor
	// at round start — e.g. a thrown weapon (throw bind -> TossInventory, which does NOT
	// go through the now-suppressed DiscardInventory) — used to survive into the new round.
	// Sweeping here guarantees a clean floor regardless of how the round was started.
	CleanupWorldForNewRound();
	DarkHorseCandidates.Empty();
	ResetSpawnSelectionForNewRound();
	Team0AlivePlayers.Empty();
	Team1AlivePlayers.Empty();

	// --- PER-TEAM SPAWN POOL SELECTION ---
	// Picks one of the precomputed FElimPlusSpawnLayouts and populates
	// Team0SelectedSpawns / Team1SelectedSpawns with up to N spawns per team
	// (N = team size). ChoosePlayerStart_Implementation then dynamically
	// scores within each team's pool to assign individual players to spawns.
	SelectSpawnLayoutForRound();

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

	UE_LOG(LogGameMode, Verbose, TEXT("Round starting sizes - Team0: %d, Team1: %d"), Team0StartingSize, Team1StartingSize);

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

// ElimPlus arena rule: players never drop their loadout. Stock
// AUTGameMode::DiscardInventory tosses the current weapon (or the Enforcer fallback)
// plus any bAlwaysDropOnDeath powerups, spawning AUTDroppedPickups — and it fired both
// on death (AUTCharacter::Died) AND from ResetPlayersForNewRound at round reset (:1389).
// The reset toss ran AFTER CleanupWorldForNewRound's sweep, so those drops survived into
// the next round. Destroy the inventory in place instead: DiscardAllInventory Destroy()s
// each item (no TossInventory), nulls the weapon + saved ammo. Nothing ever spawns, so
// the sweep-ordering issue is moot. Deliberately does NOT call Super. Candy orbs (spawned
// by the BP PreventDeath as separate AUTPickupHealth world actors) are not this pawn's
// inventory and are untouched.
void AElimPlusGame::DiscardInventory(APawn* Other, AController* Killer)
{
	if (AUTCharacter* UTC = Cast<AUTCharacter>(Other))
	{
		UTC->DiscardAllInventory();
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

	// Idempotency guard (hoisted ABOVE the warmup branch). A RestartPlayer on a
	// controller that ALREADY has a living pawn must be a no-op: the engine reuses the
	// existing pawn but still re-runs FinishRestartPlayer -> SetPlayerDefaults ->
	// GiveDefaultInventory, and stock AUTCharacter::AddInventory dedupes only by INSTANCE
	// (not by class). So a SECOND RestartPlayer during warmup (e.g. the join auto-spawn
	// racing the client's ready-up) grants a second copy of every default weapon -> the
	// doubled weapon bar reported on 327. First spawn (no pawn) is unaffected; the live
	// and lineup paths below were already gated on this guard, so hoisting it only adds
	// the missing coverage for WaitingToStart.
	if (NewPlayer->GetPawn())
	{
		return;
	}

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		Super::RestartPlayer(NewPlayer);
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
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team) return Super::ChoosePlayerStart_Implementation(Player);

	const int32 TeamIndex = PS->Team->TeamIndex;

	// Three-tier algorithm with HARD enemy-distance floor:
	//   Tier 1a: curated 4 spawns, must be >= MinimumEnemySpawnDistance from enemies
	//   Tier 1b: any spawn on the map, same hard floor (if 1a fails)
	//   Tier 2:  teammate-stack — pick the spawn closest to a teammate (last resort,
	//            ignores enemy distance; safety over solitude)

	const float EnemyBonusCap = 5000.f;
	auto ScoreCandidate = [&](float MinTeammateDist, int32 NearbyCount, float MinEnemyDist) -> float
	{
		const bool bAnyTeammate = (MinTeammateDist < FLT_MAX);
		const bool bAnyEnemy    = (MinEnemyDist    < FLT_MAX);
		const float TeammateTerm = bAnyTeammate ? -MinTeammateDist : 0.f;
		const float EnemyBonus   = bAnyEnemy ? FMath::Min(MinEnemyDist, EnemyBonusCap) * 0.5f : 0.f;
		return NearbyCount * 5000.f + TeammateTerm + EnemyBonus + FMath::FRandRange(0.f, 300.f);
	};

	auto ScanCandidate = [&](APlayerStart* Spawn, float& OutMinEnemy, float& OutMinTeammate, int32& OutNearby, bool& OutOccupied)
	{
		OutMinEnemy = FLT_MAX;
		OutMinTeammate = FLT_MAX;
		OutNearby = 0;
		OutOccupied = false;
		if (!Spawn) return;

		const FVector SpawnLoc = Spawn->GetActorLocation();
		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (!Pawn || !Pawn->PlayerState) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
			if (!OtherPS || OtherPS == PS || !OtherPS->Team) continue;

			const float Dist = (Pawn->GetActorLocation() - SpawnLoc).Size2D();
			if (OtherPS->Team->TeamIndex == TeamIndex)
			{
				if (Dist < 100.f) { OutOccupied = true; return; }
				OutMinTeammate = FMath::Min(OutMinTeammate, Dist);
				if (Dist < 800.f) OutNearby++;
			}
			else
			{
				OutMinEnemy = FMath::Min(OutMinEnemy, Dist);
			}
		}
	};

	APlayerStart* BestSpawn = nullptr;
	float BestScore = -FLT_MAX;

	// Tier 1a: curated spawns for this team, with hard enemy floor.
	TArray<APlayerStart*> MySpawns;
	if (TeamIndex == 0 && Team0SelectedSpawns.Num() > 0)      MySpawns = Team0SelectedSpawns;
	else if (TeamIndex == 1 && Team1SelectedSpawns.Num() > 0) MySpawns = Team1SelectedSpawns;

	for (APlayerStart* Spawn : MySpawns)
	{
		float MinEnemy, MinTeammate; int32 Nearby; bool bOccupied;
		ScanCandidate(Spawn, MinEnemy, MinTeammate, Nearby, bOccupied);
		if (bOccupied) continue;
		if (MinEnemy < MinimumEnemySpawnDistance) continue;   // hard floor

		const float Score = ScoreCandidate(MinTeammate, Nearby, MinEnemy);
		if (Score > BestScore) { BestScore = Score; BestSpawn = Spawn; }
	}

	// Tier 1b: full map with hard enemy floor (curated couldn't satisfy threshold).
	if (!BestSpawn && AllSpawnPointsList.Num() > 0)
	{
		for (APlayerStart* Spawn : AllSpawnPointsList)
		{
			float MinEnemy, MinTeammate; int32 Nearby; bool bOccupied;
			ScanCandidate(Spawn, MinEnemy, MinTeammate, Nearby, bOccupied);
			if (bOccupied) continue;
			if (MinEnemy < MinimumEnemySpawnDistance) continue;

			const float Score = ScoreCandidate(MinTeammate, Nearby, MinEnemy);
			if (Score > BestScore) { BestScore = Score; BestSpawn = Spawn; }
		}
		if (BestSpawn)
		{
			UE_LOG(LogGameMode, Warning,
				TEXT("ElimPlus: %s curated spawns failed %.0fu floor — using full-map spawn (passed floor)"),
				*PS->PlayerName, MinimumEnemySpawnDistance);
		}
	}

	// Tier 2: TEAMMATE-STACK fallback. No spawn on the entire map passed the
	// hard enemy floor. Spawn ON a teammate (closest spawn to any living
	// teammate). Better to die alongside a friend than alone in enemy territory.
	if (!BestSpawn && AllSpawnPointsList.Num() > 0)
	{
		TArray<FVector> TeammateLocs;
		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (!Pawn || !Pawn->PlayerState) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
			if (!OtherPS || OtherPS == PS || !OtherPS->Team) continue;
			if (OtherPS->Team->TeamIndex == TeamIndex)
			{
				TeammateLocs.Add(Pawn->GetActorLocation());
			}
		}

		if (TeammateLocs.Num() > 0)
		{
			float BestTeammateDist = FLT_MAX;
			for (APlayerStart* Spawn : AllSpawnPointsList)
			{
				if (!Spawn) continue;
				const FVector SpawnLoc = Spawn->GetActorLocation();
				float MinDist = FLT_MAX;
				for (const FVector& Loc : TeammateLocs)
				{
					MinDist = FMath::Min(MinDist, (Loc - SpawnLoc).Size2D());
				}
				if (MinDist < BestTeammateDist)
				{
					BestTeammateDist = MinDist;
					BestSpawn = Spawn;
				}
			}
			if (BestSpawn)
			{
				UE_LOG(LogGameMode, Warning,
					TEXT("ElimPlus: %s — no spawn passes %.0fu floor; teammate-stacking at %.0fu from teammate"),
					*PS->PlayerName, MinimumEnemySpawnDistance, BestTeammateDist);
			}
		}
	}

	return BestSpawn ? BestSpawn : Super::ChoosePlayerStart_Implementation(Player);
}


AActor* AElimPlusGame::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	// If we have an overridden player start (from RestartPlayer), use it
	if (OverriddenPlayerStart)
	{
		//UE_LOG(LogGameMode, Warning, TEXT("Using overriden playerstart"));
		return OverriddenPlayerStart;
	}

	return Super::FindPlayerStart_Implementation(Player, IncomingName);
}





void AElimPlusGame::InitializeSpawnPointSystem()
{
	AllSpawnPointsList.Empty();
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (APlayerStart* Spawn = *It)
		{
			AllSpawnPointsList.Add(Spawn);
		}
	}
	UE_LOG(LogGameMode, Log, TEXT("ElimPlus: Found %d spawn points"), AllSpawnPointsList.Num());
}


void AElimPlusGame::PrecomputeSpawnLayouts()
{
	// Mirrors AUWipeoutGame::PrecomputeSpawnLayouts. Multi-axis side detection
	// (E-W, N-S, two diagonals, principal eigenvector of the spawn covariance),
	// pick the axis whose median split maximizes the minimum cross-team
	// distance. Then build top-N pools per side (N = team size, currently 4),
	// rotated through several variants so SelectSpawnLayoutForRound has variety.
	ValidLayouts_4v4.Empty();
	ValidLayouts_1v1.Empty();
	PrecomputedSideA.Empty();
	PrecomputedSideB.Empty();

	const int32 N = AllSpawnPointsList.Num();
	if (N < 2)
	{
		UE_LOG(LogGameMode, Error, TEXT("ElimPlus::PrecomputeSpawnLayouts: only %d spawns!"), N);
		return;
	}

	const int32 SpawnsPerTeam = 4;

	FVector Centroid = FVector::ZeroVector;
	for (APlayerStart* S : AllSpawnPointsList) Centroid += S->GetActorLocation();
	Centroid /= N;

	// Principal axis via 2D covariance: angle = 0.5 * atan2(2*Sxy, Sxx - Syy)
	float Sxx = 0.f, Sxy = 0.f, Syy = 0.f;
	for (APlayerStart* S : AllSpawnPointsList)
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
		{ TEXT("East-West"),   FVector2D(1.f, 0.f) },
		{ TEXT("North-South"), FVector2D(0.f, 1.f) },
		{ TEXT("NE-SW"),       FVector2D(0.707f, 0.707f) },
		{ TEXT("NW-SE"),       FVector2D(0.707f, -0.707f) },
		{ TEXT("Principal"),   PrincipalAxis }
	};

	UE_LOG(LogGameMode, Log, TEXT("ElimPlus: Evaluating %d axis candidates for %d spawns (centroid %.0f,%.0f, principal %.1f deg)"),
		Candidates.Num(), N, Centroid.X, Centroid.Y, FMath::RadiansToDegrees(PrincipalAngle));

	// Helper: max pairwise 2D distance ("diameter") of a spawn set.
	// Intra-team cohesion metric — smaller = teammates spawn tighter together.
	auto IntraDiameter2D = [](const TArray<APlayerStart*>& Side) -> float
	{
		float D = 0.f;
		for (int32 i = 0; i < Side.Num(); ++i)
		{
			for (int32 j = i + 1; j < Side.Num(); ++j)
			{
				if (Side[i] && Side[j])
				{
					D = FMath::Max(D, (Side[i]->GetActorLocation() - Side[j]->GetActorLocation()).Size2D());
				}
			}
		}
		return D;
	};

	// QualityScore = MinCrossDistance2D - IntraSpreadPenalty * MaxIntraDiameter.
	// 0.5 means a 2000u intra reduction is worth the same as a 1000u cross gain.
	const float IntraSpreadPenalty = 0.5f;

	TArray<APlayerStart*> SideA, SideB;
	FString BestAxisName;
	float BestMinCross = -1.f;
	float BestAxisScore = -FLT_MAX;

	for (const FAxisCandidate& Axis : Candidates)
	{
		struct FSpawnProj { float Proj; APlayerStart* Spawn; };
		TArray<FSpawnProj> Projections;
		for (APlayerStart* S : AllSpawnPointsList)
		{
			FVector D = S->GetActorLocation() - Centroid;
			FSpawnProj Item;
			Item.Proj = D.X * Axis.Dir.X + D.Y * Axis.Dir.Y;
			Item.Spawn = S;
			Projections.Add(Item);
		}
		Projections.Sort([](const FSpawnProj& A, const FSpawnProj& B) { return A.Proj < B.Proj; });

		const int32 SplitIdx = N / 2;
		TArray<APlayerStart*> CandidateA, CandidateB;
		for (int32 i = 0; i < SplitIdx; ++i) CandidateA.Add(Projections[i].Spawn);
		for (int32 i = SplitIdx; i < N; ++i) CandidateB.Add(Projections[i].Spawn);

		// Picks for cross-distance metric: deepest N from each side
		TArray<APlayerStart*> PickA, PickB;
		for (int32 i = 0; i < FMath::Min(SpawnsPerTeam, CandidateA.Num()); ++i) PickA.Add(CandidateA[i]);
		for (int32 i = 0; i < FMath::Min(SpawnsPerTeam, CandidateB.Num()); ++i) PickB.Add(CandidateB[CandidateB.Num() - 1 - i]);

		float MinCross = FLT_MAX;
		for (APlayerStart* S0 : PickA)
			for (APlayerStart* S1 : PickB)
				MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());

		// Intra-team cohesion: max diameter of the picked 4 per side.
		const float IntraSpread = FMath::Max(IntraDiameter2D(PickA), IntraDiameter2D(PickB));
		const float AxisScore = MinCross - IntraSpreadPenalty * IntraSpread;

		UE_LOG(LogGameMode, Log, TEXT("  Axis %-12s: split %d/%d, picked %d/%d, MinCross %.0f, IntraSpread %.0f, Score %.0f"),
			*Axis.Name, CandidateA.Num(), CandidateB.Num(), PickA.Num(), PickB.Num(),
			MinCross, IntraSpread, AxisScore);

		if (AxisScore > BestAxisScore)
		{
			BestAxisScore = AxisScore;
			BestMinCross = MinCross;
			BestAxisName = Axis.Name;
			SideA = CandidateA;
			SideB = CandidateB;
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("ElimPlus: Selected axis '%s' Score %.0f, MinCross %.0f (sideA=%d, sideB=%d)"),
		*BestAxisName, BestAxisScore, BestMinCross, SideA.Num(), SideB.Num());

	PrecomputedSideA = SideA;
	PrecomputedSideB = SideB;

	// Sort each side by distance from the OTHER side's centroid — deeper spawns first
	FVector CentroidA = FVector::ZeroVector, CentroidB = FVector::ZeroVector;
	for (APlayerStart* S : SideA) CentroidA += S->GetActorLocation();
	for (APlayerStart* S : SideB) CentroidB += S->GetActorLocation();
	if (SideA.Num() > 0) CentroidA /= SideA.Num();
	if (SideB.Num() > 0) CentroidB /= SideB.Num();

	auto SortByDistFromCentroid = [](TArray<APlayerStart*>& Side, const FVector& OtherCentroid)
	{
		Side.Sort([&OtherCentroid](const APlayerStart& A, const APlayerStart& B)
		{
			return (A.GetActorLocation() - OtherCentroid).Size2D() > (B.GetActorLocation() - OtherCentroid).Size2D();
		});
	};
	SortByDistFromCentroid(SideA, CentroidB);
	SortByDistFromCentroid(SideB, CentroidA);

	const int32 SpawnsA = FMath::Min(SideA.Num(), SpawnsPerTeam);
	const int32 SpawnsB = FMath::Min(SideB.Num(), SpawnsPerTeam);

	// Main 4v4 layout — top SpawnsPerTeam from each sorted side
	if (SpawnsA >= 1 && SpawnsB >= 1)
	{
		FElimPlusSpawnLayout Layout;
		for (int32 i = 0; i < SpawnsA; ++i) Layout.T0_Spawns.Add(SideA[i]);
		for (int32 i = 0; i < SpawnsB; ++i) Layout.T1_Spawns.Add(SideB[i]);

		float MinCross = FLT_MAX;
		for (APlayerStart* S0 : Layout.T0_Spawns)
			for (APlayerStart* S1 : Layout.T1_Spawns)
				MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());

		// Intra-team cohesion penalty so the curated 4 actually cluster.
		// Prevents isolated "deep" spawns (e.g. central choke points) from being
		// included in the team's pool and stranding a player with the enemy.
		float IntraSpread = FMath::Max(IntraDiameter2D(Layout.T0_Spawns), IntraDiameter2D(Layout.T1_Spawns));
		Layout.MinCrossDistance2D = MinCross;
		Layout.QualityScore = MinCross - IntraSpreadPenalty * IntraSpread;
		ValidLayouts_4v4.Add(Layout);

		// Variants: rotate which spawns from side A are picked, keep side B fixed
		for (int32 Variant = 1; Variant < FMath::Min(SideA.Num(), 6); ++Variant)
		{
			FElimPlusSpawnLayout VLayout;
			for (int32 i = 0; i < SpawnsA; ++i) VLayout.T0_Spawns.Add(SideA[(i + Variant) % SideA.Num()]);
			for (int32 i = 0; i < SpawnsB; ++i) VLayout.T1_Spawns.Add(SideB[i]);

			MinCross = FLT_MAX;
			for (APlayerStart* S0 : VLayout.T0_Spawns)
				for (APlayerStart* S1 : VLayout.T1_Spawns)
					MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());
			float VIntraSpread = FMath::Max(IntraDiameter2D(VLayout.T0_Spawns), IntraDiameter2D(VLayout.T1_Spawns));
			VLayout.MinCrossDistance2D = MinCross;
			VLayout.QualityScore = MinCross - IntraSpreadPenalty * VIntraSpread;
			ValidLayouts_4v4.Add(VLayout);
		}
	}

	// 1v1 layouts (single spawn per team, every cross pair)
	for (int32 a = 0; a < SideA.Num(); ++a)
	{
		for (int32 b = 0; b < SideB.Num(); ++b)
		{
			const float Dist = (SideA[a]->GetActorLocation() - SideB[b]->GetActorLocation()).Size2D();
			FElimPlusSpawnLayout Layout;
			Layout.T0_Spawns.Add(SideA[a]);
			Layout.T1_Spawns.Add(SideB[b]);
			Layout.MinCrossDistance2D = Dist;
			Layout.QualityScore = Dist;
			ValidLayouts_1v1.Add(Layout);
		}
	}

	ValidLayouts_4v4.Sort([](const FElimPlusSpawnLayout& A, const FElimPlusSpawnLayout& B) { return A.QualityScore > B.QualityScore; });
	ValidLayouts_1v1.Sort([](const FElimPlusSpawnLayout& A, const FElimPlusSpawnLayout& B) { return A.QualityScore > B.QualityScore; });

	UE_LOG(LogGameMode, Log, TEXT("ElimPlus precompute: %d 4v4 layouts, %d 1v1 layouts"),
		ValidLayouts_4v4.Num(), ValidLayouts_1v1.Num());
}


void AElimPlusGame::SelectSpawnLayoutForRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();

	// Every 3rd round, force a tight 1v1 layout for variety. Otherwise pick
	// from the 4v4 pool with usage-penalty + jitter scoring.
	// Always prefer the team-size (4v4) layouts. 1v1 layouts are only a
	// fallback when no 4v4 layouts exist (degenerate maps). The previous
	// "force 1v1 every 3rd round" path broke 4v4 spawning catastrophically.
	TArray<FElimPlusSpawnLayout>& Pool = (ValidLayouts_4v4.Num() > 0) ? ValidLayouts_4v4 : ValidLayouts_1v1;

	if (Pool.Num() == 0)
	{
		UE_LOG(LogGameMode, Error, TEXT("ElimPlus: no valid spawn layouts!"));
		return;
	}

	const int32 PoolSize = FMath::Min(Pool.Num(), 30);
	int32 BestIndex = 0;
	float BestScore = -FLT_MAX;
	for (int32 i = 0; i < PoolSize; ++i)
	{
		const float UsagePenalty = Pool[i].UsageCount * 3000.0f;
		const float Jitter = FMath::FRandRange(0.0f, 500.0f);
		const float Score = Pool[i].QualityScore - UsagePenalty + Jitter;
		if (Score > BestScore) { BestScore = Score; BestIndex = i; }
	}

	FElimPlusSpawnLayout& Chosen = Pool[BestIndex];
	Chosen.UsageCount++;

	// Swap which team gets which side on odd rounds for fairness
	const bool bSwapTeams = (TotalRoundsPlayed % 2 == 1);
	const TArray<APlayerStart*>& T0Source = bSwapTeams ? Chosen.T1_Spawns : Chosen.T0_Spawns;
	const TArray<APlayerStart*>& T1Source = bSwapTeams ? Chosen.T0_Spawns : Chosen.T1_Spawns;

	Team0SelectedSpawns = T0Source;
	Team1SelectedSpawns = T1Source;

	UE_LOG(LogGameMode, Log, TEXT("ElimPlus round %d: Layout idx %d (Usage %d), CrossDist %.0f, T0 spawns=%d, T1 spawns=%d"),
		TotalRoundsPlayed, BestIndex, Chosen.UsageCount, Chosen.MinCrossDistance2D,
		Team0SelectedSpawns.Num(), Team1SelectedSpawns.Num());
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
	// (no-op) The per-player debug dump was disabled; the body + "=== DEBUG
	// PLAYER STATES ===" header log were removed to cut the spam. Re-add a
	// Verbose loop here if you need it.
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
		// Round over (custom 'RoundCooldown' state): losers are forced to spectate
		// the winners and Mouse1 (ServerViewNextPlayer) should cycle through them.
		// Deferring to Super rejected the cycle because RoundCooldown isn't "match
		// in progress" — so explicitly allow spectating any alive non-spectator.
		if (!TargetPS->bOnlySpectator)
		{
			const AController* TPC = Cast<AController>(TargetPS->GetOwner());
			const APawn* TP = TPC ? TPC->GetPawn() : nullptr;
			const AUTCharacter* TC = Cast<AUTCharacter>(TP);
			if (TP && (!TC || !TC->IsDead())) return true;
		}
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
	// Solo/practice rounds (one team empty at round start): an empty team can't
	// be "eliminated", so don't insta-award every round — let the round clock
	// run. At expiry DefaultTimer re-enters this function with the clock dead,
	// the guard goes inert, and the normal branches below award the round
	// (reason "TimeExpired" — real games are untouched since their starting
	// sizes are never 0). If the populated side wipes ITSELF (solo player dies:
	// no respawns in elimination), fall through for the instant Draw + next round.
	const bool bSoloRound    = (Team0StartingSize == 0) ^ (Team1StartingSize == 0);
	const bool bClockRunning = (RoundEndTimeSeconds > 0.f)
	                        && (GetWorld()->GetTimeSeconds() < RoundEndTimeSeconds);
	if (bSoloRound && bClockRunning)
	{
		const int32 PopulatedAlive = (Team0StartingSize == 0) ? Alive1 : Alive0;
		if (PopulatedAlive > 0)
		{
			return;   // alive vs nobody — wait for the round timer
		}
	}
	const bool Team0Eliminated = (Alive0 == 0);
	const bool Team1Eliminated = (Alive1 == 0);
	if (Team0Eliminated && !Team1Eliminated)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 1,
			FName((Team0StartingSize == 0) ? TEXT("TimeExpired") : TEXT("Elimination")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team1Eliminated && !Team0Eliminated)
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 0,
			FName((Team1StartingSize == 0) ? TEXT("TimeExpired") : TEXT("Elimination")));
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
	UE_LOG(LogGameMode, Verbose, TEXT("Overtime has started! First wave in %.1f seconds with %.1f damage"),
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
	UE_LOG(LogGameMode, Verbose, TEXT("Overtime Wave %d: %.1f damage to %d players"),
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
	UE_LOG(LogGameMode, Verbose, TEXT("Applied damage to %d players"), DamageCount);
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
	// Respect the bBalanceTeams URL/admin toggle. If the admin disabled
	// balancing, defer entirely to Super (which may also be a no-op).
	if (!bBalanceTeams)
	{
		return Super::PickBalancedTeam(PS, RequestedTeam);
	}

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
			if (!MemberPS)
			{
				TeamStrength += 1400;
				continue;
			}
			if (MemberPS->UniqueId.IsValid())
			{
				TeamStrength += RatingSystem->GetCachedElo(MemberPS->UniqueId.ToString());
			}
			else
			{
				// Bot or unrated. Same synthetic key shape as EndRoundForTeam's
				// BuildPerf path so the random-bot-ELO testing path is consistent
				// across balancer and Glicko math.
				const FString BotKey = FString::Printf(TEXT("BOT:%s"), *MemberPS->PlayerName);
				TeamStrength += RatingSystem->GetOrAssignBotElo(BotKey);
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


void AElimPlusGame::RebalanceTeamsForMatchStart()
{
	UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: ENTER (state=%s, bBalanceTeams=%s)"),
		*GetMatchState().ToString(), bBalanceTeams ? TEXT("true") : TEXT("false"));

	// Respect the URL flag / admin toggle. ?BalanceTeams=false on the server
	// command line parses into bBalanceTeams=false via AUTGameMode::InitGame.
	// Skip the Glicko shuffle entirely in that case so admins can run
	// stack-as-you-wish matches.
	if (!bBalanceTeams)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: skipped (bBalanceTeams=false)"));
		return;
	}

	if (!HasAuthority() || !RatingSystem.IsValid() || Teams.Num() < 2)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: bailed (auth=%d, rating=%d, teams=%d)"),
			HasAuthority() ? 1 : 0, RatingSystem.IsValid() ? 1 : 0, Teams.Num());
		return;
	}

	AUTGameState* GS = GetGameState<AUTGameState>();
	if (!GS) return;

	// Walk PlayerArray and build parallel arrays:
	//   Inputs: FString UniqueId per slot (real Epic ID for humans, "BOT:<name>" for bots)
	//   ControllersByIndex: AController* per slot
	// Index alignment is critical — TeamBalancer returns int slot indices that
	// reference back into both arrays.
	TArray<FElimPlusBalanceInput> Inputs;
	TArray<AController*> ControllersByIndex;
	Inputs.Reserve(GS->PlayerArray.Num());
	ControllersByIndex.Reserve(GS->PlayerArray.Num());

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		AController* C = Cast<AController>(UTPS->GetOwner());
		if (!C) continue;

		FElimPlusBalanceInput In;
		In.UniqueId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
		Inputs.Add(In);
		ControllersByIndex.Add(C);
	}

	if (Inputs.Num() < 2)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: skipped (only %d active players)"), Inputs.Num());
		return;
	}

	const FElimPlusBalanceResult Result = RatingSystem->ComputeBalancedTeams(Inputs);
	if (!Result.bValid)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: TeamBalancer returned invalid assignment, skipping"));
		return;
	}

	// Apply: any player whose new team differs from their current team gets
	// MovePlayerToTeam'd. Skip moves for already-correct assignments to avoid
	// the suicide+message cascade that ChangeTeam triggers.
	int32 MovesMade = 0;
	auto AssignToTeam = [this, &ControllersByIndex, &MovesMade](const TArray<int32>& Indices, uint8 TargetTeam)
	{
		for (int32 SlotIdx : Indices)
		{
			if (!ControllersByIndex.IsValidIndex(SlotIdx)) continue;
			AController* C = ControllersByIndex[SlotIdx];
			AUTPlayerState* UTPS = C ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
			if (!UTPS) continue;
			if (UTPS->Team && UTPS->Team->TeamIndex == TargetTeam) continue;  // already there
			MovePlayerToTeam(C, UTPS, TargetTeam);
			++MovesMade;
		}
	};
	AssignToTeam(Result.Team0Indices, 0);
	AssignToTeam(Result.Team1Indices, 1);

	UE_LOG(LogGameMode, Warning, TEXT("ElimPlus rebalance: %d players, Team0=%d str=%.1f, Team1=%d str=%.1f, diff=%.1f, moves=%d"),
		Inputs.Num(),
		Result.Team0Indices.Num(), Result.Team0Strength,
		Result.Team1Indices.Num(), Result.Team1Strength,
		Result.StrengthDifference, MovesMade);

	// Build human-readable per-team rosters and broadcast to every connected
	// player as a chat message. Mirrors what the old BP gamemode used to do —
	// the scoreboard's already up during countdown but having the result also
	// in chat gives players a stationary reference they can scroll back to.
	auto BuildTeamLine = [this, &ControllersByIndex](const TArray<int32>& Indices, const TCHAR* Prefix, float Strength) -> FString
	{
		FString Names;
		for (int32 SlotIdx : Indices)
		{
			if (!ControllersByIndex.IsValidIndex(SlotIdx)) continue;
			AController* C = ControllersByIndex[SlotIdx];
			AUTPlayerState* PS = C ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
			if (!PS) continue;

			int32 Elo = 1400;
			if (PS->UniqueId.IsValid())
			{
				Elo = RatingSystem->GetCachedElo(PS->UniqueId.ToString());
			}
			else
			{
				Elo = RatingSystem->GetOrAssignBotElo(FString::Printf(TEXT("BOT:%s"), *PS->PlayerName));
			}
			if (!Names.IsEmpty()) Names += TEXT(", ");
			Names += FString::Printf(TEXT("%s(%d)"), *PS->PlayerName, Elo);
		}
		return FString::Printf(TEXT("%s (str=%.0f): %s"), Prefix, Strength, *Names);
	};

	const FString Header  = FString::Printf(TEXT("=== ElimPlus Auto-Balance: diff=%.0f, moves=%d ==="),
	                                        Result.StrengthDifference, MovesMade);
	const FString RedLine  = BuildTeamLine(Result.Team0Indices, TEXT("RED  "), Result.Team0Strength);
	const FString BlueLine = BuildTeamLine(Result.Team1Indices, TEXT("BLUE "), Result.Team1Strength);

	UE_LOG(LogGameMode, Warning, TEXT("%s"), *Header);
	UE_LOG(LogGameMode, Warning, TEXT("%s"), *RedLine);
	UE_LOG(LogGameMode, Warning, TEXT("%s"), *BlueLine);

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) continue;
		PC->ClientMessage(Header);
		PC->ClientMessage(RedLine);
		PC->ClientMessage(BlueLine);
	}
}

// --- Mod.ini-gated match-host pause (see NCPlusHostPause.h) ---
#include "NCPlusHostPause.h"

bool AElimPlusGame::AllowPausing(APlayerController* PC)
{
	// Stock permissions (rcon admin / listen with no remotes) are preserved;
	// this only ADDS the ?HostId= match host when the server's Mod.ini sets
	// [NetcodePlus] bAllowHostPause=true.
	return Super::AllowPausing(PC) || NCPlusHostPause::HostMayPause(PC, this);
}

bool AElimPlusGame::ClearPause()
{
	// Host/rcon unpause: hold behind a short server-only resume countdown
	// (Mod.ini [NetcodePlus] UnpauseCountdownSec). Only engages while actually paused.
	if (NCPlusHostPause::DeferUnpauseForCountdown(this))
	{
		return false;
	}
	return Super::ClearPause();
}
