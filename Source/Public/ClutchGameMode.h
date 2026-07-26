#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTTeamGameMode.h"
#include "TimerManager.h"
#include "ClutchRoundState.h"
#include "ClutchGameMode.generated.h"

class AClutchRoundState;
class AClutchPoleVisual;
class AUTInventory;
class AUTPlayerState;
class AUTWeapon;
class APlayerStart;

/** Runtime-only per-weapon ammo-regen bookkeeping (never replicated). */
struct FClutchAmmoRegenState
{
	/** Time banked toward the next round; negative while an empty pause is serving. */
	float Accumulator = 0.0f;
	/** Ammo seen at the end of the previous pass, for the >0 -> 0 empty transition. */
	int32 LastAmmo = INDEX_NONE;
};

/**
 * ShootMania Elite-style 3v3 mode for UT4.
 *
 * The mode deliberately keeps UT4's stock PlayerController, PlayerState and
 * GameState classes. Clutch-specific per-player state lives in the replicated
 * AClutchRoundState actor so Blueprint children remain editor-safe.
 */
UCLASS(Config = Game)
class NETCODEPLUS_API AClutchGameMode : public AUTTeamGameMode
{
	GENERATED_BODY()

public:
	AClutchGameMode(const FObjectInitializer& ObjectInitializer);

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandleMatchHasEnded() override;
	virtual void Tick(float DeltaSeconds) override;

