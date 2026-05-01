#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTDamageType.h"
#include "Templates/SubclassOf.h"
#include "TimerManager.h"
#include "UTLineUpHelper.h"

// Full include needed (not forward decl) because TUniquePtr<FElimPlusRatingSystem>
// instantiates its destructor at this header — `delete` requires the complete type.
// Must come BEFORE the .generated.h (UHT requires .generated.h to be the last include).
#include "ElimPlusRatingSystem.h"

#include "ElimPlusGame.generated.h"

// Forward declarations to avoid pulling heavy headers here.
class AUTHUD;
class AController;
class APlayerStart;
class AElimPlusStatsReplicator;


USTRUCT()
struct FSpawnPointDataElimPlus
{
	GENERATED_BODY()

	UPROPERTY()
	APlayerStart* PlayerStart;

	UPROPERTY()
	float HeightScore;

	UPROPERTY()
	float TeamSideScore;

	UPROPERTY()
	int32 Team0UsageCount;

	UPROPERTY()
	int32 Team1UsageCount;

	UPROPERTY()
	int32 LastUsedRound;

	FSpawnPointDataElimPlus()
		: PlayerStart(nullptr)
		, HeightScore(0.0f)
		, TeamSideScore(0.0f)
		, Team0UsageCount(0)
		, Team1UsageCount(0)
		, LastUsedRound(-1)
	{
	}

	FSpawnPointDataElimPlus(APlayerStart* InPlayerStart)
		: PlayerStart(InPlayerStart)
		, HeightScore(0.0f)
		, TeamSideScore(0.0f)
		, Team0UsageCount(0)
		, Team1UsageCount(0)
		, LastUsedRound(-1)
	{
	}

	int32 GetUsageCountForTeam(int32 TeamIndex) const
	{
		return (TeamIndex == 0) ? Team0UsageCount : Team1UsageCount;
	}

	void IncrementUsageForTeam(int32 TeamIndex, int32 CurrentRound)
	{
		if (TeamIndex == 0)
		{
			Team0UsageCount++;
		}
		else
		{
			Team1UsageCount++;
		}
		LastUsedRound = CurrentRound;
	}
};



// A complete spawn layout for both teams in one struct.
// Every entry in the precomputed list is GUARANTEED safe by construction.
USTRUCT()
struct FSpawnLayoutElimPlus
{
	GENERATED_BODY()

	// Team 0 spawns (Secondary is nullptr for stack spawns)
	UPROPERTY()
	APlayerStart* T0_Primary;

	UPROPERTY()
	APlayerStart* T0_Secondary;

	// Team 1 spawns (Secondary is nullptr for stack spawns)
	UPROPERTY()
	APlayerStart* T1_Primary;

	UPROPERTY()
	APlayerStart* T1_Secondary;

	// Pre-graded quality metrics
	float MinCrossDistance2D;   // Worst-case horizontal distance between any T0 and T1 spawn
	float QualityScore;        // Overall quality of this layout
	float T0Separation;        // Teammate spread for Team 0 (0 if stacked)
	float T1Separation;        // Teammate spread for Team 1 (0 if stacked)
	int32 UsageCount;          // How many times this layout has been picked this match

	FSpawnLayoutElimPlus()
		: T0_Primary(nullptr), T0_Secondary(nullptr)
		, T1_Primary(nullptr), T1_Secondary(nullptr)
		, MinCrossDistance2D(0.f), QualityScore(0.f)
		, T0Separation(0.f), T1Separation(0.f)
		, UsageCount(0)
	{
	}
};



USTRUCT()
struct FCamperDataElimPlus
{
	GENERATED_BODY()

	FVector LocationHistory[10];
	int32 NextSlot = 0;
	bool bHasFullHistory = false;
	float LastPunishTime = 0.0f;
	int32 ConsecutiveCampCount = 0;
	bool bWarned = false;

	FCamperDataElimPlus()
	{
		for (int i = 0; i < 10; i++) LocationHistory[i] = FVector::ZeroVector;
	}
};


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerACEElimPlus, AUTPlayerState*, PlayerState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerDarkHorseElimPlus, AUTPlayerState*, PlayerState, int32, EnemiesKilled);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerHighDamageCarryElimPlus, AUTPlayerState*, PlayerState, float, DamagePercentage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnClutchSituationStartedElimPlus, AUTPlayerState*, ClutchPlayer, int32, EnemiesAlive);


