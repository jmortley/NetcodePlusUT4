#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTDamageType.h"
#include "Templates/SubclassOf.h"
#include "TimerManager.h"
#include "UTLineUpHelper.h"
#include "WipeoutGame.generated.h"

// Forward declarations
class AUTHUD;
class AController;
class APlayerStart;


// ============================================================================
// Spawn Layout (reused from TeamArenaGame for round-start spawns)
// ============================================================================

USTRUCT()
struct FWipeoutSpawnLayout
{
	GENERATED_BODY()

	/** Spawn points for Team 0 (up to 4, one per player) */
	UPROPERTY()
	TArray<APlayerStart*> T0_Spawns;

	/** Spawn points for Team 1 (up to 4, one per player) */
	UPROPERTY()
	TArray<APlayerStart*> T1_Spawns;

	float MinCrossDistance2D;
	float QualityScore;
	int32 UsageCount;

	FWipeoutSpawnLayout()
		: MinCrossDistance2D(0.f), QualityScore(0.f)
		, UsageCount(0)
	{
	}
};


// ============================================================================
// Per-player respawn tracking
// ============================================================================

USTRUCT()
struct FPendingRespawn
{
	GENERATED_BODY()

	/** Timer handle for this player's respawn */
	FTimerHandle RespawnTimerHandle;

	/** How long total this respawn takes */
	UPROPERTY()
	float TotalRespawnTime;

	/** World time when the respawn timer started */
	UPROPERTY()
	float RespawnStartTime;

	/** Which team death index triggered this respawn delay */
	UPROPERTY()
	int32 DeathIndex;

	FPendingRespawn()
		: TotalRespawnTime(0.f)
		, RespawnStartTime(0.f)
		, DeathIndex(0)
	{
	}
};


// ============================================================================
// Delegates
// ============================================================================

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerHighDamageCarryWipeout, AUTPlayerState*, PlayerState, float, DamagePercentage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnClutchSituationStartedWipeout, AUTPlayerState*, ClutchPlayer, int32, EnemiesAlive);


// ============================================================================
// UWipeoutGame — Diabotical-style Wipeout for UT4
// ============================================================================

UCLASS(Config = Game)
class NETCODEPLUS_API AUWipeoutGame : public AUTTeamGameMode
{
	GENERATED_BODY()

public:
	AUWipeoutGame(const FObjectInitializer& ObjectInitializer);

	// =======================================================================
	// WIPEOUT-SPECIFIC CONFIGURATION
	// =======================================================================

	/**
	 * Escalating respawn delays indexed by team death count.
	 * Death 0 uses index 0, death 1 uses index 1, etc.
	 * Deaths beyond the array length use the last element (cap).
	 * Blueprint-configurable so designers can tune the curve.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Respawn")
	TArray<float> RespawnDelays;

	/** Spawn protection duration after mid-round respawn (seconds) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Respawn", meta = (ClampMin = "0.0"))
	float RespawnProtectionTime;

	/** Brief grace period (seconds) after a kill before checking wipeout.
	 *  Prevents false wipeouts from simultaneous multi-kills. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Respawn", meta = (ClampMin = "0.0"))
	float WipeoutGracePeriod;

	/** If true, respawn timer escalates per-team. If false, per-individual deaths. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Respawn")
	bool bTeamSharedDeathCounter;


	// =======================================================================
	// ABILITY (e.g. healing bubble — piggybacks on Epic's boost system)
	// =======================================================================

	/** Ability class given to players on every spawn (e.g. healing bubble).
	 *  Leave None to disable abilities entirely. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Ability")
	TSubclassOf<AUTInventory> SpawnAbilityClass;


	// =======================================================================
	// TIMING & ROUND RULES
	// =======================================================================

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Timing")
	float AwardDisplayTime;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Timing")
	float PreRoundCountdown;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Timing")
	float SpectateDelay;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Rules")
	int32 ScoreLimit;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Rules")
	int32 RoundTimeSeconds;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	bool bRoundInProgress;

	/** True when round timer expired — no more respawns, fight to the last */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	bool bInSuddenDeath;

	/** True during the 3-second grace period before sudden death activates */
	bool bSuddenDeathPending = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	int32 LastRoundWinningTeamIndex;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	int32 TotalRoundsPlayed;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wipeout|WinBy2")
	bool bWinByTwo = false;


	// =======================================================================
	// WIPEOUT RUNTIME STATE
	// =======================================================================

	/** Total deaths this round for each team (drives respawn delay escalation) */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	int32 Team0DeathCount;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Wipeout|State")
	int32 Team1DeathCount;

