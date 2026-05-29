// NCPlusCTFRatingSystem — server-side wrapper around the vendored TeamGlicko2
// library for the CTF game mode. Single class handles BOTH regular CTF and
// instagib CTF (iCTF) — instance carries a bIsInstagib flag set at
// construction. Each variant has its own Mods.db table (NCRatingCTF vs
// NCRatingICTF) and emits a different mode key in BuildResultPayload.
//
// One match = one ProcessMatch call (CTF isn't round-based like ElimPlus).
// Fed with cumulative per-player K/D/damage as the performance score.
//
// Pimpl pattern (see feedback_pimpl_for_vendored_libs.md) — vendored
// TeamGlicko2 headers stay confined to the .cpp.
//
// Server-only.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

/** Per-player input for the single match-end ProcessMatch call.
 *  The CTF-specific signals below are all read server-side from AUTPlayerState
 *  at match end (direct fields + GetStatsValue). They default to 0 so any
 *  caller that only fills K/D/damage reproduces the legacy behaviour. */
struct FNCPlusCTFPlayerInput
{
	FString UniqueId;
	FString PlayerName;
	int32   TeamIndex = 0;       // 0 = red, 1 = blue
	int32   Kills     = 0;
	int32   Deaths    = 0;
	int32   Damage    = 0;

	// ── CTF objective signals ────────────────────────────────────────────
	int32   Caps          = 0;   // AUTPlayerState::FlagCaptures
	int32   Returns       = 0;   // AUTPlayerState::FlagReturns
	int32   Assists       = 0;   // AUTPlayerState::Assists (flag-cap assists)
	int32   FCKills       = 0;   // NAME_FCKills — killed the enemy flag carrier
	int32   SupportKills  = 0;   // NAME_FlagSupportKills — protected own carrier
	int32   Grabs         = 0;   // NAME_FlagGrabs
	int32   CarryAssists  = 0;   // NAME_CarryAssist — advanced flag, teammate capped
	int32   EnemyFCDamage = 0;   // NAME_EnemyFCDamage — damage-points dealt to enemy FC
};

/** Runtime-tunable CTF perf knobs, read from Mod.ini [UTPUGS_STATS] by the
 *  gamemode each match and passed into ProcessMatch / BuildResultPayload.
 *  Defaults reproduce the shipped Phase-1 state: the new perf score is computed
 *  and uploaded for observation, but the LIVE Glicko delta still uses the
 *  legacy K/D perf until bShadow is cleared at cutover. */
struct FNCPlusCTFPerfConfig
{
	bool   bEnabled        = true;   // false => pure legacy K/D perf, no new payload fields
	bool   bShadow         = true;   // true => live delta uses legacy perf; new perf observed only
	double ObjectiveWeight = 1.0;    // master multiplier on the objective half of the score
	double FeederPenalty   = 1.0;    // multiplier on the grabs-without-cap penalty
};

/** Match-end payload input. WinnerTeamIndex is 0/1; -1 means draw.
 *  bIsInstagib selects the mode label and Mods.db table — must match the
 *  rating system instance's flag (the gamemode locks it in InitGame). */
struct FNCPlusCTFMatchInput
{
	int32 WinnerTeamIndex = 0;
	int32 RedScore        = 0;
	int32 BlueScore       = 0;
	bool  bIsInstagib     = false;
	FNCPlusCTFPerfConfig Perf;
	TArray<FNCPlusCTFPlayerInput> Players;
};

/** Pimpl: opaque forward decl. */
struct FNCPlusCTFRatingSystemImpl;

class NETCODEPLUS_API FNCPlusCTFRatingSystem
{
public:
	/** bInIsInstagib locks which ladder (NCRatingCTF vs NCRatingICTF) this
	 *  instance reads/writes for its entire lifetime. The gamemode determines
	 *  instagib state at InitGame (via AUTGameMode::bIsInstagib) and rebuilds
	 *  this object if the mode changes between matches (not currently possible
	 *  mid-session, but defensive). */
	explicit FNCPlusCTFRatingSystem(bool bInIsInstagib);
	~FNCPlusCTFRatingSystem();   // Out-of-line: TUniquePtr<Impl> needs complete type.

	/** Create BOTH tables (NCRatingCTF + NCRatingICTF) if missing. Idempotent.
	 *  Call once from gamemode BeginPlay. */
	static bool InitDatabase(UWorld* World);

	/** Pull this player's rating from Mods.db for THIS instance's ladder. */
	void LoadPlayerFromDB(UWorld* World, const FString& UniqueId);

	/** Snapshot current cached ratings as "match-start" frozen values for the
	 *  per-match delta. Call from HandleMatchHasStarted. */
	void SnapshotMatchStart();

	/** Apply the match outcome once (no per-round structure in CTF). Performance
	 *  scores fed from cumulative match K/D/damage. */
	void ProcessMatch(const FNCPlusCTFMatchInput& In);

	/** Persist cached ratings to Mods.db (this instance's ladder). Call from
	 *  HandleMatchHasEnded. */
	void Flush(UWorld* World);

	/** Server-side rating accessor (rounded int). */
	int32 GetCachedElo(const FString& UniqueId) const;

	/** Drop a player from the cache (e.g. on Logout). */
	void Forget(const FString& UniqueId);

	/** Build the JSON payload pushed to ut4stats.com for global-ELO updates.
	 *  Mode in the envelope is "CTF" or "iCTF" depending on bIsInstagib. Call
	 *  AFTER ProcessMatch + Flush so RatingCache holds post-match values. */
	FString BuildResultPayload(UWorld* World, const FNCPlusCTFMatchInput& In) const;

	/** Does this instance handle iCTF rather than regular CTF? */
	bool IsInstagibMode() const;

private:
	TUniquePtr<FNCPlusCTFRatingSystemImpl> Impl;
};