	virtual bool PlayerCanRestart_Implementation(APlayerController* Player) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual bool ChangeTeam(AController* Player, uint8 NewTeam = 255, bool bBroadcast = true) override;
	virtual void SetPlayerDefaults(APawn* PlayerPawn) override;
	virtual void GiveDefaultInventory(APawn* PlayerPawn) override;
	virtual void DiscardInventory(APawn* Other, AController* Killer = nullptr) override;
	virtual bool CheckRelevance_Implementation(AActor* Other) override;

	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum,
		APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo,
		AActor* DamageCauser, TSubclassOf<UDamageType> DamageType) override;
	virtual void ScoreDamage_Implementation(int32 DamageAmount,
		AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;
	virtual bool CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget) override;

	/** Server entry point used by the stock ServerMutate transport. */
	bool SubmitAttackOrder(APlayerController* Sender, const TArray<int32>& OrderedRosterSlots);

	/** Full round duration. Defenders win when it expires. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules", meta = (ClampMin = "1.0"))
	float RoundDurationSeconds;

	/** The pole cannot be captured until this many seconds into the round. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules", meta = (ClampMin = "0.0"))
	float PoleUnlockDelaySeconds;

	/** Uncontested time on the pole needed to go from zero to 100%. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole", meta = (ClampMin = "0.1"))
	float PoleCaptureSeconds;

	/** Time for unattended progress to decay from 100% to zero. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole", meta = (ClampMin = "0.1"))
	float PoleDecaySeconds;

	/** Distance from the pole origin that counts as standing on it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole", meta = (ClampMin = "1.0"))
	float PoleCaptureRadius;

	/** Vertical half-height of the pole's cylindrical capture volume. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole", meta = (ClampMin = "1.0"))
	float PoleCaptureHalfHeight;

	/** Preferred tag for a map actor that marks the pole origin. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole")
	FName PoleActorTag;

	/** Spawn the recovered original pole mesh when a map only supplies a marker. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Pole")
	bool bUseRecoveredPoleVisual;

	/** Delay between rounds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules", meta = (ClampMin = "0.0"))
	float IntermissionSeconds;

	/** Time captains have to choose their team's attacker rotation before round one. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Order", meta = (ClampMin = "1.0"))
	float AttackOrderSelectionSeconds;

	/** Locked-order review shown after selection and before round one starts. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Order", meta = (ClampMin = "0.0"))
	float AttackOrderReviewSeconds;

	/** Number of defender projectile hits the attacker can take. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Combat", meta = (ClampMin = "1", ClampMax = "255"))
	int32 MaxAttackerHits;

	/** Derived as MaxAttackerHits * DefenderDamagePerHit. Defaults to 300. */
	UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Clutch|Combat")
	int32 AttackerHealth;

	/** Damage applied for each valid defender hit on the attacker. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Combat", meta = (ClampMin = "1"))
	int32 DefenderDamagePerHit;

	/** Damage forced for an attacker hit on a defender. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Combat", meta = (ClampMin = "1"))
	int32 AttackerDamagePerHit;

	/** Attacker weapon. Defaults to the stock sniper as a playable fallback. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout")
	TSubclassOf<AUTInventory> AttackerWeaponClass;

	/** Defender weapon. Defaults to the stock rocket launcher as a playable fallback. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout")
	TSubclassOf<AUTInventory> DefenderWeaponClass;

	/**
	 * Fixed regenerating magazine size shared by both role weapons. Zero disables
	 * the feature and restores the mode's historical infinite ammo. When positive,
	 * InitGame also forces bAmmoIsLimited so shots actually spend rounds.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout", meta = (ClampMin = "0"))
	int32 RoleWeaponMagazine;

	/** Legacy shared interval retained for serialized Blueprint compatibility. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout", meta = (ClampMin = "0.1"))
	float RoleWeaponAmmoRegenInterval;

	/** Seconds to restore one attacker-rifle round, up to the shared magazine size. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout", meta = (ClampMin = "0.1"))
	float AttackerWeaponAmmoRegenInterval;

	/** Seconds to restore one defender-rocket round, up to the shared magazine size. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout", meta = (ClampMin = "0.1"))
	float DefenderWeaponAmmoRegenInterval;

	/**
	 * Extra ShootMania-style pause before the first round refills after the clip is
	 * fully emptied. Layers on top of the regen interval, so the first round returns
	 * the applicable role interval + this many seconds after hitting empty. Even zero
	 * restarts the refill clock from empty, so dump speed no longer changes how soon
	 * the first round comes back.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Loadout", meta = (ClampMin = "0.0"))
	float RoleWeaponEmptyReloadPause;

	/** Optional match rule. Disabled gives a literal first-to-nine result. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules")
	bool bWinByTwo;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules", meta = (ClampMin = "1"))
	int32 MinimumWinMargin;

	/** Server-owned replicated state. Clients find the actor by class. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Clutch")
	AClutchRoundState* ClutchState;

	/** Resolved pole marker. Tag any map actor ClutchPole to override discovery. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Clutch|Pole")
	AActor* PoleActor;

	/** Replicated presentation actor spawned at the resolved pole marker. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Clutch|Pole")
	AClutchPoleVisual* PoleVisualActor;

	UFUNCTION(BlueprintPure, Category = "Clutch")
	AClutchRoundState* GetClutchState() const { return ClutchState; }

	UFUNCTION(BlueprintPure, Category = "Clutch|Pole")
	AActor* GetPoleActor() const { return PoleActor; }

protected:
	void EnsureClutchState();
	void RefreshRoster();
	void BeginAttackOrderSelection();
	void FinishAttackOrderSelection();
	void PrepareAttackOrderRoster();
	void HandleAttackOrderRosterChanged(uint8 ChangedTeamIndex);
	void AssignAttackOrderSelectors();

	/** Locks the current/default order for any team that has players but no human who
	 *  could ever submit one (bot-filled sides). Fires the finish check when that
	 *  completes the lock set, so a lone human vs bots starts as soon as they confirm. */
	void EvaluateAttackOrderAutoLocks();

	/** True when any connected roster entry on the team is a human player. */
	bool TeamHasHumanPlayer(uint8 TeamIndex) const;

	/** False while the server still owes bodies to the bot fill. Auto-lock fast-starts
	 *  wait for this so a round can't begin 1v1 four milliseconds after match start
	 *  while five bots are still trickling in; the selection deadline stays the
	 *  backstop, so a fill that never completes cannot wedge the match. */
	bool IsRosterFillSettled() const;

	/** Gameplay-phase watchdog: folds late joiners into the roster (some bot paths
	 *  bypass PostLogin AND ChangeTeam) and benches any pawn whose owner is not an
	 *  Active round participant, so stragglers cannot wander a live round. */
	void EnforceRoundMembership();

	/** Polled during OrderSelection: when the connected roster changes (bots join via
	 *  AddBot with no PostLogin, humans leave, ...), re-run the roster/lock/selector
	 *  reconciliation. Join-path agnostic by design. */
	UFUNCTION()
	void OnOrderSelectionRosterWatch();
	void GetConnectedTeamSlots(uint8 TeamIndex, TArray<int32>& OutSlots) const;
	void BeginRound();
	bool SelectRoundRoles();
	void BuildRoundSpawnQueue();

	UFUNCTION()
	void ProcessNextRoundSpawn();

	void FinalizeRoundStart();

	UFUNCTION()
	void ActivatePole();

	UFUNCTION()
	void HandleRoundTimeout();

	void QueueWinCheck();

	UFUNCTION()
	void EvaluateRoundWinConditions();

	void EndRound(uint8 WinningTeamIndex, FName Reason);

	UFUNCTION()
	void BeginNextRound();

	void ClearRoundPawns();
	void FreezeRoundPawn(AController* Controller);
	void ReleaseRoundPawns();
	void EnterPlaying(AController* Controller);
	void EnterSpectating(AController* Controller, AUTPlayerState* PreferredTarget = nullptr);
	void DeferredEnterSpectating(AUTPlayerState* PlayerState);
	AUTPlayerState* FindActiveTeammate(const AUTPlayerState* PlayerState) const;
	AActor* ResolvePoleActor();
	void EnsurePoleVisual();
	void UpdatePole(float DeltaSeconds);
	void UpdateAmmoRegen(float DeltaSeconds);
	bool IsCombatPhase() const;

	/** Shared clip-regen step for one weapon (empty-pause arming, grant, bookkeeping). */
	void AdvanceWeaponClipRegen(
		AUTWeapon* Weapon, float DeltaSeconds, float RegenInterval);

	/** Regenerates the equipped weapon of every living pawn outside combat rounds
	 *  (warmup, waiting, intermission), so the both-guns practice loadout keeps the
	 *  round's magazine behavior instead of running dry forever. */
	void UpdateWarmupAmmoRegen(float DeltaSeconds);
	bool IsActiveRoundPlayer(const AUTPlayerState* PlayerState) const;
	bool IsActiveRole(const AUTPlayerState* PlayerState, EClutchRole Role) const;
	int32 CountActiveDefenders() const;
	AUTPlayerState* GetActiveAttacker() const;
	void MarkDisconnected(AUTPlayerState* PlayerState);

	FTimerHandle PoleUnlockTimerHandle;
	FTimerHandle RoundTimeoutTimerHandle;
	FTimerHandle IntermissionTimerHandle;
	FTimerHandle AttackOrderSelectionTimerHandle;
	FTimerHandle SpawnQueueTimerHandle;
	FTimerHandle OrderRosterWatchTimerHandle;
	/** Connected-player counts per team last seen by the OrderSelection roster watch. */
	int32 OrderRosterWatchCounts[2];
	/** Last time the round-blocked reason was logged, to throttle the waiting loop. */
	float LastWaitingLogTime;
	/** Accumulates Tick time toward the once-per-second round-membership check. */
	float MembershipWatchAccumulator;
	TArray<TWeakObjectPtr<AController>> PendingRoundSpawns;
	TMap<TWeakObjectPtr<AController>, int32> RoundSpawnAttempts;
	/** Role starts already handed out during the current round's spawn pass, so
	 *  sequential same-role spawns (and retries) spread across the available starts
	 *  instead of all colliding on the single best one. Raw pointers are safe: player
	 *  starts are stable level actors and this is cleared each BuildRoundSpawnQueue. */
	TArray<APlayerStart*> RoundAssignedStarts;
	/** Per-weapon regen bookkeeping, keyed by the active role weapon instance. */
	TMap<TWeakObjectPtr<AUTWeapon>, FClutchAmmoRegenState> AmmoRegenState;
	int32 NextAttackerSlot[2];
	uint8 NextAttackingTeamIndex;
	bool bAllowRoundSpawns;
	bool bEndingRound;
	bool bRoundTimedOut;
	bool bPoleCaptured;
	bool bWinCheckQueued;
	bool bFinishingAttackOrderSelection;
};