	/** Per-individual death count (used if bTeamSharedDeathCounter is false) */
	UPROPERTY(Transient)
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> PlayerDeathCounts;

	/** Players currently waiting to respawn */
	UPROPERTY(Transient)
	TMap<TWeakObjectPtr<AUTPlayerState>, FPendingRespawn> PendingRespawns;

	/** Players currently under spawn protection */
	UPROPERTY(Transient)
	TMap<TWeakObjectPtr<AUTPlayerState>, float> SpawnProtectedUntil;


	// =======================================================================
	// LINK GUN TEAM HEALING
	// When stock Link Gun beam (UTDMG_Link_Alt) hits a teammate, heal them
	// instead of dealing damage. Set LinkHealPerSecond = 0 to disable.
	// =======================================================================

	/** HP healed per second by Link beam on teammates. 0 = disabled. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|LinkHeal")
	float LinkHealPerSecond;

	/** Max HP a player can be healed to (capped here, not at full stack). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|LinkHeal")
	int32 LinkHealMaxHP;

	/** Fractional heal accumulator per player (beam ticks faster than 1HP). */
	UPROPERTY(Transient)
	TMap<TWeakObjectPtr<AUTPlayerState>, float> LinkHealAccumulator;


	// =======================================================================
	// SPAWN SYSTEM (reused from TeamArena for round-start)
	// =======================================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wipeout|Spawning")
	float MinimumStackSpawnDistance2D = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wipeout|Spawning")
	float MaxTeammateSeparation2D = 2500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wipeout|Spawning")
	float MinTeammateSeparation2D = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wipeout|Spawning")
	float MinimumEnemyHorizontalDistance = 3000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Spawning", meta = (ClampMin = "500.0"))
	float MinimumEnemySpawnDistance = 2800.0f;

	/** Minimum distance from living enemies for mid-round respawns */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Spawning", meta = (ClampMin = "500.0"))
	float MidRoundMinEnemyDistance = 2000.0f;

	UPROPERTY(Transient)
	TArray<APlayerStart*> Team0SelectedSpawns;

	UPROPERTY(Transient)
	TArray<APlayerStart*> Team1SelectedSpawns;

	UPROPERTY(Transient)
	int32 CurrentRoundNumber = 0;

	UPROPERTY(Transient)
	AActor* OverriddenPlayerStart;

	TArray<FWipeoutSpawnLayout> ValidLayouts_2v2;
	TArray<FWipeoutSpawnLayout> ValidLayouts_1v1;


	// =======================================================================
	/** If true, spawned players are hidden until their client confirms control (reduces high-ping spawn disorientation) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Spawning")
	bool bEnablePingCompensatedSpawn = true;

	// SIPHON POWERUP (spawned at highest sniper location)
	// =======================================================================

	/** Blueprint pickup class to spawn at highest sniper location.
	 *  Set in the BP subclass to a child of PowerupBase with custom mesh/ghost.
	 *  If None, no siphon pickup is spawned. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Siphon")
	TSubclassOf<class AUTPickupInventory> SiphonPickupClass;

	/** The siphon pickup spawned at the highest sniper weapon base location */
	UPROPERTY(Transient)
	class AUTPickupInventory* SiphonPickup;

	/** Finds the highest sniper weapon base and spawns a Siphon powerup pickup there */
	void SpawnSiphonPickup();


	// =======================================================================
	// VICTORY / AUDIO
	// =======================================================================

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* RedTeamVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* BlueTeamVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* RoundDrawSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* MatchVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* RedTeamDominatingSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* BlueTeamDominatingSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* RedTeamTakesLeadSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Victory Audio")
	USoundBase* BlueTeamTakesLeadSound;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime Audio")
	USoundBase* OvertimeAnnouncementSound;


	// =======================================================================
	// OVERTIME (wave-based, same system as TeamArena)
	// =======================================================================

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime")
	bool bOvertimeEnabled;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime", meta = (ClampMin = "0.0"))
	float OvertimeStartDelay;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime", meta = (ClampMin = "1.0"))
	float OvertimeBaseDamage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime", meta = (ClampMin = "1.0"))
	float OvertimeDamageMultiplier;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime", meta = (ClampMin = "0.0"))
	float OvertimeMaxDamage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime")
	bool bOvertimeNonLethal;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime", meta = (ClampMin = "0.1"))
	float OvertimeWaveInterval;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Overtime")
	TSubclassOf<UUTDamageType> OvertimeDamageType;


