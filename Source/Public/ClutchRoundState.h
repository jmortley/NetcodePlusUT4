#pragma once

// NetcodePlus.h must be the first include in headers that expose UT types.
// UE4.15 unity-build ordering otherwise produces fragile cascade failures.
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "ClutchRoundState.generated.h"

class AUTPlayerState;

/** The authoritative phase of the current Clutch round. */
UENUM(BlueprintType)
enum class EClutchRoundPhase : uint8
{
	Waiting,
	OrderSelection,
	Intermission,
	Combat,
	Capture,
	RoundEnd,
	MatchEnd
};

/** A player's gameplay role for the current round. */
UENUM(BlueprintType)
enum class EClutchRole : uint8
{
	None,
	Attacker,
	Defender
};

/** A player's participation state for the current round. */
UENUM(BlueprintType)
enum class EClutchStatus : uint8
{
	Queued,
	Active,
	Benched,
	Eliminated,
	Disconnected
};

/**
 * One replicated roster row. The stock AUTPlayerState remains the canonical
 * player actor; StablePlayerId and PlayerIdFallback let UI/server code match a
 * row while that actor reference is unresolved or is being replaced on reconnect.
 */
USTRUCT(BlueprintType)
struct FClutchRosterEntry
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	AUTPlayerState* PlayerState;

	/** "uid:<net id>", otherwise stable offline "name:<lowercase name>", then pid. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	FString StablePlayerId;

	/** Stock PlayerId fallback for PIE/LAN sessions where UniqueId is invalid. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	int32 PlayerIdFallback;

	/** Display fallback while the PlayerState actor reference is unresolved. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	FString PlayerNameFallback;

	/** Stable, team-local rotation slot. 255 means unassigned. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	uint8 RosterSlot;

	/** Team-selected attacker position. Zero attacks first; 255 is unassigned. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	uint8 AttackOrderIndex;

	/** This teammate may submit the pre-match attack order for their team. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	bool bAttackOrderSelector;

	/** 0 or 1 for a playing team; 255 means no team. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	uint8 TeamIndex;

	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	EClutchRole PlayerRole;

	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	EClutchStatus PlayerStatus;

	/** Successful defender hits received by the active attacker this round. */
	UPROPERTY(BlueprintReadOnly, Category = "Clutch|Roster")
	uint8 HitsTaken;

	FClutchRosterEntry();
};

/**
 * Small always-relevant state carrier for Clutch.
 *
 * NetcodePlus must not subclass AUTGameState, AUTPlayerState, or
 * AUTPlayerController. This independent AInfo owns every Clutch-only replicated
 * value while stock UT classes continue to drive teams, pawns, and spectating.
 */
