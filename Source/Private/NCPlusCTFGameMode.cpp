// NCPlusCTFGameMode.cpp - NetcodePlus CTF with improved advantage time and instant replay
#include "NCPlusCTFGameMode.h"
#include "UnrealTournament.h"
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
#include "UTCTFScoreboard.h"
#include "UTCharacterVoice.h"
#include "UTCTFScoring.h"
#include "UTFlag.h"
#include "NPPlayerController.h"
#include "NCPlusCTFHUD.h"
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
	}
}

void ANCPlusCTFGameMode::HandleMatchHasEnded()
{
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

	Super::Logout(Exiting);
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

	ReadBool(TEXT("CTFPerfEnabled"),           CTFPerfConfig.bEnabled);
	ReadBool(TEXT("CTFRatingShadow"),          CTFPerfConfig.bShadow);
	ReadDouble(TEXT("CTFPerfObjectiveWeight"), CTFPerfConfig.ObjectiveWeight);
	ReadDouble(TEXT("CTFFlagFeederPenalty"),   CTFPerfConfig.FeederPenalty);
	ReadFloat(TEXT("CTFRatingMinPresenceFrac"),CTFRatingMinPresenceFrac);

	UE_LOG(LogGameMode, Log,
		TEXT("NCPlusCTF perf config: enabled=%s shadow=%s objW=%.2f feeder=%.2f minPresence=%.2f"),
		CTFPerfConfig.bEnabled ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.bShadow ? TEXT("true") : TEXT("false"),
		CTFPerfConfig.ObjectiveWeight, CTFPerfConfig.FeederPenalty, CTFRatingMinPresenceFrac);
}

bool ANCPlusCTFGameMode::SupportsInstantReplay() const
{
	return true;
}