	// =======================================================================
	// WARMUP
	// =======================================================================

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Warmup")
	bool bWarmupMode = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Warmup", meta = (ClampMin = "0.0"))
	float WarmupRespawnDelay = 0.5f;


	// =======================================================================
	// DAMAGE TRACKING (for High Damage Carry achievement)
	// =======================================================================

	UPROPERTY(Transient) float Team0RoundDamage;
	UPROPERTY(Transient) float Team1RoundDamage;
	UPROPERTY(Transient) TMap<TWeakObjectPtr<AUTPlayerState>, float> PlayerRoundDamage;

	/**
	 * Per-life mutual damage tracking: key = (Attacker, Victim) pair encoded as uint64,
	 * value = damage dealt. Cleared on respawn. Used for death recap messages.
	 */
	TMap<uint64, int32> LifeDamageMap;

	/** Encode two PlayerState pointers into a single uint64 key for LifeDamageMap */
	static uint64 MakeDamagePairKey(const AUTPlayerState* From, const AUTPlayerState* To);

	/** Get damage dealt from one player to another this life */
	int32 GetLifeDamage(const AUTPlayerState* From, const AUTPlayerState* To) const;

	/** Clear all life-damage entries involving a player (called on respawn) */
	void ClearLifeDamageFor(AUTPlayerState* PS);

	/** Send private death recap to victim showing mutual damage with killer */
	void SendDeathRecap(AUTPlayerState* Victim, AUTPlayerState* Killer);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Achievements", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float HighDamageCarryThreshold = 60.0f;


	// =======================================================================
	// BLUEPRINT BRIDGE EVENTS
	// =======================================================================

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnSetIntermission(bool bInIntermission, int32 IntermissionRemain);

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnSetRound(bool bInProgress, int32 RoundRemain, int32 LastWinnerTeamIndex,
		int32 T0Alive, int32 T1Alive, int32 T0Deaths, int32 T1Deaths);

	/** Called when a player dies and their respawn timer starts */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnPlayerWaitingRespawn(AUTPlayerState* PlayerState, float RespawnDelay, int32 TeamDeathCount);

	/** Called each second as a player's respawn timer ticks down */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnPlayerRespawnCountdown(AUTPlayerState* PlayerState, int32 SecondsRemaining);

	/** Called when a player respawns mid-round */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnPlayerRespawnedMidRound(AUTPlayerState* PlayerState);

	/** Called when a team is fully wiped out */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnTeamWipeout(int32 WipedTeamIndex);

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnRoundResults(int32 WinnerTeamIndex, bool bIsDraw);

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnDomination(int32 DominatingTeamIndex);

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnTakesLead(int32 LeadingTeamIndex);

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Overtime")
	void BP_OnOvertimeStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Overtime")
	void BP_OnOvertimeWave(float WaveDamage, int32 WaveNumber);

	/** Called when a player becomes the last alive on their team (clutch situation) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Wipeout|Bridge")
	void BP_OnLastPlayerAlive(int32 TeamIndex, AUTPlayerState* LastPlayer, int32 EnemiesAlive);


	// =======================================================================
	// BLUEPRINT-ASSIGNABLE DELEGATES
	// =======================================================================

	UPROPERTY(BlueprintAssignable, Category = "Wipeout|Events")
	FOnPlayerHighDamageCarryWipeout OnPlayerHighDamageCarry;

	UPROPERTY(BlueprintAssignable, Category = "Wipeout|Events")
	FOnClutchSituationStartedWipeout OnClutchSituationStarted;


	// =======================================================================
	// BLUEPRINT-CALLABLE FUNCTIONS
	// =======================================================================

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Round Control")
	void BP_RestartCurrentRound();

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Admin")
	void BP_SetTeamScores(int32 RedScore, int32 BlueScore);

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Match State")
	void BP_SetMatchState_RoundCooldown();

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Match State")
	void BP_SetMatchState_Intermission();

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Match State")
	void BP_SetMatchState_InProgress();

	/** Returns the respawn delay for the next death on a given team */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Wipeout|Respawn")
	float GetCurrentRespawnDelay(int32 TeamIndex) const;

	/** Returns the respawn delay for a specific death index */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Wipeout|Respawn")
	float GetRespawnDelayForDeathIndex(int32 DeathIndex) const;

	/** Returns seconds remaining on a player's respawn timer (0 if not pending) */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Wipeout|Respawn")
	float GetPlayerRespawnTimeRemaining(AUTPlayerState* PS) const;

	/** Returns true if a player is currently waiting to respawn */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Wipeout|Respawn")
	bool IsPlayerWaitingToRespawn(AUTPlayerState* PS) const;

