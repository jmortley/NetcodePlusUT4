// NCPlusCTFGameMode.cpp - NetcodePlus CTF with improved advantage time and instant replay
#include "NCPlusCTFGameMode.h"
#include "NCReadyUp.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"            // ValidateHat: SetOverrideHatClass / OverrideHatClass
#include "UTTeamGameMode.h"
#include "UTHUD_CTF.h"
#include "UTCTFGameMessage.h"
#include "UTCTFMajorMessage.h"
#include "UTCTFRewardMessage.h"
#include "UTFirstBloodMessage.h"
#include "UTCountDownMessage.h"
#include "UTPickup.h"
#include "UTGameMessage.h"
#include "UTMutator.h"
#include "UTCharacterMovement.h"
#include "UTTeamPlayerStart.h"
#include "TeamArenaCharacter.h"
#include "UTCTFSquadAI.h"
#include "UTWorldSettings.h"
#include "StatNames.h"
#include "Engine/DemoNetDriver.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"
#include "UTCTFScoreboard.h"
#include "UTCharacterVoice.h"
#include "UTCTFScoring.h"
#include "UTFlag.h"
#include "NPPlayerController.h"
#include "NCPlusCTFHUD.h"
#include "NCPlusHostPause.h"
#include "NCAutoPauseState.h"
#include "WarmupRoamMutator.h"
#include "NCPlusVersionGate.h"
#include "NCConcedeVote.h"
#include "CTFStatsReplicator.h"
#include "NCAccuracyStatsReplicator.h"
#include "NCICTFFlagLiftGuard.h"
#include "NCPlusCTFOTInfo.h"
#include "NCPlusCTFRatingSystem.h"
#include "NCEloUploader.h"
#include "UTGameMode.h"     // AUTGameMode::bIsInstagib
#include "UTATypes.h"                  // NAME_FCKills / NAME_FlagSupportKills / NAME_FlagGrabs / ...
#include "Misc/ConfigCacheIni.h"       // FConfigFile — Mod.ini perf knobs
#include "Misc/Paths.h"                // FPaths::GameSavedDir
#include "Containers/Ticker.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"

namespace
{
	bool IsLockedPlayerReadyCountdown(const AUTGameMode* GameMode)
	{
		if (GameMode == nullptr || GameMode->GetWorld() == nullptr)
		{
			return false;
		}

		const ANCReadyUpState* ReadyState = ANCReadyUpState::Find(GameMode->GetWorld());
		if (ReadyState == nullptr || !ReadyState->bCountdownLocked)
		{
			return false;
		}

		const FName State = GameMode->GetMatchState();
		return State == MatchState::WaitingToStart
			|| State == MatchState::PlayerIntro
			|| State == MatchState::CountdownToBegin;
	}
}

ANCPlusCTFGameMode::ANCPlusCTFGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	//PlayerControllerClass = ANPPlayerController::StaticClass(); // Enable when ready to test debounce fix
	IntermissionDuration = 30.f;
	bAllowOvertime = true;
	MercyScore = 5;
	GoalScore = 0;
	TimeLimit = 14;
	CTFScoringClass = AUTCTFScoring::StaticClass();
	DisplayName = NSLOCTEXT("UTGameMode", "NCPlusCTF", "NetcodePlus CTF");
	HUDClass = ANCPlusCTFHUD::StaticClass();

	// Advantage configuration
	AdvantageMaxDuration = 300;
	GracePeriodDuration = 10;
	bEndGameAdvantageOnlyWithinOneCap = true;

	// Spawn configuration — defaults match BP values
	FlagBaseProximityRadius = 3000.f;   // BP: FlagBlockRange
	FlagSpawnPenaltyRadius = 3000.f;    // Radius for distance-scaled penalties near flag
	FlagCarrierSpawnPenalty = 15.f;
	DroppedFlagSpawnPenalty = 10.f;
	FlagCarrierLOSPenalty = 5.f;
	EnemyBlockRange = 2500.f;           // BP: EnemyBlockRange — penalize spawns near ANY enemy
	EnemyBlockPenalty = 10.f;
	EnemyLOSBlockRange = 3000.f;        // BP: EnemyLOSBlockRange — LOS check to nearby enemies
	EnemyLOSPenalty = 8.f;

	// Legacy rollback scoring/selection (tunable via Mod.ini [UTPUGS_SPAWN]).
	SpawnTieBandWidth = 2.0f;           // starts within ~2 score pts of best = a coin-flip
	SpawnFreshnessBonus = 10.0f;        // when calm, lift unused starts to spread spawns
	SpawnFreshnessWindow = 30.0f;       // 30s since last use = fully fresh
	SpawnFlagVicinityRadius = 4000.f;   // flag within this of our base = "in the vicinity"
	SpawnKillerAvoidRadius = 2500.f;    // never respawn within this of your last killer (anti-camp)
	SpawnFlagCarrierLOSAvoidRadius = 3500.f; // prefer starts out of the EFC's direct sightline
	SpawnRobbedBaseAvoidCount = 2.f;    // when our flag's out, the 2 deepest base spawns form the avoid set — ONE blocked per respawn, alternating

	bHasHalftime = true;                // Default true; auto-set false for 3v3+ in InitGame
	bAllowFloorSlide = true;            // Enabled by default; set false in BP for Sniper CTF etc.
	OvertimeRespawnTime = 10.f;         // OT respawn cap (327 was 6; raised with the tOxX ramp 2026-08-10)
	OvertimeEscalationDelay = 360.f;    // 2s base holds for the first 6 min of OT (tOxX 2026-08-10; was 5)
	OvertimeEscalationInterval = 60.f;  // then +1s per minute (was +1s per 2 minutes)

	// Internal state
	AdvantageTimeRemaining = 0;
	GracePeriodTimeRemaining = 0;
	bGracePeriodActive = false;
	OvertimeStartWorldTime = 0.f;
	LastAdvantageCapTime = 0.f;
	bAdvantageCapEndedPeriod = false;
	LastScoreObjectTime = 0.f;
}

void ANCPlusCTFGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	NCReadyUp::Initialize(this);
	// Auto-pause must be configured before a locked ready countdown can begin.
	// The remaining perf values are harmless to load here and are consumed once
	// live play starts; loading only in HandleMatchHasStarted was too late.
	LoadCTFPerfConfig();

	// Auto-add the warmup-roam mutator (all NCPlusCTF, incl. iCTF). `mutate warmup`
	// lets players roam the map invulnerable + fire-disabled during warmup; it strips
	// itself the instant the match leaves WaitingToStart (see the mutator's
	// NotifyMatchStateChange), so it can never carry into live play.
	AddMutatorClass(AWarmupRoamMutator::StaticClass());

	// A bot-hosted PUG passes ?PugId=N — gate auto-pause-on-drop to real PUGs.
	bIsPugMatch = UGameplayStatics::HasOption(Options, TEXT("PugId"));

	// Bot-assigned teams: ?PugTeams=<ut4id>:0,<ut4id>:1,...  The bot already
	// balanced the teams; pin each listed player to their side in ChangeTeam so
	// the engine's warmup auto-balance can't reshuffle them (and nobody has to
	// hand-swap). Keys are lowercased EOS ids — same string MutBotEvents posts as
	// Ut4Id and the bot stores in players.ut4_id. Players not listed (unlinked, or
	// subs) aren't pinned and use the stock balancer.
	PugRosterTeam.Reset();
	const FString TeamsOpt = UGameplayStatics::ParseOption(Options, TEXT("PugTeams"));
	if (!TeamsOpt.IsEmpty())
	{
		TArray<FString> Entries;
		TeamsOpt.ParseIntoArray(Entries, TEXT(","), true);
		for (const FString& Entry : Entries)
		{
			FString IdPart, TeamPart;
			if (Entry.Split(TEXT(":"), &IdPart, &TeamPart))
			{
				const FString Key = IdPart.ToLower();
				const uint8 TeamNum = (uint8)FMath::Clamp(FCString::Atoi(*TeamPart), 0, 1);
				if (!Key.IsEmpty())
				{
					PugRosterTeam.Add(Key, TeamNum);
				}
			}
		}
		UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF: PUG roster parsed — %d players pinned to teams"), PugRosterTeam.Num());
	}

	IntermissionDuration = FMath::Max(1, UGameplayStatics::GetIntOption(Options, TEXT("HalftimeDuration"), IntermissionDuration));
	AdvantageMaxDuration = FMath::Max(60, UGameplayStatics::GetIntOption(Options, TEXT("AdvantageMaxDuration"), AdvantageMaxDuration));
	GracePeriodDuration = FMath::Max(3, UGameplayStatics::GetIntOption(Options, TEXT("GracePeriod"), GracePeriodDuration));

	if (bOfflineChallenge)
	{
		TimeLimit = 600;
	}

	// Auto-disable halftime for 3v3+ games — single period, full time limit.
	// 1v1/2v2 keep two halves with intermission and side switch.
	if (GameSession && GameSession->MaxPlayers > 4)
	{
		bHasHalftime = false;
	}

	// Halve time limit only for two-half games
	if (bHasHalftime && TimeLimit > 0)
	{
		TimeLimit = uint32(float(TimeLimit) * 0.5f);
	}

	// Safety: if the map has UTTeamPlayerStarts but no plain PlayerStarts,
	// the engine asserts fatally in FindPlayerStart. Spawn fallback PlayerStarts
	// at UTTeamPlayerStart locations so the engine can find them.
	bool bHasPlayerStart = false;
	bool bHasTeamStart = false;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (Cast<AUTTeamPlayerStart>(*It))
		{
			bHasTeamStart = true;
		}
		else
		{
			bHasPlayerStart = true;
		}
	}

	if (!bHasPlayerStart && bHasTeamStart)
	{
		UE_LOG(LogGameMode, Warning, TEXT("NCPlusCTF: Map has UTTeamPlayerStarts but no plain PlayerStarts — spawning fallbacks"));
		for (TActorIterator<AUTTeamPlayerStart> It(GetWorld()); It; ++It)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			APlayerStart* Fallback = GetWorld()->SpawnActor<APlayerStart>(
				APlayerStart::StaticClass(),
				It->GetActorLocation(),
				It->GetActorRotation(),
				SpawnParams);
			if (Fallback)
			{
				Fallback->PlayerStartTag = FName(*FString::Printf(TEXT("Team%d"), It->TeamNum));
			}
		}
	}

	// Match-scoped: reset the rating flush guard on each map load. Rating system
	// instance itself is constructed lazily in HandleMatchHasStarted — that's
	// when AUTGameMode::bIsInstagib is reliably settled (same timing
	// CTFStatsReplicator and MutServerShield use). Reading earlier risks the
	// Instagib BP mutator not yet having set the flag, which would route an iCTF
	// match into the regular CTF Mods.db table.
	bRatingFlushedThisMatch = false;

	// Team spawn pools rebuild per match, lazily on the first ChoosePlayerStart.
	bSpawnPoolsBuilt = false;
	Team0Spawns.Reset();
	Team1Spawns.Reset();
	SpawnLastUsedTime.Reset();
	PlayerRecentSpawns.Reset();
	PlayerLastSpawnLoc.Reset();
	RobbedSpawnRotation[0] = 0;
	RobbedSpawnRotation[1] = 0;

	// Per-match leaver/stat caches reset on each map load.
	MatchStatCache.Empty();
	PlayerJoinWorldTime.Empty();
	MatchStartWorldTime = 0.f;
	MatchFullDurationSeconds = 0.f;
}

void ANCPlusCTFGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Create both NCRatingCTF + NCRatingICTF tables now (static, idempotent).
	// The rating-system instance — which picks ONE of the two tables based on
	// bIsInstagib — is deferred to HandleMatchHasStarted (see comment in
	// InitGame above).
	if (HasAuthority())
	{
		FNCPlusCTFRatingSystem::InitDatabase(GetWorld());
		StartAutoPauseWatch();
	}
}

void ANCPlusCTFGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopAutoPauseWatch();
	NCPlusHostPause::CancelDeferredUnpause(this);
	if (AutoPauseResumeTicker.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AutoPauseResumeTicker);
		AutoPauseResumeTicker.Reset();
	}
	bAutoPauseResumeCountdownActive = false;
	AutoPauseResumeSecondsRemaining = 0;
	AutoPauseResumeEndRealTime = 0.0f;
	bAutoPauseDormantNoMarker = false;
	AutoPauseStateActor = nullptr;

	Super::EndPlay(EndPlayReason);
}

void ANCPlusCTFGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	NCReadyUp::PostLogin(this, NewPlayer);

	// Early version check — kicks outdated/missing-plugin clients within 10s of
	// PostLogin, BEFORE they can play meaningful warmup. Replaces the BP check
	// that fires at match start (which let them roam the map during warmup,
	// breaking PUGs when they got kicked at go-time). Skips bots + listen host.
	NCPlusVersionGate::SpawnFor(NewPlayer);
	// Concede-vote RPC channel (gg / F1 / F4) — skips bots + the listen host.
	NCConcede::SpawnFor(NewPlayer);

	if (!HasAuthority() || !NewPlayer) return;

	// Warmup joiner: rating system isn't constructed yet. They'll be picked up
	// en-masse by the GS->PlayerArray walk in HandleMatchHasStarted.
	// Late joiner (mid-match): rating system already exists, load immediately.
	AUTPlayerState* UTPS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (UTPS && UTPS->UniqueId.IsValid())
	{
		const FString Uid = UTPS->UniqueId.ToString();
		if (bIsPugMatch && !UTPS->bIsABot && !UTPS->bOnlySpectator
			&& UTPS->GetTeamNum() <= 1)
		{
			AutoPauseTrackedIds.Add(Uid);
		}
		// Spectators skip the rating preload (mirrors the HandleMatchHasStarted
		// bulk-load gate): they are never rated, but this used to run for every
		// mid-match caster join — a synchronous DB read (plus a first-timer
		// INSERT + fsync) on the game thread while everyone plays. A spectator
		// who later enters play is loaded by EnsureRatingLoadedForPlayer via
		// ChangeTeam instead.
		if (RatingSystem.IsValid() && !UTPS->bOnlySpectator)
		{
			RatingSystem->LoadPlayerFromDB(GetWorld(), Uid);
			// Mid-match joiner: stamp first-seen time for the leaver presence calc.
			// (Warmup joiners are stamped en-masse in HandleMatchHasStarted.) Keep
			// the earliest sighting on a rejoin.
			if (!PlayerJoinWorldTime.Contains(Uid))
			{
				PlayerJoinWorldTime.Add(Uid, GetWorld()->GetTimeSeconds());
			}
		}

		// Auto-pause: an awaited drop just rejoined — resume once everyone we're
		// waiting on is back.
		// Reconcile any logical/physical auto-pause, including one started during
		// the locked F5 countdown. PostLogin still runs while the world is paused.
		if (bAutoPaused && CTFGameState)
		{
			const bool bWasAwaited = AutoPauseAwaitIds.Contains(Uid);
			const uint8* ExpectedTeam = AutoPauseReadyCountdownTeams.Find(Uid);
			if (ExpectedTeam == nullptr)
			{
				ExpectedTeam = PugRosterTeam.Find(Uid.ToLower());
			}
			const bool bReturnedAsParticipant = !UTPS->bIsABot && !UTPS->bOnlySpectator
				&& UTPS->GetTeamNum() <= 1
				&& (ExpectedTeam == nullptr || *ExpectedTeam == UTPS->GetTeamNum());
			bool bRestoredPhysicalPause = false;
			if (AWorldSettings* WS = GetWorldSettings())
			{
				if (WS->Pauser == nullptr && !UTPS->bIsABot
					&& (bWasAwaited || bAutoPauseDormantNoMarker))
				{
					WS->Pauser = UTPS;
					WS->ForceNetUpdate();
					bAutoPauseDormantNoMarker = false;
					bRestoredPhysicalPause = true;
				}
			}

			if (bWasAwaited)
			{
				// Merely reconnecting as a spectator must not resume into a 4v5/3v4.
				// The pause-immune watcher will reconcile once ChangeTeam places this
				// exact ID back into its active (and, when pinned, assigned) team slot.
				if (!bReturnedAsParticipant)
				{
					const FString WaitingReason = FString::Printf(
						TEXT("Waiting for %s to return to their team"), *UTPS->PlayerName);
					if (bAutoPauseResumeCountdownActive)
					{
						CancelAutoPauseResumeCountdown(WaitingReason);
					}
					else
					{
						PublishAutoPausePaused(WaitingReason);
					}
					return;
				}
				AutoPauseAwaitIds.Remove(Uid);
				UE_LOG(LogGameMode, Warning,
					TEXT("NCPlusCTF auto-pause: %s rejoined (%d still out)"),
					*UTPS->PlayerName, AutoPauseAwaitIds.Num());
				if (AutoPauseAwaitIds.Num() == 0)
				{
					BeginAutoPauseResumeCountdown(TEXT("All disconnected players returned"));
				}
				else
				{
					const FString WaitingReason = FString::Printf(
						TEXT("Waiting for %d disconnected player%s"),
						AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
					if (bAutoPauseResumeCountdownActive)
					{
						CancelAutoPauseResumeCountdown(WaitingReason);
					}
					else
					{
						PublishAutoPausePaused(WaitingReason);
					}
				}
			}
			else if (bRestoredPhysicalPause)
			{
				if (AutoPauseAwaitIds.Num() == 0)
				{
					BeginAutoPauseResumeCountdown(TEXT("No disconnected players remain"));
					return;
				}
				const FString WaitingReason = FString::Printf(
					TEXT("Waiting for %d disconnected player%s"),
					AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
				if (bAutoPauseResumeCountdownActive)
				{
					CancelAutoPauseResumeCountdown(WaitingReason);
				}
				else
				{
					PublishAutoPausePaused(WaitingReason);
				}
			}
		}
	}
}

bool ANCPlusCTFGameMode::ReadyToStartMatch_Implementation()
{
	return NCReadyUp::ShouldHandle(this)
		? NCReadyUp::ReadyToStartMatch(this)
		: Super::ReadyToStartMatch_Implementation();
}

bool ANCPlusCTFGameMode::ChangeTeam(AController* Player, uint8 NewTeam, bool bBroadcast)
{
	// Bot PUG: keep each rostered player on the side the bot balanced. login picks,
	// player-initiated switches, and the engine's CountdownToBegin auto-balance
	// (AUTTeamGameMode::ShouldBalanceTeams) all funnel through ChangeTeam, so this
	// is the one place that pins them — the auto-balance is exactly what was
	// swapping people onto the wrong team. Non-roster joiners (subs, late fills,
	// or anyone who hasn't /linked) get the stock balancer via Super.
	if (bIsPugMatch && PugRosterTeam.Num() > 0 && Player && HasAuthority())
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
		if (PS && !PS->bOnlySpectator && PS->UniqueId.IsValid())
		{
			// Match on UniqueId.ToString() — the same id this gamemode keys the
			// rating DB on in PostLogin, which equals MutBotEvents' Ut4Id and the
			// bot's players.ut4_id.
			if (const uint8* Assigned = PugRosterTeam.Find(PS->UniqueId.ToString().ToLower()))
			{
				const uint8 Want = *Assigned;
				// Already on the right side — accept without re-suiciding them
				// (MovePlayerToTeam kills the pawn on an actual move).
				if (PS->Team && PS->Team->TeamIndex == Want)
				{
					EnsureRatingLoadedForPlayer(Player);
					return true;
				}
				const bool bMoved = MovePlayerToTeam(Player, PS, Want);
				if (bMoved)
				{
					EnsureRatingLoadedForPlayer(Player);
				}
				return bMoved;
			}
		}
	}

	const bool bChanged = Super::ChangeTeam(Player, NewTeam, bBroadcast);
	if (bChanged)
	{
		EnsureRatingLoadedForPlayer(Player);
	}
	return bChanged;
}

// Rating preload for a player ENTERING PLAY mid-match. PostLogin deliberately
// skips spectators (they are never rated; the preload was hitching caster
// joins), so every successful team entry funnels through here instead.
// Idempotent and cheap on repeat: LoadPlayerFromDB is cache-first, the
// first-seen stamp is Contains-guarded, and pre-match calls no-op because
// RatingSystem isn't constructed until match start (the bulk load covers those).
void ANCPlusCTFGameMode::EnsureRatingLoadedForPlayer(AController* Player)
{
	if (!HasAuthority() || !RatingSystem.IsValid() || !Player)
	{
		return;
	}
	AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
	if (!PS || !PS->UniqueId.IsValid())
	{
		return;
	}
	const FString Uid = PS->UniqueId.ToString();
	if (bIsPugMatch && !PS->bIsABot && !PS->bOnlySpectator && PS->GetTeamNum() <= 1)
	{
		AutoPauseTrackedIds.Add(Uid);
	}
	RatingSystem->LoadPlayerFromDB(GetWorld(), Uid);
	if (!PlayerJoinWorldTime.Contains(Uid))
	{
		PlayerJoinWorldTime.Add(Uid, GetWorld()->GetTimeSeconds());
	}
}