UCLASS(Config = Game)
class NETCODEPLUS_API AElimPlusGame : public AUTTeamGameMode {
	GENERATED_BODY()

public:
	AElimPlusGame(const FObjectInitializer& ObjectInitializer);

	// === Configurable Timings ===
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Timing")
	float AwardDisplayTime;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Timing")
	float PreRoundCountdown;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Timing")
	float SpawnProtectionTime;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Timing")
	float SpectateDelay;
	// -------- Round Rules --------
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Rules")
	int32 ScoreLimit;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Rules")
	int32 RoundTimeSeconds;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Rules")
	int32 RoundStartDelaySeconds;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Rules")
	bool bAllowRespawnMidRound;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Arena|State")
	bool bRoundInProgress;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Arena|State")
	int32 LastRoundWinningTeamIndex;

	bool ValidateSpawnLocation(const FVector& TestLocation);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
	float MinimumStackSpawnDistance2D = 4000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
	int32 ForceStackEveryNRounds = 3;
	/** Total number of rounds played (including draws) - accessible from Blueprint */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Arena|State")
	int32 TotalRoundsPlayed;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WinBy2")
	bool bWinByTwo = false;

	// -------- Random bot ELO (for balancer + visible variety) --------
	/** When true, each bot gets a stable random ELO in [BotEloMin, BotEloMax]
	 *  used by PickBalancedTeam, the Glicko match math, AND the replicator (so
	 *  the scoreboard shows the assigned ELO instead of a flat 1400). Default
	 *  ON — bots having varied ELO is the more useful baseline for a server
	 *  that mixes bots and humans, and bot-only matches are still clamped to
	 *  ±5 by HumansWithHumanOpposition gating in FlushAtMatchEnd. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "ElimPlus|Testing")
	bool bRandomizeBotElo = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "ElimPlus|Testing", meta = (ClampMin = "0", ClampMax = "5000"))
	int32 BotEloMin = 1400;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "ElimPlus|Testing", meta = (ClampMin = "0", ClampMax = "5000"))
	int32 BotEloMax = 1600;

	/** Restart the current round - useful for handling disconnections or other issues during official matches */
	UFUNCTION(BlueprintCallable, Category = "TeamArena|Round Control")
	void BP_RestartCurrentRound();

	UFUNCTION(BlueprintCallable, Category = "TeamArena|Admin")
	void BP_SetTeamScores(int32 RedScore, int32 BlueScore);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spectating")
	bool useBPSpecFunction;

	/** If true, the game will automatically pause if a player disconnects mid-match. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Rules")
	bool bCompetitiveAutoPause;

	// Blueprint-implementable function for spectating
	UFUNCTION(BlueprintImplementableEvent, Category = "Spectating")
	void BP_SpectatePSImplementation(AUTPlayerState* PlayerState);

	// C++ calls these; BP implements them (no headers, no reflection in C++).
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnSetIntermission(bool bInIntermission, int32 IntermissionRemain);

	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnSetRound(bool bInProgress, int32 RoundRemain, int32 LastWinnerTeamIndex, const TArray<AUTPlayerState*>& Team0AlivePlayers, const TArray<AUTPlayerState*>& Team1AlivePlayers);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Arena|Bridge")
	TArray<AUTPlayerState*> Team0AlivePlayers;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Arena|Bridge")
	TArray<AUTPlayerState*> Team1AlivePlayers;

	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnLastManStanding(int32 LastManTeamIndex, AUTPlayerState* LastManPlayerState);

	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnRoundResults(int32 WinnerTeamIndex, bool bIsDraw, bool bAllWinnersAlive);

	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnDomination(int32 DominatingTeamIndex);

	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Bridge")
	void BP_OnTakesLead(int32 LeadingTeamIndex);

	// Awards
	UFUNCTION(BlueprintCallable, Category = "TeamArena|Achievements")
	void RecordACE(AUTPlayerState* PlayerState);

	UFUNCTION(BlueprintCallable, Category = "TeamArena|Achievements")
	void RecordDarkHorse(AUTPlayerState* PlayerState, int32 EnemiesKilled);

	UFUNCTION(BlueprintCallable, Category = "TeamArena|Achievements")
	void RecordHighDamageCarry(AUTPlayerState* PlayerState, float DamagePercentage);

	/** Minimum damage percentage required for WRECKER achievement (default 70%) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Achievements", meta = (ClampMin = "0.0", ClampMax = "100.0", ToolTip = "Minimum percentage of team damage a player must deal to earn High Damage Carry achievement"))
	float HighDamageCarryThreshold = 75.0f;

	/** Called when a player becomes the last one alive on their team (1vX) */
	UPROPERTY(BlueprintAssignable, Category = "TeamArena|Events")
	FOnClutchSituationStartedElimPlus OnClutchSituationStarted;

