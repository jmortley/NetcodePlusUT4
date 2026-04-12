// NCPlusCTFGameMode.h - NetcodePlus CTF with improved advantage time and instant replay
#pragma once
#include "NetcodePlus.h"
#include "UTCTFGameState.h"
#include "UTCTFScoring.h"
#include "UTCTFBaseGame.h"
#include "NCPlusCTFGameMode.generated.h"

// Safe property access across DLL boundary — uses runtime UProperty reflection
// instead of direct member access which has wrong offsets due to layout mismatch.
// The plugin DLL's compiled class layout differs from the engine DLL's, so
// CTFGameState->bPlayingAdvantage reads garbage. These helpers do runtime name
// lookup via FindField which always returns the correct offset.
namespace NCPlusReflection
{
	inline bool GetBool(UObject* Obj, const TCHAR* PropName)
	{
		UBoolProperty* Prop = FindField<UBoolProperty>(Obj->GetClass(), PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : false;
	}
	inline void SetBool(UObject* Obj, const TCHAR* PropName, bool Value)
	{
		UBoolProperty* Prop = FindField<UBoolProperty>(Obj->GetClass(), PropName);
		if (Prop) Prop->SetPropertyValue_InContainer(Obj, Value);
	}
	inline uint8 GetByte(UObject* Obj, const TCHAR* PropName)
	{
		UByteProperty* Prop = FindField<UByteProperty>(Obj->GetClass(), PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : 0;
	}
	inline void SetByte(UObject* Obj, const TCHAR* PropName, uint8 Value)
	{
		UByteProperty* Prop = FindField<UByteProperty>(Obj->GetClass(), PropName);
		if (Prop) Prop->SetPropertyValue_InContainer(Obj, Value);
	}
	inline int32 GetInt(UObject* Obj, const TCHAR* PropName)
	{
		UIntProperty* Prop = FindField<UIntProperty>(Obj->GetClass(), PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : 0;
	}
}

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

	/** Distance within which ANY living enemy penalizes a spawn point.
	 *  Prevents spawning directly on top of enemies regardless of flag state.
	 *  BP equivalent: EnemyBlockRange. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float EnemyBlockRange;

	/** Score penalty applied per nearby enemy within EnemyBlockRange. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float EnemyBlockPenalty;

	/** Distance within which an enemy with LOS to spawn point adds penalty.
	 *  BP equivalent: EnemyLOSBlockRange. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float EnemyLOSBlockRange;

	/** Score penalty for spawns with clear LOS to a nearby enemy. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float EnemyLOSPenalty;

	// ── Movement Configuration ───────────────────────────────────────

	/** If true, the match has two halves with intermission (side switch).
	 *  Auto-set to true for small games (MaxPlayers <= 4), false for larger games.
	 *  Can be overridden in Blueprint. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Halftime")
	bool bHasHalftime;

	/** If false, floor sliding is disabled for all players.
	 *  Useful for modes like Sniper CTF where slide animations
	 *  desync from the hitbox, making players hard to hit unfairly. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	bool bAllowFloorSlide;

	// ── Overtime Configuration ────────────────────────────────────────

	/** Respawn wait time during overtime (seconds). Replaces Epic's extended
	 *  overtime that escalated to 10s. Set to 0 to use normal respawn time. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Overtime")
	float OvertimeRespawnTime;

	// ── Game Flow Overrides ──────────────────────────────────────────
	// NOTE: Floor slide disable is enforced via ATeamArenaCharacter::CanSlide_Implementation()
	// which reads bAllowFloorSlide from this game mode. No RestartPlayer override needed.

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
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
	bool AreAllFlagsHome() const;

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

	// ── Recent Spawn Tracking (IG+ style) ───────────────────────────
	// Penalize reusing the same spawn point. Tracks last 2 spawns per player.

	struct FRecentSpawns
	{
		TWeakObjectPtr<APlayerStart> Last;
		TWeakObjectPtr<APlayerStart> SecondLast;
	};

	TMap<TWeakObjectPtr<AController>, FRecentSpawns> PlayerRecentSpawns;

	/** Penalty multiplier for using the same spawn as 2 spawns ago (0.5 = half score). IG+ default. */
	float SpawnRecentPenaltyMultiplier = 0.5f;

	/** Penalty for spawning within this radius of your last spawn point */
	float SpawnNearLastRadius = 4000.f;

	/** Penalty scale for near-last-spawn distance */
	float SpawnNearLastPenalty = 6.f;

	// ── Stats Replicator ────────────────────────────────────────────

	/** Replicated stats for scoreboard (grabs, accuracy) */
	UPROPERTY(Transient)
	class ACTFStatsReplicator* CTFStatsRep = nullptr;

	// ── Overtime Tracking ────────────────────────────────────────────

	/** World time when overtime started — used to delay respawn escalation. */
	float OvertimeStartWorldTime;

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