void ANCPlusCTFGameMode::HandleMatchHasEnded()
{
	if (HasAuthority())
	{
		if (bAutoPaused)
		{
			CompleteAutoPauseResume(TEXT("Match ended"));
		}
		AutoPauseTrackedIds.Reset();
		AutoPauseReadyCountdownIds.Reset();
		AutoPauseReadyCountdownTeams.Reset();
	}
	Super::HandleMatchHasEnded();

	if (!HasAuthority() || !RatingSystem.IsValid() || bRatingFlushedThisMatch)
	{
		return;
	}

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS) return;

	// Build the single-match input. Perf config + leaver snapshots were captured
	// at match start / Logout; here we fold in everyone still present.
	FNCPlusCTFMatchInput In;
	In.bIsInstagib = RatingSystem->IsInstagibMode();
	In.Perf        = CTFPerfConfig;

	int32 RedScore  = 0;
	int32 BlueScore = 0;
	if (GS->Teams.Num() >= 2 && GS->Teams[0] && GS->Teams[1])
	{
		RedScore  = static_cast<int32>(GS->Teams[0]->Score);
		BlueScore = static_cast<int32>(GS->Teams[1]->Score);
	}
	In.RedScore  = RedScore;
	In.BlueScore = BlueScore;
	if (RedScore > BlueScore)      In.WinnerTeamIndex = 0;
	else if (BlueScore > RedScore) In.WinnerTeamIndex = 1;
	else                            In.WinnerTeamIndex = -1;

	// Players still present overwrite any earlier Logout snapshot with their
	// complete final line — present == most complete, and always rated.
	for (APlayerState* APS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		if (!UTPS->UniqueId.IsValid()) continue;  // bot
		if (UTPS->GetTeamNum() > 1) continue;      // no valid CTF team slot

		FNCPlusCTFPlayerInput P;
		CapturePlayerStats(UTPS, P);
		const FString Key = P.UniqueId;   // copy key first — Add arg eval order is unspecified
		MatchStatCache.Add(Key, MoveTemp(P));
	}

	// Feed the union of present players + leavers who cleared the presence
	// threshold at Logout, so team sizes / z-scores reflect the real roster
	// and a rage-quitter can't dodge the result by disconnecting.
	In.Players.Reserve(MatchStatCache.Num());
	for (const TPair<FString, FNCPlusCTFPlayerInput>& Pair : MatchStatCache)
	{
		In.Players.Add(Pair.Value);
	}

	// Snapshot BEFORE the ProcessMatch in-memory update so the delta is correct.
	// (HandleMatchHasStarted is the canonical snapshot point for ElimPlus, but
	// CTF can lose players between match-start and match-end via mid-match join,
	// and SnapshotMatchStart is idempotent — calling here captures the actual
	// pre-update state for whoever ended up playing.)
	RatingSystem->SnapshotMatchStart();
	RatingSystem->ProcessMatch(In);
	RatingSystem->Flush(GetWorld());
	bRatingFlushedThisMatch = true;

	const FString Json = RatingSystem->BuildResultPayload(GetWorld(), In);
	if (!Json.IsEmpty())
	{
		FNCEloUploader::PostMatchResult(GetWorld(), Json);
	}
}

void ANCPlusCTFGameMode::ForceClearUnpauseDelegates(AActor* PauseActor)
{
	AWorldSettings* WS = GetWorldSettings();
	APlayerController* DepartingPC = Cast<APlayerController>(PauseActor);
	APlayerState* DepartingPS = DepartingPC != nullptr ? DepartingPC->PlayerState : nullptr;
	const bool bWasAutoPauseMarker = bAutoPaused && WS != nullptr
		&& DepartingPS != nullptr && WS->Pauser == DepartingPS;

	// APlayerController::Destroyed calls this before AController::Destroyed calls
	// Logout. The parent may invoke virtual ClearPause; mark that call as teardown
	// so it explicitly clears instead of starting a user-facing resume countdown.
	bForceClearingPauseActor = true;
	Super::ForceClearUnpauseDelegates(PauseActor);
	bForceClearingPauseActor = false;

	const bool bLostNonDormantAutoMarker = bAutoPaused && !bAutoPauseDormantNoMarker
		&& WS != nullptr && WS->Pauser == nullptr;
	if ((!bWasAutoPauseMarker && !bLostNonDormantAutoMarker) || !bAutoPaused || WS == nullptr)
	{
		return;
	}

	// Keep the world frozen across the tiny ForceClear -> Logout gap. Logout will
	// then add a tracked leaver's exact ID and may choose the marker again.
	if (APlayerState* Replacement = FindAutoPauseMarker(DepartingPS))
	{
		WS->Pauser = Replacement;
		WS->ForceNetUpdate();
		bAutoPauseDormantNoMarker = false;
	}
	else
	{
		if (AutoPauseAwaitIds.Num() == 0)
		{
			CompleteAutoPauseResume(TEXT("Pause marker left after all players returned"));
			return;
		}
		bAutoPauseDormantNoMarker = true;
		const FString WaitingReason = FString::Printf(
			TEXT("Waiting for %d disconnected player%s"),
			AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
		if (bAutoPauseResumeCountdownActive)
		{
			CancelAutoPauseResumeCountdown(WaitingReason);
		}
		else
		{
			PublishAutoPausePaused(WaitingReason);
		}
	}
}

void ANCPlusCTFGameMode::Logout(AController* Exiting)
{
	// Snapshot a leaver's stats while their PlayerState is still intact (the
	// engine duplicates+destroys it just after this). Rate them on the final
	// result if they were present long enough — a rage-quitter near the end
	// must not dodge the loss by disconnecting; a genuine early leaver is
	// dropped (and excluded from the team z-score, since the match was
	// effectively short-handed). RatingSystem.IsValid() => match has started;
	// !bRatingFlushedThisMatch => not yet processed. Deliberately does NOT call
	// Forget — the rating must stay cached for the match-end ProcessMatch/Flush.
	if (HasAuthority() && Exiting && RatingSystem.IsValid() && !bRatingFlushedThisMatch)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (UTPS && !UTPS->bIsABot && !UTPS->bOnlySpectator
			&& UTPS->UniqueId.IsValid() && UTPS->GetTeamNum() <= 1)
		{
			const FString Uid = UTPS->UniqueId.ToString();

			const float Now      = GetWorld()->GetTimeSeconds();
			const float JoinTime = PlayerJoinWorldTime.FindRef(Uid);   // 0.f if unknown
			const bool  bCanThreshold = (MatchFullDurationSeconds > 1.f);
			const float Frac     = bCanThreshold ? ((Now - JoinTime) / MatchFullDurationSeconds) : 1.f;

			if (!bCanThreshold || Frac >= CTFRatingMinPresenceFrac)
			{
				FNCPlusCTFPlayerInput P;
				CapturePlayerStats(UTPS, P);
				MatchStatCache.Add(Uid, MoveTemp(P));
				UE_LOG(LogGameMode, Log,
					TEXT("NCPlusCTF: rated leaver %s (presence %.0f%%, takes match result)"),
					*UTPS->PlayerName, Frac * 100.f);
			}
			else
			{
				// Below threshold — drop any prior snapshot so they're neither
				// rated nor counted in the roster.
				MatchStatCache.Remove(Uid);
				UE_LOG(LogGameMode, Log,
					TEXT("NCPlusCTF: dropped early leaver %s (presence %.0f%% < %.0f%%)"),
					*UTPS->PlayerName, Frac * 100.f, CTFRatingMinPresenceFrac * 100.f);
			}
		}
	}

	// Auto-pause: a participant dropping mid-PUG freezes the match until they
	// rejoin (or a manual unpause is requested). Server-only; uses the engine world-pause
	// (WorldSettings->Pauser) — the same primitive as the `pause` command.
	// Runs BEFORE Super (the leaver's PlayerState is still intact here).
	const bool bLivePausePhase = CTFGameState
		&& (CTFGameState->IsMatchInProgress() || CTFGameState->IsMatchInOvertime());
	ANCReadyUpState* LockedReadyState = IsLockedPlayerReadyCountdown(this)
		? ANCReadyUpState::Find(GetWorld()) : nullptr;
	CaptureLockedReadyParticipants(LockedReadyState);
	const bool bPauseEligiblePhase = bLivePausePhase || LockedReadyState != nullptr;
	if (HasAuthority() && bAutoPauseOnDrop && bIsPugMatch && Exiting && CTFGameState
		&& bPauseEligiblePhase)
	{
		AUTPlayerState* LeavePS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (LeavePS && !LeavePS->bIsABot && LeavePS->UniqueId.IsValid())
		{
			// During the locked pre-match countdown, only the frozen ready roster is
			// expected. A late roster addition cannot create a new awaited ID.
			const FString LeaveId = LeavePS->UniqueId.ToString();
			const bool bExpectedParticipant = bLivePausePhase
				|| AutoPauseReadyCountdownIds.Contains(LeaveId);
			if (bExpectedParticipant)
			{
				if (!LeavePS->bOnlySpectator && LeavePS->GetTeamNum() <= 1)
				{
					AutoPauseTrackedIds.Add(LeaveId);
				}
				const uint8* ExpectedReturnTeam = AutoPauseReadyCountdownTeams.Find(LeaveId);
				if (ExpectedReturnTeam == nullptr)
				{
					ExpectedReturnTeam = PugRosterTeam.Find(LeaveId.ToLower());
				}
				bool bSameIdStillPresent = false;
				for (APlayerState* OtherPS : CTFGameState->PlayerArray)
				{
					AUTPlayerState* OtherUTPS = Cast<AUTPlayerState>(OtherPS);
					if (OtherUTPS != nullptr && OtherUTPS != LeavePS
						&& !OtherUTPS->IsPendingKillPending() && !OtherUTPS->bIsInactive
						&& !OtherUTPS->bIsABot && !OtherUTPS->bOnlySpectator
						&& OtherUTPS->UniqueId.IsValid() && OtherUTPS->GetTeamNum() <= 1
						&& (ExpectedReturnTeam == nullptr
							|| *ExpectedReturnTeam == OtherUTPS->GetTeamNum())
						&& OtherUTPS->UniqueId.ToString().Equals(LeaveId, ESearchCase::IgnoreCase))
					{
						bSameIdStillPresent = true;
						break;
					}
				}
				if (AutoPauseTrackedIds.Contains(LeaveId) && !bSameIdStillPresent)
				{
					bool bCancelEmptyReadyCountdown = false;
					if (!bLivePausePhase && LockedReadyState != nullptr
						&& GetMatchState() == MatchState::WaitingToStart)
					{
						int32 RemainingParticipants = 0;
						for (APlayerState* OtherPS : CTFGameState->PlayerArray)
						{
							AUTPlayerState* OtherUTPS = Cast<AUTPlayerState>(OtherPS);
							if (OtherUTPS != nullptr && OtherUTPS != LeavePS
								&& !OtherUTPS->IsPendingKillPending() && !OtherUTPS->bIsInactive
								&& !OtherUTPS->bIsABot && !OtherUTPS->bOnlySpectator
								&& OtherUTPS->UniqueId.IsValid() && OtherUTPS->GetTeamNum() <= 1)
							{
								const FString OtherId = OtherUTPS->UniqueId.ToString();
								const uint8* ExpectedTeam = AutoPauseReadyCountdownTeams.Find(OtherId);
								if (AutoPauseReadyCountdownIds.Contains(OtherId)
									&& !AutoPauseAwaitIds.Contains(OtherId)
									&& (ExpectedTeam == nullptr
										|| *ExpectedTeam == OtherUTPS->GetTeamNum()))
								{
									++RemainingParticipants;
								}
							}
						}
						bCancelEmptyReadyCountdown = RemainingParticipants == 0;
					}

					if (bCancelEmptyReadyCountdown)
					{
						LockedReadyState->CancelCountdown();
						AutoPauseReadyCountdownIds.Reset();
						AutoPauseReadyCountdownTeams.Reset();
						CTFGameState->SetRemainingTime(0.f);
						if (bAutoPaused)
						{
							CompleteAutoPauseResume(TEXT("Ready countdown emptied"));
						}
						UE_LOG(LogGameMode, Warning,
							TEXT("NCPlusCTF auto-pause: final ready participant left - countdown cancelled"));
					}
					else
					{
						BeginOrHoldAutoPause(LeaveId, LeavePS->PlayerName, LeavePS);
					}
				}
			}
		}
	}

	// If an untracked fallback marker (spectator/bot) leaves, rehome the engine
	// marker without adding that observer to the exact-ID participant wait.
	if (HasAuthority() && bAutoPaused && Exiting)
	{
		AUTPlayerState* MarkerPS = Cast<AUTPlayerState>(Exiting->PlayerState);
		AWorldSettings* WS = GetWorldSettings();
		if (MarkerPS && WS && WS->Pauser == MarkerPS
			&& (!MarkerPS->UniqueId.IsValid()
				|| !AutoPauseAwaitIds.Contains(MarkerPS->UniqueId.ToString())))
		{
			if (APlayerState* Replacement = FindAutoPauseMarker(MarkerPS))
			{
				WS->Pauser = Replacement;
				WS->ForceNetUpdate();
				bAutoPauseDormantNoMarker = false;
			}
			else
			{
				if (AutoPauseAwaitIds.Num() == 0)
				{
					CompleteAutoPauseResume(TEXT("Fallback pause marker left after all players returned"));
				}
				else
				{
					bAutoPauseDormantNoMarker = true;
					const FString WaitingReason = FString::Printf(
						TEXT("Waiting for %d disconnected player%s"),
						AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
					if (bAutoPauseResumeCountdownActive)
					{
						CancelAutoPauseResumeCountdown(WaitingReason);
					}
					else
					{
						PublishAutoPausePaused(WaitingReason);
					}
					const bool bWasPhysicallyPaused = WS->Pauser != nullptr;
					Super::ClearPause();
					if (bWasPhysicallyPaused && WS->Pauser == nullptr)
					{
						WS->ForceNetUpdate();
						NCPlusHostPause::ResyncServerWorldTime(this);
					}
				}
			}
		}
	}

	if (Exiting)
	{
		const TWeakObjectPtr<AController> Key(Exiting);
		PlayerRecentSpawns.Remove(Key);
		PlayerLastSpawnLoc.Remove(Key);
	}

	Super::Logout(Exiting);
}

APlayerState* ANCPlusCTFGameMode::FindAutoPauseMarker(const APlayerState* Excluded) const
{
	// Prefer a present human participant who hasn't dropped. A spectator/bot is
	// a safe physical fallback. As a final fallback, a hard-silent participant
	// whose controller has not reached Logout yet can still hold Pauser; teardown
	// will rehome it again. Logout never adds fallback markers to the exact-ID wait.
	if (!CTFGameState) return nullptr;
	APlayerState* Fallback = nullptr;
	APlayerState* AwaitedFallback = nullptr;
	for (APlayerState* PS : CTFGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS == Excluded || UTPS->IsPendingKillPending() || UTPS->bIsInactive)
		{
			continue;
		}
		if (UTPS->UniqueId.IsValid()
			&& AutoPauseAwaitIds.Contains(UTPS->UniqueId.ToString()))
		{
			if (AwaitedFallback == nullptr && !UTPS->bOnlySpectator && !UTPS->bIsABot)
			{
				AwaitedFallback = UTPS;
			}
			continue;
		}
		if (!UTPS->bOnlySpectator && !UTPS->bIsABot && UTPS->UniqueId.IsValid())
		{
			return UTPS;
		}
		if (Fallback == nullptr)
		{
			Fallback = UTPS;
		}
	}
	return Fallback != nullptr ? Fallback : AwaitedFallback;
}

void ANCPlusCTFGameMode::BeginOrHoldAutoPause(const FString& LeaverId,
	const FString& LeaverName, const APlayerState* ExitingPlayerState,
	APlayerState* PreferredPauseMarker)
{
	AWorldSettings* WS = GetWorldSettings();
	if (!WS || LeaverId.IsEmpty()) return;

	// A player drop wins over every pending resume path, including a host/rcon
	// countdown that may have started before this became an automatic pause.
	NCPlusHostPause::CancelDeferredUnpause(this);
	AutoPauseAwaitIds.Add(LeaverId);

	// (Re)point the pause marker at a still-present player — never a leaver (their
	// PlayerState is torn down in Super::Logout). With no marker, retain the
	// logical pause + exact IDs so the first reconnect can restore the freeze.
	APlayerState* Marker = PreferredPauseMarker;
	AUTPlayerState* PreferredUTPS = Cast<AUTPlayerState>(PreferredPauseMarker);
	if (Marker == nullptr || Marker == ExitingPlayerState || Marker->IsPendingKillPending()
		|| (PreferredUTPS != nullptr && PreferredUTPS->bIsInactive))
	{
		Marker = FindAutoPauseMarker(ExitingPlayerState);
	}
	if (!Marker)
	{
		const bool bWasAutoPaused = bAutoPaused;
		bAutoPaused = true;
		bAutoPauseDormantNoMarker = true;
		const FString PauseReason = FString::Printf(
			TEXT("Waiting for %d disconnected player%s"),
			AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
		if (bAutoPauseResumeCountdownActive)
		{
			CancelAutoPauseResumeCountdown(PauseReason);
		}
		else
		{
			PublishAutoPausePaused(PauseReason);
		}
		if (WS->Pauser == ExitingPlayerState)
		{
			const bool bWasPhysicallyPaused = WS->Pauser != nullptr;
			Super::ClearPause();
			if (bWasPhysicallyPaused && WS->Pauser == nullptr)
			{
				WS->ForceNetUpdate();
				NCPlusHostPause::ResyncServerWorldTime(this);
			}
		}
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: no pause marker remains; preserving %d awaited ID%s%s"),
			AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"),
			bWasAutoPaused ? TEXT("") : TEXT(" (logical pause started)"));
		return;
	}

	WS->Pauser = Marker;   // engine world-pause; replicated, clients show paused
	WS->ForceNetUpdate();
	bAutoPauseDormantNoMarker = false;
	const bool bWasAutoPaused = bAutoPaused;
	bAutoPaused = true;
	const FString PauseReason = FString::Printf(TEXT("Waiting for %s to reconnect"), *LeaverName);
	if (bAutoPauseResumeCountdownActive)
	{
		CancelAutoPauseResumeCountdown(PauseReason);
	}
	else
	{
		PublishAutoPausePaused(PauseReason);
	}

	if (!bWasAutoPaused)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: %s dropped — match PAUSED until rejoin/manual resume. awaiting=%d"),
			*LeaverName, AutoPauseAwaitIds.Num());
	}
	else
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: %s also dropped while paused — awaiting=%d"),
			*LeaverName, AutoPauseAwaitIds.Num());
	}
}

void ANCPlusCTFGameMode::StartAutoPauseWatch()
{
	if (!HasAuthority() || !bAutoPauseOnDrop || !bIsPugMatch
		|| AutoPauseDetectTicker.IsValid())
	{
		return;
	}

	AutoPauseDetectTicker = FTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ANCPlusCTFGameMode::TickAutoPauseDetect),
		0.25f);
	UE_LOG(LogGameMode, Log,
		TEXT("NCPlusCTF auto-pause: connection watcher armed (detect %.1fs)"),
		PauseDropDetectSeconds);
}

void ANCPlusCTFGameMode::CaptureLockedReadyParticipants(
	const ANCReadyUpState* ReadyState)
{
	if (ReadyState == nullptr)
	{
		return;
	}
	for (AUTPlayerState* PS : ReadyState->ReadyPlayers)
	{
		if (PS != nullptr && !PS->IsPendingKillPending() && !PS->bIsInactive
			&& !PS->bIsABot && !PS->bOnlySpectator
			&& PS->UniqueId.IsValid() && PS->GetTeamNum() <= 1)
		{
			const FString Uid = PS->UniqueId.ToString();
			AutoPauseReadyCountdownIds.Add(Uid);
			// Follow an authoritative pre-live balance while the original ready
			// PlayerState is still present, then freeze the expected team on drop.
			if (!AutoPauseAwaitIds.Contains(Uid))
			{
				AutoPauseReadyCountdownTeams.Add(Uid, PS->GetTeamNum());
			}
		}
	}
}

void ANCPlusCTFGameMode::RefreshReadyParticipantTeamsFromActivePlayers()
{
	if (CTFGameState == nullptr || AutoPauseReadyCountdownIds.Num() == 0)
	{
		return;
	}

	struct FReadyTeamCandidate
	{
		uint8 Team = 255;
		int32 Count = 0;
		bool bAmbiguous = false;
	};
	TMap<FString, FReadyTeamCandidate> Candidates;
	for (APlayerState* BasePS : CTFGameState->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (PS == nullptr || PS->IsPendingKillPending() || PS->bIsInactive
			|| PS->bIsABot || PS->bOnlySpectator || !PS->UniqueId.IsValid()
			|| PS->GetTeamNum() > 1)
		{
			continue;
		}
		const FString Uid = PS->UniqueId.ToString();
		if (!AutoPauseReadyCountdownIds.Contains(Uid) || AutoPauseAwaitIds.Contains(Uid))
		{
			continue;
		}

		FReadyTeamCandidate& Candidate = Candidates.FindOrAdd(Uid);
		if (Candidate.Count == 0)
		{
			Candidate.Team = PS->GetTeamNum();
		}
		else if (Candidate.Team != PS->GetTeamNum())
		{
			Candidate.bAmbiguous = true;
		}
		++Candidate.Count;
	}

	for (const TPair<FString, FReadyTeamCandidate>& Pair : Candidates)
	{
		// Fast reconnects can briefly leave two PlayerStates. Only replace the
		// frozen value when every active participant copy agrees on the side.
		if (Pair.Value.Count > 0 && !Pair.Value.bAmbiguous)
		{
			AutoPauseReadyCountdownTeams.Add(Pair.Key, Pair.Value.Team);
		}
	}
}

void ANCPlusCTFGameMode::StopAutoPauseWatch()
{
	if (AutoPauseDetectTicker.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AutoPauseDetectTicker);
		AutoPauseDetectTicker.Reset();
	}
}