	// Awards UPROPERTYs
	UPROPERTY(BlueprintAssignable, Category = "TeamArena|Events")
	FOnPlayerACEElimPlus OnPlayerACE;

	UPROPERTY(BlueprintAssignable, Category = "TeamArena|Events")
	FOnPlayerDarkHorseElimPlus OnPlayerDarkHorse;

	UPROPERTY(BlueprintAssignable, Category = "TeamArena|Events")
	FOnPlayerHighDamageCarryElimPlus OnPlayerHighDamageCarry;

	/** BP Callable functions regarding changing MatchState */
	UFUNCTION(BlueprintCallable, Category = "TeamArena|Match State")
	void BP_SetMatchState_RoundCooldown();

	UFUNCTION(BlueprintCallable, Category = "TeamArena|Match State")
	void BP_SetMatchState_Intermission();

	UFUNCTION(BlueprintCallable, Category = "TeamArena|Match State")
	void BP_SetMatchState_InProgress();

	// -------- Replay opt-in (FlagRun does this) --------
	virtual bool SupportsInstantReplay() const override;

	UFUNCTION(BlueprintNativeEvent)
	bool CanSpectate(APlayerController* Viewer, APlayerState* ViewTarget);
	virtual bool CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget) override;

	/** Optional HUD to use while replay is active (can be left null). */
	//UPROPERTY(EditDefaultsOnly, Category = "Replay")
	//TSubclassOf<AUTHUD> ReplayHUDClass;

	// -------- Overtime Settings --------
	// -------- Wave-Based Overtime Settings --------
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime")
	bool bOvertimeEnabled;

	/** Delay before first damage wave hits after overtime starts */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime", meta = (ClampMin = "0.0"))
	float OvertimeStartDelay;

	/** Base damage dealt by the first wave */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime", meta = (ClampMin = "1.0"))
	float OvertimeBaseDamage;

	/** Damage multiplier applied each wave (1.5 = 50% increase) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime", meta = (ClampMin = "1.0"))
	float OvertimeDamageMultiplier;

	/** Maximum damage per wave (0 = no limit) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime", meta = (ClampMin = "0.0"))
	float OvertimeMaxDamage;

	/** If true, damage waves cannot kill players (leaves them at 1 HP) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime")
	bool bOvertimeNonLethal;
	/** Time interval between damage waves (in seconds) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime", meta = (ClampMin = "0.1"))
	float OvertimeWaveInterval;

	/** Damage type used for overtime waves */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime")
	TSubclassOf<UUTDamageType> OvertimeDamageType;



	// BP hooks for UI/SFX (must be UFUNCTIONs)
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Overtime")
	void BP_OnOvertimeStarted();

	/** Called when a damage wave is about to hit - spawn your UI flash/wave effects here */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Overtime")
	void BP_OnOvertimeWave(float WaveDamage, int32 WaveNumber);

	/** Legacy function - kept for compatibility but no longer called */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|Overtime", meta = (DeprecatedFunction))
	void BP_OnOvertimeTick(float CurrentDPS);


	// === Warmup config ===
	/** If true, play TDM-like warmup: instant (or short-delay) respawns, no rounds, no scoring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Warmup")
	bool bWarmupMode = false;

	/** Seconds before auto-respawn in warmup (0 = instant). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Warmup", meta = (ClampMin = "0.0"))
	float WarmupRespawnDelay = 0.5f;

	// -------- UT overrides --------
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void InitGameState() override;
	//virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandleMatchHasEnded() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void DefaultTimer() override;
	void Logout(AController* Exiting) override;
	//void BeginDestroy() override;
	//static void CleanupCDO();
	/**
	 * NEW: Intercepts match state changes to drive round flow.
	 * This is the key function from UTShowdown.
	 */
	virtual void CallMatchStateChangeNotify() override;
	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser, TSubclassOf<UDamageType> DamageType) override;

	/** Override core UT score check to allow win by 2 */
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;

	// In UE4 these are valid overrides on AGameMode*
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName = TEXT("")) override;
	//virtual float   RatePlayerStart(APlayerStart* Start, AController* Player) override;
	virtual void    RestartPlayer(AController* NewPlayer) override;
	virtual void    ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual void	ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;

	/** Glicko-aware team picker. Refines tie-breaks among smallest-size teams using
	 *  cached effective Elo (lower-strength team wins) — but only outside live rounds.
	 *  Mid-round (bRoundInProgress) it defers entirely to the size-based parent impl. */
	virtual uint8 PickBalancedTeam(AUTPlayerState* PS, uint8 RequestedTeam) override;

	// -------- Victory Audio (Blueprint Editable) --------
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* RedTeamVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* BlueTeamVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* RoundDrawSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* MatchVictorySound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* RedTeamDominatingSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* BlueTeamDominatingSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* RedTeamTakesLeadSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Victory Audio")
	USoundBase* BlueTeamTakesLeadSound;

	// -------- Last Man Standing Sounds --------
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Last Man Standing")
	USoundBase* LastManStandingSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Last Man Standing")
	USoundBase* EnemyLastManStandingSound;

	/** Check and broadcast last man standing status */
	void CheckLastManStanding(int32 Alive0, int32 Alive1);

	/** Broadcast last man standing sounds */
	void BroadcastLastManStanding(int32 LastManTeamIndex, AUTPlayerState* LastManPlayerState);

	void BroadcastOvertimeCountdown(int32 CountdownValue);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Overtime Audio")
	USoundBase* OvertimeAnnouncementSound;

	void BroadcastOvertimeAnnouncement();

	// -------- Anti-Camp Configuration --------

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|AntiCamp")
	bool bEnableAntiCamp = true;

	/** Dimensions of the box (radius/extent) a player must stay within to be flagged */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|AntiCamp")
	float CampThreshold = 400.0f;

	/** How often (in seconds) to sample positions and check for camping */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|AntiCamp")
	float CampCheckInterval = 1.0f;

	/** Minimum time between punishments/warnings (in seconds) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|AntiCamp")
	float CampWarnCooldown = 5.0f;


	// -------- Anti-Camp Events --------

	/** * Triggered when camping is detected.
	 * @param CamperPS - The player detected.
	 * @param CampCount - How many consecutive checks they have failed (1 = Warning, 4+ = Kick/Kill?)
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|AntiCamp")
	void BP_OnCamperDetected(AUTPlayerState* CamperPS, int32 CampCount);

	/** Triggered when a player moves enough to clear their camping status (useful to hide UI warnings) */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|AntiCamp")
	void BP_OnCamperClear(AUTPlayerState* CamperPS);

