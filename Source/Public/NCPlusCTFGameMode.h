// NCPlusCTFGameMode.h - NetcodePlus CTF with improved advantage time and instant replay
#pragma once
#include "NetcodePlus.h"
#include "UTCTFGameState.h"
#include "UTCTFScoring.h"
#include "UTCTFBaseGame.h"
#include "Containers/Ticker.h"

// Full include needed (not forward decl) because TUniquePtr<FNCPlusCTFRatingSystem>
// instantiates its destructor at this header — `delete` requires the complete type.
// Must come BEFORE the .generated.h (UHT requires .generated.h to be the last include).
#include "NCPlusCTFRatingSystem.h"

#include "NCPlusCTFGameMode.generated.h"

class ANCAutoPauseState;

// Safe property access across DLL boundary — uses runtime UProperty reflection
// instead of direct member access which has wrong offsets due to layout mismatch.
// The plugin DLL's compiled class layout differs from the engine DLL's, so
// CTFGameState->bPlayingAdvantage reads garbage. These helpers do runtime name
// lookup via FindField which always returns the correct offset.
namespace NCPlusReflection
{
	template <typename PropertyType>
	inline PropertyType* FindCachedProperty(UObject* Obj, const TCHAR* PropName)
	{
		if (!Obj) return nullptr;
		// Shipping UClasses and their UProperty tables are stable for process life.
		// Cache by runtime class + property name so render-rate HUD reads do not walk
		// the reflection field chain every frame. Each PropertyType gets its own map.
		static TMap<UClass*, TMap<FName, PropertyType*>> Cache;
		TMap<FName, PropertyType*>& ClassCache = Cache.FindOrAdd(Obj->GetClass());
		const FName Name(PropName);
		if (PropertyType** Found = ClassCache.Find(Name)) return *Found;
		PropertyType* Resolved = FindField<PropertyType>(Obj->GetClass(), PropName);
		ClassCache.Add(Name, Resolved);
		return Resolved;
	}