bool ANCPlusCTFGameMode::TickAutoPauseDetect(float /*DeltaTime*/)
{
	if (!HasAuthority() || !bAutoPauseOnDrop || !bIsPugMatch || CTFGameState == nullptr)
	{
		return true;
	}

	const bool bLockedReadyCountdown = IsLockedPlayerReadyCountdown(this);
	const bool bPauseEligiblePhase = CTFGameState->IsMatchInProgress()
		|| CTFGameState->IsMatchInOvertime()
		|| bLockedReadyCountdown;
	if (!bPauseEligiblePhase)
	{
		// An unlocked warmup is either pre-countdown or a cancelled countdown, so
		// discard its snapshot. Preserve it across halftime/intermission: a late
		// second-half drop still needs the original team check. Match end performs
		// terminal cleanup explicitly in HandleMatchHasEnded.
		if (GetMatchState() == MatchState::WaitingToStart)
		{
			AutoPauseReadyCountdownIds.Reset();
			AutoPauseReadyCountdownTeams.Reset();
		}
		return true;
	}

	UWorld* World = GetWorld();
	UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
	if (Driver == nullptr)
	{
		return true;
	}

	const double NetNow = Driver->Time;
	ANCReadyUpState* LockedReadyState = bLockedReadyCountdown
		? ANCReadyUpState::Find(World) : nullptr;
	CaptureLockedReadyParticipants(LockedReadyState);

	struct FObservedParticipantConnection
	{
		AUTPlayerState* Representative = nullptr;
		FString PlayerName;
		double BestSilenceSeconds = 0.0;
		bool bHasSample = false;
		bool bHealthy = false;
	};
	TMap<FString, FObservedParticipantConnection> ObservedConnections;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = Cast<APlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (PS == nullptr || PS->bIsInactive || PS->bIsABot || PS->bOnlySpectator
			|| !PS->UniqueId.IsValid() || PS->GetTeamNum() > 1)
		{
			continue;
		}

		// Null for a listen host's local player; only remote connections need a
		// silence check. NetDriver time continues while the game world is paused.
		UNetConnection* Connection = Cast<UNetConnection>(PC->Player);
		if (Connection == nullptr)
		{
			continue;
		}

		const FString Uid = PS->UniqueId.ToString();
		if (LockedReadyState != nullptr && !AutoPauseReadyCountdownIds.Contains(Uid)
			&& !AutoPauseAwaitIds.Contains(Uid))
		{
			continue;
		}
		const uint8* ExpectedTeam = AutoPauseReadyCountdownTeams.Find(Uid);
		if (ExpectedTeam == nullptr)
		{
			ExpectedTeam = PugRosterTeam.Find(Uid.ToLower());
		}
		if (ExpectedTeam != nullptr && *ExpectedTeam != PS->GetTeamNum())
		{
			continue;
		}

		const double SilenceSeconds = NetNow - Connection->LastReceiveTime;
		FObservedParticipantConnection& Observation = ObservedConnections.FindOrAdd(Uid);
		if (!Observation.bHasSample || SilenceSeconds < Observation.BestSilenceSeconds)
		{
			Observation.Representative = PS;
			Observation.PlayerName = PS->PlayerName;
			Observation.BestSilenceSeconds = SilenceSeconds;
			Observation.bHasSample = true;
		}
		// During a fast reconnect the old and new controllers can coexist. Treat the
		// ID as healthy if any qualifying connection is receiving traffic.
		Observation.bHealthy = Observation.bHealthy
			|| SilenceSeconds <= PauseDropDetectSeconds;
	}

	TArray<FString> RecoveredIds;
	TArray<FString> NewlySilentIds;
	for (const TPair<FString, FObservedParticipantConnection>& Pair : ObservedConnections)
	{
		if (Pair.Value.bHealthy)
		{
			if (AutoPauseAwaitIds.Contains(Pair.Key))
			{
				RecoveredIds.Add(Pair.Key);
			}
		}
		else if (!AutoPauseAwaitIds.Contains(Pair.Key))
		{
			NewlySilentIds.Add(Pair.Key);
		}
	}

	for (const FString& Uid : RecoveredIds)
	{
		const FObservedParticipantConnection* Observation = ObservedConnections.Find(Uid);
		AutoPauseAwaitIds.Remove(Uid);
		if (Observation != nullptr)
		{
			UE_LOG(LogGameMode, Warning,
				TEXT("NCPlusCTF auto-pause: %s connection recovered (%d still out)"),
				*Observation->PlayerName, AutoPauseAwaitIds.Num());
		}
	}

	// Add the full silent set first so marker selection cannot choose another
	// connection that this same observation pass already knows is dead.
	for (const FString& Uid : NewlySilentIds)
	{
		AutoPauseAwaitIds.Add(Uid);
	}
	for (const FString& Uid : NewlySilentIds)
	{
		const FObservedParticipantConnection* Observation = ObservedConnections.Find(Uid);
		if (Observation == nullptr)
		{
			continue;
		}
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: %s silent %.1fs - pausing before Logout"),
			*Observation->PlayerName, Observation->BestSilenceSeconds);
		// The silent controller is still alive here and can safely hold Pauser until
		// Logout rehomes it. This guarantees world time freezes even if it is the
		// last remaining marker.
		BeginOrHoldAutoPause(Uid, Observation->PlayerName, nullptr,
			Observation->Representative);
	}

	if (NewlySilentIds.Num() == 0 && RecoveredIds.Num() > 0 && bAutoPaused)
	{
		if (AutoPauseAwaitIds.Num() == 0)
		{
			BeginAutoPauseResumeCountdown(TEXT("All disconnected players returned"));
		}
		else
		{
			const FString WaitingReason = FString::Printf(
				TEXT("Waiting for %d disconnected player%s"),
				AutoPauseAwaitIds.Num(), AutoPauseAwaitIds.Num() == 1 ? TEXT("") : TEXT("s"));
			if (bAutoPauseResumeCountdownActive)
			{
				CancelAutoPauseResumeCountdown(WaitingReason);
			}
			else
			{
				PublishAutoPausePaused(WaitingReason);
			}
		}
	}
	return true;
}

TArray<FString> ANCPlusCTFGameMode::GetSortedAutoPauseAwaitIds() const
{
	TArray<FString> Result;
	Result.Reserve(AutoPauseAwaitIds.Num());
	for (const FString& Id : AutoPauseAwaitIds)
	{
		Result.Add(Id);
	}
	Result.Sort();
	return Result;
}

ANCAutoPauseState* ANCPlusCTFGameMode::GetOrCreateAutoPauseState()
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || World == nullptr)
	{
		return nullptr;
	}

	if (AutoPauseStateActor != nullptr && AutoPauseStateActor->GetWorld() == World)
	{
		return AutoPauseStateActor;
	}

	if (ANCAutoPauseState* Existing = ANCAutoPauseState::Find(World))
	{
		AutoPauseStateActor = Existing;
		return AutoPauseStateActor;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = GameState;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AutoPauseStateActor = World->SpawnActor<ANCAutoPauseState>(SpawnParams);
	return AutoPauseStateActor;
}

void ANCPlusCTFGameMode::PublishAutoPausePaused(const FString& Reason)
{
	if (ANCAutoPauseState* State = GetOrCreateAutoPauseState())
	{
		State->SetPaused(Reason, GetSortedAutoPauseAwaitIds());
	}
}

void ANCPlusCTFGameMode::BeginAutoPauseResumeCountdown(const FString& Reason)
{
	if (!HasAuthority() || !bAutoPaused)
	{
		return;
	}
	if (bAutoPauseResumeCountdownActive)
	{
		if (ANCAutoPauseState* State = GetOrCreateAutoPauseState())
		{
			State->UpdateResumeCountdown(AutoPauseResumeSecondsRemaining,
				GetSortedAutoPauseAwaitIds());
		}
		return;
	}

	const int32 Duration = FMath::Clamp(AutoPauseResumeCountdownSec, 0, 60);
	if (Duration <= 0)
	{
		CompleteAutoPauseResume(Reason);
		return;
	}

	bAutoPauseResumeCountdownActive = true;
	AutoPauseResumeSecondsRemaining = Duration;
	AutoPauseResumeEndRealTime = GetWorld()->GetRealTimeSeconds() + float(Duration);
	if (ANCAutoPauseState* State = GetOrCreateAutoPauseState())
	{
		State->BeginResumeCountdown(Reason, Duration, GetSortedAutoPauseAwaitIds());
	}

	AutoPauseResumeTicker = FTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ANCPlusCTFGameMode::TickAutoPauseResume), 0.1f);
	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF auto-pause: resume countdown started (%ds, %s)"), Duration, *Reason);
}

void ANCPlusCTFGameMode::CancelAutoPauseResumeCountdown(const FString& Reason)
{
	if (AutoPauseResumeTicker.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AutoPauseResumeTicker);
		AutoPauseResumeTicker.Reset();
	}
	bAutoPauseResumeCountdownActive = false;
	AutoPauseResumeSecondsRemaining = 0;
	AutoPauseResumeEndRealTime = 0.0f;
	PublishAutoPausePaused(Reason);
	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF auto-pause: resume countdown cancelled (%s)"), *Reason);
}

bool ANCPlusCTFGameMode::TickAutoPauseResume(float /*DeltaTime*/)
{
	if (!HasAuthority() || !bAutoPaused || GetWorld() == nullptr)
	{
		AutoPauseResumeTicker.Reset();
		bAutoPauseResumeCountdownActive = false;
		AutoPauseResumeSecondsRemaining = 0;
		AutoPauseResumeEndRealTime = 0.0f;
		return false;
	}

	const int32 NewRemaining = FMath::Max(0, FMath::CeilToInt(
		AutoPauseResumeEndRealTime - GetWorld()->GetRealTimeSeconds()));
	if (NewRemaining != AutoPauseResumeSecondsRemaining)
	{
		AutoPauseResumeSecondsRemaining = NewRemaining;
		if (ANCAutoPauseState* State = GetOrCreateAutoPauseState())
		{
			State->UpdateResumeCountdown(AutoPauseResumeSecondsRemaining,
				GetSortedAutoPauseAwaitIds());
		}
	}
	if (AutoPauseResumeSecondsRemaining > 0)
	{
		return true;
	}

	AutoPauseResumeTicker.Reset();
	bAutoPauseResumeCountdownActive = false;
	AutoPauseResumeEndRealTime = 0.0f;
	CompleteAutoPauseResume(TEXT("Resume countdown completed"));
	return false;
}

void ANCPlusCTFGameMode::CompleteAutoPauseResume(const FString& Reason)
{
	if (AutoPauseResumeTicker.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AutoPauseResumeTicker);
		AutoPauseResumeTicker.Reset();
	}
	bAutoPauseResumeCountdownActive = false;
	AutoPauseResumeSecondsRemaining = 0;
	AutoPauseResumeEndRealTime = 0.0f;

	AWorldSettings* WS = GetWorldSettings();
	const bool bWasPaused = WS != nullptr && WS->Pauser != nullptr;
	// Qualified Super call is an explicit clear (never a toggle). It also removes
	// stock Pausers delegates if a manual pause overlapped the automatic pause.
	Super::ClearPause();
	const bool bDidUnpause = bWasPaused && WS != nullptr && WS->Pauser == nullptr;
	if (bDidUnpause)
	{
		WS->ForceNetUpdate();
		NCPlusHostPause::ResyncServerWorldTime(this);
	}
	bAutoPaused = false;
	bAutoPauseDormantNoMarker = false;
	AutoPauseAwaitIds.Reset();
	if (ANCAutoPauseState* State = GetOrCreateAutoPauseState())
	{
		State->SetInactive(Reason);
	}
	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF auto-pause: complete (%s, worldUnpaused=%s)"),
		*Reason, bDidUnpause ? TEXT("true") : TEXT("false"));
}

void ANCPlusCTFGameMode::CapturePlayerStats(AUTPlayerState* UTPS, FNCPlusCTFPlayerInput& Out) const
{
	Out.UniqueId   = UTPS->UniqueId.ToString();
	Out.PlayerName = UTPS->PlayerName;
	Out.TeamIndex  = UTPS->GetTeamNum();
	Out.Kills      = UTPS->Kills;
	Out.Deaths     = UTPS->Deaths;
	Out.Damage     = static_cast<int32>(UTPS->DamageDone);

	// CTF objective signals. Caps/Returns/Assists are direct replicated fields;
	// the rest live in the server-side StatsData TMap — same GetStatsValue path
	// CTFStatsReplicator already uses for FlagGrabs.
	Out.Caps          = UTPS->FlagCaptures;
	Out.Returns       = UTPS->FlagReturns;
	Out.Assists       = UTPS->Assists;
	Out.FCKills       = static_cast<int32>(UTPS->GetStatsValue(NAME_FCKills));
	Out.SupportKills  = static_cast<int32>(UTPS->GetStatsValue(NAME_FlagSupportKills));
	Out.Grabs         = static_cast<int32>(UTPS->GetStatsValue(NAME_FlagGrabs));
	Out.CarryAssists  = static_cast<int32>(UTPS->GetStatsValue(NAME_CarryAssist));
	Out.EnemyFCDamage = static_cast<int32>(UTPS->GetStatsValue(NAME_EnemyFCDamage));

	// Possession + denial signals (previously captured by the engine but never fed
	// to ranking). FlagHeldTime = offensive carry/possession seconds; FlagDenials =
	// clutch save (enemy carrier killed near the would-be-cap base); FlagHeldDeny
	// [Time] = the both-flags-out hold. Same StatsData path as the objectives above.
	Out.CarryTime    = UTPS->GetStatsValue(NAME_FlagHeldTime);
	Out.Denials      = static_cast<int32>(UTPS->GetStatsValue(NAME_FlagDenials));
	Out.HeldDeny     = static_cast<int32>(UTPS->GetStatsValue(NAME_FlagHeldDeny));
	Out.HeldDenyTime = UTPS->GetStatsValue(NAME_FlagHeldDenyTime);

	// Resolve positional role from the dwell accumulated by SampleRoleDwell (1Hz).
	// OffLean = EnemyFrac - OwnFrac (-1 pure defense .. +1 pure offense); the mid
	// bucket doesn't shift it. Fractions + label are observability-only (payload).
	// No samples (e.g. never spawned) -> all 0 -> neutral role weights in perf.
	if (const FNCPlusCTFRoleDwell* D = RoleDwell.Find(Out.UniqueId))
	{
		const float Total = D->OwnSec + D->MidSec + D->EnemySec;
		if (Total > 0.f)
		{
			Out.OwnFrac   = D->OwnSec   / Total;
			Out.MidFrac   = D->MidSec   / Total;
			Out.EnemyFrac = D->EnemySec / Total;
			Out.OffLean   = Out.EnemyFrac - Out.OwnFrac;

			if      (Out.OffLean >=  0.34f)        { Out.Role = TEXT("offense"); }
			else if (Out.OffLean <= -0.34f)        { Out.Role = TEXT("defense"); }
			else if (D->CoverSec > D->FallbackSec) { Out.Role = TEXT("mid-cover"); }
			else if (D->FallbackSec > D->CoverSec) { Out.Role = TEXT("mid-fallback"); }
			else                                   { Out.Role = TEXT("mid"); }
		}
	}
}

void ANCPlusCTFGameMode::SampleRoleDwell()
{
	// 1Hz presence sampler for role-aware perf: one second per living player,
	// bucketed by map zone (CreditRoleDwell). Combat locations are credited
	// separately, at higher weight, from ScoreKill.
	AUTCTFGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (!GS || !(GS->IsMatchInProgress() || GS->IsMatchInOvertime()))
	{
		return;
	}
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		APawn* Pawn = C ? C->GetPawn() : nullptr;
		AUTPlayerState* PS = C ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
		if (Pawn && PS && !PS->bOnlySpectator)
		{
			CreditRoleDwell(PS, Pawn->GetActorLocation(), 1.f);   // one second of presence
		}
	}
}

void ANCPlusCTFGameMode::CreditRoleDwell(AUTPlayerState* PS, const FVector& Loc, float Weight)
{
	// Project Loc onto the flag-base axis t = dOwn/(dOwn+dEnemy) and add Weight to
	// the own(<0.4) / mid / enemy(>=0.6) bucket. Cover/fallback refine the mid
	// LABEL only (not OffLean): enemy-half while we hold their flag vs own-half
	// while our flag is out. Keyed by UniqueId to match CapturePlayerStats.
	if (!PS || !PS->UniqueId.IsValid() || Weight <= 0.f)
	{
		return;
	}
	const int32 Team = PS->GetTeamNum();
	if (Team != 0 && Team != 1)
	{
		return;
	}
	AUTCTFGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (!GS)
	{
		return;
	}
	AUTCTFFlagBase* OwnBase   = GS->GetFlagBase((uint8)Team);
	AUTCTFFlagBase* EnemyBase = GS->GetFlagBase((uint8)(1 - Team));
	if (!IsValid(OwnBase) || !IsValid(EnemyBase))
	{
		return;
	}
	const float dOwn   = (Loc - OwnBase->GetActorLocation()).Size();
	const float dEnemy = (Loc - EnemyBase->GetActorLocation()).Size();
	const float Denom  = dOwn + dEnemy;
	if (Denom <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const float t = dOwn / Denom;   // 0 = at own base, 1 = at enemy base

	FNCPlusCTFRoleDwell& D = RoleDwell.FindOrAdd(PS->UniqueId.ToString());
	if (t < 0.40f)
	{
		D.OwnSec += Weight;
		if (GS->GetFlagState((uint8)Team) != CarriedObjectState::Home)
		{
			D.FallbackSec += Weight;
		}
	}
	else if (t >= 0.60f)
	{
		D.EnemySec += Weight;
		if (GS->GetFlagState((uint8)(1 - Team)) == CarriedObjectState::Held)
		{
			D.CoverSec += Weight;
		}
	}
	else
	{
		D.MidSec += Weight;
	}
}

void ANCPlusCTFGameMode::ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	// Role-aware ratings: a fight is a far stronger role signal than idle presence,
	// so credit combat locations into role dwell at CTFRoleCombatWeight (vs 1.0 per
	// presence-second) — the victim where they died, the killer where they fought.
	// Captures aggressive offense even when idle enemy-half time is tiny (a runner
	// who grabs, caps, dies fast). Read locations BEFORE Super (it runs the engine
	// death/respawn path). Super does ALL scoring; this only touches rating dwell.
	if (CTFPerfConfig.bRoleAware && CTFRoleCombatWeight > 0.f && CTFGameState
		&& (CTFGameState->IsMatchInProgress() || CTFGameState->IsMatchInOvertime()))
	{
		if (AUTPlayerState* VictimPS = Other ? Cast<AUTPlayerState>(Other->PlayerState) : nullptr)
		{
			if (KilledPawn)
			{
				CreditRoleDwell(VictimPS, KilledPawn->GetActorLocation(), CTFRoleCombatWeight);
			}
		}
		if (Killer && Killer != Other)
		{
			AUTPlayerState* KillerPS = Cast<AUTPlayerState>(Killer->PlayerState);
			APawn* KillerPawn = Killer->GetPawn();
			if (KillerPS && KillerPawn)
			{
				CreditRoleDwell(KillerPS, KillerPawn->GetActorLocation(), CTFRoleCombatWeight);
			}
		}
	}

	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);
}

void ANCPlusCTFGameMode::LoadCTFPerfConfig()
{
	AutoPauseResumeCountdownSec = NCPlusHostPause::GetUnpauseCountdownSeconds();

	// Members already hold defaults; only override what Mod.ini [UTPUGS_STATS]
	// specifies (same section + load pattern as NCEloUploader).
	const FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");
	if (!FPaths::FileExists(ModIniPath))
	{
		UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF perf: Mod.ini not found, using defaults"));
		return;
	}

	FConfigFile ModIni;
	ModIni.Read(ModIniPath);
	const FConfigSection* Section = ModIni.Find(TEXT("UTPUGS_STATS"));
	if (!Section)
	{
		return;
	}

	auto ReadBool = [Section](const TCHAR* Key, bool& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = V->GetValue().ToBool(); }
	};
	auto ReadDouble = [Section](const TCHAR* Key, double& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = FCString::Atod(*V->GetValue()); }
	};
	auto ReadFloat = [Section](const TCHAR* Key, float& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = FCString::Atof(*V->GetValue()); }
	};
	auto ReadInt = [Section](const TCHAR* Key, int32& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = FCString::Atoi(*V->GetValue()); }
	};

	ReadBool(TEXT("CTFPerfEnabled"),           CTFPerfConfig.bEnabled);
	ReadBool(TEXT("CTFRatingShadow"),          CTFPerfConfig.bShadow);
	ReadDouble(TEXT("CTFPerfObjectiveWeight"), CTFPerfConfig.ObjectiveWeight);
	ReadDouble(TEXT("CTFFlagFeederPenalty"),   CTFPerfConfig.FeederPenalty);
	ReadFloat(TEXT("CTFRatingMinPresenceFrac"),CTFRatingMinPresenceFrac);
	ReadBool(TEXT("CTFRoleAware"),             CTFPerfConfig.bRoleAware);
	ReadDouble(TEXT("CTFRoleWeightStrength"),  CTFPerfConfig.RoleWeightStrength);
	ReadFloat(TEXT("CTFRoleCombatWeight"),     CTFRoleCombatWeight);
	ReadFloat(TEXT("CTFRespawnWait"),          CTFRespawnWait);
	ReadFloat(TEXT("CTFRespawnWaitSmall"),     CTFRespawnWaitSmall);
	ReadInt(TEXT("CTFSmallGameMaxPlayers"),    CTFSmallGameMaxPlayers);
	ReadBool(TEXT("AutoPauseOnDrop"),          bAutoPauseOnDrop);
	ReadFloat(TEXT("PauseDropDetectSeconds"),  PauseDropDetectSeconds);
	PauseDropDetectSeconds = FMath::Clamp(PauseDropDetectSeconds, 1.0f, 30.0f);

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF perf config: enabled=%s shadow=%s objW=%.2f feeder=%.2f minPresence=%.2f roleAware=%s roleStr=%.2f combatW=%.1f respawn=%.2f autoPause=%s pauseDetect=%.1fs resumeCountdown=%ds"),
		CTFPerfConfig.bEnabled ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.bShadow ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.ObjectiveWeight, CTFPerfConfig.FeederPenalty, CTFRatingMinPresenceFrac,
		CTFPerfConfig.bRoleAware ? TEXT("true") : TEXT("false"), CTFPerfConfig.RoleWeightStrength, CTFRoleCombatWeight, CTFRespawnWait,
		bAutoPauseOnDrop ? TEXT("true") : TEXT("false"), PauseDropDetectSeconds,
		AutoPauseResumeCountdownSec);
}

