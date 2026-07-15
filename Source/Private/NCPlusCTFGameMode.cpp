// NCPlusCTFGameMode.cpp - NetcodePlus CTF with improved advantage time and instant replay
#include "NCPlusCTFGameMode.h"
#include "NCFireValCollector.h"
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
#include "WarmupRoamMutator.h"
#include "NCPlusVersionGate.h"
#include "CTFStatsReplicator.h"
#include "NCAccuracyStatsReplicator.h"
#include "NCPlusCTFOTInfo.h"
#include "NCPlusCTFRatingSystem.h"
#include "NCEloUploader.h"
#include "UTGameMode.h"     // AUTGameMode::bIsInstagib
#include "UTATypes.h"                  // NAME_FCKills / NAME_FlagSupportKills / NAME_FlagGrabs / ...
#include "Misc/ConfigCacheIni.h"       // FConfigFile — Mod.ini perf knobs
#include "Misc/Paths.h"                // FPaths::GameSavedDir

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

	// Spawn selection (tie-band + freshness; tunable via Mod.ini [UTPUGS_SPAWN])
	SpawnTieBandWidth = 2.0f;           // starts within ~2 score pts of best = a coin-flip
	SpawnFreshnessBonus = 10.0f;        // when calm, lift unused starts to spread spawns
	SpawnFreshnessWindow = 30.0f;       // 30s since last use = fully fresh
	SpawnFlagVicinityRadius = 4000.f;   // flag within this of our base = "in the vicinity"
	SpawnKillerAvoidRadius = 2500.f;    // never respawn within this of your last killer (anti-camp)
	SpawnFlagCarrierLOSAvoidRadius = 3500.f; // prefer starts out of the EFC's direct sightline
	SpawnRobbedBaseAvoidCount = 2.f;    // when our flag's out, the 2 deepest base spawns form the avoid set — ONE blocked per respawn, alternating

	bHasHalftime = true;                // Default true; auto-set false for 3v3+ in InitGame
	bAllowFloorSlide = true;            // Enabled by default; set false in BP for Sniper CTF etc.
	OvertimeRespawnTime = 6.f;          // Fixed 6s respawn in overtime (replaces Epic's 10s escalation)

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
	}
}

void ANCPlusCTFGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// Early version check — kicks outdated/missing-plugin clients within 10s of
	// PostLogin, BEFORE they can play meaningful warmup. Replaces the BP check
	// that fires at match start (which let them roam the map during warmup,
	// breaking PUGs when they got kicked at go-time). Skips bots + listen host.
	NCPlusVersionGate::SpawnFor(NewPlayer);

	if (!HasAuthority() || !NewPlayer) return;

	// Warmup joiner: rating system isn't constructed yet. They'll be picked up
	// en-masse by the GS->PlayerArray walk in HandleMatchHasStarted.
	// Late joiner (mid-match): rating system already exists, load immediately.
	if (!RatingSystem.IsValid()) return;

	AUTPlayerState* UTPS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (UTPS && UTPS->UniqueId.IsValid())
	{
		const FString Uid = UTPS->UniqueId.ToString();
		RatingSystem->LoadPlayerFromDB(GetWorld(), Uid);
		// Mid-match joiner: stamp first-seen time for the leaver presence calc.
		// (Warmup joiners are stamped en-masse in HandleMatchHasStarted.) Keep
		// the earliest sighting on a rejoin.
		if (!PlayerJoinWorldTime.Contains(Uid))
		{
			PlayerJoinWorldTime.Add(Uid, GetWorld()->GetTimeSeconds());
		}

		// Auto-pause: an awaited drop just rejoined — resume once everyone we're
		// waiting on is back.
		if (bAutoPaused && AutoPauseAwaitIds.Contains(Uid))
		{
			AutoPauseAwaitIds.Remove(Uid);
			UE_LOG(LogGameMode, Warning, TEXT("NCPlusCTF auto-pause: %s rejoined (%d still out)"),
				*UTPS->PlayerName, AutoPauseAwaitIds.Num());
			if (AutoPauseAwaitIds.Num() == 0)
			{
				EndAutoPause(TEXT("all dropped players rejoined"));
			}
		}
	}
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
					return true;
				}
				return MovePlayerToTeam(Player, PS, Want);
			}
		}
	}

	return Super::ChangeTeam(Player, NewTeam, bBroadcast);
}