	inline bool GetBool(UObject* Obj, const TCHAR* PropName)
	{
		UBoolProperty* Prop = FindCachedProperty<UBoolProperty>(Obj, PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : false;
	}
	inline void SetBool(UObject* Obj, const TCHAR* PropName, bool Value)
	{
		UBoolProperty* Prop = FindCachedProperty<UBoolProperty>(Obj, PropName);
		if (Prop) Prop->SetPropertyValue_InContainer(Obj, Value);
	}
	inline uint8 GetByte(UObject* Obj, const TCHAR* PropName)
	{
		UByteProperty* Prop = FindCachedProperty<UByteProperty>(Obj, PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : 0;
	}
	inline void SetByte(UObject* Obj, const TCHAR* PropName, uint8 Value)
	{
		UByteProperty* Prop = FindCachedProperty<UByteProperty>(Obj, PropName);
		if (Prop) Prop->SetPropertyValue_InContainer(Obj, Value);
	}
	inline int32 GetInt(UObject* Obj, const TCHAR* PropName)
	{
		UIntProperty* Prop = FindCachedProperty<UIntProperty>(Obj, PropName);
		return Prop ? Prop->GetPropertyValue_InContainer(Obj) : 0;
	}
	inline void SetObject(UObject* Obj, const TCHAR* PropName, UObject* Value)
	{
		UObjectPropertyBase* Prop = FindCachedProperty<UObjectPropertyBase>(Obj, PropName);
		if (Prop) Prop->SetObjectPropertyValue_InContainer(Obj, Value);
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

	/** Defer host/rcon and automatic-pause resumes behind a pause-safe countdown.
	 *  Automatic pauses use their replicated, cancellable state machine; other
	 *  pauses use NCPlusHostPause::DeferUnpauseForCountdown. */
	virtual bool ClearPause() override;

	/** Prevent a departing pause marker's engine cleanup from masquerading as a
	 *  player-requested resume before Logout can update the exact-ID wait. */
	virtual void ForceClearUnpauseDelegates(AActor* PauseActor) override;

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

	/** Hard-exclude starts with direct LOS to the enemy flag carrier inside this
	 *  radius when a safer start remains. Falls back rather than failing to spawn.
	 *  0 disables. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Spawning")
	float SpawnFlagCarrierLOSAvoidRadius;

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

	/** Respawn wait CAP during overtime (seconds). The escalation below climbs
	 *  from the 2s base toward this. Set to 0 to disable OT respawn handling. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Overtime")
	float OvertimeRespawnTime;

	/** Seconds of overtime the 2s base holds before escalation begins.
	 *  Default 360 (tOxX 2026-08-10; the old NewCTF ramp used a hardcoded 300). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Overtime")
	float OvertimeEscalationDelay;

	/** Seconds of overtime per +1s of respawn wait once escalation is running.
	 *  Default 60 = +1s every minute (old ramp: hardcoded 120). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Overtime")
	float OvertimeEscalationInterval;

	/** Regrab audibility (frenchempire 2026-08-06): stock plays the loud
	 *  flag-taken alarm only for grabs from the base stand
	 *  (AUTCTFFlagBase::ObjectWasPickedUp gates on bWasHome), so re-taking a
	 *  DROPPED flag is nearly silent — a positional pickup blip plus a voice
	 *  line the announcer can cancel. When enabled, picking up a dropped flag
	 *  replays the base's alarm cues at the grab location. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CTF|Announcements")
	bool bRegrabTakenAlarm = true;

	// ── Game Flow Overrides ──────────────────────────────────────────
	// NOTE: Floor slide disable is enforced via ATeamArenaCharacter::CanSlide_Implementation()
	// which reads bAllowFloorSlide from this game mode. No RestartPlayer override needed.

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual bool ReadyToStartMatch_Implementation() override;
	virtual void Logout(AController* Exiting) override;

	/** Pin bot-PUG players to their bot-assigned team (see PugRosterTeam). The
	 *  single choke point for login picks, manual switches, and the engine's
	 *  CountdownToBegin auto-balance — all route through ChangeTeam. Non-roster
	 *  joiners (subs/late fills) defer to Super's stock balancing. */
	virtual bool ChangeTeam(AController* Player, uint8 NewTeam = 255, bool bBroadcast = true) override;

	/** Rating preload for a player entering play mid-match (spec→player and team
	 *  entries; PostLogin skips spectators so caster joins stop touching the DB).
	 *  Idempotent — cache-first load, first-seen stamped once, no-op pre-match. */
	void EnsureRatingLoadedForPlayer(AController* Player);
	virtual void HandleMatchHasEnded() override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual void RestartPlayerAtPlayerStart(AController* NewPlayer, AActor* StartSpot) override;
	virtual float RatePlayerStart(APlayerStart* P, AController* Player) override;

	// Unlock entitlement-gated cosmetics: force the player's chosen hat as an OverrideHatClass (which the
	// engine does NOT entitlement-check) so the community master's missing cosmetic entitlements can't
	// strip it. Server-side, never kicks. See impl.
	virtual bool ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass) override;

	/** Own spawn selection from authored AUTTeamPlayerStart TeamNum pools.
	 *  The default path is NewCTF's rotating primary/secondary system; the old
	 *  RatePlayerStart weighted selector remains available as a rollback. */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual void ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn, AUTPlayerState* Holder, FName Reason) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer);
	virtual void CheckGameTime() override;
	virtual void DefaultTimer() override;
	virtual float GetTravelDelay() override;

	/** Regrab-alarm hook: every flag transition announces through here
	 *  (AUTCarriedObject::SendGameMessage), so the dropped(3)→taken(4) pair
	 *  identifies a regrab without subclassing the engine flag actors.
	 *  Only ever adds sounds — no message is suppressed or altered. */
	virtual void BroadcastLocalized(AActor* Sender, TSubclassOf<ULocalMessage> Message, int32 Switch = 0, APlayerState* RelatedPlayerState_1 = NULL, APlayerState* RelatedPlayerState_2 = NULL, UObject* OptionalObject = NULL) override;

	/** Play the home base's taken-alarm cues for a regrab, positioned at the
	 *  flag's current location instead of the base stand. */
	void PlayRegrabTakenAlarm(AUTCarriedObject* Flag);

