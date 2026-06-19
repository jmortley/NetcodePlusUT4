// NCPlusCTFGameMode.h - NetcodePlus CTF with improved advantage time and instant replay
#pragma once
#include "NetcodePlus.h"
#include "UTCTFGameState.h"
#include "UTCTFScoring.h"
#include "UTCTFBaseGame.h"

// Full include needed (not forward decl) because TUniquePtr<FNCPlusCTFRatingSystem>
// instantiates its destructor at this header — `delete` requires the complete type.
// Must come BEFORE the .generated.h (UHT requires .generated.h to be the last include).
#include "NCPlusCTFRatingSystem.h"

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

/** Per-player positional dwell accumulator for role inference. Seconds spent in
 *  each zone (own/mid/enemy half, by flag-base-axis projection t=d0/(d0+d1))
 *  plus flag-state-conditioned excursions that distinguish a mid's cover (push
 *  to escort the runner) from fallback (drop back to defend). Server-only,
 *  reset per match. */
struct FNCPlusCTFRoleDwell
{
	float OwnSec      = 0.f;
	float MidSec      = 0.f;
	float EnemySec    = 0.f;
	float FallbackSec = 0.f;   // own half while own flag is out (Held/Dropped)
	float CoverSec    = 0.f;   // enemy half while our team holds the enemy flag
};

UCLASS(Abstract)
class NETCODEPLUS_API ANCPlusCTFGameMode : public AUTCTFBaseGame
{
	GENERATED_UCLASS_BODY()

	/** Stock pause permissions + Mod.ini-gated match-host pause ([NetcodePlus]
	 *  bAllowHostPause — see NCPlusHostPause.h). */
	virtual bool AllowPausing(APlayerController* PC) override;

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

	// ── Spawn Selection (tie-band + freshness; all overridable via Mod.ini [UTPUGS_SPAWN]) ──

	/** Candidates scoring within this margin of the best are treated as equally
	 *  good and one is picked at RANDOM — breaks the deterministic "always one
	 *  side" players reported. Wider = more variety. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnTieBandWidth;

	/** When no flag is active near our base, add up to this bonus to a candidate
	 *  scaled by how long since the team last used it — forces spread across
	 *  unused starts and makes a fresh respawn meaningful again. 0 disables. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnFreshnessBonus;

	/** Seconds since last use at which a start counts as fully fresh (staleness = 1). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnFreshnessWindow;

	/** A flag (enemy carrier or a dropped flag) within this distance of our flag
	 *  base counts as "in the vicinity" → suppress the freshness spread and keep
	 *  safe, scored spawns. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnFlagVicinityRadius;

	/** Hard-exclude spawn starts within this distance of the player's LAST KILLER
	 *  (when that killer is alive and near our spawns — i.e. camping our base) so you
	 *  don't respawn into whoever just fragged you. Never empties the pool. 0 disables. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnKillerAvoidRadius;

	/** When your OWN flag isn't home (stolen or dropped), drop this many of your
	 *  team's starts nearest your flag base form the avoid SET — exactly ONE of
	 *  them is excluded per respawn, rotating through the set (nearest, then
	 *  2nd-nearest, ...) so defenders always keep a base spawn available but
	 *  can't rely on one fixed spot while the flag is out. You still respawn
	 *  biased forward toward the carrier's escape. 0 disables. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnRobbedBaseAvoidCount;

	/** Per-team rotation cursor for the robbed-base exclusion above (which of the
	 *  N nearest starts is blocked this respawn). Runtime only; resets per map. */
	int32 RobbedSpawnRotation[2] = { 0, 0 };

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

	/** If true, spawned players are hidden until their client confirms control */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	bool bEnablePingCompensatedSpawn = true;

	// ── Overtime Configuration ────────────────────────────────────────

	/** Respawn wait time during overtime (seconds). Replaces Epic's extended
	 *  overtime that escalated to 10s. Set to 0 to use normal respawn time. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Overtime")
	float OvertimeRespawnTime;

	// ── Game Flow Overrides ──────────────────────────────────────────
	// NOTE: Floor slide disable is enforced via ATeamArenaCharacter::CanSlide_Implementation()
	// which reads bAllowFloorSlide from this game mode. No RestartPlayer override needed.

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	/** Pin bot-PUG players to their bot-assigned team (see PugRosterTeam). The
	 *  single choke point for login picks, manual switches, and the engine's
	 *  CountdownToBegin auto-balance — all route through ChangeTeam. Non-roster
	 *  joiners (subs/late fills) defer to Super's stock balancing. */
	virtual bool ChangeTeam(AController* Player, uint8 NewTeam = 255, bool bBroadcast = true) override;
	virtual void HandleMatchHasEnded() override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual float RatePlayerStart(APlayerStart* P, AController* Player) override;

