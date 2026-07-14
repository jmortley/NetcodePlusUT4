#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTTeamGameMode.h"
#include "TimerManager.h"
#include "ClutchRoundState.h"
#include "ClutchGameMode.generated.h"

class AClutchRoundState;
class AUTInventory;
class AUTPlayerState;

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

	/** Delay between rounds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Rules", meta = (ClampMin = "0.0"))
	float IntermissionSeconds;

	/** Time captains have to choose their team's attacker rotation before round one. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Clutch|Order", meta = (ClampMin = "1.0"))
	float AttackOrderSelectionSeconds;

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
	void UpdatePole(float DeltaSeconds);
	bool IsCombatPhase() const;
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
	TArray<TWeakObjectPtr<AController>> PendingRoundSpawns;
	TMap<TWeakObjectPtr<AController>, int32> RoundSpawnAttempts;
	int32 NextAttackerSlot[2];
	uint8 NextAttackingTeamIndex;
	bool bAllowRoundSpawns;
	bool bEndingRound;
	bool bRoundTimedOut;
	bool bPoleCaptured;
	bool bWinCheckQueued;
	bool bFinishingAttackOrderSelection;
};