void ANCPlusCTFGameMode::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();

	FNCFireValCollector::Get().ReportOnce(GetWorld());   // emit [FireVal] + CSV (guards double-route)

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
	// rejoin (or an admin unpauses). Server-only; uses the engine world-pause
	// (WorldSettings->Pauser) — the same primitive as the `pause` command.
	// Runs BEFORE Super (the leaver's PlayerState is still intact here).
	if (HasAuthority() && bAutoPauseOnDrop && bIsPugMatch && Exiting && CTFGameState
		&& (CTFGameState->IsMatchInProgress() || CTFGameState->IsMatchInOvertime()))
	{
		AUTPlayerState* LeavePS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (LeavePS && !LeavePS->bIsABot && !LeavePS->bOnlySpectator
			&& LeavePS->UniqueId.IsValid() && LeavePS->GetTeamNum() <= 1)
		{
			BeginOrHoldAutoPause(LeavePS->UniqueId.ToString(), LeavePS->PlayerName);
		}
	}

	Super::Logout(Exiting);
}

APlayerState* ANCPlusCTFGameMode::FindAutoPauseMarker() const
{
	// A present, non-spectator player who hasn't dropped — used as the engine
	// pause marker (WorldSettings->Pauser must be non-null to hold the pause).
	if (!CTFGameState) return nullptr;
	for (APlayerState* PS : CTFGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (UTPS && !UTPS->bOnlySpectator && UTPS->UniqueId.IsValid()
			&& !AutoPauseAwaitIds.Contains(UTPS->UniqueId.ToString()))
		{
			return UTPS;
		}
	}
	return nullptr;
}

void ANCPlusCTFGameMode::BeginOrHoldAutoPause(const FString& LeaverId, const FString& LeaverName)
{
	AWorldSettings* WS = GetWorldSettings();
	if (!WS) return;

	AutoPauseAwaitIds.Add(LeaverId);

	// (Re)point the pause marker at a still-present player — never a leaver (their
	// PlayerState is torn down in Super::Logout). If nobody's left, nothing to do.
	APlayerState* Marker = FindAutoPauseMarker();
	if (!Marker)
	{
		if (bAutoPaused) { EndAutoPause(TEXT("no players remain")); }
		else { AutoPauseAwaitIds.Reset(); }
		return;
	}

	WS->Pauser = Marker;   // engine world-pause; replicated, clients show paused
	if (!bAutoPaused)
	{
		bAutoPaused = true;
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: %s dropped — match PAUSED until rejoin (or admin unpause). awaiting=%d"),
			*LeaverName, AutoPauseAwaitIds.Num());
	}
	else
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("NCPlusCTF auto-pause: %s also dropped while paused — awaiting=%d"),
			*LeaverName, AutoPauseAwaitIds.Num());
	}
}