	/** Per-team-flag drop tracking for the regrab alarm. Set by the dropped(3)
	 *  broadcast or the 1Hz state sampling in DefaultTimer (which covers the
	 *  near-cap "Denied" path that defers the dropped broadcast — see
	 *  UTCTFFlag::Drop); consumed by the next taken(4). */
	bool bFlagWasDropped[2] = { false, false };

	/** Debounce so message storms can't replay the alarm back-to-back. */
	float LastRegrabAlarmTime[2] = { -100.f, -100.f };

	virtual void HandleFlagCapture(AUTCharacter* HolderPawn, AUTPlayerState* Holder) override;
	virtual void HandleMatchIntermission() override;
	virtual void HandleExitingIntermission() override;
	virtual void HandleMatchInOvertime() override;
	virtual void EndGame(AUTPlayerState* Winner, FName Reason) override;

	/** CTF-aware end-game replay: feature the DECISIVE flag cap (the cap that ended the
	 *  match) via the stock ClientQueueCoolMoment path, gated on demo maturity. Skips on
	 *  short matches / no recent cap to avoid the stock client killcam seek crash (Map.h:527).
	 *  Server-side; see the .cpp for the full rationale. */
	virtual void PickMostCoolMoments(bool bClearCoolMoments = false, int32 CoolMomentsToShow = 1) override;

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
	 *  until they (and any others who dropped) rejoin, or a manual unpause is
	 *  requested. Uses the engine world-pause (WorldSettings->Pauser).
	 *  Mod.ini [UTPUGS_STATS] AutoPauseOnDrop. */
	bool bAutoPauseOnDrop = true;

	/** Pause-safe automatic resume countdown. Defaults to the shared host-pause
	 *  value and reads [NetcodePlus] UnpauseCountdownSec from Mod.ini. */
	int32 AutoPauseResumeCountdownSec = 7;

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

	/** Human participant IDs observed during this PUG. Kept across spectator
	 *  transitions so returning-as-spectator cannot evade drop tracking. */
	TSet<FString> AutoPauseTrackedIds;

	/** Logical exact-ID wait is active but no live PlayerState can hold Pauser. */
	bool bAutoPauseDormantNoMarker = false;

	/** Replicated authoritative pause snapshot consumed by the CTF HUD. */
	UPROPERTY(Transient)
	ANCAutoPauseState* AutoPauseStateActor = nullptr;

	/** Pause-immune automatic-resume ticker state (server-only). */
	FDelegateHandle AutoPauseResumeTicker;
	int32 AutoPauseResumeSecondsRemaining = 0;
	float AutoPauseResumeEndRealTime = 0.0f;
	bool bAutoPauseResumeCountdownActive = false;
	bool bForceClearingPauseActor = false;

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

	/** Auto-pause helpers (server-only). Pausing is an explicit Pauser assignment;
	 *  resuming is an explicit clear after a pause-immune authoritative countdown.
	 *  A new tracked drop cancels an active resume and republishes Paused state. */
	void BeginOrHoldAutoPause(const FString& LeaverId, const FString& LeaverName,
		const APlayerState* ExitingPlayerState);
	void BeginAutoPauseResumeCountdown(const FString& Reason);
	void CancelAutoPauseResumeCountdown(const FString& Reason);
	void CompleteAutoPauseResume(const FString& Reason);
	bool TickAutoPauseResume(float DeltaTime);
	ANCAutoPauseState* GetOrCreateAutoPauseState();
	void PublishAutoPausePaused(const FString& Reason);
	TArray<FString> GetSortedAutoPauseAwaitIds() const;
	class APlayerState* FindAutoPauseMarker(const APlayerState* Excluded = nullptr) const;

	/** Override spawn penalty weights + selection knobs from Mod.ini [UTPUGS_SPAWN]. */
	void LoadSpawnConfig();

	/** True if a non-home flag (enemy carrier or a dropped flag) is within
	 *  SpawnFlagVicinityRadius of the given team's flag base. */
	bool IsFlagNearOwnBase(uint8 TeamIndex) const;

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

	/** Eligibility-aware variants: only flags marked bAdvantageFlagEligible count,
	 *  so a retired flag a stand-camper fresh-grabs cannot sustain advantage. */
	bool IsAnyEligibleFlagHeld() const;
	bool IsAnyEligibleFlagOut() const;

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