void ANCPlusCTFGameMode::LoadSpawnConfig()
{
	// Same Mod.ini load pattern as LoadCTFPerfConfig, section [UTPUGS_SPAWN].
	// Members already hold ctor defaults; only override what the ini specifies.
	const FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");
	if (!FPaths::FileExists(ModIniPath))
	{
		return;
	}

	FConfigFile ModIni;
	ModIni.Read(ModIniPath);
	const FConfigSection* Section = ModIni.Find(TEXT("UTPUGS_SPAWN"));
	if (!Section)
	{
		return;
	}

	auto ReadFloat = [Section](const TCHAR* Key, float& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = FCString::Atof(*V->GetValue()); }
	};
	auto ReadBool = [Section](const TCHAR* Key, bool& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = V->GetValue().ToBool(); }
	};
	auto ReadInt = [Section](const TCHAR* Key, int32& Out)
	{
		if (const FConfigValue* V = Section->Find(FName(Key))) { Out = FCString::Atoi(*V->GetValue()); }
	};

	// Penalty weights (the side-clustering knobs — soften these to let mid back in).
	ReadFloat(TEXT("FlagCarrierSpawnPenalty"), FlagCarrierSpawnPenalty);
	ReadFloat(TEXT("DroppedFlagSpawnPenalty"), DroppedFlagSpawnPenalty);
	ReadFloat(TEXT("FlagCarrierLOSPenalty"),   FlagCarrierLOSPenalty);
	ReadFloat(TEXT("EnemyBlockRange"),         EnemyBlockRange);
	ReadFloat(TEXT("EnemyBlockPenalty"),       EnemyBlockPenalty);
	ReadFloat(TEXT("EnemyLOSBlockRange"),      EnemyLOSBlockRange);
	ReadFloat(TEXT("EnemyLOSPenalty"),         EnemyLOSPenalty);
	ReadFloat(TEXT("FlagBaseProximityRadius"), FlagBaseProximityRadius);
	ReadFloat(TEXT("FlagSpawnPenaltyRadius"),  FlagSpawnPenaltyRadius);
	ReadFloat(TEXT("SpawnRecentPenaltyMultiplier"), SpawnRecentPenaltyMultiplier);
	ReadFloat(TEXT("SpawnNearLastRadius"),     SpawnNearLastRadius);
	ReadFloat(TEXT("SpawnNearLastPenalty"),    SpawnNearLastPenalty);

	// Selection knobs (tie-band + freshness + killer-avoid).
	ReadBool(TEXT("SpawnWeightedRandom"),      bSpawnWeightedRandom);
	ReadFloat(TEXT("SpawnRandomBase"),         SpawnRandomBase);
	ReadFloat(TEXT("SpawnRandomSpread"),       SpawnRandomSpread);
	const float ConfiguredSpawnRandomBase = SpawnRandomBase;
	const float ConfiguredSpawnRandomSpread = SpawnRandomSpread;
	SpawnRandomBase = FMath::IsFinite(SpawnRandomBase)
		? FMath::Max(0.f, SpawnRandomBase)
		: 20.f;
	SpawnRandomSpread = FMath::IsFinite(SpawnRandomSpread)
		? FMath::Max(0.f, SpawnRandomSpread)
		: 1.f;
	if (bSpawnWeightedRandom && SpawnRandomSpread <= 0.f)
	{
		// A zero/negative spread either removes score weighting or reverses it.
		// Weighted mode must retain a positive score-to-ceiling relationship.
		SpawnRandomSpread = 1.f;
	}
	if (SpawnRandomBase != ConfiguredSpawnRandomBase
		|| SpawnRandomSpread != ConfiguredSpawnRandomSpread)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF spawn config corrected: SpawnRandomBase %.2f -> %.2f, SpawnRandomSpread %.2f -> %.2f"),
			ConfiguredSpawnRandomBase, SpawnRandomBase,
			ConfiguredSpawnRandomSpread, SpawnRandomSpread);
	}
	ReadFloat(TEXT("SpawnEnemyHardRadius"),    SpawnEnemyHardRadius);
	ReadFloat(TEXT("SpawnEnemyBelowZ"),        SpawnEnemyBelowZ);
	ReadBool(TEXT("SpawnUseNewCTFSelection"), bSpawnUseNewCTFSelection);
	ReadInt(TEXT("SpawnSystemThreshold"), SpawnSystemThreshold);
	ReadFloat(TEXT("SpawnFriendlyBlockRange"), SpawnFriendlyBlockRange);
	ReadFloat(TEXT("SpawnFriendlyVisionBlockRange"), SpawnFriendlyVisionBlockRange);
	ReadFloat(TEXT("SpawnFlagBlockRange"), SpawnFlagBlockRange);
	ReadInt(TEXT("SpawnMinCycleDistance"), SpawnMinCycleDistance);
	ReadBool(TEXT("SpawnExtrapolateMovement"), bSpawnExtrapolateMovement);
	ReadBool(TEXT("SpawnSecondaryEnabled"), bSpawnSecondaryEnabled);
	ReadFloat(TEXT("SpawnSecondaryMaxDistance"), SpawnSecondaryMaxDistance);
	ReadFloat(TEXT("SpawnSecondaryOwnTeamWeight"), SpawnSecondaryOwnTeamWeight);
	ReadFloat(TEXT("SpawnSecondaryCarrierWeight"), SpawnSecondaryCarrierWeight);
	ReadFloat(TEXT("SpawnTieBandWidth"),       SpawnTieBandWidth);
	ReadFloat(TEXT("SpawnFreshnessBonus"),     SpawnFreshnessBonus);
	ReadFloat(TEXT("SpawnFreshnessWindow"),    SpawnFreshnessWindow);
	ReadFloat(TEXT("SpawnFlagVicinityRadius"), SpawnFlagVicinityRadius);
	ReadFloat(TEXT("SpawnKillerAvoidRadius"),  SpawnKillerAvoidRadius);
	ReadFloat(TEXT("SpawnFlagCarrierLOSAvoidRadius"), SpawnFlagCarrierLOSAvoidRadius);
	ReadFloat(TEXT("SpawnRobbedBaseAvoidCount"), SpawnRobbedBaseAvoidCount);
	ReadBool(TEXT("LogSpawnChoices"), bLogSpawnChoices);

	SpawnSystemThreshold = FMath::Max(0, SpawnSystemThreshold);
	SpawnFriendlyBlockRange = FMath::IsFinite(SpawnFriendlyBlockRange) ? FMath::Max(0.f, SpawnFriendlyBlockRange) : 150.f;
	SpawnFriendlyVisionBlockRange = FMath::IsFinite(SpawnFriendlyVisionBlockRange) ? FMath::Max(0.f, SpawnFriendlyVisionBlockRange) : 150.f;
	SpawnFlagBlockRange = FMath::IsFinite(SpawnFlagBlockRange) ? FMath::Max(0.f, SpawnFlagBlockRange) : 750.f;
	SpawnMinCycleDistance = FMath::Max(0, SpawnMinCycleDistance);
	SpawnSecondaryMaxDistance = FMath::IsFinite(SpawnSecondaryMaxDistance) ? FMath::Max(1.f, SpawnSecondaryMaxDistance) : 2000.f;
	SpawnSecondaryOwnTeamWeight = FMath::IsFinite(SpawnSecondaryOwnTeamWeight) ? FMath::Max(0.f, SpawnSecondaryOwnTeamWeight) : 0.2f;
	SpawnSecondaryCarrierWeight = FMath::IsFinite(SpawnSecondaryCarrierWeight) ? FMath::Max(0.f, SpawnSecondaryCarrierWeight) : 2.f;

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF spawn config [UTPUGS_SPAWN]: newctf=%s threshold=%d enemy=%.0f enemyLOS=%.0f friendly=%.0f/%.0f flag=%.0f cycle=%d extrap=%s secondary=%s max=%.0f weights=%.2f/%.2f | legacy=%s randBase=%.1f randSpread=%.2f tieBand=%.1f freshBonus=%.1f freshWin=%.0f killerAvoid=%.0f efcLOSAvoid=%.0f robbedAvoid=%.0f enemyBelowZ=%.0f logChoices=%s"),
		bSpawnUseNewCTFSelection ? TEXT("true") : TEXT("false"), SpawnSystemThreshold,
		SpawnEnemyHardRadius, EnemyLOSBlockRange, SpawnFriendlyBlockRange, SpawnFriendlyVisionBlockRange,
		SpawnFlagBlockRange, SpawnMinCycleDistance, bSpawnExtrapolateMovement ? TEXT("true") : TEXT("false"),
		bSpawnSecondaryEnabled ? TEXT("true") : TEXT("false"), SpawnSecondaryMaxDistance,
		SpawnSecondaryOwnTeamWeight, SpawnSecondaryCarrierWeight,
		bSpawnWeightedRandom ? TEXT("weighted-random") : TEXT("tie-band"), SpawnRandomBase, SpawnRandomSpread,
		SpawnTieBandWidth, SpawnFreshnessBonus, SpawnFreshnessWindow, SpawnKillerAvoidRadius, SpawnFlagCarrierLOSAvoidRadius, SpawnRobbedBaseAvoidCount,
		SpawnEnemyBelowZ,
		bLogSpawnChoices ? TEXT("true") : TEXT("false"));
	if (!bSpawnUseNewCTFSelection)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF legacy spawn config: flagCarrier=%.1f droppedFlag=%.1f carrierLOS=%.1f enemy=%.1f/%.0f enemyLOS=%.1f/%.0f flagBase=%.0f flagRadius=%.0f recent=%.2f nearLast=%.1f/%.0f flagVicinity=%.0f"),
			FlagCarrierSpawnPenalty, DroppedFlagSpawnPenalty, FlagCarrierLOSPenalty,
			EnemyBlockPenalty, EnemyBlockRange, EnemyLOSPenalty, EnemyLOSBlockRange,
			FlagBaseProximityRadius, FlagSpawnPenaltyRadius, SpawnRecentPenaltyMultiplier,
			SpawnNearLastPenalty, SpawnNearLastRadius, SpawnFlagVicinityRadius);
	}
}

bool ANCPlusCTFGameMode::IsFlagNearOwnBase(uint8 TeamIndex) const
{
	AUTCTFGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (!GS)
	{
		return false;
	}
	AUTCTFFlagBase* OwnBase = GS->GetFlagBase(TeamIndex);
	if (!IsValid(OwnBase))
	{
		return false;
	}
	const FVector OwnBaseLoc = OwnBase->GetActorLocation();

	// Any non-home flag (enemy carrier or a dropped flag) close to our base = pressure.
	for (uint8 t = 0; t < 2; ++t)
	{
		AUTCTFFlagBase* FB = GS->GetFlagBase(t);
		if (!IsValid(FB) || !IsValid(FB->MyFlag))
		{
			continue;
		}
		const FName State = GS->GetFlagState(t);
		if (State == CarriedObjectState::Home)
		{
			continue;
		}
		const FVector FlagLoc = (State == CarriedObjectState::Held && IsValid(FB->MyFlag->HoldingPawn))
			? FB->MyFlag->HoldingPawn->GetActorLocation()
			: FB->MyFlag->GetActorLocation();
		if ((FlagLoc - OwnBaseLoc).Size() < SpawnFlagVicinityRadius)
		{
			return true;
		}
	}
	return false;
}

bool ANCPlusCTFGameMode::SupportsInstantReplay() const
{
	return true;
}

bool ANCPlusCTFGameMode::ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass)
{
	// Force the player's chosen hat as an OverrideHatClass (NOT entitlement-checked) on the NEXT tick —
	// after ServerReceiveHatClass runs ValidateEntitlements + strips the un-entitled cosmetic — so the
	// override is the final word. The community master grants base + map entitlements but withholds
	// cosmetic ones, which is what trips the strip; OverrideHatClass sidesteps it. Server-side; never kicks.
	// The log reveals whether the class actually LOADED: NULL there = content/pak problem (not the strip),
	// and no override can conjure a class that won't load.
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

// ── Spawn Lifecycle ─────────────────────────────────────────────────
bool ANCPlusCTFGameMode::IsLiveSpawnCommitState() const
{
	const FName State = GetMatchState();
	return State == MatchState::InProgress
		|| State == MatchState::MatchIsInOvertime
		|| State == MatchState::MatchExitingIntermission;
}

void ANCPlusCTFGameMode::ClearCachedRespawnChoices()
{
	if (!CTFGameState)
	{
		return;
	}
	for (APlayerState* BasePS : CTFGameState->PlayerArray)
	{
		if (AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS))
		{
			// These are engine-owned UPROPERTY pointers, so use reflected offsets
			// across the plugin/engine DLL boundary rather than direct field writes.
			NCPlusReflection::SetObject(PS, TEXT("RespawnChoiceA"), nullptr);
			NCPlusReflection::SetObject(PS, TEXT("RespawnChoiceB"), nullptr);
		}
	}
}

void ANCPlusCTFGameMode::RestartPlayerAtPlayerStart(AController* NewPlayer, AActor* StartSpot)
{
	APawn* PawnBeforeRestart = NewPlayer ? NewPlayer->GetPawn() : nullptr;
	Super::RestartPlayerAtPlayerStart(NewPlayer, StartSpot);

	// UE4's normal respawn path passes the selected start as a local variable but
	// never publishes it back to Controller::StartSpot. Only commit after Super
	// actually produced a pawn; preview choices and failed spawns must not advance
	// the team queue or recent-use history.
	APlayerStart* UsedStart = Cast<APlayerStart>(StartSpot);
	if (NewPlayer && NewPlayer->GetPawn() && NewPlayer->GetPawn() != PawnBeforeRestart && UsedStart)
	{
		if (IsLiveSpawnCommitState())
		{
			NewPlayer->StartSpot = UsedStart;
			CommitUsedSpawn(NewPlayer, UsedStart);
		}
	}
}

void ANCPlusCTFGameMode::CommitUsedSpawn(AController* Player, APlayerStart* UsedStart)
{
	if (!Player || !UsedStart || !GetWorld())
	{
		return;
	}

	const TWeakObjectPtr<AController> PlayerKey(Player);
	FRecentSpawns& Recent = PlayerRecentSpawns.FindOrAdd(PlayerKey);
	Recent.ThirdLast = Recent.SecondLast;
	Recent.SecondLast = Recent.Last;
	Recent.Last = UsedStart;
	SpawnLastUsedTime.Add(UsedStart, GetWorld()->GetTimeSeconds());

	if (!bSpawnUseNewCTFSelection)
	{
		return;
	}

	AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
	if (!PS || !PS->Team || PS->Team->TeamIndex > 1)
	{
		return;
	}

	const int32 TeamIndex = int32(PS->Team->TeamIndex);
	TArray<TWeakObjectPtr<APlayerStart>>& Pool = (TeamIndex == 0) ? Team0Spawns : Team1Spawns;
	const int32 UsedIndex = Pool.IndexOfByPredicate([UsedStart](const TWeakObjectPtr<APlayerStart>& Entry)
	{
		return Entry.Get() == UsedStart;
	});
	if (UsedIndex != INDEX_NONE)
	{
		const TWeakObjectPtr<APlayerStart> UsedEntry = Pool[UsedIndex];
		Pool.RemoveAt(UsedIndex, 1, false);
		Pool.Add(UsedEntry);
	}

	// Preserve NCP's robbed-base rotation, but advance it only for a real spawn.
	// ChoosePlayerStart is also called for previews, which must remain side-effect free.
	if (CTFGameState && CTFGameState->GetFlagState(uint8(TeamIndex)) != CarriedObjectState::Home)
	{
		++RobbedSpawnRotation[TeamIndex];
	}
}

void ANCPlusCTFGameMode::RestartPlayer(AController* NewPlayer)
{
	// Idempotency guard: a RestartPlayer on a controller that ALREADY has a living pawn
	// must be a no-op. The engine reuses the existing pawn but still re-runs
	// SetPlayerDefaults -> GiveDefaultInventory, and stock AddInventory dedupes only by
	// instance (not class), so a second warmup RestartPlayer would grant a second copy of
	// every default weapon = the doubled weapon bar. First spawn (no pawn) is unaffected;
	// RestartPlayerAtPlayerStart records the real start only when a spawn succeeds.
	if (NewPlayer && NewPlayer->GetPawn())
	{
		return;
	}

	Super::RestartPlayer(NewPlayer);

	// Optional spawn diagnostics — gated to LIVE match and reporting the PAWN's
	// actual world location. Warning verbosity survives Shipping when an admin opts in.
	APawn* SpawnedPawnForLog = NewPlayer ? NewPlayer->GetPawn() : nullptr;
	if (bLogSpawnChoices && SpawnedPawnForLog && CTFGameState && IsLiveSpawnCommitState())
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
		const int32 TeamIdx = (PS && PS->Team) ? int32(PS->Team->TeamIndex) : -1;
		const FString OwnFlag   = (TeamIdx >= 0) ? CTFGameState->GetFlagState(TeamIdx).ToString()     : TEXT("?");
		const FString EnemyFlag = (TeamIdx >= 0) ? CTFGameState->GetFlagState(1 - TeamIdx).ToString() : TEXT("?");

		const FVector SpawnLoc = SpawnedPawnForLog->GetActorLocation();
		FVector& LastLoc = PlayerLastSpawnLoc.FindOrAdd(TWeakObjectPtr<AController>(NewPlayer));
		const int32 DistFromLast = LastLoc.IsZero() ? -1 : FMath::RoundToInt((SpawnLoc - LastLoc).Size());
		const TCHAR* RecentTag = (DistFromLast < 0) ? TEXT("fresh") : (DistFromLast < 250 ? TEXT("same") : TEXT("moved"));
		LastLoc = SpawnLoc;

		APlayerStart* SS = Cast<APlayerStart>(NewPlayer->StartSpot.Get());
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF spawn: %s(T%d) pawn=(%.0f,%.0f) recent=%s dist_from_last=%d | StartSpot=%s | own_flag=%s enemy_flag=%s"),
			PS ? *PS->PlayerName : TEXT("?"), TeamIdx,
			SpawnLoc.X, SpawnLoc.Y, RecentTag, DistFromLast,
			SS ? *SS->GetName() : TEXT("(none)"),
			*OwnFlag, *EnemyFlag);
	}

	// Ping-compensated spawn: hide pawn until client confirms control.
	// Skip bots (no remote client to confirm).
	ATeamArenaCharacter* SpawnedChar = (NewPlayer && NewPlayer->GetPawn()) ? Cast<ATeamArenaCharacter>(NewPlayer->GetPawn()) : nullptr;
	if (bEnablePingCompensatedSpawn && SpawnedChar && NewPlayer->GetPawn()->GetRemoteRole() == ROLE_AutonomousProxy)
	{
		SpawnedChar->BeginPingCompensatedSpawnHide();   // ping-floored: skips low-ping spawners
	}
}

// ── Spawn Rating ────────────────────────────────────────────────────