void ANCPlusCTFGameMode::EndAutoPause(const TCHAR* Reason)
{
	if (AWorldSettings* WS = GetWorldSettings())
	{
		WS->Pauser = nullptr;
	}
	bAutoPaused = false;
	AutoPauseAwaitIds.Reset();
	UE_LOG(LogGameMode, Warning, TEXT("NCPlusCTF auto-pause: resuming (%s)"), Reason);
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

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF perf config: enabled=%s shadow=%s objW=%.2f feeder=%.2f minPresence=%.2f roleAware=%s roleStr=%.2f combatW=%.1f respawn=%.2f autoPause=%s"),
		CTFPerfConfig.bEnabled ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.bShadow ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.ObjectiveWeight, CTFPerfConfig.FeederPenalty, CTFRatingMinPresenceFrac,
		CTFPerfConfig.bRoleAware ? TEXT("true") : TEXT("false"), CTFPerfConfig.RoleWeightStrength, CTFRoleCombatWeight, CTFRespawnWait,
		bAutoPauseOnDrop ? TEXT("true") : TEXT("false"));
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
	ReadFloat(TEXT("SpawnTieBandWidth"),       SpawnTieBandWidth);
	ReadFloat(TEXT("SpawnFreshnessBonus"),     SpawnFreshnessBonus);
	ReadFloat(TEXT("SpawnFreshnessWindow"),    SpawnFreshnessWindow);
	ReadFloat(TEXT("SpawnFlagVicinityRadius"), SpawnFlagVicinityRadius);
	ReadFloat(TEXT("SpawnKillerAvoidRadius"),  SpawnKillerAvoidRadius);
	ReadFloat(TEXT("SpawnFlagCarrierLOSAvoidRadius"), SpawnFlagCarrierLOSAvoidRadius);
	ReadFloat(TEXT("SpawnRobbedBaseAvoidCount"), SpawnRobbedBaseAvoidCount);

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF spawn config [UTPUGS_SPAWN]: tieBand=%.1f freshBonus=%.1f freshWin=%.0f flagVic=%.0f killerAvoid=%.0f efcLOSAvoid=%.0f robbedAvoid=%.0f | flagPen=%.1f dropPen=%.1f enemyBlk=%.1f/%.0f enemyLOS=%.1f/%.0f"),
		SpawnTieBandWidth, SpawnFreshnessBonus, SpawnFreshnessWindow, SpawnFlagVicinityRadius, SpawnKillerAvoidRadius, SpawnFlagCarrierLOSAvoidRadius, SpawnRobbedBaseAvoidCount,
		FlagCarrierSpawnPenalty, DroppedFlagSpawnPenalty, EnemyBlockPenalty, EnemyBlockRange, EnemyLOSPenalty, EnemyLOSBlockRange);
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

// ── Floor Slide ─────────────────────────────────────────────────────
// Floor slide disable temporarily removed — needs reimplementation
// without changing TeamArenaCharacter class layout
void ANCPlusCTFGameMode::RestartPlayer(AController* NewPlayer)
{
	// Idempotency guard: a RestartPlayer on a controller that ALREADY has a living pawn
	// must be a no-op. The engine reuses the existing pawn but still re-runs
	// SetPlayerDefaults -> GiveDefaultInventory, and stock AddInventory dedupes only by
	// instance (not class), so a second warmup RestartPlayer would grant a second copy of
	// every default weapon = the doubled weapon bar. First spawn (no pawn) is unaffected;
	// the anti-repeat tracking below only has fresh StartSpot data when a spawn occurred.
	if (NewPlayer && NewPlayer->GetPawn())
	{
		return;
	}

	Super::RestartPlayer(NewPlayer);

	// Anti-repeat tracking (IG+ style): keep PlayerRecentSpawns updated from the
	// chosen StartSpot so RatePlayerStart's anti-repeat has data. Runs every spawn
	// (incl. warmup) — harmless.
	if (NewPlayer && NewPlayer->StartSpot.IsValid())
	{
		APlayerStart* UsedStart = Cast<APlayerStart>(NewPlayer->StartSpot.Get());
		if (UsedStart)
		{
			FRecentSpawns& Recent = PlayerRecentSpawns.FindOrAdd(TWeakObjectPtr<AController>(NewPlayer));
			Recent.SecondLast = Recent.Last;
			Recent.Last = UsedStart;
		}
	}

	// Spawn log — gated to LIVE match (warmup respawns were inflating the count so it
	// never matched real deaths) and reporting the PAWN's ACTUAL world location. The
	// previous version logged NewPlayer->StartSpot, which can lag the real per-life
	// spawn and falsely read "same"; StartSpot is printed alongside so any divergence
	// from the pawn is visible. Warning verbosity to survive the Shipping UE_LOG strip.
	APawn* SpawnedPawnForLog = NewPlayer ? NewPlayer->GetPawn() : nullptr;
	if (SpawnedPawnForLog && CTFGameState && CTFGameState->IsMatchInProgress())
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
	// Small games (1v1, 2v2): use Epic's default spawn system only.
	// Custom flag + enemy penalties make all spawns equally bad when there
	// aren't enough spawn points to avoid both flags and enemies.
	// Epic's base system handles small games well (last-killer avoidance,
	// 0.2f score floor, ChoosePlayerStart fallback).
	if (GameSession && GameSession->MaxPlayers <= 4)
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

			// Distance-based penalty: closer enemy = bigger penalty
			if (EnemyBlockRange > 0.f && DistToEnemy < EnemyBlockRange)
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
			// Penalize using the spawn from 2 lives ago
			if (Recent->SecondLast.IsValid() && Recent->SecondLast.Get() == P)
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
// We own ChoosePlayerStart (mirroring ElimPlus/Wipeout) so the engine spawn
// pipeline can't bypass RatePlayerStart. On maps whose UTTeamPlayerStart tags
// are unreliable, the stock pipeline was starving the rating loop (-20 on every
// candidate) and falling through to a deterministic engine pick — pinning every
// player to one spawn for the whole match. Here the candidate pool is curated
// per-team by nearest flag base (ground truth via GetFlagBase), each team start
// is retagged so the bUseTeamStarts=true -20 guard trusts geometry, and the pool
// is scored with the existing RatePlayerStart (flag-state, enemy/LOS, anti-repeat).

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

	bSpawnPoolsBuilt = true;

	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF spawn-pools built: T0=%d T1=%d team starts (by author TeamNum; %d skipped non-0/1)"),
		Team0Spawns.Num(), Team1Spawns.Num(), Skipped);
}

