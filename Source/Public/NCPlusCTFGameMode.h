// NCPlusCTFGameMode.h - NetcodePlus CTF with improved advantage time and instant replay
#pragma once
#include "NetcodePlus.h"
#include "UTCTFGameState.h"
#include "UTCTFScoring.h"
#include "UTCTFBaseGame.h"
#include "NCPlusCTFGameMode.generated.h"

UCLASS(Abstract)
class NETCODEPLUS_API ANCPlusCTFGameMode : public AUTCTFBaseGame
{
	GENERATED_UCLASS_BODY()

	// ── Advantage Configuration ──────────────────────────────────────

	/** Max seconds advantage lasts while a flag is held before forcing grace period. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Advantage")
	int32 AdvantageMaxDuration;

	/** Seconds after all flags return home before ending the half/game. Picking up a flag cancels it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Advantage")
	int32 GracePeriodDuration;

	/** If true, end-of-game advantage only triggers if score difference is <= 1 cap. Halftime always allows advantage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Advantage")
	bool bEndGameAdvantageOnlyWithinOneCap;

	// ── Spawn Configuration ─────────────────────────────────────────

	/** Distance from a flag base within which an actor is considered "in the base area." */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float FlagBaseProximityRadius;

	/** Distance from a flag carrier or dropped flag within which spawns are penalized. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float FlagSpawnPenaltyRadius;

	/** Score penalty applied when a spawn is near a flag carrier in the base area. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float FlagCarrierSpawnPenalty;

	/** Score penalty applied when a spawn is near a dropped flag in its own base. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float DroppedFlagSpawnPenalty;

	/** Score penalty for spawns with direct LOS to an enemy flag carrier. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float FlagCarrierLOSPenalty;

	// ── Game Flow Overrides ──────────────────────────────────────────

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual float RatePlayerStart(APlayerStart* P, AController* Player) override;
	virtual void ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn, AUTPlayerState* Holder, FName Reason) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer);
	virtual void CheckGameTime() override;
	virtual void DefaultTimer() override;
	virtual float GetTravelDelay() override;

	virtual void HandleFlagCapture(AUTCharacter* HolderPawn, AUTPlayerState* Holder) override;
	virtual void HandleMatchIntermission() override;
	virtual void HandleExitingIntermission() override;
	virtual void HandleMatchInOvertime() override;
	virtual void EndGame(AUTPlayerState* Winner, FName Reason) override;

	virtual bool PlayerCanRestart_Implementation(APlayerController* Player);
	virtual bool SupportsInstantReplay() const override;

	void BuildServerResponseRules(FString& OutRules);

	virtual void GetGood() override;

protected:

	virtual void HandleMatchHasStarted() override;
	virtual void HandleEnteringOvertime();

	// ── Advantage Time System ────────────────────────────────────────

	/** Returns true if any flag in the game is currently held by a player. */
	bool IsAnyFlagHeld() const;

	/** Determines whether advantage should start when time expires.
	 *  Halftime: always if a flag is held. End of game: only within 1 cap diff (configurable). */
	virtual bool ShouldEnterAdvantage() const;

	/** Enter advantage mode: both teams get to play, 60s timer starts. */
	void EnterAdvantage();

	/** Check if advantage conditions still hold. Returns false if advantage should end. */
	virtual bool CheckAdvantage();

	/** Start the grace period countdown (10s default). */
	void StartGracePeriod();

	/** Cancel grace period (flag was picked up). */
	void CancelGracePeriod();

	/** End the current half: go to intermission, overtime, or end game. */
	virtual void EndOfHalf();

	// ── Advantage State ──────────────────────────────────────────────

	/** Remaining seconds in the advantage period (counts down from AdvantageMaxDuration). */
	int32 AdvantageTimeRemaining;

	/** Remaining seconds in the grace period (counts down from GracePeriodDuration). */
	int32 GracePeriodTimeRemaining;

	/** True when all flags are home and we're counting down before ending. */
	bool bGracePeriodActive;

	// ── Replay Tracking ──────────────────────────────────────────────

	/** Time of the last cap during advantage (for replay selection). */
	float LastAdvantageCapTime;

	/** Player who capped during advantage. */
	TWeakObjectPtr<AUTPlayerState> LastAdvantageCapPlayer;

	/** True if an advantage cap ended the game or half. */
	bool bAdvantageCapEndedPeriod;

	/** World time before which a FlagCapture ScoreObject is rejected. Prevents double caps on maps with no geometry between bases. */
	float LastScoreObjectTime;

public:
	virtual void CreateGameURLOptions(TArray<TSharedPtr<TAttributePropertyBase>>& MenuProps);
};