	/** Returns number of players currently waiting to respawn on a team */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Wipeout|Respawn")
	int32 CountPendingRespawnsOnTeam(int32 TeamIndex) const;

	/** Returns true if any teammate will respawn within the given number of seconds */
	bool HasImminentRespawnOnTeam(int32 TeamIndex, float WithinSeconds) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spectating")
	bool useBPSpecFunction;

	UFUNCTION(BlueprintImplementableEvent, Category = "Spectating")
	void BP_SpectatePSImplementation(AUTPlayerState* PlayerState);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Wipeout|Rules")
	bool bCompetitiveAutoPause;


	// =======================================================================
	// UT OVERRIDES
	// =======================================================================

	virtual bool SupportsInstantReplay() const override;

	UFUNCTION(BlueprintNativeEvent)
	bool CanSpectate(APlayerController* Viewer, APlayerState* ViewTarget);
	virtual bool CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget) override;

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;

	/** Replicated damage broadcaster — spawned in BeginPlay, replicates DamageDone to clients */
	UPROPERTY(Transient)
	class AWipeoutDamageReplicator* DamageReplicator;
	virtual void InitGameState() override;
	virtual void HandleMatchHasStarted() override;
	virtual void DefaultTimer() override;
	void Logout(AController* Exiting) override;

	virtual void CallMatchStateChangeNotify() override;
	virtual bool CheckRelevance_Implementation(AActor* Other) override;
	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser, TSubclassOf<UDamageType> DamageType) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;

	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName = TEXT("")) override;
	virtual void    RestartPlayer(AController* NewPlayer) override;
	virtual void    ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual void    ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	virtual APawn*  SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;