AActor* ANCPlusCTFGameMode::ChoosePlayerStart_Implementation(AController* Player)
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
	TArray<float> DistOwnBase;
	Cands.Reserve(Pool.Num());
	Scores.Reserve(Pool.Num());
	KillerAdj.Reserve(Pool.Num());
	EFCLOSAdj.Reserve(Pool.Num());
	DistOwnBase.Reserve(Pool.Num());
	static FName NAME_CTFSpawnEFCLOS = FName(TEXT("CTFSpawnEFCLOS"));
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

	// Lexicographic safety tier: last-killer clearance is highest priority, then
	// EFC LOS clearance, then the rotating robbed-base exclusion. Each lower bit
	// can only decide between candidates tied on every higher-priority rule. If all
	// starts violate a rule, that bit is absent from every tier and spawning still
	// succeeds using the remaining protections.
	uint8 BestSafetyTier = 0;
	auto GetSafetyTier = [&](int32 i) -> uint8
	{
		const bool bKillerClear = !(bHaveKiller && KillerAdj[i]);
		const bool bEFCClear = !(EnemyFlagCarrier && EFCLOSAdj[i]);
		return (bKillerClear ? 4 : 0) | (bEFCClear ? 2 : 0) | (!RobbedAdj[i] ? 1 : 0);
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

	// Collect the tie-band (all within SpawnTieBandWidth of the best) and pick one at random.
	TArray<APlayerStart*> TopBand;
	for (int32 i = 0; i < Cands.Num(); ++i)
	{
		if (!Eligible(i)) { continue; }
		if (Scores[i] >= BestScore - SpawnTieBandWidth) { TopBand.Add(Cands[i]); }
	}

	APlayerStart* Best = (TopBand.Num() > 0) ? TopBand[FMath::RandRange(0, TopBand.Num() - 1)] : nullptr;
	if (!Best)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Record team-wide use for the freshness spread — only count real in-match spawns
	// so warmup / pre-match prepopulation picks don't pollute staleness.
	if (CTFGameState && CTFGameState->IsMatchInProgress())
	{
		SpawnLastUsedTime.Add(Best, Now);
	}

	// Confirmation log (Warning survives the Shipping UE_LOG strip). Pairs with the
	// "NCPlusCTF spawn:" line in RestartPlayer to show selection is ours + rotating.
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
		*PS->PlayerName, TeamIndex, *Best->GetName(), (uint32)BestSafetyTier, TopBand.Num(), bForceFresh ? 1 : 0,
		KillerBlocked, EFCLOSBlocked, RobbedBlocked, Pool.Num());

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

	bool bAnyFlagHeld = IsAnyFlagHeld();

	if (bAnyFlagHeld)
	{
		// A flag is being held - advantage continues, cancel any grace period
		if (bGracePeriodActive)
		{
			CancelGracePeriod();
		}
		return true;
	}

	// No flags held - either we're already in grace or need to start it
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