// -------- Staggered Spawn System --------
/** Queue of controllers waiting to be spawned (processed one per frame) */
	UPROPERTY(Transient)
	TArray<AController*> PendingSpawnQueue;

	/** Timer handle for staggered spawn processing */
	FTimerHandle TimerHandle_StaggeredSpawn;

	/** Process the next player in the spawn queue */
	void ProcessNextSpawn();

	/** Called after all queued spawns are complete */
	void OnAllPlayersSpawned();

protected:
	/** Map to store history for each player without modifying PlayerState class */
    TMap<TWeakObjectPtr<AUTPlayerState>, FCamperDataElimPlus> CamperTracker;

    FTimerHandle TimerHandle_CampCheck;

    void StartCampCheckTimer();
    void StopCampCheckTimer();
    
    UFUNCTION()
    void CheckForCampers();

	/** Replicated AInfo actor that broadcasts per-player stats (Damage, PPR, ELO, LG_Acc)
	 *  to clients for the HUD/Scoreboard. Spawned in HandleMatchHasStarted. */
	UPROPERTY()
	AElimPlusStatsReplicator* StatsReplicator;

	/** Server-side rating system (vendored TeamGlicko2 + Mods.db persistence).
	 *  Owns per-player PlayerRating cache. Updated per-round in EndRoundForTeam,
	 *  flushed to DB + replicator at HandleMatchHasEnded. Non-UObject, server-only. */
	TUniquePtr<FElimPlusRatingSystem> RatingSystem;

	/** Idempotency guard for FlushAtMatchEnd. The engine may invoke
	 *  HandleMatchHasEnded twice (Super dispatches it via state machine, plus our
	 *  derived branches). Reset in InitGame; set true after the first flush. */
	UPROPERTY(Transient)
	bool bRatingFlushedThisMatch = false;

	// -------- Round flow --------
	UPROPERTY()
	AUTPlayerState* RoundWinningKiller;

	UFUNCTION()
	void DelayedEndGame(int32 WinnerTeamIndex, FName Reason);

	UPROPERTY()
	APawn* WinningKillerPawn = nullptr;

	float RoundWinningKillTime;

	void BroadcastKillReplay();

	bool bPendingDarkHorseReplay;
	/** * REWRITTEN: Now just sets the state to MatchIntermission and sets the timer.
	 */
	void StartIntermission(int32 Seconds);
	/** NEW: Delay after dying before forcing a player to spectate teammates */
	/**
	 * NEW: Called when state enters MatchIntermission.
	 * Hides pawns, resets spawns, and forces spectator views.
	 */
	virtual void HandleMatchIntermission();

	/** * REWRITTEN: Now called by the state machine *after* intermission.
	 * Responsible for cleaning old pawns and spawning new ones.
	 */
	void StartNextRound();

	void CheckRoundWinConditions();
	//void PrepareNextRound(); // This function is no longer needed and has been removed.

	bool GetAliveCounts(int32& OutAliveTeam0, int32& OutAliveTeam1) const;

	/**
	 * REWRITTEN: Now handles scoring, checks for game-end, and calls StartIntermission.
	 */
	void EndRoundForTeam(int32 WinnerTeamIndex, FName Reason);

	int32 GetTiebreakWinnerByTeamHealth() const;
	void ResetPlayersForNewRound();
	void CleanupWorldForNewRound();
	//void GrantSpawnProtection(AController* PC);
	//void OnSpawnProtectionBroken(class AUTCharacter* Character);
	void BroadcastRoundResults(int32 WinnerTeamIndex, bool bIsDraw);

	// -------- Victory Message Configuration --------
	/** Updates the victory message class with sounds from GameMode settings */
	void UpdateVictoryMessageSounds();

	// -------- Domination and Lead Tracking --------
	/** Check and broadcast domination/lead messages after a round ends */
	void CheckForDominationAndLead(int32 WinnerTeamIndex);

	/** Broadcast domination message (team is leading by 5+ rounds) */
	void BroadcastDomination(int32 DominatingTeamIndex);

	/** Broadcast takes lead message (team takes the lead from a tie or changes lead) */
	void BroadcastTakesLead(int32 LeadingTeamIndex);

	UPROPERTY(Transient)
	float WinCheckHoldUntilSeconds = 0.f;

	/** used to deny RestartPlayer() except for our forced spawn at round start */
	bool bAllowPlayerRespawns;

	/** Previous round scores for domination/lead detection */
	UPROPERTY(Transient)
	int32 PreviousRedScore;

	UPROPERTY(Transient)
	int32 PreviousBlueScore;

	/** Flag to track if domination message has been broadcast */
	UPROPERTY(Transient)
	bool bHasBroadcastTeamDominating;

	/** Track team sizes at round start for last man standing detection */
	UPROPERTY(Transient)
	int32 Team0StartingSize;

	UPROPERTY(Transient)
	int32 Team1StartingSize;

	/** Track if last man standing has been announced this round for each team */
	UPROPERTY(Transient)
	bool bTeam0LastManAnnounced;

	UPROPERTY(Transient)
	bool bTeam1LastManAnnounced;

	/** Force a dead player into spectate. Prefer teammates; if none alive, allow enemy spectate. */
	void ForceTeamSpectate(class AUTPlayerState* DeadPS);

	/** Finds a living teammate for PS (nullptr if none). */
	class AUTPlayerState* FindAliveTeammate(class AUTPlayerState* PS) const;

	/** Finds a living enemy for PS (nullptr if none). */
	class AUTPlayerState* FindAliveEnemy(class AUTPlayerState* PS) const;

	AUTPlayerState* FindAliveOnTeamPS(int32 TeamIndex) const;
	AUTPlayerState* FindAnyOnTeamPS(int32 TeamIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Team Arena")
	void ForceLosersToViewWinners(int32 WinnerTeamIndex);
	void DebugPlayerStates();
	/** NEW: Called by a timer to force a dead player to spectate after a delay */
	UFUNCTION()
	void DelayedForceSpectate(class AUTPlayerState* DeadPS);
	/** Returns number of living players on a given team index (0/1). */
	int32 CountAliveOnTeam(int32 TeamIndex) const;
	/** Deferred call to HandleMatchHasStarted from BeginPlay */
	UFUNCTION()
	void DeferredHandleMatchStart();

	/** Deferred call to CheckRoundWinConditions from overtime */
	UFUNCTION()
	void DeferredCheckRoundWinConditions();

	/** Timer for delayed win check at round start */
	FTimerHandle InitialWinCheckHandle;

	/** Performs the delayed win check */
	void DelayedInitialWinCheck();

	/** Timer for delaying the round end logic to fix camera bugs */
	FTimerHandle TH_RoundEndDelay;

	/** Function to be called by the timer to end the round */
	UFUNCTION()
	void DelayedEndRound(int32 WinnerTeamIndex, FName Reason);

	/** Deferred call to HandleMatchHasStarted from BeginPlay */
	//UFUNCTION()
	//void DeferredHandleMatchStart();

	/** Deferred call to CheckRoundWinConditions from overtime */
	//UFUNCTION()
	//void DeferredCheckRoundWinConditions();

	// -------- Overtime --------
	void StartOvertime();
	void StopOvertime();
	void ExecuteOvertimeWave();


	// Wave-based overtime state
	FTimerHandle OvertimeWaveTimerHandle;
	FTimerHandle OvertimeCountdownTimerHandle;
	//FTimerHandle TH_NextRound; // No longer needed
	int32 CurrentOvertimeWave = 0;
	float CurrentWaveDamage = 0.0f;

	// Replay HUD helper (optional)
	//void MaybeApplyReplayHUD();

	// -------- Runtime --------
	/** REPURPOSED: This now acts as the master countdown for intermission */
	int32 IntermissionSecondsRemaining = 0;

	float RoundEndTimeSeconds = 0.f;

	// Needs TimerManager.h in the .cpp, but type is fine here.
	FTimerHandle OvertimeTimerHandle;
	//FTimerHandle TH_NextRound;
	//float OvertimeStartTimeSeconds = 0.f;

	// -------- Enhanced Spawn Selection System --------
	UPROPERTY(Transient)
	TArray<FSpawnPointDataElimPlus> AllSpawnPoints;

	UPROPERTY(Transient)
	TArray<APlayerStart*> Team0SelectedSpawns;

	UPROPERTY(Transient)
	TArray<APlayerStart*> Team1SelectedSpawns;

	UPROPERTY(Transient)
	int32 CurrentRoundNumber = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning")
	float SpawnOffsetDistance = 90.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning")
	int32 MaxSpawnOffsetAttempts = 4;

	UPROPERTY(Transient)
	bool bSpawnPointsInitialized = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Weight for distance from enemy spawns when selecting spawn points"))
	float SpawnDistanceWeight = 0.30f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Weight for spawn point height when selecting spawn points"))
	float SpawnHeightWeight = 0.10f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Weight for spawn usage frequency (promotes variety) when selecting spawn points"))
	float SpawnUsageWeight = 0.45f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Weight for separation between team spawn points when selecting spawn points"))
	float SpawnSeparationWeight = 0.15f;

	// -------- Enhanced Spawn Selection Functions --------
	void InitializeSpawnPointSystem();
	void ScoreAllSpawnPoints();
	void SelectOptimalSpawnPairForTeam(int32 TeamIndex);
	void FindMaxDistanceSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, const TArray<APlayerStart*>& EnemySpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary);
	bool IsSpawnOnHomeSide(const FSpawnPointDataElimPlus& SpawnData, int32 TeamIndex) const;
	//void FindMaxDistanceSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, const TArray<APlayerStart*>& EnemySpawns, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary);
	float CalculateMinDistanceToEnemySpawns(APlayerStart* SpawnPoint, const TArray<APlayerStart*>& EnemySpawns);
	TArray<FSpawnPointDataElimPlus*> GetSpawnCandidatesForTeam(int32 TeamIndex);
	FVector FindSafeSpawnOffset(APlayerStart* BaseSpawn, int32 AttemptIndex);
	bool IsLocationClearOfPlayers(const FVector& Location, float CheckRadius = 85.0f);
	void ResetSpawnSelectionForNewRound();
	/** Minimum distance required between team spawns and enemy spawns */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "500.0", ClampMax = "15000.0"))
	float MinimumEnemySpawnDistance;
	// Precomputed layout arrays (filled once at map load, never modified during gameplay)
	TArray<FSpawnLayoutElimPlus> ValidLayouts_2v2;   // Both teams get split spawns
	TArray<FSpawnLayoutElimPlus> ValidLayouts_1v1;   // Both teams stack on single spawn

	// Tuning: maximum teammate separation to still count as a "pair"
	// Beyond this, teammates are too far to help each other
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
	float MaxTeammateSeparation2D = 2500.0f;

	// Tuning: minimum teammate separation for split spawns
	// Below this, they're basically stacked anyway - not worth splitting
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
	float MinTeammateSeparation2D = 600.0f;

	// Precomputation
	void PrecomputeSpawnLayouts();

	// Runtime selection (called at round start)
	void SelectSpawnLayoutForRound();

	// Helper
	float GetMinCrossTeamDistance2D(const TArray<APlayerStart*>& TeamA, const TArray<APlayerStart*>& TeamB);

    // Minimum horizontal distance required from enemy. 
    // Defaults to something high like 3000.0f to force cross-map spawns.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spawning")
	float MinimumEnemyHorizontalDistance = 3000.0f;

	/** Preferred distance between team spawns and enemy spawns (used for scoring) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning", meta = (ClampMin = "1000.0", ClampMax = "15000.0"))
	float PreferredEnemySpawnDistance;
	
	UPROPERTY(Transient)
	AActor* OverriddenPlayerStart;


	// Server Management 
		/** Handle all server management tasks (cleanup, map voting, bots, etc.) */
	void HandleServerManagement();

	/** Handle instance cleanup logic */
	void HandleInstanceCleanup();

	/** Handle map voting logic */
	void HandleMapVoting();

	/** Track empty server time for non-hub cleanup */
	UPROPERTY()
	int32 EmptyServerTime;

	/** Last lobby update timestamp */
	UPROPERTY()
	float LastLobbyUpdateTime;



	/** Track team damage for each round */
	UPROPERTY(Transient)
	float Team0RoundDamage;
	UPROPERTY(Transient)
	float Team1RoundDamage;

	/** Track individual player damage for the current round */
	UPROPERTY(Transient)
	//TMap<AUTPlayerState*, float> PlayerRoundDamage;
	TMap<TWeakObjectPtr<AUTPlayerState>, float> PlayerRoundDamage;

	/** Per-player match-aggregate PPR: sum of completed-round PPRs and round count.
	 *  PPR(Current) shown on scoreboard = Sum / Count (excludes in-progress round).
	 *  Cleared on InitGame (every map load is a fresh match). Round kills come from
	 *  AUTPlayerState::RoundKills (engine resets to 0 in ResetPlayersForNewRound),
	 *  round damage from PlayerRoundDamage (existing tracking). PPR per round =
	 *  RoundKills + RoundDamage * 0.01. */
	TMap<TWeakObjectPtr<AUTPlayerState>, float> PerPlayerMatchPPRSum;
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> PerPlayerMatchPPRRoundCount;

	void CheckRoundAchievements(int32 WinnerTeamIndex, FName Reason);
	void CheckForACE(int32 WinnerTeamIndex);
	void CheckForDarkHorse(int32 WinnerTeamIndex);
	void CheckForHighDamageCarry(int32 WinnerTeamIndex);
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> DarkHorseCandidates;
	// New spawn selection methods
	void FindLeastUsedSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary);
	void FindBalancedRandomSpawnPair(const TArray<FSpawnPointDataElimPlus*>& CandidateSpawns, const TArray<APlayerStart*>& EnemySpawns, int32 TeamIndex, APlayerStart*& OutPrimary, APlayerStart*& OutSecondary);
};