float ANCPlusCTFGameMode::RatePlayerStart(APlayerStart* P, AController* Player)
{
	if (bForceEpicSpawnRating)
	{
		return Super::RatePlayerStart(P, Player);
	}

	// NewCTF's carve-out is based on connected competitors, not the server's
	// configured capacity. Dead players still count; spectators do not.
	const bool bUseEpicForSmallGame = bSpawnUseNewCTFSelection
		? (CountSpawnSystemCompetitors() <= SpawnSystemThreshold)
		: (GameSession && GameSession->MaxPlayers <= 4);
	if (bUseEpicForSmallGame)
	{
		return Super::RatePlayerStart(P, Player);
	}

	float Result = Super::RatePlayerStart(P, Player);

	// Use engine accessor GetFlagBase() instead of directly reading FlagBases.
	// The plugin DLL has a different memory layout offset for FlagBases than the
	// engine DLL (class layout mismatch), so direct member access reads garbage.
	// GetFlagBase() is compiled in the engine DLL with the correct offsets.
	AUTCTFGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (Result <= 0.f || !GS || !Player)
	{
		return Result;
	}

	AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
	if (!PS || !PS->Team)
	{
		return Result;
	}

	const uint8 PlayerTeamNum = PS->Team->TeamIndex;
	const FVector StartLoc = P->GetActorLocation();
	static FName NAME_CTFSpawnCheck = FName(TEXT("CTFSpawnCheck"));

	// Iterate team indices and use engine accessor — CTF is always 2 teams
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		AUTCTFFlagBase* FlagBase = GS->GetFlagBase(TeamIdx);
		if (!IsValid(FlagBase) || !IsValid(FlagBase->MyFlag))
		{
			continue;
		}

		AUTFlag* Flag = FlagBase->MyFlag;
		const uint8 FlagTeamNum = FlagBase->GetTeamNum();
		const FName FlagState = GS->GetFlagState(TeamIdx);
		const FVector FlagBaseLoc = FlagBase->GetActorLocation();

		// Only process OUR flag — "what is the enemy doing with our flag?"
		if (FlagTeamNum != PlayerTeamNum)
		{
			continue;
		}

		if (FlagState == CarriedObjectState::Held && IsValid(Flag->HoldingPawn))
		{
			// Enemy is carrying our flag
			const FVector CarrierLoc = Flag->HoldingPawn->GetActorLocation();

			// Don't let defenders spawn near the enemy flag carrier when carrier is in our base
			float CarrierToBaseDist = (CarrierLoc - FlagBaseLoc).Size();
			if (CarrierToBaseDist < FlagBaseProximityRadius)
			{
				float SpawnToCarrierDist = (StartLoc - CarrierLoc).Size();
				if (SpawnToCarrierDist < FlagSpawnPenaltyRadius)
				{
					float DistanceFactor = 1.f - (SpawnToCarrierDist / FlagSpawnPenaltyRadius);
					Result -= FlagCarrierSpawnPenalty * DistanceFactor;
				}
			}

			// LOS check: penalize spawns with direct sightline to enemy flag carrier (anywhere on map)
			FVector EyeLoc = StartLoc + FVector(0.f, 0.f, 64.f); // approximate eye height
			UCapsuleComponent* CarrierCapsule = Flag->HoldingPawn->GetCapsuleComponent();
			float CarrierHalfHeight = CarrierCapsule ? CarrierCapsule->GetScaledCapsuleHalfHeight() : 64.f;
			FVector CarrierEyeLoc = CarrierLoc + FVector(0.f, 0.f, CarrierHalfHeight);

			if (!GetWorld()->LineTraceTestByChannel(
				EyeLoc,
				CarrierEyeLoc,
				COLLISION_TRACE_WEAPONNOCHARACTER,
				FCollisionQueryParams(NAME_CTFSpawnCheck, false)))
			{
				// Clear LOS to enemy flag carrier — bad spawn
				Result -= FlagCarrierLOSPenalty;
			}
		}
		else if (FlagState == CarriedObjectState::Dropped)
		{
			// Our flag is dropped — don't let friendlies spawn near it if it's in our base area
			// Prevents free flag returns from spawn
			const FVector DroppedFlagLoc = Flag->GetActorLocation();
			float FlagToBaseDist = (DroppedFlagLoc - FlagBaseLoc).Size();

			if (FlagToBaseDist < FlagBaseProximityRadius)
			{
				float SpawnToFlagDist = (StartLoc - DroppedFlagLoc).Size();
				if (SpawnToFlagDist < FlagSpawnPenaltyRadius)
				{
					float DistanceFactor = 1.f - (SpawnToFlagDist / FlagSpawnPenaltyRadius);
					Result -= DroppedFlagSpawnPenalty * DistanceFactor;
				}
			}
		}
	}

	// ── Enemy proximity & LOS penalties (independent of flag state) ──
	// Penalize spawns near ANY living enemy, not just the flag carrier.
	// Matches the BP EnemyBlockRange / EnemyLOSBlockRange behavior.
	if (EnemyBlockRange > 0.f || EnemyLOSBlockRange > 0.f)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* C = It->Get();
			if (!C || !C->GetPawn()) continue;

			AUTPlayerState* EnemyPS = Cast<AUTPlayerState>(C->PlayerState);
			if (!EnemyPS || !EnemyPS->Team || EnemyPS->Team->TeamIndex == PlayerTeamNum)
				continue; // Skip teammates and non-team players

			AUTCharacter* EnemyChar = Cast<AUTCharacter>(C->GetPawn());
			if (!EnemyChar || EnemyChar->IsDead()) continue;

			const FVector EnemyLoc = EnemyChar->GetActorLocation();
			const float DistToEnemy = (StartLoc - EnemyLoc).Size();

			// IG+ MinSpawnZVariance (NewTDM.uc): an enemy well BELOW a start is
			// separated by a floor, not standing next to it. Plain 3D distance
			// counts a pit dweller as adjacent, which on vertical maps pushes the
			// picker off perfectly good starts and crowds everyone onto the few
			// remaining ones. Only the PROXIMITY term is discounted here — if he
			// can actually see the start the LOS penalty below still lands (a
			// deliberate refinement; IG+ drops both).
			const bool bEnemyWellBelow = (SpawnEnemyBelowZ > 0.f)
				&& ((StartLoc.Z - EnemyLoc.Z) >= SpawnEnemyBelowZ);

			// Distance-based penalty: closer enemy = bigger penalty
			if (!bEnemyWellBelow && EnemyBlockRange > 0.f && DistToEnemy < EnemyBlockRange)
			{
				float DistFactor = 1.f - (DistToEnemy / EnemyBlockRange);
				Result -= EnemyBlockPenalty * DistFactor;
			}

			// LOS penalty: can the enemy see this spawn point?
			if (EnemyLOSBlockRange > 0.f && DistToEnemy < EnemyLOSBlockRange)
			{
				FVector SpawnEye = StartLoc + FVector(0.f, 0.f, 64.f);
				FVector EnemyEye = EnemyLoc + FVector(0.f, 0.f, EnemyChar->BaseEyeHeight);

				if (!GetWorld()->LineTraceTestByChannel(
					SpawnEye, EnemyEye,
					COLLISION_TRACE_WEAPONNOCHARACTER,
					FCollisionQueryParams(NAME_CTFSpawnCheck, false)))
				{
					// Clear LOS from spawn to enemy — penalize
					Result -= EnemyLOSPenalty;
				}
			}
		}
	}

	// ── Recent spawn penalties (IG+ style) ──
	// Discourage reusing the same spawn points to prevent predictable spawns.
	if (Player)
	{
		FRecentSpawns* Recent = PlayerRecentSpawns.Find(TWeakObjectPtr<AController>(Player));
		if (Recent)
		{
			// Hard-block back-to-back identical spawns. Bypassing the 0.2
			// floor below is the whole point — a 0.1x multiplier still
			// floored to 0.2, which could beat a heavily-penalized clean
			// alternative on small maps. Any spawn that survives the floor
			// (>= 0.2) outranks this 0.001, so the only way to land back on
			// the same start is a map with literally one usable spawn.
			if (Recent->Last.IsValid() && Recent->Last.Get() == P)
			{
				return 0.001f;
			}
			// Penalize the spawns from 2 AND 3 lives ago (IG+ LastStartSpot2/3).
			// Three-deep memory is what stops a two-start map alternating A-B-A-B
			// forever: with only 2 remembered, the third choice is unconstrained.
			if ((Recent->SecondLast.IsValid() && Recent->SecondLast.Get() == P)
				|| (Recent->ThirdLast.IsValid() && Recent->ThirdLast.Get() == P))
			{
				Result *= SpawnRecentPenaltyMultiplier; // 0.5x
			}

			// Penalize spawning near your last spawn point
			if (Recent->Last.IsValid())
			{
				float DistToLastSpawn = (StartLoc - Recent->Last->GetActorLocation()).Size();
				if (DistToLastSpawn < SpawnNearLastRadius)
				{
					float DistFactor = 1.f - (DistToLastSpawn / SpawnNearLastRadius);
					Result -= SpawnNearLastPenalty * DistFactor;
				}
			}
		}
	}

	return FMath::Max(Result, 0.2f);
}

// ── Team-aware spawn ownership ───────────────────────────────────────
// The map-authored TeamNum pools are rotating queues for the NewCTF primary and
// secondary passes. The former weighted RatePlayerStart path remains available
// behind SpawnUseNewCTFSelection for live rollback.

void ANCPlusCTFGameMode::BuildTeamSpawnPools()
{
	Team0Spawns.Reset();
	Team1Spawns.Reset();

	// Trust the map authors' TeamNum (these maps are tagged correctly). Bucket
	// each AUTTeamPlayerStart into its own team's pool — no mutation. Geometry
	// retagging is intentionally NOT done: it would mis-flip starts the mapper
	// deliberately placed on asymmetric maps. Plain APlayerStarts (incl. the
	// InitGame engine-assert fallbacks) carry no TeamNum and would hit the -20
	// wrong-team guard, so they are excluded; they exist only to satisfy the
	// engine's FindPlayerStart.
	int32 Skipped = 0;
	for (TActorIterator<AUTTeamPlayerStart> It(GetWorld()); It; ++It)
	{
		AUTTeamPlayerStart* TPS = *It;
		if (!IsValid(TPS)) continue;

		if (TPS->TeamNum == 0)      { Team0Spawns.Add(TPS); }
		else if (TPS->TeamNum == 1) { Team1Spawns.Add(TPS); }
		else                        { Skipped++; }
	}

	// NewCTF gives each team's queue one shake per live match. Thereafter the
	// first viable start is deterministic and successful uses rotate to the tail.
	auto ShufflePool = [](TArray<TWeakObjectPtr<APlayerStart>>& Pool)
	{
		for (int32 Index = Pool.Num() - 1; Index > 0; --Index)
		{
			Pool.Swap(Index, FMath::RandRange(0, Index));
		}
	};
	ShufflePool(Team0Spawns);
	ShufflePool(Team1Spawns);

	bSpawnPoolsBuilt = true;

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF spawn-pools built: T0=%d T1=%d team starts (by author TeamNum; %d skipped non-0/1)"),
		Team0Spawns.Num(), Team1Spawns.Num(), Skipped);
}

int32 ANCPlusCTFGameMode::CountSpawnSystemCompetitors() const
{
	int32 Count = 0;
	const AUTGameState* GS = CTFGameState ? CTFGameState : (GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr);
	if (!GS)
	{
		return Count;
	}

	for (APlayerState* BasePS : GS->PlayerArray)
	{
		const AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (PS && !PS->bOnlySpectator && !PS->bIsInactive && PS->Team && PS->Team->TeamIndex <= 1)
		{
			++Count;
		}
	}
	return Count;
}

AActor* ANCPlusCTFGameMode::ChooseEpicPlayerStart(AController* Player)
{
	const bool bPreviousGuard = bForceEpicSpawnRating;
	bForceEpicSpawnRating = true;
	AActor* Result = Super::ChoosePlayerStart_Implementation(Player);
	bForceEpicSpawnRating = bPreviousGuard;
	return Result;
}

AActor* ANCPlusCTFGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	if (!bSpawnUseNewCTFSelection)
	{
		return ChooseLegacyPlayerStart(Player);
	}

	// NewCTF keys this threshold to connected competitors, not server capacity
	// and not the number of pawns that happen to be alive this frame.
	if (!Player || CountSpawnSystemCompetitors() <= SpawnSystemThreshold)
	{
		return ChooseEpicPlayerStart(Player);
	}

	if (APlayerStart* Best = ChooseNewCTFPlayerStart(Player))
	{
		return Best;
	}

	if (bLogSpawnChoices)
	{
		const AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
		UE_LOG(LogGameMode, Warning, TEXT("NCPlusCTF pick: %s -> stock | system=newctf primary=none secondary=none"),
			PS ? *PS->PlayerName : TEXT("?"));
	}
	return ChooseEpicPlayerStart(Player);
}

APlayerStart* ANCPlusCTFGameMode::ChooseNewCTFPlayerStart(AController* Player)
{
	AUTPlayerState* SpawnPS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!SpawnPS || !SpawnPS->Team || SpawnPS->Team->TeamIndex > 1 || !GetWorld())
	{
		return nullptr;
	}

	if (!bSpawnPoolsBuilt)
	{
		BuildTeamSpawnPools();
	}

	const int32 TeamIndex = int32(SpawnPS->Team->TeamIndex);
	TArray<TWeakObjectPtr<APlayerStart>>& Pool = (TeamIndex == 0) ? Team0Spawns : Team1Spawns;
	for (int32 Index = Pool.Num() - 1; Index >= 0; --Index)
	{
		if (!Pool[Index].IsValid())
		{
			Pool.RemoveAt(Index, 1, false);
		}
	}
	if (Pool.Num() == 0)
	{
		return nullptr;
	}

	const int32 CycleExcluded = FMath::Clamp(SpawnMinCycleDistance, 0, FMath::Max(0, Pool.Num() - 1));
	const int32 EligibleCount = Pool.Num() - CycleExcluded;

	struct FSpawnParticipant
	{
		AUTCharacter* Character = nullptr;
		AUTPlayerState* PlayerState = nullptr;
		uint8 TeamNum = 255;
		FVector PawnLocation = FVector::ZeroVector;
		FVector CurrentEye = FVector::ZeroVector;
		FVector PredictedEye = FVector::ZeroVector;
		float DefaultEyeHeight = 0.f;
		bool bPredicted = false;
		bool bFlagCarrier = false;
	};

	TArray<FSpawnParticipant> Participants;
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* OtherController = It->Get();
		AUTPlayerState* OtherPS = OtherController ? Cast<AUTPlayerState>(OtherController->PlayerState) : nullptr;
		AUTCharacter* OtherChar = OtherController ? Cast<AUTCharacter>(OtherController->GetPawn()) : nullptr;
		if (!OtherPS || OtherPS->bOnlySpectator || OtherPS->bIsInactive || !OtherPS->Team || OtherPS->Team->TeamIndex > 1
			|| !OtherChar || OtherChar->IsPendingKillPending() || OtherChar->IsDead() || OtherChar->Health <= 0)
		{
			continue;
		}

		FSpawnParticipant Entry;
		Entry.Character = OtherChar;
		Entry.PlayerState = OtherPS;
		Entry.TeamNum = OtherPS->Team->TeamIndex;
		Entry.PawnLocation = OtherChar->GetActorLocation();
		Entry.CurrentEye = Entry.PawnLocation + FVector(0.f, 0.f, OtherChar->BaseEyeHeight);
		Entry.PredictedEye = Entry.CurrentEye;
		const AUTCharacter* CharacterCDO = OtherChar->GetClass()->GetDefaultObject<AUTCharacter>();
		Entry.DefaultEyeHeight = CharacterCDO ? CharacterCDO->BaseEyeHeight : OtherChar->BaseEyeHeight;
		if (bSpawnExtrapolateMovement && OtherChar->GetRemoteRole() == ROLE_AutonomousProxy)
		{
			const float HalfRTTSeconds = FMath::Clamp(OtherPS->ExactPing, 0.f, 250.f) * 0.0005f;
			if (HalfRTTSeconds > 0.f)
			{
				Entry.PredictedEye += OtherChar->GetVelocity() * HalfRTTSeconds;
				Entry.bPredicted = true;
			}
		}
		Entry.bFlagCarrier = Cast<AUTFlag>(OtherChar->GetCarriedObject()) != nullptr;
		Participants.Add(Entry);
	}

	AUTCharacter* EnemyFlagCarrier = nullptr;
	bool bOwnFlagOut = false;
	FVector OwnBaseLoc = FVector::ZeroVector;
	if (CTFGameState)
	{
		AUTCTFFlagBase* OwnBase = CTFGameState->GetFlagBase(uint8(TeamIndex));
		if (IsValid(OwnBase))
		{
			OwnBaseLoc = OwnBase->GetActorLocation();
			bOwnFlagOut = CTFGameState->GetFlagState(uint8(TeamIndex)) != CarriedObjectState::Home;
			if (IsValid(OwnBase->MyFlag) && IsValid(OwnBase->MyFlag->HoldingPawn))
			{
				EnemyFlagCarrier = Cast<AUTCharacter>(OwnBase->MyFlag->HoldingPawn);
			}
		}
	}

	// Preserve NCP's rotating robbed-base exclusion as one extra primary rule.
	APlayerStart* RobbedBlockedStart = nullptr;
	const int32 RobbedAvoidCount = FMath::Clamp(FMath::TruncToInt(SpawnRobbedBaseAvoidCount), 0, FMath::Max(0, Pool.Num() - 1));
	if (bOwnFlagOut && RobbedAvoidCount > 0)
	{
		TArray<int32> Nearest;
		for (int32 Rank = 0; Rank < RobbedAvoidCount; ++Rank)
		{
			int32 NearestIndex = INDEX_NONE;
			float NearestDistance = FLT_MAX;
			for (int32 Index = 0; Index < Pool.Num(); ++Index)
			{
				APlayerStart* Candidate = Pool[Index].Get();
				if (!Candidate || Nearest.Contains(Index))
				{
					continue;
				}
				const float Distance = (Candidate->GetActorLocation() - OwnBaseLoc).SizeSquared();
				if (Distance < NearestDistance)
				{
					NearestDistance = Distance;
					NearestIndex = Index;
				}
			}
			if (NearestIndex != INDEX_NONE)
			{
				Nearest.Add(NearestIndex);
			}
		}
		if (Nearest.Num() > 0)
		{
			RobbedBlockedStart = Pool[Nearest[RobbedSpawnRotation[TeamIndex] % Nearest.Num()]].Get();
		}
	}

	static FName NAME_NewCTFSpawnLOS(TEXT("NewCTFSpawnLOS"));
	auto HasLineOfSight = [this](const FVector& SpawnLocation, const FVector& SpawnEye, const FVector& TargetEye) -> bool
	{
		FCollisionQueryParams Params(NAME_NewCTFSpawnLOS, false);
		return !GetWorld()->LineTraceTestByChannel(SpawnLocation, TargetEye, COLLISION_TRACE_WEAPONNOCHARACTER, Params)
			|| !GetWorld()->LineTraceTestByChannel(SpawnEye, TargetEye, COLLISION_TRACE_WEAPONNOCHARACTER, Params);
	};

	// Rejection counters follow NewCTF's documented order, followed by NCP's
	// last-killer and robbed-base protections.
	int32 EnemyVisionBlocked = 0;
	int32 EnemyRangeBlocked = 0;
	int32 FriendlyVisionBlocked = 0;
	int32 FriendlyRangeBlocked = 0;
	int32 CarrierBlocked = 0;
	int32 FlagBlocked = 0;
	int32 KillerBlocked = 0;
	int32 RobbedBlocked = 0;
	APlayerStart* Primary = nullptr;

	for (int32 Index = 0; Index < EligibleCount && !Primary; ++Index)
	{
		APlayerStart* Candidate = Pool[Index].Get();
		if (!Candidate)
		{
			continue;
		}

		const FVector SpawnLocation = Candidate->GetActorLocation();
		int32 RejectReason = 0;

		for (const FSpawnParticipant& Other : Participants)
		{
			const bool bEnemy = Other.TeamNum != uint8(TeamIndex);
			const FVector SpawnEye = SpawnLocation + FVector(0.f, 0.f, Other.DefaultEyeHeight);
			const float Distance = FMath::Min(
				(SpawnLocation - Other.PawnLocation).Size(),
				(SpawnEye - Other.PredictedEye).Size());
			const float VisionRange = bEnemy
				? ((Other.Character == EnemyFlagCarrier)
					? FMath::Max(EnemyLOSBlockRange, SpawnFlagCarrierLOSAvoidRadius)
					: EnemyLOSBlockRange)
				: SpawnFriendlyVisionBlockRange;
			const bool bBelowCandidate = bEnemy && SpawnEnemyBelowZ > 0.f
				&& (SpawnLocation.Z - Other.PawnLocation.Z) >= SpawnEnemyBelowZ;
			const bool bNeedLOS = (VisionRange > 0.f && Distance <= VisionRange)
				|| (bBelowCandidate && SpawnEnemyHardRadius > 0.f && Distance <= SpawnEnemyHardRadius);
			bool bVisible = false;
			if (bNeedLOS)
			{
				bVisible = HasLineOfSight(SpawnLocation, SpawnEye, Other.PredictedEye);
				if (!bVisible && Other.bPredicted)
				{
					bVisible = HasLineOfSight(SpawnLocation, SpawnEye, Other.CurrentEye);
				}
			}

			if (bEnemy && VisionRange > 0.f && bVisible && Distance <= VisionRange)
			{
				RejectReason = 1;
				break;
			}
			if (bEnemy && SpawnEnemyHardRadius > 0.f && Distance <= SpawnEnemyHardRadius
				&& !(bBelowCandidate && !bVisible))
			{
				RejectReason = 2;
				break;
			}
			if (!bEnemy && SpawnFriendlyVisionBlockRange > 0.f && bVisible && Distance <= SpawnFriendlyVisionBlockRange)
			{
				RejectReason = 3;
				break;
			}
			if (!bEnemy && SpawnFriendlyBlockRange > 0.f && Distance <= SpawnFriendlyBlockRange)
			{
				RejectReason = 4;
				break;
			}
			if (bEnemy && Other.bFlagCarrier && SpawnFlagBlockRange > 0.f && Distance <= SpawnFlagBlockRange)
			{
				RejectReason = 5;
				break;
			}
			if (bEnemy && SpawnKillerAvoidRadius > 0.f && SpawnPS->LastKillerPlayerState == Other.PlayerState
				&& Distance <= SpawnKillerAvoidRadius)
			{
				RejectReason = 8;
				break;
			}
		}

		if (RejectReason == 0 && SpawnFlagBlockRange > 0.f && CTFGameState)
		{
			for (uint8 FlagTeam = 0; FlagTeam < 2; ++FlagTeam)
			{
				AUTCTFFlagBase* FlagBase = CTFGameState->GetFlagBase(FlagTeam);
				AUTFlag* Flag = IsValid(FlagBase) ? FlagBase->MyFlag : nullptr;
				if (IsValid(Flag) && !IsValid(Flag->HoldingPawn)
					&& (SpawnLocation - Flag->GetActorLocation()).Size() <= SpawnFlagBlockRange)
				{
					RejectReason = 6;
					break;
				}
			}
		}

		if (RejectReason == 0 && Candidate == RobbedBlockedStart)
		{
			RejectReason = 9;
		}

		switch (RejectReason)
		{
			case 0: Primary = Candidate; break;
			case 1: ++EnemyVisionBlocked; break;
			case 2: ++EnemyRangeBlocked; break;
			case 3: ++FriendlyVisionBlocked; break;
			case 4: ++FriendlyRangeBlocked; break;
			case 5: ++CarrierBlocked; break;
			case 6: ++FlagBlocked; break;
			case 8: ++KillerBlocked; break;
			case 9: ++RobbedBlocked; break;
			default: break;
		}
	}

	if (Primary)
	{
		if (bLogSpawnChoices)
		{
			UE_LOG(LogGameMode, Warning,
				TEXT("NCPlusCTF pick: %s(T%d) -> %s | system=primary cycle=%d/%d blocked=evis:%d enemy:%d fvis:%d friend:%d carrier:%d flag:%d killer:%d robbed:%d"),
				*SpawnPS->PlayerName, TeamIndex, *Primary->GetName(), CycleExcluded, Pool.Num(),
				EnemyVisionBlocked, EnemyRangeBlocked, FriendlyVisionBlocked, FriendlyRangeBlocked,
				CarrierBlocked, FlagBlocked, KillerBlocked, RobbedBlocked);
		}
		return Primary;
	}

	if (!bSpawnSecondaryEnabled)
	{
		return nullptr;
	}

	APlayerStart* Secondary = nullptr;
	float BestDistanceSum = -FLT_MAX;
	for (int32 Index = 0; Index < EligibleCount; ++Index)
	{
		APlayerStart* Candidate = Pool[Index].Get();
		if (!Candidate)
		{
			continue;
		}

		float DistanceSum = 0.f;
		for (const FSpawnParticipant& Other : Participants)
		{
			float Distance = FMath::Min(
				(Candidate->GetActorLocation() - Other.PredictedEye).Size(),
				SpawnSecondaryMaxDistance);
			if (Other.TeamNum == uint8(TeamIndex))
			{
				Distance *= SpawnSecondaryOwnTeamWeight;
			}
			else if (Other.bFlagCarrier)
			{
				Distance *= SpawnSecondaryCarrierWeight;
			}
			DistanceSum += Distance;
		}

		if (!Secondary || DistanceSum > BestDistanceSum)
		{
			Secondary = Candidate;
			BestDistanceSum = DistanceSum;
		}
	}

	if (Secondary && bLogSpawnChoices)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF pick: %s(T%d) -> %s | system=secondary weight=%.0f cycle=%d/%d primaryBlocked=evis:%d enemy:%d fvis:%d friend:%d carrier:%d flag:%d killer:%d robbed:%d"),
			*SpawnPS->PlayerName, TeamIndex, *Secondary->GetName(), BestDistanceSum, CycleExcluded, Pool.Num(),
			EnemyVisionBlocked, EnemyRangeBlocked, FriendlyVisionBlocked, FriendlyRangeBlocked,
			CarrierBlocked, FlagBlocked, KillerBlocked, RobbedBlocked);
	}
	return Secondary;
}