void ANCPlusCTFGameMode::DefaultTimer()
{
	Super::DefaultTimer();

	// Role-aware ratings: sample every living player's map zone once per second
	// (DefaultTimer is 1Hz) while the match is live. Self-guards on match state.
	SampleRoleDwell();

	// Tick advantage timer (DefaultTimer fires every second).
	// Advantage lasts up to AdvantageMaxDuration (5 min default).
	// All flags home → instant end (NewCTF style, no grace period).
	if (CTFGameState && NCPlusReflection::GetBool(CTFGameState, TEXT("bPlayingAdvantage")) && CTFGameState->IsMatchInProgress())
	{
		// All flags home → end immediately (NewCTF: IsEveryFlagHome check in Timer)
		if (AreAllFlagsHome())
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

	// Overtime respawn escalation (NewCTF style): base respawn for first 5 minutes,
	// then escalate +1s per 2 minutes, capping at OvertimeRespawnTime (6s). Only for 3v3+.
	// Ramp: 0-5min=2s, 5-7min=3s, 7-9min=4s, 9-11min=5s, 11min+=6s
	if (CTFGameState && CTFGameState->IsMatchInOvertime() && OvertimeRespawnTime > 0.f
		&& GameSession && GameSession->MaxPlayers > 4)
	{
		float OTElapsed = GetWorld()->GetTimeSeconds() - OvertimeStartWorldTime;
		float BaseRespawn = 2.f;
		float OTEscalationStartTime = 300.f; // 5 minutes before escalation begins
		int32 ExtraSeconds = (OTElapsed > OTEscalationStartTime)
			? FMath::FloorToInt((OTElapsed - OTEscalationStartTime) / 120.f)
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
	// Only call super (which starts replay recording, announces match, etc.) on first half.
	// Non-halftime games always call super (there's no second half).
	if (!bHasHalftime || !NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf")))
	{
		Super::HandleMatchHasStarted();
		FNCFireValCollector::Get().Reset();   // first half only — accumulate samples across both halves
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

	// Rating system: construct ONCE per map load, locking in bIsInstagib now
	// that the mutator chain is reliably settled. The `!IsValid()` guard makes
	// the halftime second call a no-op so we don't reconstruct mid-match.
	// Bulk-load every already-connected human from PlayerArray — warmup
	// PostLogins were skipped because the rating system didn't exist yet.
	if (HasAuthority() && !RatingSystem.IsValid())
	{
		const bool bInstagib = bIsInstagib;
		RatingSystem = MakeUnique<FNCPlusCTFRatingSystem>(bInstagib);
		UE_LOG(LogGameMode, Log, TEXT("NCPlusCTF: rating system constructed for %s ladder"),
			bInstagib ? TEXT("iCTF") : TEXT("CTF"));

		// Stamp the match-start clock + intended full length for the leaver
		// presence threshold, and load the runtime perf knobs (once per map).
		MatchStartWorldTime = GetWorld()->GetTimeSeconds();
		MatchFullDurationSeconds = (bHasHalftime ? float(TimeLimit) * 2.f : float(TimeLimit)) * 60.f;
		LoadCTFPerfConfig();
		LoadSpawnConfig();

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
#include "NCPlusHostPause.h"

bool ANCPlusCTFGameMode::AllowPausing(APlayerController* PC)
{
	// Stock permissions (rcon admin / listen with no remotes) are preserved; this ADDS
	// the ?HostId= match host ([NetcodePlus] bAllowHostPause) AND the two bot-designated
	// team captains ([NetcodePlus] bAllowCaptainPause, ?Captains=) — see NCPlusHostPause.
	return Super::AllowPausing(PC) || NCPlusHostPause::MayPause(PC, this);
}

bool ANCPlusCTFGameMode::ClearPause()
{
	// Host/rcon unpause: hold behind a short server-only resume countdown
	// (Mod.ini [NetcodePlus] UnpauseCountdownSec). Only engages while actually paused.
	if (NCPlusHostPause::DeferUnpauseForCountdown(this))
	{
		return false;
	}
	return Super::ClearPause();
}
