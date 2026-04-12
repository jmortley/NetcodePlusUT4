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

	// Track recent spawns for anti-repeat penalties (IG+ style)
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
			// Can't use the exact same spawn as last time
			if (Recent->Last.IsValid() && Recent->Last.Get() == P)
			{
				Result *= 0.1f; // Nearly eliminate — don't hard-block in case all others are worse
			}
			// Penalize using the spawn from 2 lives ago
			else if (Recent->SecondLast.IsValid() && Recent->SecondLast.Get() == P)
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

	// Spawn CTF stats replicator for scoreboard (grabs, accuracy)
	if (HasAuthority() && !CTFStatsRep)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		CTFStatsRep = GetWorld()->SpawnActor<ACTFStatsReplicator>(SpawnParams);
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