// ── Floor Slide ─────────────────────────────────────────────────────
// Floor slide disable temporarily removed — needs reimplementation
// without changing TeamArenaCharacter class layout
void ANCPlusCTFGameMode::RestartPlayer(AController* NewPlayer)
{
	Super::RestartPlayer(NewPlayer);

	// Track recent spawns for anti-repeat penalties (IG+ style).
	// Snapshot prior Recent BEFORE updating so the log can classify the
	// chosen spawn as fresh / same-as-last / two-back.
	if (NewPlayer && NewPlayer->StartSpot.IsValid())
	{
		APlayerStart* UsedStart = Cast<APlayerStart>(NewPlayer->StartSpot.Get());
		if (UsedStart)
		{
			FRecentSpawns& Recent = PlayerRecentSpawns.FindOrAdd(TWeakObjectPtr<AController>(NewPlayer));

			// Classify against the prior (pre-update) state.
			const TCHAR* RecentTag = TEXT("fresh");
			int32 DistFromLast = -1;
			if (Recent.Last.IsValid())
			{
				DistFromLast = FMath::RoundToInt(
					(UsedStart->GetActorLocation() - Recent.Last->GetActorLocation()).Size());
				if (Recent.Last.Get() == UsedStart) RecentTag = TEXT("same");
				else if (Recent.SecondLast.IsValid() && Recent.SecondLast.Get() == UsedStart) RecentTag = TEXT("2back");
			}

			// Final-pick log. Grep "NCPlusCTF spawn:" after a match to see
			// every chosen spawn with enough context to spot anti-repeat
			// misfires or flag-state-correlated spawnkill complaints.
			// Warning verbosity (not Log) so the line survives the Shipping
			// build's static UE_LOG strip — Log is below COMPILED_IN_MINIMUM_
			// VERBOSITY (=Display) in Shipping w/o USE_LOGGING_IN_SHIPPING,
			// which silently elided the entire format string out of the .so.
			AUTPlayerState* PS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
			AUTCTFGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
			const int32 TeamIdx = (PS && PS->Team) ? int32(PS->Team->TeamIndex) : -1;
			const FString OwnFlag   = (GS && TeamIdx >= 0) ? GS->GetFlagState(TeamIdx).ToString()     : TEXT("?");
			const FString EnemyFlag = (GS && TeamIdx >= 0) ? GS->GetFlagState(1 - TeamIdx).ToString() : TEXT("?");
			const FVector SL = UsedStart->GetActorLocation();
			UE_LOG(LogGameMode, Warning,
				TEXT("NCPlusCTF spawn: %s(T%d) -> %s (%.0f,%.0f) | recent=%s dist_from_last=%d | own_flag=%s enemy_flag=%s"),
				PS ? *PS->PlayerName : TEXT("?"), TeamIdx,
				*UsedStart->GetName(), SL.X, SL.Y,
				RecentTag, DistFromLast,
				*OwnFlag, *EnemyFlag);

			Recent.SecondLast = Recent.Last;
			Recent.Last = UsedStart;
		}
	}

	// Ping-compensated spawn: hide pawn until client confirms control.
	// Skip bots (no remote client to confirm).
	ATeamArenaCharacter* SpawnedChar = (NewPlayer && NewPlayer->GetPawn()) ? Cast<ATeamArenaCharacter>(NewPlayer->GetPawn()) : nullptr;
	if (bEnablePingCompensatedSpawn && SpawnedChar && NewPlayer->GetPawn()->GetRemoteRole() == ROLE_AutonomousProxy)
	{
		SpawnedChar->bPingCompensatedSpawnPending = true;
		SpawnedChar->SetActorHiddenInGame(true);
		SpawnedChar->SetActorEnableCollision(false);
		SpawnedChar->SpawnHiddenTimestamp = GetWorld()->GetTimeSeconds();
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

	// Score the curated own-team pool with the existing RatePlayerStart. The pool
	// is the player's own team (by author TeamNum), so the -20 wrong-team guard
	// never trips here — it stays a safety net. RatePlayerStart's jitter and its
	// -8 last-spot penalty keep picks rotating instead of pinning.
	APlayerStart* Best = nullptr;
	float BestScore = -FLT_MAX;
	for (const TWeakObjectPtr<APlayerStart>& WP : Pool)
	{
		APlayerStart* Candidate = WP.Get();
		if (!Candidate) continue;

		const float Score = RatePlayerStart(Candidate, Player);
		if (Score > BestScore)
		{
			BestScore = Score;
			Best = Candidate;
		}
	}

	if (!Best)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Confirmation log (Warning to survive the Shipping UE_LOG strip). Pairs with
	// the "NCPlusCTF spawn:" line in RestartPlayer — together they prove this
	// override drives selection and that picks rotate per life.
	UE_LOG(LogGameMode, Warning,
		TEXT("NCPlusCTF pick: %s(T%d) -> %s score=%.1f (pool=%d)"),
		*PS->PlayerName, TeamIndex, *Best->GetName(), BestScore, Pool.Num());

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

	Super::ScoreObject_Implementation(GameObject, HolderPawn, Holder, Reason);

	if (Holder != nullptr && Holder->Team != nullptr && !CTFGameState->HasMatchEnded() && !CTFGameState->IsMatchIntermission())
	{
		if (Reason == FName("FlagCapture"))
		{
			// Boost CoolFactor for all captures (for replay selection)
			Holder->AddCoolFactorEvent(200.0f);
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

void ANCPlusCTFGameMode::EndGame(AUTPlayerState* Winner, FName Reason)
{
	// Select the end-of-game replay before calling Super (which sets MatchEnded).
	// Only call PickMostCoolMoments if instant replay is actually supported —
	// standalone PIE and dedicated servers without demo recording will crash
	// if we try to access replay data that was never initialized.
	if (SupportsInstantReplay() && GetWorld()->DemoNetDriver != nullptr)
	{
		PickMostCoolMoments();
	}

	Super::EndGame(Winner, Reason);
}

// ── Match State Handlers ─────────────────────────────────────────────

void ANCPlusCTFGameMode::HandleMatchHasStarted()
{
	// Only call super (which starts replay recording, announces match, etc.) on first half.
	// Non-halftime games always call super (there's no second half).
	if (!bHasHalftime || !NCPlusReflection::GetBool(CTFGameState, TEXT("bSecondHalf")))
	{
		Super::HandleMatchHasStarted();
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
