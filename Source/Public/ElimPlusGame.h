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


// Per-team spawn pool layout. Mirrors AUWipeoutGame's FWipeoutSpawnLayout —
// see WipeoutGame.h for the design rationale. Each layout holds up to N
// candidate spawns per team (N = team size, currently 4); the runtime spawn
// picker (ChoosePlayerStart_Implementation) does dynamic per-player scoring
// within these pools.
USTRUCT()
struct FElimPlusSpawnLayout
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<APlayerStart*> T0_Spawns;

	UPROPERTY()
	TArray<APlayerStart*> T1_Spawns;

	float MinCrossDistance2D;
	float QualityScore;
	int32 UsageCount;

	FElimPlusSpawnLayout()
		: MinCrossDistance2D(0.f), QualityScore(0.f), UsageCount(0)
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

	// Unlock entitlement-gated cosmetics (boxhat etc.): force the chosen hat via OverrideHatClass so the
	// community master's withheld cosmetic entitlements can't strip it. Server-side, never kicks. See impl
	// (mirrors ANCPlusCTFGameMode / AUWipeoutGame).
	virtual bool ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass) override;

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
	virtual void HandlePlayerIntro() override;
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

	/** Stock pause permissions + Mod.ini-gated match-host pause ([NetcodePlus]
	 *  bAllowHostPause — see NCPlusHostPause.h). */
	virtual bool AllowPausing(APlayerController* PC) override;

	/** Defer a host/rcon unpause behind a short server-only resume countdown
	 *  (see NCPlusHostPause::DeferUnpauseForCountdown). */
	virtual bool ClearPause() override;

	/** Override core UT score check to allow win by 2 */
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;

	// In UE4 these are valid overrides on AGameMode*
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName = TEXT("")) override;
	//virtual float   RatePlayerStart(APlayerStart* Start, AController* Player) override;
	virtual void    RestartPlayer(AController* NewPlayer) override;
	virtual void    ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual void	ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	/** ElimPlus arena rule: players NEVER drop their loadout — not on death, not at
	 *  round reset. Override the stock weapon/Enforcer/powerup toss to destroy the
	 *  pawn's inventory in place (AUTCharacter::DiscardAllInventory) so no
	 *  AUTDroppedPickup ever spawns. Candy orbs (BP PreventDeath spawns them as
	 *  separate AUTPickupHealth world actors) are NOT inventory, so unaffected. */
	virtual void DiscardInventory(APawn* Other, AController* Killer) override;

	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;

	/** Glicko-aware team picker. Refines tie-breaks among smallest-size teams using
	 *  cached effective Elo (lower-strength team wins) — but only outside live rounds.
	 *  Mid-round (bRoundInProgress) it defers entirely to the size-based parent impl. */
	virtual uint8 PickBalancedTeam(AUTPlayerState* PS, uint8 RequestedTeam) override;

	/** Full pre-match team rebalance via Tron's TeamGlicko2::TeamBalancer. Walks
	 *  PlayerArray, asks the rating system for the optimal 2-team split, then
	 *  calls MovePlayerToTeam for any player whose team changed. Called once per
	 *  match at the WaitingToStart -> PlayerIntro transition so players can
	 *  watch the shuffle on the auto-shown scoreboard during the countdown. */
	void RebalanceTeamsForMatchStart();

	/** 6-0 blowout balance (publics): make the SINGLE change — one 1-for-1 swap,
	 *  or one move off a team that is both stronger and up a man — that best
	 *  narrows the CURRENT-match PPR gap (who is performing THIS match, not the
	 *  lifetime Glicko that just produced the 6-0). Replaced the full TeamBalancer
	 *  re-partition (2026-07-11), which could re-seat most of the lobby mid-match.
	 *  No-ops when the PPR gap is already small or no single change improves it.
	 *  Armed by EndRoundForTeam, consumed at the next StartNextRound before
	 *  anything spawns (same rationale as the pre-match rebalance). Scores are
	 *  NOT reset. */
	void MidGameShufflePPR();

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

	// Anti-camp. These are the BP/CDO defaults; each is overridable per-server at
	// runtime via Mod.ini [NetcodePlus] (read in InitGame): ElimEnableAntiCamp /
	// ElimCampThreshold / ElimCampCheckInterval / ElimCampWarnCooldown. A key left
	// out of Mod.ini keeps the default below. Detection is C++ (CheckForCampers);
	// the response is Blueprint (BP_OnCamperDetected / BP_OnCamperClear).
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

	/** Idempotency guard for RebalanceTeamsForMatchStart. CountdownToBegin can
	 *  fire multiple times if the lobby loses then re-gains the start condition.
	 *  Reset in InitGame and HandleMatchHasEnded so the next match can rebalance.
	 *  Set true the first time the rebalance runs. */
	UPROPERTY(Transient)
	bool bDidPreMatchRebalance = false;

	/** True when this match was launched as a bot PUG (?PugId on the URL). Set in
	 *  InitGame. Uneven-team health scaling is gated to NON-PUG games only. */
	bool bIsPugMatch = false;

	/** 6-0 blowout mid-game shuffle (non-PUG, requires ?BalanceTeams): default
	 *  ON; Mod.ini [NetcodePlus] ElimMidGameShuffle=false disables. See
	 *  MidGameShufflePPR. */
	bool bElimMidGameShuffle = true;
	/** Armed by EndRoundForTeam when the score reaches exactly 6-0; consumed at
	 *  the next StartNextRound. Fires at most once per match (bDidMidGameShuffle). */
	UPROPERTY(Transient)
	bool bPendingMidGameShuffle = false;
	UPROPERTY(Transient)
	bool bDidMidGameShuffle = false;

	/** Uneven-team health scaling (NON-PUG only): when the teams differ in size,
	 *  the short-handed team spawns tougher and the larger team softer, scaled
	 *  PROPORTIONALLY to the head-count gap — ElimUnevenHealthPct% per missing
	 *  player (4v5 = ±5%, 4v6 = ±10%, ...), capped at ±50%. Read from Mod.ini
	 *  [NetcodePlus] in InitGame (ElimUnevenHealthScaling / ElimUnevenHealthPct).
	 *  HP is read from the pawn's BP-set HealthMax and scaled — never hardcoded;
	 *  armor is left untouched. */
	bool  bElimUnevenHealthScaling = true;
	float ElimUnevenHealthPct      = 5.f;

	/** Scale a freshly-spawned pawn's HealthMax+Health for uneven teams (non-PUG).
	 *  No-op on even teams / PUGs / when disabled. Server-only. */
	void ApplyUnevenTeamHealthScaling(class AUTCharacter* Char);

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

	// -------- Spawn Selection (Wipeout-style: per-team pool + dynamic per-player scoring) --------
	/** Flat list of every APlayerStart found at map-load time. Source data for
	 *  PrecomputeSpawnLayouts. */
	UPROPERTY(Transient)
	TArray<APlayerStart*> AllSpawnPointsList;

	/** Per-team pools selected by SelectSpawnLayoutForRound — N spawns per team
	 *  (N = team size, currently 4). ChoosePlayerStart_Implementation does
	 *  dynamic per-player scoring within these pools. */
	UPROPERTY(Transient)
	TArray<APlayerStart*> Team0SelectedSpawns;

	UPROPERTY(Transient)
	TArray<APlayerStart*> Team1SelectedSpawns;

	/** Cached side splits from PrecomputeSpawnLayouts (winning multi-axis side
	 *  partition). Used as fallback if all curated layouts get exhausted. */
	UPROPERTY(Transient)
	TArray<APlayerStart*> PrecomputedSideA;

	UPROPERTY(Transient)
	TArray<APlayerStart*> PrecomputedSideB;

	UPROPERTY(Transient)
	int32 CurrentRoundNumber = 0;

	UPROPERTY(Transient)
	bool bSpawnPointsInitialized = false;

	/** Precomputed layouts ranked by MinCrossDistance2D. _4v4 covers the standard
	 *  team-spawn case; _1v1 are single-spawn-per-team layouts used periodically
	 *  (every 3rd round) for tighter close-quarters rounds. */
	TArray<FElimPlusSpawnLayout> ValidLayouts_4v4;
	TArray<FElimPlusSpawnLayout> ValidLayouts_1v1;

	/** Hard floor on enemy distance during ChoosePlayerStart_Implementation —
	 *  any candidate spawn closer than this to a living enemy is rejected
	 *  (with 3-tier fallback if all candidates fail). Mirrors WipeoutGame's
	 *  default; raised from the original 2800 once 4v4 made tighter spawns
	 *  problematic. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Arena|Spawning",
		meta = (ClampMin = "500.0", ClampMax = "15000.0"))
	float MinimumEnemySpawnDistance = 4000.0f;

	void InitializeSpawnPointSystem();
	void PrecomputeSpawnLayouts();
	void SelectSpawnLayoutForRound();
	void ResetSpawnSelectionForNewRound();
	
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

	/** Per-player OVERKILL-INCLUSIVE match-cumulative damage (each hit counted at full
	 *  value incl. the portion beyond victim HP). Drives the scoreboard DMG column via
	 *  AElimPlusStatsReplicator — NOT the engine AUTPlayerState::DamageDone, which stays
	 *  overkill-stripped for StatSQL. Accumulated in ScoreDamage; cleared on InitGame;
	 *  entry removed on Logout. */
	TMap<TWeakObjectPtr<AUTPlayerState>, float> PerPlayerMatchDamage;

public:
	/** Server-only: overkill-inclusive match damage for a player (0 if none).
	 *  Read by AElimPlusStatsReplicator to populate the scoreboard DMG column. */
	float GetMatchDamageForPlayer(AUTPlayerState* PS) const;
protected:

	void CheckRoundAchievements(int32 WinnerTeamIndex, FName Reason);
	void CheckForACE(int32 WinnerTeamIndex);
	void CheckForDarkHorse(int32 WinnerTeamIndex);
	void CheckForHighDamageCarry(int32 WinnerTeamIndex);
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> DarkHorseCandidates;
};