AActor* ANCPlusCTFGameMode::ChooseLegacyPlayerStart(AController* Player)
{
	// Small games (1v1/2v2): keep Epic's default selection — same carve-out as
	// RatePlayerStart; too few starts for team pools to help.
	if (GameSession && GameSession->MaxPlayers <= 4)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	if (!bSpawnPoolsBuilt)
	{
		BuildTeamSpawnPools();
	}

	const int32 TeamIndex = PS->Team->TeamIndex;
	TArray<TWeakObjectPtr<APlayerStart>>& Pool = (TeamIndex == 0) ? Team0Spawns : Team1Spawns;

	// No team starts tagged for this team — never fail to spawn; defer to engine.
	if (Pool.Num() == 0)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Score the curated own-team pool with the existing RatePlayerStart (the -20
	// wrong-team guard never trips here — own-team pool — so it stays a safety net).
	// Two shaping passes on top of the raw score:
	//   * Freshness: when no flag is active near our base, reward starts the team
	//     hasn't used recently (scaled by staleness) — forces spread across unused
	//     starts and makes a fresh respawn meaningful again (was: same safe spot).
	//   * Tie-band: pick at RANDOM among everything within SpawnTieBandWidth of the
	//     best — kills the deterministic "always one side" players reported.
	const bool bForceFresh = (SpawnFreshnessBonus > 0.f) && !IsFlagNearOwnBase((uint8)TeamIndex);
	const float Now = GetWorld()->GetTimeSeconds();

	// Anti-camp: find the player's LAST KILLER's current location (if alive on the
	// map) so we can hard-exclude spawns next to whoever just fragged us — the
	// "spawned in front of the killer" case when they're sitting in our base.
	FVector KillerLoc = FVector::ZeroVector;
	bool bHaveKiller = false;
	if (SpawnKillerAvoidRadius > 0.f && PS->LastKillerPlayerState)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* C = It->Get();
			if (C && C->PlayerState == PS->LastKillerPlayerState && C->GetPawn())
			{
				AUTCharacter* KillerChar = Cast<AUTCharacter>(C->GetPawn());
				if (KillerChar && !KillerChar->IsDead())
				{
					KillerLoc = KillerChar->GetActorLocation();
					bHaveKiller = true;
				}
				break;
			}
		}
	}

	// A defender should not materialize in the enemy flag carrier's sightline.
	// This is an eligibility preference rather than an absolute failure condition:
	// if every start inside the configured radius has LOS, the tiering below falls
	// back to the best remaining safety tier and still permits the respawn.
	AUTCharacter* EnemyFlagCarrier = nullptr;
	if (SpawnFlagCarrierLOSAvoidRadius > 0.f && CTFGameState)
	{
		AUTCTFFlagBase* OwnBase = CTFGameState->GetFlagBase((uint8)TeamIndex);
		if (IsValid(OwnBase) && IsValid(OwnBase->MyFlag)
			&& CTFGameState->GetFlagState((uint8)TeamIndex) == CarriedObjectState::Held
			&& IsValid(OwnBase->MyFlag->HoldingPawn))
		{
			AUTCharacter* CarrierChar = Cast<AUTCharacter>(OwnBase->MyFlag->HoldingPawn);
			if (CarrierChar && !CarrierChar->IsDead())
			{
				EnemyFlagCarrier = CarrierChar;
			}
		}
	}

	// When our OWN flag isn't home, drop ONE of the deepest starts at our
	// (just-robbed) base — alternating which — so we respawn biased forward toward
	// the carrier's escape rather than behind it: a lightweight slice of UT99's
	// "hard to leave" that no longer locks defenders out of the whole base area.
	FVector OwnBaseLoc = FVector::ZeroVector;
	const int32 RobbedAvoid = FMath::TruncToInt(SpawnRobbedBaseAvoidCount);
	bool bOwnFlagOut = false;
	if (RobbedAvoid > 0 && CTFGameState)
	{
		AUTCTFFlagBase* OwnBase = CTFGameState->GetFlagBase((uint8)TeamIndex);
		if (IsValid(OwnBase) && CTFGameState->GetFlagState((uint8)TeamIndex) != CarriedObjectState::Home)
		{
			OwnBaseLoc = OwnBase->GetActorLocation();
			bOwnFlagOut = true;
		}
	}

	TArray<APlayerStart*> Cands;
	TArray<float> Scores;
	TArray<bool> KillerAdj;
	TArray<bool> EFCLOSAdj;
	TArray<bool> EnemyAdj;
	TArray<float> DistOwnBase;
	Cands.Reserve(Pool.Num());
	Scores.Reserve(Pool.Num());
	KillerAdj.Reserve(Pool.Num());
	EFCLOSAdj.Reserve(Pool.Num());
	EnemyAdj.Reserve(Pool.Num());
	DistOwnBase.Reserve(Pool.Num());
	static FName NAME_CTFSpawnEFCLOS = FName(TEXT("CTFSpawnEFCLOS"));
	static FName NAME_CTFSpawnEnemy = FName(TEXT("CTFSpawnEnemy"));
	for (const TWeakObjectPtr<APlayerStart>& WP : Pool)
	{
		APlayerStart* Candidate = WP.Get();
		if (!Candidate) continue;

		float Score = RatePlayerStart(Candidate, Player);
		if (bForceFresh)
		{
			const float* LastUsed = SpawnLastUsedTime.Find(Candidate);
			const float Staleness = LastUsed
				? FMath::Clamp((Now - *LastUsed) / FMath::Max(1.f, SpawnFreshnessWindow), 0.f, 1.f)
				: 1.f; // never used this match = maximally fresh
			Score += SpawnFreshnessBonus * Staleness;
		}
		Cands.Add(Candidate);
		Scores.Add(Score);
		KillerAdj.Add(bHaveKiller && ((Candidate->GetActorLocation() - KillerLoc).Size() < SpawnKillerAvoidRadius));

		bool bHasEFCLOS = false;
		if (EnemyFlagCarrier)
		{
			const FVector StartLoc = Candidate->GetActorLocation();
			const FVector CarrierLoc = EnemyFlagCarrier->GetActorLocation();
			if ((StartLoc - CarrierLoc).Size() < SpawnFlagCarrierLOSAvoidRadius)
			{
				const FVector SpawnEye = StartLoc + FVector(0.f, 0.f, 64.f);
				const FVector CarrierEye = CarrierLoc + FVector(0.f, 0.f, EnemyFlagCarrier->BaseEyeHeight);
				bHasEFCLOS = !GetWorld()->LineTraceTestByChannel(
					SpawnEye, CarrierEye,
					COLLISION_TRACE_WEAPONNOCHARACTER,
					FCollisionQueryParams(NAME_CTFSpawnEFCLOS, false));
			}
		}
		EFCLOSAdj.Add(bHasEFCLOS);

		// IG+ MinSpawnDistance (NewTDM.uc): a start with a live enemy this close is
		// REFUSED, not merely penalised — a score penalty can still lose to a start
		// that scores well on everything else, which is how players end up
		// materialising in someone's face. Escape hatch, also IG+: an enemy well
		// BELOW the start with no sightline to it is floor-separated, not a threat.
		// This is only a tier preference — if every start in the pool is violated
		// (small map, heavy pressure) the bit drops out of every tier and spawning
		// proceeds on the remaining protections.
		bool bEnemyTooClose = false;
		if (SpawnEnemyHardRadius > 0.f)
		{
			const FVector CandLoc = Candidate->GetActorLocation();
			for (FConstControllerIterator EIt = GetWorld()->GetControllerIterator(); EIt && !bEnemyTooClose; ++EIt)
			{
				AController* EC = EIt->Get();
				if (!EC || !EC->GetPawn()) { continue; }
				AUTPlayerState* EPS = Cast<AUTPlayerState>(EC->PlayerState);
				if (!EPS || !EPS->Team || EPS->Team->TeamIndex == TeamIndex) { continue; }
				AUTCharacter* EChar = Cast<AUTCharacter>(EC->GetPawn());
				if (!EChar || EChar->IsDead()) { continue; }

				const FVector ELoc = EChar->GetActorLocation();
				if ((CandLoc - ELoc).Size() >= SpawnEnemyHardRadius) { continue; }

				if (SpawnEnemyBelowZ > 0.f && (CandLoc.Z - ELoc.Z) >= SpawnEnemyBelowZ)
				{
					// Below us: only dangerous if he can actually see the start.
					const FVector SpawnEye = CandLoc + FVector(0.f, 0.f, 64.f);
					const FVector EnemyEye = ELoc + FVector(0.f, 0.f, EChar->BaseEyeHeight);
					const bool bClearLOS = !GetWorld()->LineTraceTestByChannel(
						SpawnEye, EnemyEye, COLLISION_TRACE_WEAPONNOCHARACTER,
						FCollisionQueryParams(NAME_CTFSpawnEnemy, false));
					if (!bClearLOS) { continue; }
				}
				bEnemyTooClose = true;
			}
		}
		EnemyAdj.Add(bEnemyTooClose);

		DistOwnBase.Add(bOwnFlagOut ? (Candidate->GetActorLocation() - OwnBaseLoc).Size() : FLT_MAX);
	}

	// Rank the RobbedAvoid starts nearest our robbed base, then mark exactly ONE
	// of them — rotating through the set per respawn (nearest, 2nd-nearest, ...)
	// so defenders always keep a base spawn available but can't rely on one fixed
	// spot while the flag is out. (Was: drop ALL RobbedAvoid nearest at once.)
	TArray<bool> RobbedAdj;
	RobbedAdj.Init(false, Cands.Num());
	if (bOwnFlagOut)
	{
		TArray<int32> NearestRanked;   // candidate indices, ascending distance to base
		const int32 SetSize = FMath::Min(RobbedAvoid, FMath::Max(0, Cands.Num() - 1));
		for (int32 k = 0; k < SetSize; ++k)
		{
			int32 MinIdx = -1; float MinD = FLT_MAX;
			for (int32 i = 0; i < Cands.Num(); ++i)
			{
				if (!NearestRanked.Contains(i) && DistOwnBase[i] < MinD) { MinD = DistOwnBase[i]; MinIdx = i; }
			}
			if (MinIdx < 0) break;
			NearestRanked.Add(MinIdx);
		}
		if (NearestRanked.Num() > 0)
		{
			int32& Rotation = RobbedSpawnRotation[FMath::Clamp(TeamIndex, 0, 1)];
			RobbedAdj[NearestRanked[Rotation % NearestRanked.Num()]] = true;
			++Rotation;
		}
	}

	// Lexicographic safety tier: no-enemy-on-top-of-us is highest priority (IG+
	// MinSpawnDistance), then last-killer clearance, then EFC LOS clearance, then
	// the rotating robbed-base exclusion. Each lower bit can only decide between
	// candidates tied on every higher-priority rule. If all starts violate a rule,
	// that bit is absent from every tier and spawning still succeeds using the
	// remaining protections.
	uint8 BestSafetyTier = 0;
	auto GetSafetyTier = [&](int32 i) -> uint8
	{
		const bool bEnemyClear = !EnemyAdj[i];
		const bool bKillerClear = !(bHaveKiller && KillerAdj[i]);
		const bool bEFCClear = !(EnemyFlagCarrier && EFCLOSAdj[i]);
		return (bEnemyClear ? 8 : 0) | (bKillerClear ? 4 : 0) | (bEFCClear ? 2 : 0) | (!RobbedAdj[i] ? 1 : 0);
	};
	for (int32 i = 0; i < Cands.Num(); ++i)
	{
		BestSafetyTier = FMath::Max(BestSafetyTier, GetSafetyTier(i));
	}
	auto Eligible = [&](int32 i) -> bool
	{
		return GetSafetyTier(i) == BestSafetyTier;
	};

	float BestScore = -FLT_MAX;
	for (int32 i = 0; i < Cands.Num(); ++i)
	{
		if (!Eligible(i)) { continue; }
		BestScore = FMath::Max(BestScore, Scores[i]);
	}

	APlayerStart* Best = nullptr;
	// Starts in real contention for this pick — weighted mode counts live (non-zero)
	// ceilings, legacy counts the tie band. Feeds band= in the pick diagnostic below.
	int32 ContentionCount = 0;
	if (bSpawnWeightedRandom)
	{
		// IG+ weighted-random pick (NewTDM.uc: CurrentScore = Rand(Max(DefaultSpawnWeight
		// + CurrentScore, 0)), highest draw wins). Every start still in contention
		// draws from [0, ceiling); equal starts are a true coin-flip and a slightly
		// worse start still wins a real share of the time. This is what the tie-band
		// could not do — it picked the SAME start every life whenever one led by more
		// than SpawnTieBandWidth ("siempre en el mismo sitio").
		//
		// Ceilings are measured DOWN FROM THE BEST, never up from the worst: Epic's
		// base rating returns hard negatives for starts it has already rejected
		// (-8 just-used, -5 respawn choice, -10 telefrag, -20 wrong team — and our
		// Result<=0 early-out passes them through untouched), so a worst-relative
		// ceiling would have handed the start you just left a real chance of coming
		// back. Falling off the best instead gives anything SpawnRandomBase /
		// SpawnRandomSpread points below the leader a ceiling of zero — no draw, no
		// chance — which keeps every engine rejection excluded.
		float BestDraw = -1.f;
		APlayerStart* TopScorer = nullptr;
		for (int32 i = 0; i < Cands.Num(); ++i)
		{
			if (!Eligible(i)) { continue; }
			if (TopScorer == nullptr && Scores[i] >= BestScore) { TopScorer = Cands[i]; }

			const float Ceiling = SpawnRandomBase - (BestScore - Scores[i]) * SpawnRandomSpread;
			if (Ceiling <= 0.f) { continue; }
			ContentionCount++;
			const float Draw = FMath::FRandRange(0.f, Ceiling);
			if (Draw > BestDraw)
			{
				BestDraw = Draw;
				Best = Cands[i];
			}
		}
		// Only reachable if SpawnRandomBase was configured to 0 or below: keep the
		// scorer's verdict rather than dropping to the engine picker.
		if (Best == nullptr) { Best = TopScorer; }
	}
	else
	{
		// Legacy: coin-flip among everything within SpawnTieBandWidth of the best.
		TArray<APlayerStart*> TopBand;
		for (int32 i = 0; i < Cands.Num(); ++i)
		{
			if (!Eligible(i)) { continue; }
			if (Scores[i] >= BestScore - SpawnTieBandWidth) { TopBand.Add(Cands[i]); }
		}
		ContentionCount = TopBand.Num();
		if (TopBand.Num() > 0) { Best = TopBand[FMath::RandRange(0, TopBand.Num() - 1)]; }
	}

	if (!Best)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Optional selection diagnostics. Pairs with RestartPlayer's actual-pawn line.
	if (bLogSpawnChoices)
	{
		int32 KillerBlocked = 0, EFCLOSBlocked = 0, RobbedBlocked = 0;
		for (int32 i = 0; i < Cands.Num(); ++i)
		{
			if (Eligible(i)) { continue; }
			if ((BestSafetyTier & 4) && bHaveKiller && KillerAdj[i]) { KillerBlocked++; }
			else if ((BestSafetyTier & 2) && EnemyFlagCarrier && EFCLOSAdj[i]) { EFCLOSBlocked++; }
			else if ((BestSafetyTier & 1) && RobbedAdj[i]) { RobbedBlocked++; }
		}
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF pick: %s(T%d) -> %s | tier=%u band=%d fresh=%d kblk=%d efcblk=%d rbblk=%d (pool=%d)"),
			*PS->PlayerName, TeamIndex, *Best->GetName(), (uint32)BestSafetyTier, ContentionCount, bForceFresh ? 1 : 0,
			KillerBlocked, EFCLOSBlocked, RobbedBlocked, Pool.Num());
	}

	return Best;
}

float ANCPlusCTFGameMode::GetTravelDelay()
{
	// GetScoringPlays() is inline and unsafe across the DLL boundary — use a safe fallback
	return Super::GetTravelDelay() + 6.f;
}

// ── Helpers ──────────────────────────────────────────────────────────

bool ANCPlusCTFGameMode::IsAnyFlagHeld() const
{
	if (!IsValid(CTFGameState))
	{
		return false;
	}
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		if (CTFGameState->GetFlagState(TeamIdx) == CarriedObjectState::Held)
		{
			return true;
		}
	}
	return false;
}

bool ANCPlusCTFGameMode::AreAllFlagsHome() const
{
	if (!IsValid(CTFGameState))
	{
		return true;
	}
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		if (CTFGameState->GetFlagState(TeamIdx) != CarriedObjectState::Home)
		{
			return false;
		}
	}
	return true;
}

bool ANCPlusCTFGameMode::IsAnyEligibleFlagHeld() const
{
	if (!IsValid(CTFGameState))
	{
		return false;
	}
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		if (bAdvantageFlagEligible[TeamIdx]
			&& CTFGameState->GetFlagState(TeamIdx) == CarriedObjectState::Held)
		{
			return true;
		}
	}
	return false;
}

bool ANCPlusCTFGameMode::IsAnyEligibleFlagOut() const
{
	if (!IsValid(CTFGameState))
	{
		return false;
	}
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		if (bAdvantageFlagEligible[TeamIdx]
			&& CTFGameState->GetFlagState(TeamIdx) != CarriedObjectState::Home)
		{
			return true;
		}
	}
	return false;
}

