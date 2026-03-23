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
#include "UTCTFSquadAI.h"
#include "UTWorldSettings.h"
#include "StatNames.h"
#include "Engine/DemoNetDriver.h"
#include "UTCTFScoreboard.h"
#include "UTCharacterVoice.h"
#include "UTCTFScoring.h"
#include "UTFlag.h"

ANCPlusCTFGameMode::ANCPlusCTFGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IntermissionDuration = 30.f;
	bAllowOvertime = true;
	MercyScore = 5;
	GoalScore = 0;
	TimeLimit = 14;
	CTFScoringClass = AUTCTFScoring::StaticClass();
	DisplayName = NSLOCTEXT("UTGameMode", "NCPlusCTF", "NetcodePlus CTF");

	// Advantage configuration
	AdvantageMaxDuration = 60;
	GracePeriodDuration = 10;
	bEndGameAdvantageOnlyWithinOneCap = true;

	// Spawn configuration
	FlagBaseProximityRadius = 4000.f;
	FlagSpawnPenaltyRadius = 3500.f;
	FlagCarrierSpawnPenalty = 15.f;
	DroppedFlagSpawnPenalty = 10.f;
	FlagCarrierLOSPenalty = 5.f;

	// Internal state
	AdvantageTimeRemaining = 0;
	GracePeriodTimeRemaining = 0;
	bGracePeriodActive = false;
	LastAdvantageCapTime = 0.f;
	bAdvantageCapEndedPeriod = false;
	LastScoreObjectTime = 0.f;
}

void ANCPlusCTFGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	IntermissionDuration = FMath::Max(1, UGameplayStatics::GetIntOption(Options, TEXT("HalftimeDuration"), IntermissionDuration));
	AdvantageMaxDuration = FMath::Max(10, UGameplayStatics::GetIntOption(Options, TEXT("AdvantageMaxDuration"), AdvantageMaxDuration));
	GracePeriodDuration = FMath::Max(3, UGameplayStatics::GetIntOption(Options, TEXT("GracePeriod"), GracePeriodDuration));

	if (bOfflineChallenge)
	{
		TimeLimit = 600;
	}
	// Halve time limit for two halves
	if (TimeLimit > 0)
	{
		TimeLimit = uint32(float(TimeLimit) * 0.5f);
	}
}

bool ANCPlusCTFGameMode::SupportsInstantReplay() const
{
	return true;
}

// ── Spawn System ────────────────────────────────────────────────────