	/** Per-team-flag advantage eligibility: true while this team's flag is part of
	 *  the play that earned advantage. Snapshotted in EnterAdvantage from which
	 *  flags were out; a flag's return retires it (BroadcastLocalized switch 0/1).
	 *  A fresh grab of a retired flag is inert — allowed, but it cannot sustain
	 *  advantage or re-arm the timer (stand-camper cherry-pick exploit, HuMPTY
	 *  report). Meaningful only while bPlayingAdvantage. */
	bool bAdvantageFlagEligible[2] = { false, false };

	// ── Recent Spawn Tracking (IG+ style) ───────────────────────────
	// The rollback selector tracks the last 3 successful spawns per player.
	// NewCTF's default path instead prevents reuse with the team-wide queue tail.

	struct FRecentSpawns
	{
		TWeakObjectPtr<APlayerStart> Last;
		TWeakObjectPtr<APlayerStart> SecondLast;
		TWeakObjectPtr<APlayerStart> ThirdLast;
	};

	TMap<TWeakObjectPtr<AController>, FRecentSpawns> PlayerRecentSpawns;

	/** Per-player last ACTUAL spawn (pawn) world location, for a truthful in-match
	 *  rotation/dist log that doesn't depend on the (sometimes stale) StartSpot. */
	TMap<TWeakObjectPtr<AController>, FVector> PlayerLastSpawnLoc;

	/** Emit one detailed Warning-level line for every live CTF/iCTF spawn.
	 *  Default-off; Mod.ini [UTPUGS_SPAWN] LogSpawnChoices=true enables it. */
	bool bLogSpawnChoices = false;

	/** Penalty multiplier for using the same spawn as 2 spawns ago (0.5 = half score). IG+ default. */
	float SpawnRecentPenaltyMultiplier = 0.5f;

	/** Penalty for spawning within this radius of your last spawn point */
	float SpawnNearLastRadius = 4000.f;

	/** Penalty scale for near-last-spawn distance */
	float SpawnNearLastPenalty = 6.f;

	// ── Legacy rollback selector (IG+ weighted-random / tie-band) ────
	// The tie-band picks deterministically whenever one start is more than
	// SpawnTieBandWidth ahead — on many maps that is the SAME start every life,
	// which is the "siempre en el mismo sitio" report. IG+ instead gives every
	// eligible start a random draw whose ceiling grows with its score and takes
	// the highest draw: a start half as good as the best still wins ~25% of the
	// time, and equal starts are a true coin-flip. Never deterministic.

	/** Within the rollback path: 1 = IG+ weighted-random draw, 0 = tie-band coin-flip. */
	bool bSpawnWeightedRandom = true;

	/** Draw ceiling of the BEST eligible start. Larger = flatter (more random). */
	float SpawnRandomBase = 20.f;

	/** Ceiling lost per score point below the best eligible start. A start more
	 *  than (SpawnRandomBase / SpawnRandomSpread) points behind the leader draws
	 *  nothing and can never be picked — which is what keeps Epic's rejected
	 *  starts (just-used -8, respawn-choice -5, telefrag -10, wrong-team -20)
	 *  out. Larger = greedier; smaller = more variety AND more risk. */
	float SpawnRandomSpread = 1.f;

	/** A start with a live enemy inside this radius fails the NewCTF primary pass.
	 *  The secondary pass still guarantees a spawn when every start is blocked.
	 *  Also used by the rollback/legacy selector's highest safety tier. 0 = off. */
	float SpawnEnemyHardRadius = 1200.f;

	/** IG+ MinSpawnZVariance: an enemy at least this far BELOW a start is treated
	 *  as floor-separated rather than adjacent (discounts the proximity penalty,
	 *  and clears the hard radius above when he also has no sightline). 0 = off. */
	float SpawnEnemyBelowZ = 190.f;

	/** Master rollback switch. False restores the pre-port weighted selector. */
	bool bSpawnUseNewCTFSelection = true;

	/** Use Epic's selector at or below this many connected competitors. */
	int32 SpawnSystemThreshold = 4;

	/** A teammate inside this radius blocks a primary candidate. */
	float SpawnFriendlyBlockRange = 150.f;

	/** A teammate with line of sight inside this radius blocks a primary candidate. */
	float SpawnFriendlyVisionBlockRange = 150.f;