	// Unlock entitlement-gated cosmetics: force the player's chosen hat as an OverrideHatClass (which the
	// engine does NOT entitlement-check) so the community master's missing cosmetic entitlements can't
	// strip it. Server-side, never kicks. See impl.
	virtual bool ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass) override;

	/** Own spawn selection (ElimPlus/Wipeout pattern) so the engine pipeline
	 *  can't bypass RatePlayerStart. Curates an own-team pool from the map's
	 *  AUTTeamPlayerStart TeamNum tags (trusted, not mutated) and scores it with
	 *  RatePlayerStart. See NCPlusCTFGameMode.cpp. */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
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

	/** Timer callback: after EndGame holds the match open ncp.CTFReplayHoldSec seconds for
	 *  the end-of-match instant-replay window, this fires the real (deferred) Super::EndGame. */
	void FinishDelayedEndGame();

	/** Server-side rating system for CTF or iCTF (separate ladders, single
	 *  instance per match — bIsInstagib locked at construction).
	 *  Constructed lazily in HandleMatchHasStarted, not BeginPlay: that's the
	 *  earliest point AUTGameMode::bIsInstagib is reliably set by the Instagib
	 *  BP mutator chain (same timing CTFStatsReplicator + MutServerShield use).
	 *  Flushed at HandleMatchHasEnded. Non-UObject, server-only. */
	TUniquePtr<FNCPlusCTFRatingSystem> RatingSystem;

	/** Guard against the engine routing HandleMatchHasEnded twice. Reset in
	 *  InitGame; set true after the first successful flush. */
	UPROPERTY(Transient)
	bool bRatingFlushedThisMatch = false;

	// ── CTF rating: leaver capture + per-match perf config ───────────────

	/** Per-match snapshot of rating-relevant stats, keyed by UniqueId.
	 *  Populated on Logout (while the PlayerState is still intact) and at match
	 *  end from PlayerArray, so rage-quitters/leavers are still rated and the
	 *  team z-scores aren't distorted by a missing roster slot. Cleared in
	 *  InitGame. Non-replicated, server-only. */
	TMap<FString, FNCPlusCTFPlayerInput> MatchStatCache;

	/** UniqueId -> world seconds first seen this match. Drives the leaver
	 *  presence threshold: rate rage-quitters who left late, drop genuine
	 *  early leavers. Populated in PostLogin / HandleMatchHasStarted. */
	TMap<FString, float> PlayerJoinWorldTime;

	/** UniqueId -> positional dwell, accumulated each second by SampleRoleDwell
	 *  while the match is in progress. Resolved into role + OffLean at capture
	 *  time. Cleared in InitGame. */
	TMap<FString, FNCPlusCTFRoleDwell> RoleDwell;

	/** World seconds at match start, and the intended full match length, used
	 *  for the leaver presence fraction. Stamped in HandleMatchHasStarted. */
	float MatchStartWorldTime = 0.f;
	float MatchFullDurationSeconds = 0.f;

	/** CTF perf knobs, loaded from Mod.ini [UTPUGS_STATS] in HandleMatchHasStarted. */
	FNCPlusCTFPerfConfig CTFPerfConfig;

	/** Fraction of MatchFullDurationSeconds a leaver must have been present for
	 *  to be rated (rage-quit dodge guard). Mod.ini override; default 0.5. */
	float CTFRatingMinPresenceFrac = 0.5f;

	/** Weight a kill/death location adds to a fighter's role dwell vs the 1.0 a
	 *  one-second presence sample adds — a fight is a stronger role signal than
	 *  idle standing. Mod.ini [UTPUGS_STATS]; default 4. 0 = positional only. */
	float CTFRoleCombatWeight = 4.f;

	/** Regulation respawn delay (seconds). Applied in HandleMatchHasStarted AFTER
	 *  InitGame's integer ?RespawnWait parse and any BP default, so it wins and
	 *  supports fractional values (e.g. 1.5). Mod.ini [UTPUGS_STATS] CTFRespawnWait.
	 *  Overtime escalation still ramps respawn up from here. */
	float CTFRespawnWait = 1.5f;

	/** Respawn delay for small games (1v1). Used instead of CTFRespawnWait when
	 *  GameSession->MaxPlayers <= CTFSmallGameMaxPlayers. Keyed on player count in
	 *  HandleMatchHasStarted so bot- AND hub-hosted (ruleset) matches both get the
	 *  right value automatically with no ?RespawnWait. Mod.ini [UTPUGS_STATS]
	 *  CTFRespawnWaitSmall; default 1.0. */
	float CTFRespawnWaitSmall = 1.0f;

	/** MaxPlayers at/below which a match is "small" and uses CTFRespawnWaitSmall.
	 *  Mod.ini [UTPUGS_STATS] CTFSmallGameMaxPlayers; default 2 (1v1). Raise to 4
	 *  to also give 2v2 the fast respawn. */
	int32 CTFSmallGameMaxPlayers = 2;

	/** Auto-pause the match when a participant drops out of a bot PUG (?PugId),
	 *  until they (and any others who dropped) rejoin, or an admin unpauses via
	 *  the `pause` command. Uses the engine world-pause (WorldSettings->Pauser).
	 *  No auto-resume timeout yet. Mod.ini [UTPUGS_STATS] AutoPauseOnDrop. */
	bool bAutoPauseOnDrop = true;

	/** True when this match was launched as a bot PUG (?PugId present). */
	bool bIsPugMatch = false;

	/** Bot-assigned teams: lowercased EOS id (== the bot's players.ut4_id, ==
	 *  MutBotEvents' Ut4Id) -> team (0 red, 1 blue). Parsed from ?PugTeams in
	 *  InitGame; consulted in ChangeTeam to pin each rostered player to the side
	 *  the bot balanced, so the engine's warmup auto-balance can't reshuffle the
	 *  match. Empty for non-PUG games and for players who haven't /linked (those
	 *  fall through to the stock balancer). Server-only. */
	TMap<FString, uint8> PugRosterTeam;

	/** True while an auto-pause is currently active. */
	bool bAutoPaused = false;

	/** UniqueIds of dropped participants we're waiting on before resuming. */
	TSet<FString> AutoPauseAwaitIds;

	/** Read the rating-relevant stats off a live AUTPlayerState into Out, then
	 *  resolve its role (OffLean / fractions / label) from accumulated RoleDwell. */
	void CapturePlayerStats(class AUTPlayerState* UTPS, FNCPlusCTFPlayerInput& Out) const;

	/** Sample every living player's zone (own/mid/enemy) + flag-state-conditioned
	 *  cover/fallback into RoleDwell. Called once per second from DefaultTimer
	 *  while the match is in progress. */
	void SampleRoleDwell();

	/** Add Weight to a player's RoleDwell bucket for a world location (projected
	 *  onto the flag-base axis). Shared by SampleRoleDwell (presence, Weight=1)
	 *  and ScoreKill (combat, Weight=CTFRoleCombatWeight). */
	void CreditRoleDwell(class AUTPlayerState* PS, const FVector& Loc, float Weight);

	/** Credits kill+death locations into role dwell, then Super does all scoring. */
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;

	/** Load CTFPerfConfig + CTFRatingMinPresenceFrac from Mod.ini [UTPUGS_STATS]. */
	void LoadCTFPerfConfig();

	/** Auto-pause helpers (server-only). BeginOrHoldAutoPause records a dropped
	 *  participant and (re)points the engine pause marker at a still-present
	 *  player; EndAutoPause clears it; FindAutoPauseMarker returns a present,
	 *  non-dropped PlayerState to hold the pause on (or null if none remain). */
	void BeginOrHoldAutoPause(const FString& LeaverId, const FString& LeaverName);
	void EndAutoPause(const TCHAR* Reason);
	class APlayerState* FindAutoPauseMarker() const;

	/** Override spawn penalty weights + selection knobs from Mod.ini [UTPUGS_SPAWN]. */
	void LoadSpawnConfig();

	/** True if a non-home flag (enemy carrier or a dropped flag) is within
	 *  SpawnFlagVicinityRadius of the given team's flag base. */
	bool IsFlagNearOwnBase(uint8 TeamIndex) const;

	virtual bool PlayerCanRestart_Implementation(APlayerController* Player);
	virtual bool SupportsInstantReplay() const override;

	/** CTF-aware end-game replay: replay the decisive flag capture (the cap that
	 *  ended the match) via ClientPlayInstantReplay, NOT the crash-prone
	 *  ClientQueueCoolMoment/CoolMomentCamStart path. Server-side; no end replay on a
	 *  timelimit end with no recent cap. See the .cpp for the crash rationale. */
	virtual void PickMostCoolMoments(bool bClearCoolMoments = false, int32 CoolMomentsToShow = 1) override;

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

	/** Per-player last ACTUAL spawn (pawn) world location, for a truthful in-match
	 *  rotation/dist log that doesn't depend on the (sometimes stale) StartSpot. */
	TMap<TWeakObjectPtr<AController>, FVector> PlayerLastSpawnLoc;

	/** Penalty multiplier for using the same spawn as 2 spawns ago (0.5 = half score). IG+ default. */
	float SpawnRecentPenaltyMultiplier = 0.5f;

	/** Penalty for spawning within this radius of your last spawn point */
	float SpawnNearLastRadius = 4000.f;

	/** Penalty scale for near-last-spawn distance */
	float SpawnNearLastPenalty = 6.f;

	// ── Team-aware spawn pools (curated from author TeamNum tags) ─────
	// Built lazily on the first ChoosePlayerStart. Server-only; never replicated.

	/** Own-team candidate starts, partitioned by authored TeamNum. */
	TArray<TWeakObjectPtr<APlayerStart>> Team0Spawns;
	TArray<TWeakObjectPtr<APlayerStart>> Team1Spawns;

	/** Server time each start was last chosen (team-wide), for the freshness
	 *  spread. Reset per match. Server-only. */
	TMap<TWeakObjectPtr<APlayerStart>, float> SpawnLastUsedTime;

	/** False until BuildTeamSpawnPools has populated the pools this match. */
	bool bSpawnPoolsBuilt = false;

	/** Bucket each AUTTeamPlayerStart into Team0Spawns / Team1Spawns by its
	 *  authored TeamNum (trusted, not mutated). Plain APlayerStarts are excluded. */
	void BuildTeamSpawnPools();

	// ── Stats Replicator ────────────────────────────────────────────

	/** Replicated stats for scoreboard (grabs, accuracy) */
	UPROPERTY(Transient)
	class ACTFStatsReplicator* CTFStatsRep = nullptr;

	/** Replicates GS->ElapsedTime snapshot at the moment OT begins so the
	 *  HUD can render an OT count-up timer. Engine's
	 *  AUTCTFGameState::OvertimeStartTime isn't replicated. */
	UPROPERTY(Transient)
	class ANCPlusCTFOTInfo* OTInfo = nullptr;

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

	/** World time of the most recent flag capture (any cap), for end-game replay
	 *  selection. The cap that ends the match (scorelimit / golden / mercy) is the
	 *  decisive moment and should be featured over a generic frag. */
	float LastCapTime;

	/** Player who scored the most recent flag capture. */
	TWeakObjectPtr<AUTPlayerState> LastCapPlayer;

	/** Pawn of the player who scored the most recent flag capture, captured at cap
	 *  time. Focus actor for the decisive-cap instant replay (mirrors ElimPlus's
	 *  WinningKillerPawn). GC-nulled UPROPERTY -> null if the capper pawn already died. */
	UPROPERTY()
	APawn* LastCapPawn = nullptr;

	/** Max age (s) for the last cap to still count as "the decider" when EndGame
	 *  runs. A scorelimit/golden/mercy end credits the cap microseconds before
	 *  EndGame, so it is always within this window; a timelimit end (last cap long
	 *  ago) falls through to the stock cool-factor picker. */
	float FeatureCapMaxAgeSeconds = 15.f;

	/** Set by PickMostCoolMoments when it sends an end-of-match instant replay; EndGame then
	 *  defers Super::EndGame (ncp.CTFReplayHoldSec) so the replay isn't cut off by MatchEnded. */
	bool bEndGameReplayActive = false;

	/** Winner/Reason captured when EndGame is deferred for the instant-replay window. */
	UPROPERTY()
	AUTPlayerState* PendingEndGameWinner = nullptr;
	FName PendingEndGameReason;

	/** World time before which a FlagCapture ScoreObject is rejected. Prevents double caps on maps with no geometry between bases. */
	float LastScoreObjectTime;

public:
	virtual void CreateGameURLOptions(TArray<TSharedPtr<TAttributePropertyBase>>& MenuProps);
};