bool ANCPlusCTFGameMode::ShouldEnterAdvantage() const
{
	// Advantage triggers if ANY flag is not home (held or dropped).
	// NewCTF (UT99) uses this — a dropped flag still means the play is live.
	if (AreAllFlagsHome())
	{
		return false;
	}

	// Halftime advantage (two-half games only): always allowed regardless of score
	if (bHasHalftime && !NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf")))
	{
		return true;
	}

	// End-of-game advantage: only within 1 cap difference (configurable)
	if (bEndGameAdvantageOnlyWithinOneCap && Teams.Num() >= 2)
	{
		int32 ScoreDiff = FMath::Abs(Teams[0]->Score - Teams[1]->Score);
		return ScoreDiff <= 1;
	}

	return true;
}

// ── Advantage Time System ────────────────────────────────────────────

void ANCPlusCTFGameMode::EnterAdvantage()
{
	NCPlusReflection::SetBool(CTFGameState, TEXT("bPlayingAdvantage"), true);
	// 255 = both teams have advantage (not just one specific team)
	NCPlusReflection::SetByte(CTFGameState, TEXT("AdvantageTeamIndex"), 255);
	NCPlusReflection::SetBool(CTFGameState, TEXT("bStopGameClock"), true);

	AdvantageTimeRemaining = AdvantageMaxDuration;
	bGracePeriodActive = false;
	GracePeriodTimeRemaining = 0;

	// Eligibility snapshot: only the flags already out (held or dropped) belong to
	// the play that earned advantage. A flag grabbed fresh AFTER this point buys
	// nothing — the grab is allowed, but it can neither sustain advantage nor
	// re-arm the timer. Counters the stand-camper cherry-pick: regrab the
	// returning flag inside the 1Hz DefaultTimer window and ride a brand-new play
	// for another AdvantageMaxDuration.
	for (uint8 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		bAdvantageFlagEligible[TeamIdx] = IsValid(CTFGameState)
			&& CTFGameState->GetFlagState(TeamIdx) != CarriedObjectState::Home;
	}

	// Stamp advantage start for the HUD's elapsed-time counter.
	if (OTInfo && CTFGameState)
	{
		OTInfo->AdvantageStartElapsed = CTFGameState->ElapsedTime;
	}

	// Broadcast advantage message (message index 6 = "Advantage" in UTCTFGameMessage)
	// Pass NULL for team since both teams get advantage
	BroadcastLocalized(this, UUTCTFGameMessage::StaticClass(), 6, nullptr, nullptr, nullptr);
}

void ANCPlusCTFGameMode::StartGracePeriod()
{
	bGracePeriodActive = true;
	GracePeriodTimeRemaining = GracePeriodDuration;
}

void ANCPlusCTFGameMode::CancelGracePeriod()
{
	bGracePeriodActive = false;
	GracePeriodTimeRemaining = 0;
}

bool ANCPlusCTFGameMode::CheckAdvantage()
{
	if (!CTFGameState || !CTFGameState->GetFlagBase(0) || !CTFGameState->GetFlagBase(1))
	{
		return false;
	}

	// Eligibility-aware: only a flag from the play that earned advantage sustains
	// it. A retired flag someone fresh-grabbed counts as "not held" here.
	bool bEligibleFlagHeld = IsAnyEligibleFlagHeld();

	if (bEligibleFlagHeld)
	{
		// A flag is being held - advantage continues, cancel any grace period
		if (bGracePeriodActive)
		{
			CancelGracePeriod();
		}
		return true;
	}

	// No eligible flags held - either we're already in grace or need to start it
	if (!bGracePeriodActive)
	{
		StartGracePeriod();
	}

	// Grace period is ticking - handled in DefaultTimer
	return true;
}

void ANCPlusCTFGameMode::EndOfHalf()
{
	// Clear advantage state
	NCPlusReflection::SetBool(CTFGameState, TEXT("bPlayingAdvantage"), false);
	NCPlusReflection::SetByte(CTFGameState, TEXT("AdvantageTeamIndex"), 255);
	bGracePeriodActive = false;
	GracePeriodTimeRemaining = 0;
	bAdvantageFlagEligible[0] = false;
	bAdvantageFlagEligible[1] = false;
	if (OTInfo)
	{
		OTInfo->AdvantageStartElapsed = -1;
	}

	if (!bHasHalftime || NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf")))
	{
		// End of game (or single-period game with no halftime)
		AUTTeamInfo* WinningTeam = CTFGameState->FindLeadingTeam();
		if (WinningTeam != nullptr)
		{
			AUTPlayerState* WinningPS = FindBestPlayerOnTeam(WinningTeam->GetTeamNum());
			EndGame(WinningPS, FName(TEXT("TimeLimit")));
		}
		else if (bAllowOvertime)
		{
			if (OTInfo && CTFGameState)
			{
				OTInfo->OvertimeStartElapsed = CTFGameState->ElapsedTime;
			}
			SetMatchState(MatchState::MatchIsInOvertime);
		}
		else
		{
			EndGame(nullptr, FName(TEXT("TimeLimit")));
		}
	}
	else
	{
		// First half -> intermission
		SetMatchState(MatchState::MatchIntermission);
	}
}

// ── Core Game Loop ───────────────────────────────────────────────────

void ANCPlusCTFGameMode::CheckGameTime()
{
	// Let base handle intermission countdown
	Super::CheckGameTime();

	if (!CTFGameState->IsMatchInProgress() || TimeLimit == 0)
	{
		return;
	}

	if (NCPlusReflection::GetInt(CTFGameState, TEXT("RemainingTime")) <= 0)
	{
		// ── Overtime: golden cap, first score wins ──
		if (CTFGameState->IsMatchInOvertime())
		{
			AUTTeamInfo* WinningTeam = CTFGameState->FindLeadingTeam();
			if (WinningTeam != nullptr)
			{
				AUTPlayerState* WinningPS = FindBestPlayerOnTeam(WinningTeam->GetTeamNum());
				EndGame(WinningPS, FName(TEXT("TimeLimit")));
			}
			return;
		}

		// ── Already playing advantage ──
		if (NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage")))
		{
			// Advantage timer ran out and grace period also expired (or flags still held)
			// This is handled by DefaultTimer ticking down and calling EndOfHalf
			// But also stop the game clock if it isn't already
			if (!NCPlusReflection::GetBool(CTFGameState, TEXT("bStopGameClock")))
			{
				NCPlusReflection::SetBool(CTFGameState, TEXT("bStopGameClock"), true);
			}
			return;
		}

		// ── Time just expired, not yet in advantage ──
		if (bAllowOvertime && !UTGameState->IsMatchInOvertime())
		{
			NCPlusReflection::SetBool(UTGameState, TEXT("bStopGameClock"), true);
		}

		if (ShouldEnterAdvantage())
		{
			EnterAdvantage();
		}
		else
		{
			EndOfHalf();
		}
	}
	else if (NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage")))
	{
		// Clock is still running but advantage is active - check if it should end
		// This handles the case where advantage was entered and something changed
		if (!CheckAdvantage())
		{
			EndOfHalf();
		}
	}
}

void ANCPlusCTFGameMode::BroadcastLocalized(AActor* Sender, TSubclassOf<ULocalMessage> Message, int32 Switch, APlayerState* RelatedPlayerState_1, APlayerState* RelatedPlayerState_2, UObject* OptionalObject)
{
	// Regrab-alarm detection: the flags themselves have no pickup-from-dropped
	// notification, but every transition message funnels through here. A
	// taken(4) that follows a drop signal for the same team flag is a regrab.
	bool bAdvantageFlagReturned = false;
	if (Message)
	{
		AUTCarriedObject* Flag = Cast<AUTCarriedObject>(Sender);
		const uint8 FlagTeam = Flag ? Flag->GetTeamNum() : 255;
		if (FlagTeam < 2)
		{
			if (Message->IsChildOf(UUTCTFGameMessage::StaticClass()))
			{
				if (Switch == 3)
				{
					bFlagWasDropped[FlagTeam] = true;
				}
				else if ((Switch == 0 || Switch == 1) && !Flag->bGradualAutoReturn)
				{
					// A return (touch or auto). No ObjectState check: genuine returns
					// broadcast while the flag is still Dropped (UTCarriedObject.cpp:357
					// — Home happens later in the same stack). The ONLY mid-drop 0/1
					// emitter is the gradual-auto-return reminder (UTCarriedObject.cpp
					// :873-876), excluded by the bGradualAutoReturn guard; that flag is
					// always false for NCPlusCTF flags, and even on a hypothetical
					// gradual map the 1Hz sampler re-marks Dropped within a second.
					bFlagWasDropped[FlagTeam] = false;

					// Advantage eligibility: a returned flag's play is over — retire
					// it. The end-of-half check runs after Super so the return
					// message reaches players before any half-end cascade.
					bAdvantageFlagEligible[FlagTeam] = false;
					bAdvantageFlagReturned = true;
				}
				else if (Switch == 4)
				{
					if (bFlagWasDropped[FlagTeam])
					{
						PlayRegrabTakenAlarm(Flag);
					}
					bFlagWasDropped[FlagTeam] = false;
				}
			}
			else if (Message->IsChildOf(UUTCTFRewardMessage::StaticClass()) && Switch == 6)
			{
				// Near-cap "Denied" drop (UTCTFFlag::Drop): the dropped(3) broadcast
				// is deferred 0.8s and self-suppresses if the flag is regrabbed
				// first — this reward message is the authoritative drop signal for
				// that path, so an instant cherry-pick at the stand still alarms.
				bFlagWasDropped[FlagTeam] = true;
			}
		}
	}

	Super::BroadcastLocalized(Sender, Message, Switch, RelatedPlayerState_1, RelatedPlayerState_2, OptionalObject);

	// Advantage cherry-pick counter: this return may have retired the LAST eligible
	// flag. End the half NOW, in the same stack as the return broadcast — the
	// return fires while the flag is still Dropped (see the switch 0/1 branch
	// above), provably before any pickup can land — so a stand-camper's instant
	// regrab arrives after the half is already over instead of buying a fresh
	// AdvantageMaxDuration. The 1Hz DefaultTimer cannot see a return+grab that
	// happens inside one second; this site can.
	if (bAdvantageFlagReturned
		&& CTFGameState && CTFGameState->IsMatchInProgress()
		&& NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage"))
		&& !IsAnyEligibleFlagOut())
	{
		EndOfHalf();
	}
}

void ANCPlusCTFGameMode::PlayRegrabTakenAlarm(AUTCarriedObject* Flag)
{
	if (!bRegrabTakenAlarm || Flag == nullptr)
	{
		return;
	}
	AUTCTFFlagBase* Base = Cast<AUTCTFFlagBase>(Flag->HomeBase);
	if (Base == nullptr || Base->FlagTakenSound == nullptr)
	{
		return;
	}
	const uint8 FlagTeam = Flag->GetTeamNum();
	const float Now = GetWorld()->GetTimeSeconds();
	if (FlagTeam > 1 || Now - LastRegrabAlarmTime[FlagTeam] < 1.f)
	{
		return;
	}
	LastRegrabAlarmTime[FlagTeam] = Now;

	// Mirror of AUTCTFFlagBase::ObjectWasPickedUp's bWasHome branch
	// (UTCTFFlagBase.cpp:98-115), with the enemy's positional cue moved from
	// the base stand to where the flag was actually grabbed.
	USoundBase* EnemyCue = Base->EnemyFlagTakenSound ? Base->EnemyFlagTakenSound : Base->FlagTakenSound;
	for (FConstPlayerControllerIterator Iterator = GetWorld()->GetPlayerControllerIterator(); Iterator; ++Iterator)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(*Iterator);
		if (PC == nullptr)
		{
			continue;
		}
		if ((PC->PlayerState && PC->PlayerState->bOnlySpectator) || (PC->GetTeamNum() == FlagTeam))
		{
			PC->UTClientPlaySound(Base->FlagTakenSound);
		}
		else
		{
			PC->HearSound(EnemyCue, Flag, Flag->GetActorLocation(), true, false, SAT_None);
		}
	}
}

void ANCPlusCTFGameMode::DefaultTimer()
{
	Super::DefaultTimer();

	// Role-aware ratings: sample every living player's map zone once per second
	// (DefaultTimer is 1Hz) while the match is live. Self-guards on match state.
	SampleRoleDwell();

	// Regrab-alarm bookkeeping: sample flag states at 1Hz so a dropped flag is
	// tracked even when the dropped(3) broadcast was deferred (the near-cap
	// "Denied" path in UTCTFFlag::Drop delays it 0.8s and DelayedDropMessage
	// self-suppresses if any newer flag message landed in between). A flag
	// observed back Home clears the mark.
	if (CTFGameState)
	{
		for (int32 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
		{
			const FName FlagState = CTFGameState->GetFlagState((uint8)TeamIdx);
			if (FlagState == CarriedObjectState::Dropped)
			{
				bFlagWasDropped[TeamIdx] = true;
			}
			else if (FlagState == CarriedObjectState::Home)
			{
				bFlagWasDropped[TeamIdx] = false;
			}
		}
	}

	// Tick advantage timer (DefaultTimer fires every second).
	// Advantage lasts up to AdvantageMaxDuration (5 min default).
	// All ELIGIBLE flags home → instant end (NewCTF style, no grace period).
	if (CTFGameState && NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage")) && CTFGameState->IsMatchInProgress())
	{
		// Every ELIGIBLE flag home → end immediately (NewCTF: IsEveryFlagHome check
		// in Timer). Eligible-only so a retired flag fresh-grabbed by a
		// stand-camper (cherry-pick exploit) no longer keeps advantage alive.
		if (!IsAnyEligibleFlagOut())
		{
			EndOfHalf();
			return;
		}

		// Overall advantage time limit (5 min default)
		AdvantageTimeRemaining--;
		if (AdvantageTimeRemaining <= 0)
		{
			EndOfHalf();
			return;
		}
	}

	// Overtime respawn escalation: the 2s base applies from the first OT tick
	// (jumping up from the normal match wait — the "immediate" bump players
	// notice), holds through OvertimeEscalationDelay, then gains +1s per
	// OvertimeEscalationInterval, capping at OvertimeRespawnTime. Defaults
	// (tOxX 2026-08-10): 0-6min=2s, then 3s at 6:00, 4s at 7:00 ... 10s cap at
	// 13:00. (Old NewCTF ramp: 5-min hold, +1s per 2 min, 6s cap.) 3v3+ only.
	if (CTFGameState && CTFGameState->IsMatchInOvertime() && OvertimeRespawnTime > 0.f
		&& GameSession && GameSession->MaxPlayers > 4)
	{
		float OTElapsed = GetWorld()->GetTimeSeconds() - OvertimeStartWorldTime;
		float BaseRespawn = 2.f;
		// First +1 lands AT the delay boundary ("after 6 min it starts
		// increasing"), then one more per interval.
		int32 ExtraSeconds = (OTElapsed >= OvertimeEscalationDelay)
			? 1 + FMath::FloorToInt((OTElapsed - OvertimeEscalationDelay)
				/ FMath::Max(1.f, OvertimeEscalationInterval))
			: 0;
		float NewRespawn = FMath::Min(BaseRespawn + (float)ExtraSeconds, OvertimeRespawnTime);
		if (NewRespawn > RespawnWaitTime)
		{
			RespawnWaitTime = NewRespawn;
			CTFGameState->SetRespawnWaitTime(RespawnWaitTime);

			// Notify all players of the respawn increase
			FString Msg = FString::Printf(TEXT("Overtime: respawn time increased to %ds"), FMath::RoundToInt(NewRespawn));
			for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
			{
				APlayerController* PC = Cast<APlayerController>(It->Get());
				if (PC)
				{
					PC->ClientMessage(Msg);
				}
			}
		}
	}
}

// ── Scoring ──────────────────────────────────────────────────────────

void ANCPlusCTFGameMode::ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn, AUTPlayerState* Holder, FName Reason)
{
	// Double-capture prevention: reject FlagCapture within 0.5s of last cap.
	// Handles maps with no geometry between flag bases where both teams
	// could trigger OnOverlapBegin on the same frame.
	if (Reason == FName("FlagCapture"))
	{
		float CurrentTime = GetWorld()->GetTimeSeconds();
		if (CurrentTime < LastScoreObjectTime)
		{
			return;
		}
		LastScoreObjectTime = CurrentTime + 0.5f;
	}

	// Record the decisive cap BEFORE Super: a scorelimit/golden/mercy cap ends the match
	// INSIDE Super::ScoreObject_Implementation (-> EndGame -> end-match replay) before the
	// post-Super !HasMatchEnded() bookkeeping runs, so the deciding cap would otherwise never
	// be recorded (at cap limit 1 the featured moment is always blank). UniqueId is the replay
	// focus (ClientQueueCoolMoment), so no pawn capture is needed.
	if (Reason == FName("FlagCapture") && Holder != nullptr && Holder->Team != nullptr
		&& CTFGameState && !CTFGameState->HasMatchEnded() && !CTFGameState->IsMatchIntermission())
	{
		LastCapPlayer = Holder;
		LastCapTime   = GetWorld()->GetTimeSeconds();
	}

	Super::ScoreObject_Implementation(GameObject, HolderPawn, Holder, Reason);

	if (Holder != nullptr && Holder->Team != nullptr && !CTFGameState->HasMatchEnded() && !CTFGameState->IsMatchIntermission())
	{
		if (Reason == FName("FlagCapture"))
		{
			// Boost CoolFactor for all captures (for replay selection). The LastCap*
			// capture for the instant replay now happens BEFORE Super (above), so the
			// scorelimit-winning cap is recorded before it ends the match.
			Holder->AddCoolFactorEvent(200.0f);
		}
		else if (Reason == FName("SentHome"))
		{
			// Credit a flag return so a clutch defensive save is replay-eligible
			// (and can fill a secondary clip, or be featured if no cap ends the
			// match). Engine credits near-base denials in UTCTFFlag::Drop but
			// never plain returns. Below the 200 cap weight so a cap still wins.
			Holder->AddCoolFactorEvent(120.0f);
		}
	}
}

void ANCPlusCTFGameMode::HandleFlagCapture(AUTCharacter* HolderPawn, AUTPlayerState* Holder)
{
	if (CTFGameState->IsMatchInOvertime())
	{
		EndGame(Holder, FName(TEXT("GoldenCap")));
	}
	else
	{
		CheckScore(Holder);

		// Cap during advantage ends the half immediately
		if (NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage")) && !CTFGameState->HasMatchEnded())
		{
			// Track for replay: advantage cap is the "coolest" moment
			LastAdvantageCapPlayer = Holder;
			LastAdvantageCapTime = GetWorld()->GetTimeSeconds();
			bAdvantageCapEndedPeriod = true;

			// Extra CoolFactor boost to ensure this is selected as end-of-game replay
			// (200.0 already added in ScoreObject, this adds another 400.0 = 600 total)
			Holder->AddCoolFactorEvent(400.0f);

			EndOfHalf();
		}
	}
}

bool ANCPlusCTFGameMode::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (Scorer->Team != nullptr)
	{
		if (GoalScore > 0 && Scorer->Team->Score >= GoalScore)
		{
			EndGame(Scorer, FName(TEXT("scorelimit")));
		}
		else if (MercyScore > 0)
		{
			int32 Spread = Scorer->Team->Score;
			for (AUTTeamInfo* OtherTeam : Teams)
			{
				if (OtherTeam != Scorer->Team)
				{
					Spread = FMath::Min<int32>(Spread, Scorer->Team->Score - OtherTeam->Score);
				}
			}
			if (Spread >= MercyScore)
			{
				EndGame(Scorer, FName(TEXT("MercyScore")));
			}
		}
	}
	return true;
}

// ── End Game & Replay ────────────────────────────────────────────────

// ============================================================================
// DESIGN NOTE — iCTF/CTF end-of-match cap replay vs. the Map.h:527 client crash
//
// SYMPTOM (Shipping CLIENT, at match end):
//   Assertion failed: Pair != nullptr [Containers/Map.h Line:527]
//   last log lines ALWAYS: LogUTKillcam CoolMomentCamStart -> KillcamGoToTime -> crash.
//   = the stock end-of-match cool-moment KILLCAM DEMO SEEK
//   (UUTKillcamPlayback::KillcamGoToTime -> UDemoNetDriver::GotoTimeInSeconds) doing a
//   FindChecked() on a NetGUID it cannot resolve while reconstructing the rewound frame.
//
// THEORIES DISPROVEN BY MATCH LOGS — do NOT re-litigate these:
//   - NOT rewind distance      : a 13.97s rewind crashed, an 18.01s rewind survived.
//   - NOT seek depth / the map : CTF-Duku-v03 both crashed AND survived in one session/build.
//   - NOT cap-vs-frag frame, and NOT the +200/+400 cap CoolFactor boosts (live v326 ships
//     those and is crash-free).
//   evidence (seek-abs / match-len / result):
//       36.6 /  72.7  CRASH        66.9 /  80.9  CRASH
//      109.0 / 153.9  CRASH       187.0 / 205.0  OK   <- only the long match survived
//
// ROOT CAUSE = demo MATURITY. The seek reconstructs a past frame; in a young, still-settling
//   early-match world some actor's NetGUID isn't resolved yet -> FindChecked. A SHORT match
//   (e.g. 1 cap to win) can only ever seek early, so it ALWAYS crashes; a long match seeks
//   into a settled world and survives. Compounded because the DECIDING cap was never recorded
//   for the replay (the LastCap bookkeeping ran AFTER Super::ScoreObject, which had already
//   ended the match) -> the picker fell back to a stale frag -> an even earlier seek. The live
//   build "works for users" only because real matches run long enough; nobody plays 1-cap.
//
// WHY THE FIX IS SERVER-SIDE ONLY: the failing seek is stock Epic CLIENT code
//   (UTKillcamPlayback / UDemoNetDriver). We do NOT fork base UT — everything stays in the
//   plugin. So our only levers are WHEN the replay fires and WHICH frame it targets, both
//   decided here on the server. The client side is untouched (no client roll).
//
// THE FIX (all in this file):
//   1. ScoreObject records the decisive cap BEFORE Super (keeps a match-ending cap).
//   2. EndGame gates on server demo age (ncp.CTFReplayMinDemoSeconds): short match -> skip
//      entirely (no crash, no replay).
//   3. PickMostCoolMoments features ONLY that cap via stock ClientQueueCoolMoment, so the
//      seek lands at the newest / most-resolved end-of-match frame (NOT ClientPlayInstantReplay
//      — its real-pawn-GUID focus is a separate crash vector a prior session hit).
//   Tune ncp.CTFReplayMinDemoSeconds down to the real match-length floor. Residual edge: a
//   late-joining client has a shorter local killcam demo than the server, so the server-measured
//   maturity gate can't fully protect that client — not fixable without editing stock code.
// ============================================================================

// Minimum demo length (seconds) before the iCTF/CTF end-of-match replay will fire. The crash
// (stock client UUTKillcamPlayback::KillcamGoToTime -> GotoTimeInSeconds, Map.h:527 FindChecked
// on an unresolved NetGUID) happens when the rewound frame lands in a still-settling early-match
// world. Match logs proved it's demo AGE, not rewind distance or the map: short matches crash at
// any rewind (13.97s and 44.96s both crashed), while a 205s match survived a deep seek. We cannot
// harden the seek (it's stock Epic CLIENT code), so we gate on maturity: below this, skip the
// replay (no crash, no replay). Tune to your typical match length; 0 = always fire (not advised).
static TAutoConsoleVariable<float> CVarCTFReplayMinDemoSeconds(
	TEXT("ncp.CTFReplayMinDemoSeconds"), 200.f,
	TEXT("Min server demo seconds before the CTF/iCTF end-of-match cap replay fires (short matches crash the client killcam seek). 0 = always."));

// Seconds of build-up shown before the featured cap, so the run-up plays (not just the score frame).
static TAutoConsoleVariable<float> CVarCTFReplayBuildupSeconds(
	TEXT("ncp.CTFReplayBuildupSeconds"), 8.f,
	TEXT("Seconds of build-up before the featured decisive cap in the CTF/iCTF end-of-match replay."));

void ANCPlusCTFGameMode::EndGame(AUTPlayerState* Winner, FName Reason)
{
	// End-of-match cap replay, GATED ON DEMO MATURITY. The client killcam demo seek crashes
	// (Map.h:527) when the rewound frame lands in a still-settling early-match world; this is
	// per-match demo age, not rewind distance / cap-vs-frag / map (proven by logs — see the
	// CVarCTFReplayMinDemoSeconds note). The seek lives in stock Epic CLIENT code we don't edit,
	// so we only fire once the demo is old enough for the seek to resolve, and PickMostCoolMoments
	// features the just-scored decisive cap (newest, most-resolved frame). Short matches (1-cap
	// tests) fall below the gate and skip the replay entirely — no crash, no replay.
	const float DemoAge = (GetWorld()->DemoNetDriver != nullptr) ? GetWorld()->DemoNetDriver->DemoCurrentTime : 0.f;
	const float MinAge  = CVarCTFReplayMinDemoSeconds.GetValueOnGameThread();
	if (SupportsInstantReplay() && GetWorld()->DemoNetDriver != nullptr && DemoAge >= MinAge)
	{
		PickMostCoolMoments();
	}
	else if (GetWorld()->DemoNetDriver != nullptr)
	{
		UE_LOG(LogGameMode, Display, TEXT("NCPlusCTF: end-match replay skipped — demo %.1fs < min %.1fs (short-match killcam-seek crash guard)"), DemoAge, MinAge);
	}

	Super::EndGame(Winner, Reason);
}

void ANCPlusCTFGameMode::PickMostCoolMoments(bool bClearCoolMoments, int32 CoolMomentsToShow)
{
	const float Now = GetWorld()->TimeSeconds;

	// Feature ONLY the decisive cap (captured before Super in ScoreObject), via the stock
	// ClientQueueCoolMoment RPC — the same empty-focus seek the engine uses for cool moments,
	// NOT ClientPlayInstantReplay (its real-pawn-GUID focus is a separate crash vector that the
	// last session hit). A deciding cap was credited microseconds ago, so the rewind is tiny and
	// the seek lands at the newest, fully-resolved end-of-match frame. No recent cap (timelimit
	// end / stale cap) => skip rather than let stock PMCM seek to a possibly-early frag frame.
	AUTPlayerState* FeaturePS = LastCapPlayer.Get();
	const bool bRecentCap = FeaturePS != nullptr
		&& FeaturePS->UniqueId.IsValid()
		&& LastCapTime > 0.f
		&& (Now - LastCapTime) <= FeatureCapMaxAgeSeconds;

	if (!bRecentCap)
	{
		UE_LOG(LogGameMode, Display, TEXT("NCPlusCTF: end-match replay skipped — no recent decisive cap to feature"));
		return;
	}

	const float Rewind = (Now - LastCapTime) + CVarCTFReplayBuildupSeconds.GetValueOnGameThread();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->ClientQueueCoolMoment(FeaturePS->UniqueId, Rewind);
		}
	}
	UE_LOG(LogGameMode, Warning, TEXT("NCPlusCTF replay: featured decisive cap by %s (rewind %.1fs, demo %.1fs)"),
		*FeaturePS->PlayerName, Rewind,
		(GetWorld()->DemoNetDriver != nullptr) ? GetWorld()->DemoNetDriver->DemoCurrentTime : 0.f);
}

// ── Match State Handlers ─────────────────────────────────────────────

void ANCPlusCTFGameMode::HandleMatchHasStarted()
{
	// Preserve the locked-ready identity/team snapshot into live play. A hard
	// network loss in the final detection window may not cross the silence
	// threshold until just after StartMatch; retaining this data still rejects a
	// spectator or wrong-side reconnect (including an unlinked player). Read only
	// the frozen ReadyPlayers pointers, not PlayerArray duplicates from reconnects.
	const bool bSecondHalfStart = CTFGameState != nullptr
		&& NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf"));
	if (HasAuthority() && AutoPauseReadyCountdownIds.Num() > 0)
	{
		if (bSecondHalfStart)
		{
			// Side switching applies to current/replacement PlayerStates; the original
			// ReadyPlayers pointer may have been destroyed by a first-half reconnect.
			RefreshReadyParticipantTeamsFromActivePlayers();
		}
		else
		{
			CaptureLockedReadyParticipants(ANCReadyUpState::Find(GetWorld()));
		}
	}

	// Epic restarts every player synchronously inside Super::HandleMatchHasStarted.
	// Load spawn tuning and discard warmup queue/history before that happens so
	// the opening live spawns use the configured algorithm and remain recorded.
	// RatingSystem is constructed below on this same first call and stays valid
	// through halftime, making this a once-per-map initialization guard.
	const bool bFirstLiveStart = HasAuthority() && !RatingSystem.IsValid();
	if (bFirstLiveStart)
	{
		LoadSpawnConfig();
		bSpawnPoolsBuilt = false;
		Team0Spawns.Reset();
		Team1Spawns.Reset();
		SpawnLastUsedTime.Reset();
		PlayerRecentSpawns.Reset();
		PlayerLastSpawnLoc.Reset();
		RobbedSpawnRotation[0] = 0;
		RobbedSpawnRotation[1] = 0;
		ClearCachedRespawnChoices();
	}

	// Only call super (which starts replay recording, announces match, etc.) on first half.
	// Non-halftime games always call super (there's no second half).
	if (!bHasHalftime || !NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf")))
	{
		Super::HandleMatchHasStarted();
	}
	if (HasAuthority() && AutoPauseReadyCountdownIds.Num() > 0)
	{
		// Super may perform the final stock balance. Initial start trusts the frozen
		// pointers; later halves aggregate current active copies without accepting a
		// spectator or an ambiguous duplicate for that ID.
		if (bSecondHalfStart)
		{
			RefreshReadyParticipantTeamsFromActivePlayers();
		}
		else
		{
			CaptureLockedReadyParticipants(ANCReadyUpState::Find(GetWorld()));
		}
	}

	// Spawn CTF stats replicator for scoreboard (grabs, accuracy).
	// MUST stay in HandleMatchHasStarted — spawning in BeginPlay or earlier
	// is documented to cause client crashes (see ElimPlusGame.cpp:236-237 +
	// WipeoutGame.cpp:254-255). Side effect: scoreboard's instagib-vs-normal
	// layout falls back to NormalLayout during warmup because the replicator
	// doesn't exist yet. Accepted trade-off vs the crash risk.
	if (HasAuthority() && !CTFStatsRep)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		CTFStatsRep = GetWorld()->SpawnActor<ACTFStatsReplicator>(SpawnParams);
	}

	// Per-weapon hits/shots replicator for the accuracy HUD widget.
	ANCAccuracyStatsReplicator::EnsureSpawned(this);

	// Spawn OT info replicator so the HUD can render an OT count-up clock.
	// OvertimeStartElapsed gets stamped in HandleEnteringOvertime / the
	// SetMatchState(MatchIsInOvertime) sites.
	if (HasAuthority() && !OTInfo)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		OTInfo = GetWorld()->SpawnActor<ANCPlusCTFOTInfo>(SpawnParams);
	}
	if (OTInfo)
	{
		// Mirror gamemode halftime decision so HUD can suppress "1st Half" label.
		OTInfo->bHasHalftime = bHasHalftime;
	}

	// iCTF only: replace Epic's immediate lift-crush flag return with a swept,
	// safe relocation that preserves the normal auto-return timer. The guard
	// remains spawned when its runtime CVar is off so it can be re-enabled live.
	if (HasAuthority() && bIsInstagib)
	{
		ANCICTFFlagLiftGuard::EnsureSpawned(GetWorld());
	}

	// Rating system: construct ONCE per map load, locking in bIsInstagib now
	// that the mutator chain is reliably settled. The `!IsValid()` guard makes
	// the halftime second call a no-op so we don't reconstruct mid-match.
	// Bulk-load every already-connected human from PlayerArray — warmup
	// PostLogins were skipped because the rating system didn't exist yet.
	if (HasAuthority() && !RatingSystem.IsValid())
	{
		AutoPauseTrackedIds.Reset();
		const bool bInstagib = bIsInstagib;
		RatingSystem = MakeUnique<FNCPlusCTFRatingSystem>(bInstagib);
		UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF: rating system constructed for %s ladder"),
			bInstagib ? TEXT("iCTF") : TEXT("CTF"));

		// Stamp the match-start clock + intended full length for the leaver
		// presence threshold. Runtime perf knobs were loaded during InitGame so
		// AutoPauseOnDrop is already authoritative during the ready countdown.
		MatchStartWorldTime = GetWorld()->GetTimeSeconds();
		MatchFullDurationSeconds = (bHasHalftime ? float(TimeLimit) * 2.f : float(TimeLimit)) * 60.f;

		// Apply the regulation respawn wait, keyed on match size so bot-hosted AND
		// hub-hosted (ruleset) games both get it automatically from MaxPlayers - no
		// ?RespawnWait option required. Small games (1v1: MaxPlayers <=
		// CTFSmallGameMaxPlayers) use CTFRespawnWaitSmall (1.0s); larger use
		// CTFRespawnWait (1.5s). This is the authoritative value: the respawn
		// countdown reads GameState->RespawnWaitTime (MAX'd with the per-player
		// field, which CTF never raises in-match), and this runs after InitGame's
		// ?RespawnWait parse so it wins. Supports fractional values; overtime
		// escalation (DefaultTimer) still ramps respawn up from here.
		const int32 MaxP = GameSession ? GameSession->MaxPlayers : 10;
		const float WantRespawn = (MaxP <= CTFSmallGameMaxPlayers) ? CTFRespawnWaitSmall : CTFRespawnWait;
		RespawnWaitTime = WantRespawn;
		if (CTFGameState)
		{
			CTFGameState->SetRespawnWaitTime(WantRespawn);
		}
		UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF respawn: MaxPlayers=%d -> %.2fs (<=%d uses %.2f, else %.2f)"),
			MaxP, WantRespawn, CTFSmallGameMaxPlayers, CTFRespawnWaitSmall, CTFRespawnWait);

		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS)
		{
			int32 LoadedCount = 0;
			for (APlayerState* PS : GS->PlayerArray)
			{
				AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
				if (!UTPS || UTPS->bOnlySpectator) continue;
				if (!UTPS->UniqueId.IsValid()) continue;   // bot
				const FString Uid = UTPS->UniqueId.ToString();
				if (bIsPugMatch && UTPS->GetTeamNum() <= 1)
				{
					AutoPauseTrackedIds.Add(Uid);
				}
				RatingSystem->LoadPlayerFromDB(GetWorld(), Uid);
				// Present at match start => full presence credit.
				if (!PlayerJoinWorldTime.Contains(Uid))
				{
					PlayerJoinWorldTime.Add(Uid, MatchStartWorldTime);
				}
				++LoadedCount;
			}
			UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF: bulk-loaded %d human ratings from PlayerArray"), LoadedCount);
		}
	}
}