protected:

	// =======================================================================
	// ROUND FLOW
	// =======================================================================

	void StartIntermission(int32 Seconds);
	virtual void HandleMatchIntermission();
	void StartNextRound();
	/** Reset all pickup respawn timers at round start (Belt/Amp get fresh cycle) */
	void ResetPickupTimers();

	// --- Shield Belt substitution ---
	// If a map has no ShieldBelt pickup, the first Chest/Vest pickup is
	// converted into a ShieldBelt so every map has belt control.
	/** True once we've confirmed the map has a native ShieldBelt pickup */
	bool bMapHasShieldBelt = false;
	/** First Chest/Vest pickup found — kept alive until BeginPlay resolves it */
	UPROPERTY(Transient)
	AUTPickupInventory* PendingVestPickup = nullptr;
	/** Resolve the vest→belt swap after all actors have been CheckRelevance'd */
	void ResolveShieldBeltSubstitution();
	void CheckWipeoutCondition();
	void EndRoundForTeam(int32 WinnerTeamIndex, FName Reason);
	bool GetAliveCounts(int32& OutAliveTeam0, int32& OutAliveTeam1) const;
	int32 GetTiebreakWinnerByTeamHealth() const;
	void ResetPlayersForNewRound();
	void CleanupWorldForNewRound();
	void BroadcastRoundResults(int32 WinnerTeamIndex, bool bIsDraw);

	UFUNCTION()
	void DelayedEndRound(int32 WinnerTeamIndex, FName Reason);

	UFUNCTION()
	void DelayedEndGame(int32 WinnerTeamIndex, FName Reason);

	/** Override EndGame to guard against replay crashes in PIE/standalone.
	 *  Only calls PickMostCoolMoments if DemoNetDriver is running. */
	virtual void EndGame(AUTPlayerState* Winner, FName Reason) override;

	UFUNCTION()
	void DeferredHandleMatchStart();

	UFUNCTION()
	void DeferredCheckWipeout();

	void DelayedInitialWinCheck();


	// =======================================================================
	// WIPEOUT RESPAWN SYSTEM (the core new mechanic)
	// =======================================================================

	/** Start a respawn timer for a dead player */
	void StartRespawnTimer(AUTPlayerState* DeadPS);

	/** Called when a player's respawn timer fires — actually respawns them */
	UFUNCTION()
	void OnRespawnTimerFired(AUTPlayerState* PS);

	/** Cancel all pending respawn timers (round end / cleanup) */
	void CancelAllPendingRespawns();

	/** Cancel a specific player's pending respawn */
	void CancelPendingRespawn(AUTPlayerState* PS);

	/** Get the respawn delay for the Nth death on a team */
	float ComputeRespawnDelay(int32 TeamIndex, AUTPlayerState* PS) const;

	/** Respawn countdown tick (fires every second for BP_OnPlayerRespawnCountdown) */
	void TickRespawnCountdowns();
	FTimerHandle RespawnCountdownTickHandle;


	// =======================================================================
	// MID-ROUND SPAWN SELECTION
	// =======================================================================

	/** Choose a spawn point for a mid-round respawn (far from enemies) */
	AActor* ChooseMidRoundSpawn(AController* Player);


	// =======================================================================
	// SPECTATING
	// =======================================================================

	void ForceTeamSpectate(AUTPlayerState* DeadPS);
	AUTPlayerState* FindAliveTeammate(AUTPlayerState* PS) const;
	AUTPlayerState* FindAliveEnemy(AUTPlayerState* PS) const;
	AUTPlayerState* FindAliveOnTeamPS(int32 TeamIndex) const;
	AUTPlayerState* FindAnyOnTeamPS(int32 TeamIndex) const;
	int32 CountAliveOnTeam(int32 TeamIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Wipeout")
	void ForceLosersToViewWinners(int32 WinnerTeamIndex);


	// =======================================================================
	// SPAWN LAYOUT SYSTEM (precomputed, for round-start only)
	// =======================================================================

	void InitializeSpawnPointSystem();
	void PrecomputeSpawnLayouts();
	void SelectSpawnLayoutForRound();
	void ResetSpawnSelectionForNewRound();

	UPROPERTY(Transient)
	TArray<APlayerStart*> AllSpawnPointsList;

	/** Full geographic side arrays from anchor-based clustering.
	 *  Persisted at map load so ChoosePlayerStart can use them without re-clustering.
	 *  Contains ALL spawns on each side (no top-N filtering). */
	TArray<APlayerStart*> PrecomputedSideA;
	TArray<APlayerStart*> PrecomputedSideB;

	UPROPERTY(Transient)
	bool bSpawnPointsInitialized = false;


	// =======================================================================
	// OVERTIME
	// =======================================================================

	void StartOvertime();
	void StopOvertime();
	void ExecuteOvertimeWave();
	void BroadcastOvertimeCountdown(int32 CountdownValue);
	void BroadcastOvertimeAnnouncement();

	FTimerHandle OvertimeWaveTimerHandle;
	FTimerHandle OvertimeCountdownTimerHandle;
	int32 CurrentOvertimeWave = 0;
	float CurrentWaveDamage = 0.0f;


	// =======================================================================
	// SERVER MANAGEMENT
	// =======================================================================

	void HandleServerManagement();
	void HandleInstanceCleanup();
	void HandleMapVoting();

	UPROPERTY() int32 EmptyServerTime;
	UPROPERTY() float LastLobbyUpdateTime;


	// =======================================================================
	// DOMINATION / LEAD TRACKING
	// =======================================================================

	void CheckForDominationAndLead(int32 WinnerTeamIndex);
	void BroadcastDomination(int32 DominatingTeamIndex);
	void BroadcastTakesLead(int32 LeadingTeamIndex);
	void UpdateVictoryMessageSounds();

	UPROPERTY(Transient) int32 PreviousRedScore;
	UPROPERTY(Transient) int32 PreviousBlueScore;
	UPROPERTY(Transient) bool bHasBroadcastTeamDominating;


	// =======================================================================
	// ACHIEVEMENT: HIGH DAMAGE CARRY
	// =======================================================================

	void CheckForHighDamageCarry(int32 WinnerTeamIndex);

	UFUNCTION(BlueprintCallable, Category = "Wipeout|Achievements")
	void RecordHighDamageCarry(AUTPlayerState* PlayerState, float DamagePercentage);


	// =======================================================================
	// REPLAY
	// =======================================================================

	void BroadcastKillReplay();

	UPROPERTY() AUTPlayerState* RoundWinningKiller;
	UPROPERTY() APawn* WinningKillerPawn = nullptr;
	float RoundWinningKillTime;


	// =======================================================================
	// INTERNAL RUNTIME
	// =======================================================================

	int32 IntermissionSecondsRemaining = 0;
	float RoundEndTimeSeconds = 0.f;
	float WinCheckHoldUntilSeconds = 0.f;
	bool bAllowPlayerRespawns;

	FTimerHandle TH_RoundEndDelay;
	FTimerHandle InitialWinCheckHandle;
	FTimerHandle OvertimeTimerHandle;

	/** Tracks if last-player-alive has been announced for each team this round */
	bool bTeam0LastAliveAnnounced;
	bool bTeam1LastAliveAnnounced;

	/** Track team sizes at round start */
	int32 Team0StartingSize;
	int32 Team1StartingSize;
};