float ANCPlusCTFGameMode::RatePlayerStart(APlayerStart* P, AController* Player)
{
	float Result = Super::RatePlayerStart(P, Player);

	if (Result <= 0.f || !CTFGameState || !Player)
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

	for (AUTCTFFlagBase* FlagBase : CTFGameState->FlagBases)
	{
		if (!FlagBase || !FlagBase->MyFlag)
		{
			continue;
		}

		AUTFlag* Flag = FlagBase->MyFlag;
		const uint8 FlagTeamNum = FlagBase->GetTeamNum();
		const FName FlagState = FlagBase->GetCarriedObjectState();
		const FVector FlagBaseLoc = FlagBase->GetActorLocation();

		// Only process OUR flag — "what is the enemy doing with our flag?"
		if (FlagTeamNum != PlayerTeamNum)
		{
			continue;
		}

		if (FlagState == CarriedObjectState::Held && Flag->HoldingPawn)
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
			FVector CarrierEyeLoc = CarrierLoc + FVector(0.f, 0.f, Flag->HoldingPawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());

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

	return FMath::Max(Result, 0.2f);
}

float ANCPlusCTFGameMode::GetTravelDelay()
{
	return Super::GetTravelDelay() + (CTFGameState ? FMath::Max(6.f, 1.f + CTFGameState->GetScoringPlays().Num()) : 6.f);
}

// ── Helpers ──────────────────────────────────────────────────────────

bool ANCPlusCTFGameMode::IsAnyFlagHeld() const
{
	if (!CTFGameState)
	{
		return false;
	}
	for (AUTCTFFlagBase* Base : CTFGameState->FlagBases)
	{
		if (Base != nullptr && Base->GetCarriedObjectState() == CarriedObjectState::Held)
		{
			return true;
		}
	}
	return false;
}

bool ANCPlusCTFGameMode::ShouldEnterAdvantage() const
{
	if (!IsAnyFlagHeld())
	{
		return false;
	}

	// Halftime advantage: always allowed regardless of score
	if (!CTFGameState->bSecondHalf)
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
	CTFGameState->bPlayingAdvantage = true;
	// 255 = both teams have advantage (not just one specific team)
	CTFGameState->AdvantageTeamIndex = 255;
	CTFGameState->bStopGameClock = true;

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
	if (!CTFGameState || CTFGameState->FlagBases.Num() < 2)
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
	CTFGameState->bPlayingAdvantage = false;
	CTFGameState->AdvantageTeamIndex = 255;
	bGracePeriodActive = false;
	GracePeriodTimeRemaining = 0;

	if (CTFGameState->bSecondHalf)
	{
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

	if (CTFGameState->GetRemainingTime() <= 0)
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
		if (CTFGameState->bPlayingAdvantage)
		{
			// Advantage timer ran out and grace period also expired (or flags still held)
			// This is handled by DefaultTimer ticking down and calling EndOfHalf
			// But also stop the game clock if it isn't already
			if (!CTFGameState->bStopGameClock)
			{
				CTFGameState->bStopGameClock = true;
			}
			return;
		}

		// ── Time just expired, not yet in advantage ──
		if (bAllowOvertime && !UTGameState->IsMatchInOvertime())
		{
			UTGameState->bStopGameClock = true;
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
	else if (CTFGameState->bPlayingAdvantage)
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

	// Tick advantage and grace period timers (DefaultTimer fires every second)
	if (CTFGameState && CTFGameState->bPlayingAdvantage && CTFGameState->IsMatchInProgress())
	{
		if (bGracePeriodActive)
		{
			// Check if a flag was picked up since last tick (polling approach)
			if (IsAnyFlagHeld())
			{
				CancelGracePeriod();
			}
			else
			{
				GracePeriodTimeRemaining--;

				// Broadcast countdown
				if (GracePeriodTimeRemaining >= 0 && GracePeriodTimeRemaining < 10)
				{
					BroadcastLocalized(this, UUTCountDownMessage::StaticClass(), GracePeriodTimeRemaining + 1);
				}

				if (GracePeriodTimeRemaining <= 0)
				{
					EndOfHalf();
					return;
				}
			}
		}
		else
		{
			// Advantage main timer ticking
			AdvantageTimeRemaining--;

			if (AdvantageTimeRemaining <= 0)
			{
				// Advantage time exhausted - start grace period regardless
				StartGracePeriod();
			}
		}
	}

	// Port from AUTCTFGameMode: increase respawn delay in extended overtime
	if (CTFGameState && CTFGameState->IsMatchInOvertime())
	{
		float OvertimeElapsed = CTFGameState->ElapsedTime - CTFGameState->OvertimeStartTime;
		if (OvertimeElapsed > TimeLimit)
		{
			RespawnWaitTime = 10.f;
			CTFGameState->SetRespawnWaitTime(RespawnWaitTime);
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
		if (Reason == FName("SentHome"))
		{
			// Flag was returned during advantage
			if (CTFGameState->bPlayingAdvantage)
			{
				if (!IsAnyFlagHeld() && !bGracePeriodActive)
				{
					StartGracePeriod();
				}
			}
		}
		else if (Reason == FName("FlagCapture"))
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
		if (CTFGameState->bPlayingAdvantage && !CTFGameState->HasMatchEnded())
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
	if (GetWorld()->WorldType == EWorldType::PIE)
	{
		return;
	}

	// Select the end-of-game replay before calling Super (which sets MatchEnded)
	// Call PickMostCoolMoments - advantage caps have 600.0 CoolFactor so they'll be selected.
	// For blowouts, the regular "coolest" capture wins via its 200.0 + surrounding kills.
	PickMostCoolMoments();

	Super::EndGame(Winner, Reason);
}

// ── Match State Handlers ─────────────────────────────────────────────

void ANCPlusCTFGameMode::HandleMatchHasStarted()
{
	// Only call super (which starts replay recording, announces match, etc.) on first half
	if (!CTFGameState->bSecondHalf)
	{
		Super::HandleMatchHasStarted();
	}
}

void ANCPlusCTFGameMode::HandleMatchIntermission()
{
	Super::HandleMatchIntermission();

	// Clear advantage state for halftime
	CTFGameState->bPlayingAdvantage = false;
	CTFGameState->AdvantageTeamIndex = 255;
	bGracePeriodActive = false;
	bAdvantageCapEndedPeriod = false;

	BroadcastLocalized(this, UUTCTFMajorMessage::StaticClass(), 11, nullptr, nullptr, nullptr);
}

void ANCPlusCTFGameMode::HandleExitingIntermission()
{
	const bool bWasSecondHalf = CTFGameState->bSecondHalf;

	// Set before calling super so HandleMatchHasStarted can read it
	CTFGameState->bSecondHalf = true;

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
	CTFGameState->SetTimeLimit(6000);
	SetMatchState(MatchState::MatchIsInOvertime);
	CTFGameState->bPlayingAdvantage = false;
	bGracePeriodActive = false;
}

void ANCPlusCTFGameMode::HandleMatchInOvertime()
{
	BroadcastLocalized(this, UUTCTFMajorMessage::StaticClass(), 12, nullptr, nullptr, nullptr);
	CTFGameState->bPlayingAdvantage = false;
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