	/** An enemy flag carrier or any unheld flag inside this radius blocks primary. */
	float SpawnFlagBlockRange = 750.f;

	/** Number of most recently used team starts excluded from primary and secondary. */
	int32 SpawnMinCycleDistance = 1;

	/** Predict remote pawn movement by half RTT for spawn distance/vision checks. */
	bool bSpawnExtrapolateMovement = true;

	/** Use the weighted-distance secondary pass when every primary candidate fails. */
	bool bSpawnSecondaryEnabled = true;

	/** Per-player distance contribution cap in the secondary pass. */
	float SpawnSecondaryMaxDistance = 2000.f;

	/** Secondary distance weight for teammates. */
	float SpawnSecondaryOwnTeamWeight = 0.2f;

	/** Secondary distance weight for enemy flag carriers. */
	float SpawnSecondaryCarrierWeight = 2.f;

	// ── Team-aware spawn pools (curated from author TeamNum tags) ─────
	// Built lazily on the first ChoosePlayerStart. Server-only; never replicated.

	/** Own-team candidate starts, partitioned by authored TeamNum and used as
	 *  rotating queues by the NewCTF selector. */
	TArray<TWeakObjectPtr<APlayerStart>> Team0Spawns;
	TArray<TWeakObjectPtr<APlayerStart>> Team1Spawns;

	/** Server time each start was last chosen (team-wide), for the freshness
	 *  spread. Reset per match. Server-only. */
	TMap<TWeakObjectPtr<APlayerStart>, float> SpawnLastUsedTime;

	/** False until BuildTeamSpawnPools has populated the pools this match. */
	bool bSpawnPoolsBuilt = false;

	/** Bucket each AUTTeamPlayerStart into Team0Spawns / Team1Spawns by its
	 *  authored TeamNum (trusted, not mutated), then shuffle each queue once.
	 *  Plain APlayerStarts are excluded. */
	void BuildTeamSpawnPools();

	/** Pre-NewCTF weighted/tie-band selector retained behind the rollback switch. */
	AActor* ChooseLegacyPlayerStart(AController* Player);

	/** Run Epic's picker without dispatching back into NCP's RatePlayerStart. */
	AActor* ChooseEpicPlayerStart(AController* Player);

	/** NewCTF primary/secondary selector. Selection is side-effect free; a start
	 *  is moved to the queue tail only after RestartPlayerAtPlayerStart succeeds. */
	APlayerStart* ChooseNewCTFPlayerStart(AController* Player);

	/** Count connected, assigned competitors (alive or dead; spectators excluded). */
	int32 CountSpawnSystemCompetitors() const;

	/** Commit a successfully used team start to the queue/history. */
	void CommitUsedSpawn(AController* Player, APlayerStart* UsedStart);

	/** True only for gameplay spawns, plus second-half respawns performed while
	 *  the state machine is exiting intermission; excludes lineup previews. */
	bool IsLiveSpawnCommitState() const;

	/** Discard intro/intermission preview choices before a serialized live restart. */
	void ClearCachedRespawnChoices();

	/** Synchronous guard used only while Epic's fallback picker rates starts. */
	bool bForceEpicSpawnRating = false;

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

	/** World time before which a FlagCapture ScoreObject is rejected. Prevents double caps on maps with no geometry between bases. */
	float LastScoreObjectTime;

	/** Player who scored the most recent flag capture (end-of-match replay focus). Captured
	 *  BEFORE Super::ScoreObject so a scorelimit/golden/mercy cap that ends the match inside
	 *  Super is still recorded as the decisive moment. UniqueId is the ClientQueueCoolMoment focus. */
	TWeakObjectPtr<AUTPlayerState> LastCapPlayer;

	/** World time of the most recent flag capture. */
	float LastCapTime = 0.f;

	/** Max age (s) for the last cap to still count as "the decider" when EndGame runs. A
	 *  cap-driven end credits the cap microseconds before EndGame, so it's always inside this
	 *  window; a timelimit end (stale cap) falls through to no replay. */
	float FeatureCapMaxAgeSeconds = 15.f;

public:
	virtual void CreateGameURLOptions(TArray<TSharedPtr<TAttributePropertyBase>>& MenuProps);
};