void ANCPlusCTFGameMode::HandleMatchIntermission()
{
	Super::HandleMatchIntermission();

	// Clear advantage state for halftime
	NCPlusReflection::SetBool(CTFGameState, TEXT("bPlayingAdvantage"), false);
	NCPlusReflection::SetByte(CTFGameState, TEXT("AdvantageTeamIndex"), 255);
	bGracePeriodActive = false;
	bAdvantageCapEndedPeriod = false;
	if (OTInfo)
	{
		OTInfo->AdvantageStartElapsed = -1;
	}

	BroadcastLocalized(this, UUTCTFMajorMessage::StaticClass(), 11, nullptr, nullptr, nullptr);
}

void ANCPlusCTFGameMode::HandleExitingIntermission()
{
	const bool bWasSecondHalf = NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf"));
	ClearCachedRespawnChoices();
	AUTWorldSettings* Settings = GetWorld() ? Cast<AUTWorldSettings>(GetWorld()->GetWorldSettings()) : nullptr;
	if (Settings && NCPlusReflection::GetBool(Settings, TEXT("bAllowSideSwitching")))
	{
		// Super swaps every team actor (including AUTTeamPlayerStart::TeamNum)
		// before it synchronously respawns the players. Invalidate the cached
		// authored-team queues now so the first respawn rebuilds them after the swap.
		bSpawnPoolsBuilt = false;
		Team0Spawns.Reset();
		Team1Spawns.Reset();
	}

	// Set before calling super so HandleMatchHasStarted can read it
	NCPlusReflection::SetBool(CTFGameState, TEXT("bSecondHalf"), true);

	Super::HandleExitingIntermission();

	if (bWasSecondHalf)
	{
		// Coming out of overtime intermission
		SetMatchState(MatchState::MatchEnteringOvertime);
		CTFGameState->SetTimeLimit(0);
	}
}

void ANCPlusCTFGameMode::HandleEnteringOvertime()
{
	OvertimeStartWorldTime = GetWorld()->GetTimeSeconds();
	if (OTInfo && CTFGameState)
	{
		OTInfo->OvertimeStartElapsed = CTFGameState->ElapsedTime;
		OTInfo->AdvantageStartElapsed = -1;        // OT replaces advantage on the HUD
	}
	CTFGameState->SetTimeLimit(6000);
	SetMatchState(MatchState::MatchIsInOvertime);
	NCPlusReflection::SetBool(CTFGameState, TEXT("bPlayingAdvantage"), false);
	bGracePeriodActive = false;
}

void ANCPlusCTFGameMode::HandleMatchInOvertime()
{
	// Every OT entry dispatches here, including EndOfHalf's direct
	// SetMatchState(MatchIsInOvertime) — the only path 3v3+ games take, since
	// HandleEnteringOvertime (the other stamp site) is reachable solely via the
	// <=4-player halftime config. Unstamped, the respawn ramp read 0, computed
	// OTElapsed as total world uptime, and opened at the 10s cap on OT's first
	// tick instead of holding the 2s base for OvertimeEscalationDelay.
	if (OvertimeStartWorldTime <= 0.f)
	{
		OvertimeStartWorldTime = GetWorld()->GetTimeSeconds();
	}
	BroadcastLocalized(this, UUTCTFMajorMessage::StaticClass(), 12, nullptr, nullptr, nullptr);
	NCPlusReflection::SetBool(CTFGameState, TEXT("bPlayingAdvantage"), false);
	bGracePeriodActive = false;
}

bool ANCPlusCTFGameMode::PlayerCanRestart_Implementation(APlayerController* Player)
{
	if (Player == nullptr || Player->IsPendingKillPending())
	{
		return false;
	}

	if (!HasMatchStarted())
	{
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(Player);
		if (!UTPC || !UTPC->UTPlayerState || !UTPC->UTPlayerState->bIsWarmingUp)
		{
			return false;
		}
	}
	else if (!CTFGameState->IsMatchInProgress() || CTFGameState->IsMatchIntermission())
	{
		return false;
	}

	return Player->CanRestartPlayer();
}

// ── Debug ────────────────────────────────────────────────────────────

void ANCPlusCTFGameMode::GetGood()
{
#if !(UE_BUILD_SHIPPING)
	if (GetNetMode() == NM_Standalone)
	{
		Super::GetGood();
		IntermissionDuration = 5;
		Teams[0]->Score = 1;
		Teams[1]->Score = 9;
	}
#endif
}

// ── Server Browser & Config ──────────────────────────────────────────

void ANCPlusCTFGameMode::BuildServerResponseRules(FString& OutRules)
{
	OutRules += FString::Printf(TEXT("Goal Score\t%i\t"), GoalScore);
	OutRules += FString::Printf(TEXT("Time Limit\t%i\t"), TimeLimit);

	if (TimeLimit > 0)
	{
		if (IntermissionDuration <= 0)
		{
			OutRules += FString::Printf(TEXT("No Halftime\tTrue\t"));
		}
		else
		{
			OutRules += FString::Printf(TEXT("Halftime\tTrue\t"));
			OutRules += FString::Printf(TEXT("Halftime Duration\t%i\t"), IntermissionDuration);
		}
	}

	OutRules += FString::Printf(TEXT("Advantage Duration\t%i\t"), AdvantageMaxDuration);
	OutRules += FString::Printf(TEXT("Grace Period\t%i\t"), GracePeriodDuration);

	AUTMutator* Mut = BaseMutator;
	while (Mut)
	{
		OutRules += FString::Printf(TEXT("Mutator\t%s\t"), *Mut->DisplayName.ToString());
		Mut = Mut->NextMutator;
	}
}

void ANCPlusCTFGameMode::CreateGameURLOptions(TArray<TSharedPtr<TAttributePropertyBase>>& MenuProps)
{
	Super::CreateGameURLOptions(MenuProps);
	MenuProps.Add(MakeShareable(new TAttributeProperty<int32>(this, &MercyScore, TEXT("MercyScore"))));
	MenuProps.Add(MakeShareable(new TAttributeProperty<int32>(this, &AdvantageMaxDuration, TEXT("AdvantageMaxDuration"))));
	MenuProps.Add(MakeShareable(new TAttributeProperty<int32>(this, &GracePeriodDuration, TEXT("GracePeriod"))));
}

// NOTE: CreateConfigWidgets is NOT overridden here because SUTStyle/SUWindowsStyle
// are private headers of the game module that plugins cannot access. The parent
// AUTCTFGameMode::CreateConfigWidgets provides the standard CTF UI. Our custom
// settings (AdvantageMaxDuration, GracePeriod, MercyScore) are exposed via
// CreateGameURLOptions and can be set via URL params or config.

// --- Mod.ini-gated match-host pause (see NCPlusHostPause.h) ---

bool ANCPlusCTFGameMode::AllowPausing(APlayerController* PC)
{
	// Stock permissions (rcon admin / listen with no remotes) are preserved; this ADDS
	// the ?HostId= match host ([NetcodePlus] bAllowHostPause) AND the two bot-designated
	// team captains ([NetcodePlus] bAllowCaptainPause, ?Captains=) — see NCPlusHostPause.
	return Super::AllowPausing(PC) || NCPlusHostPause::MayPause(PC, this);
}

bool ANCPlusCTFGameMode::ClearPause()
{
	if (bForceClearingPauseActor)
	{
		AWorldSettings* CleanupWS = GetWorldSettings();
		const bool bWasPaused = CleanupWS != nullptr && CleanupWS->Pauser != nullptr;
		const bool bResult = Super::ClearPause();
		if (bWasPaused && CleanupWS != nullptr && CleanupWS->Pauser == nullptr)
		{
			CleanupWS->ForceNetUpdate();
			NCPlusHostPause::ResyncServerWorldTime(this);
		}
		return bResult;
	}

	// Automatic pauses own one authoritative, cancellable countdown. Never let
	// the independent host/rcon ticker race it.
	if (bAutoPaused)
	{
		AWorldSettings* AutoPauseWS = GetWorldSettings();
		if (bAutoPauseDormantNoMarker)
		{
			if (AutoPauseWS == nullptr || AutoPauseWS->Pauser == nullptr)
			{
				// No world pause remains to count down against. This cannot be the
				// departing-marker cleanup path (that still has a non-null Pauser),
				// so treat it as an explicit server/rcon escape from the preserved
				// logical wait and clear the authoritative snapshot immediately.
				CompleteAutoPauseResume(TEXT("Dormant unpause requested"));
				return true;
			}
			// A live caller has supplied a new physical marker to a formerly
			// markerless logical pause. Adopt it and honor the requested resume via
			// the same authoritative countdown as every other auto-pause.
			bAutoPauseDormantNoMarker = false;
			NCPlusHostPause::CancelDeferredUnpause(this);
			BeginAutoPauseResumeCountdown(TEXT("Dormant unpause requested"));
			return false;
		}
		if (AutoPauseWS == nullptr || AutoPauseWS->Pauser == nullptr)
		{
			// Defensive recovery for an externally cleared physical marker.
			bAutoPauseDormantNoMarker = true;
			return false;
		}
		NCPlusHostPause::CancelDeferredUnpause(this);
		BeginAutoPauseResumeCountdown(TEXT("Unpause requested"));
		return false;
	}

	// Host/rcon unpause: hold behind a short server-only resume countdown
	// (Mod.ini [NetcodePlus] UnpauseCountdownSec). Only engages while actually paused.
	if (NCPlusHostPause::DeferUnpauseForCountdown(this))
	{
		return false;
	}
	AWorldSettings* WS = GetWorldSettings();
	const bool bWasPaused = WS != nullptr && WS->Pauser != nullptr;
	const bool bResult = Super::ClearPause();
	if (bWasPaused && WS != nullptr && WS->Pauser == nullptr)
	{
		WS->ForceNetUpdate();
		NCPlusHostPause::ResyncServerWorldTime(this);
	}
	return bResult;
}