UCLASS(NotPlaceable, BlueprintType)
class NETCODEPLUS_API AClutchRoundState : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	static const uint8 NoTeam = 255;
	static const uint8 UnassignedSlot = 255;

	// ---------------------------------------------------------------------
	// Replicated round state
	// ---------------------------------------------------------------------

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	EClutchRoundPhase Phase;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Roster")
	TArray<FClutchRosterEntry> Roster;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	AUTPlayerState* ActiveAttacker;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	uint8 AttackingTeamIndex;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	int32 RoundNumber;

	/** Bit 0 locks team 0's order; bit 1 locks team 1's order. */
	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Order")
	uint8 AttackOrderLockedMask;

	/** Server-time deadline for the pre-match order picker. */
	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Order")
	float AttackOrderDeadlineServerTime;

	/** All timestamps use AGameStateBase::GetServerWorldTimeSeconds(). */
	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Timing")
	float RoundStartServerTime;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Timing")
	float PoleUnlockServerTime;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Timing")
	float RoundEndServerTime;

	/** Capture percentage in the inclusive range 0..100. */
	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Objective")
	float PoleProgress;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Rules")
	int32 ScoreGoal;

	/** Attacker armor pips/hits-to-kill. This is not UT inventory armor. */
	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Rules")
	uint8 MaxAttackerHits;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	uint8 LastWinningTeamIndex;

	UPROPERTY(Replicated, Transient, BlueprintReadOnly, Category = "Clutch|Round")
	FName LastEndReason;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---------------------------------------------------------------------
	// Identity and lookup
	// ---------------------------------------------------------------------

	/** Builds the strongest identity available without introducing a custom PS. */
	static FString BuildStablePlayerId(const AUTPlayerState* PlayerState);

	const FClutchRosterEntry* FindEntry(const AUTPlayerState* PlayerState) const;
	const FClutchRosterEntry* FindEntryByIdentity(const FString& StablePlayerId, int32 PlayerIdFallback) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Roster")
	bool GetEntryForPlayer(AUTPlayerState* PlayerState, FClutchRosterEntry& OutEntry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Roster")
	bool GetEntryByIdentity(const FString& StablePlayerId, int32 PlayerIdFallback, FClutchRosterEntry& OutEntry) const;

	// ---------------------------------------------------------------------
	// Derived state: replacements for the old custom PlayerState fields
	// ---------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool IsGameplayPhase() const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool IsEntryRoundActive(const FClutchRosterEntry& Entry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool ShouldEntrySpectate(const FClutchRosterEntry& Entry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool EntryUsesAttackerWeapon(const FClutchRosterEntry& Entry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool EntryUsesDefenderWeapon(const FClutchRosterEntry& Entry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	int32 GetEntryArmorRemaining(const FClutchRosterEntry& Entry) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool IsPlayerRoundActive(AUTPlayerState* PlayerState) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool ShouldPlayerSpectate(AUTPlayerState* PlayerState) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool PlayerUsesAttackerWeapon(AUTPlayerState* PlayerState) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	bool PlayerUsesDefenderWeapon(AUTPlayerState* PlayerState) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Derived")
	int32 GetPlayerArmorRemaining(AUTPlayerState* PlayerState) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Order")
	bool IsAttackOrderLocked(uint8 TeamIndex) const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Order")
	bool AreAttackOrdersLocked() const;

	UFUNCTION(BlueprintPure, Category = "Clutch|Order")
	bool IsPlayerAttackOrderSelector(AUTPlayerState* PlayerState) const;

	/** Returns connected team roster slots in the replicated selected order. */
	void GetTeamAttackOrderSlots(uint8 TeamIndex, TArray<int32>& OutSlots) const;

	// ---------------------------------------------------------------------
	// Authority-only mutation API
	// ---------------------------------------------------------------------

	bool ResetForMatch(int32 InScoreGoal, uint8 InMaxAttackerHits);
	bool BeginAttackOrderSelection(float InDeadlineServerTime);

	bool BeginRound(uint8 InAttackingTeamIndex, AUTPlayerState* InActiveAttacker,
		int32 InRoundNumber, float InRoundStartServerTime,
		float InPoleUnlockServerTime, float InRoundEndServerTime);
	bool StartRoundClock(float InRoundStartServerTime,
		float InPoleUnlockServerTime, float InRoundEndServerTime);

	bool SetPhase(EClutchRoundPhase NewPhase);
	bool SetPoleProgress(float NewProgress);
	bool FinishRound(uint8 WinningTeamIndex, FName EndReason, bool bMatchEnded);

	bool UpsertPlayer(AUTPlayerState* PlayerState, uint8 TeamIndex, uint8 RosterSlot,
		EClutchRole PlayerRole, EClutchStatus PlayerStatus);

	/** Retains identity/slot so a reconnect can reclaim the same roster row. */
	bool DetachPlayer(AUTPlayerState* PlayerState);

	bool ClearRoster();
	bool SetTeamAttackOrder(uint8 TeamIndex, const TArray<int32>& OrderedRosterSlots,
		bool bLockOrder);
	bool SetAttackOrderLocked(uint8 TeamIndex, bool bLocked);
	bool SetAttackOrderSelectors(AUTPlayerState* Team0Selector,
		AUTPlayerState* Team1Selector);
	bool SetPlayerRoundState(AUTPlayerState* PlayerState, EClutchRole PlayerRole,
		EClutchStatus PlayerStatus, uint8 HitsTaken);
	bool SetPlayerHitCount(AUTPlayerState* PlayerState, uint8 HitsTaken);
	uint8 AddAttackerHit(AUTPlayerState* PlayerState);

	// ---------------------------------------------------------------------
	// Pure rules helpers (no world or actor mutation)
	// ---------------------------------------------------------------------

	/** Returns the next sorted eligible slot after PreviousSlot, wrapping once. */
	static int32 SelectNextRotationSlot(const TArray<int32>& EligibleSlots, int32 PreviousSlot);

	/** Validates that ProposedSlots is an exact, duplicate-free permutation. */
	static bool IsValidAttackOrder(const TArray<int32>& EligibleSlots,
		const TArray<int32>& ProposedSlots);

	/** Returns the entry after PreviousSlot while preserving the supplied order. */
	static int32 SelectNextOrderedSlot(const TArray<int32>& OrderedSlots,
		int32 PreviousSlot);

	/** Maps a valid two-team attacker index to the defending team; otherwise 255. */
	static uint8 GetDefendingTeam(uint8 InAttackingTeamIndex);

	/** Shared rule used by live spectator cycling and deterministic tests. */
	static bool CanSpectateRosterEntry(uint8 ViewerTeamIndex,
		const FClutchRosterEntry& TargetEntry, bool bTargetAlive);

	/** Advances the 0..100 pole meter for one server step. */
	static float AdvancePoleProgress(float CurrentProgress, float DeltaSeconds,
		bool bAttackerPresent, bool bDefenderPresent,
		float CaptureSeconds, float DecaySeconds);

	/**
	 * Advances a fixed regenerating weapon magazine for one server step. Returns
	 * the rounds to grant now (already capped to the remaining deficit) and
	 * updates AccumulatorSeconds with the unspent time. A full magazine parks the
	 * accumulator at zero so the refill clock restarts from the next shot.
	 */
	static int32 AdvanceAmmoRegen(float& AccumulatorSeconds, float DeltaSeconds,
		int32 CurrentAmmo, int32 MagazineSize, float RegenInterval);

	/** Resolves the role-vs-role damage override; zero means the hit is invalid. */
	static int32 ResolveRoleDamage(EClutchRole DamageDealerRole,
		EClutchRole VictimRole, int32 DefenderDamage, int32 AttackerDamage);

	/**
	 * Resolves a round using Elite's deterministic priority:
	 * timeout, all defenders eliminated, attacker eliminated, pole captured.
	 * Returns 255 while the round has no winner.
	 */
	static uint8 ResolveRoundWinner(uint8 InAttackingTeamIndex, bool bTimedOut,
		int32 DefendersAlive, bool bAttackerAlive, bool bPoleCaptured);

	static bool HasWonMatch(int32 WinnerScore, int32 LoserScore, int32 GoalScore,
		bool bWinByTwo, int32 MinimumWinMargin);

private:
	FClutchRosterEntry* FindMutableEntry(const AUTPlayerState* PlayerState);
	FClutchRosterEntry* FindMutableEntryByIdentity(const FString& StablePlayerId, int32 PlayerIdFallback);
	/** Force an immediate replication pass unless this is a high-frequency value. */
	void MarkStateDirty(bool bForceImmediate = true);
};
