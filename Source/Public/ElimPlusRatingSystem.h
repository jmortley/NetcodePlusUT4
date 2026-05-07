// ElimPlusRatingSystem — server-side wrapper around the vendored TeamGlicko2
// library. Handles Mods.db persistence and bridges between AUTPlayerState +
// our own AElimPlusStatsReplicator and Tron's PlayerRating / TeamGlicko2System.
//
// Pimpl is intentional: the vendored TeamGlicko2 headers transitively include
// <iostream> / <iosfwd> / <vector> / <cmath> which under UE4 4.15's unity build
// pollute the PCH chain and break engine UT headers (UDataAsset, ULocalMessage,
// etc.) when this header lands at a unity-bundle head. Hiding TeamGlicko2 types
// behind FElimPlusRatingSystemImpl keeps the .h dependency surface to UE4 core
// only — every consumer (ElimPlusGame.h, ElimPlusGame.cpp) can include us without
// dragging Tron's headers into their compilation unit.
//
// Design notes:
//   - ELO is computed per round (ProcessRound) for accuracy, but the values
//     replicated to clients are FROZEN at match-start. Only at HandleMatchHasEnded
//     does the new rating + total match delta get pushed to the replicator and
//     persisted to Mods.db. Avoids HUD ELO ping-ponging round to round.
//   - Lives outside any UObject hierarchy — gamemode owns a TUniquePtr<>.
//   - Server-only.
#pragma once

#include "CoreMinimal.h"

class UWorld;
class AElimPlusStatsReplicator;

/** Per-player perf score input to a round update. Tron's library expects
 *  `Kills*1 + Deaths*(-1) + Damage*(1/220)`. */
struct FElimPlusPlayerRoundPerf
{
	FString UniqueId;
	int32 Kills = 0;
	int32 Deaths = 0;
	float Damage = 0.f;

	double ToPerfScore() const
	{
		return double(Kills) - double(Deaths) + double(Damage) / 220.0;
	}
};

/** Round-level outcome bundled for ProcessRound. */
struct FElimPlusRoundResult
{
	TArray<FElimPlusPlayerRoundPerf> WinnerTeam;
	TArray<FElimPlusPlayerRoundPerf> LoserTeam;
	bool bIsDraw = false;
};

/** Input slot for ComputeBalancedTeams. UniqueId is either the real Epic ID
 *  (humans) or the synthetic "BOT:<PlayerName>" key (bots). */
struct FElimPlusBalanceInput
{
	FString UniqueId;
};

/** TeamBalancer output, UE4-friendly (no STL types). The Team0/Team1Indices
 *  reference back into the input TArray<FElimPlusBalanceInput> the caller
 *  passed in, so the caller can map indices back to AControllers. */
struct FElimPlusBalanceResult
{
	TArray<int32> Team0Indices;
	TArray<int32> Team1Indices;
	float Team0Strength = 0.f;
	float Team1Strength = 0.f;
	float StrengthDifference = 0.f;
	bool bValid = false;
};

/** Per-player audit info supplied by the gamemode at match end. UniqueId must
 *  match the rating system's cache key (FString from AUTPlayerState::StatsID
 *  or PlayerName, NOT the synthetic "BOT:..." prefix — bots are filtered out
 *  before the payload is built). */
struct FNCElimPlusPlayerInput
{
	FString UniqueId;
	FString PlayerName;
	int32   TeamIndex = 0;        // 0 = red, 1 = blue
	int32   Kills     = 0;        // match-aggregate
	int32   Deaths    = 0;
	int32   Damage    = 0;
};

/** Match-end payload input. WinnerTeamIndex is 0/1; -1 means draw. */
struct FNCElimPlusMatchInput
{
	int32 WinnerTeamIndex = 0;
	int32 RedScore        = 0;
	int32 BlueScore       = 0;
	TArray<FNCElimPlusPlayerInput> Players;
};

/** Pimpl: opaque forward decl of the implementation struct. The real definition
 *  lives in ElimPlusRatingSystem.cpp where it can freely use TeamGlicko2 types. */
struct FElimPlusRatingSystemImpl;

class NETCODEPLUS_API FElimPlusRatingSystem
{
public:
	FElimPlusRatingSystem();
	~FElimPlusRatingSystem();   // Out-of-line: TUniquePtr<Impl> dtor needs the complete type.

	/** Run the CREATE TABLE IF NOT EXISTS on Mods.db. Call once from gamemode
	 *  BeginPlay (server only). Idempotent. */
	static bool InitDatabase(UWorld* World);

	/** Pull this player's rating row from Mods.db into the in-memory cache.
	 *  If no row exists, creates an INSERT with default 1400/350/0.06 and caches
	 *  defaults. Call from PostLogin. */
	void LoadPlayerFromDB(UWorld* World, const FString& UniqueId);

	/** Snapshot current cached ratings as the "match-start" frozen values. The
	 *  replicator displays these throughout the match. Call from
	 *  HandleMatchHasStarted, after all players are loaded. */
	void SnapshotMatchStart();

	/** Apply a single round's outcome to the cached PlayerRatings. Internal
	 *  in-memory update only — does NOT push to the replicator (display stays
	 *  frozen until match end). */
	void ProcessRound(const FElimPlusRoundResult& Result);

	/** Persist all cached ratings to Mods.db and push final values + match-delta
	 *  to the replicator. Call from HandleMatchHasEnded. */
	void FlushAtMatchEnd(UWorld* World, AElimPlusStatsReplicator* Replicator);

	/** Server-side accessor for current cached rating (rounded int). Returns
	 *  1400 if not loaded. */
	int32 GetCachedElo(const FString& UniqueId) const;

	/** Add this round's PPR (Kills + Damage*0.01) to the player's lifetime totals.
	 *  Pure cache update; persistence happens in FlushAtMatchEnd. Server-only. */
	void RecordRoundPPR(const FString& UniqueId, float RoundPPR);

	/** Server-side accessor for lifetime PPR (TotalPoints / max(1, RoundsPlayed)).
	 *  Returns 0 for players with no completed rounds. */
	float GetCachedLifetimePPR(const FString& UniqueId) const;

	/** TESTING: enable per-bot random ELO assignment so PickBalancedTeam has
	 *  varied team strengths to balance against. When enabled, each unique bot
	 *  key (e.g. "BOT:KiloBot") gets a stable random rating in [Min, Max] on
	 *  first lookup; subsequent lookups return the cached value. Disabled by
	 *  default — production servers should leave this off so bots count as 1400.
	 *  Used by both PickBalancedTeam (team-strength sum) and ProcessRound (bot
	 *  placeholder PlayerRating) for consistency. */
	void SetBotRandomEloRange(bool bEnabled, int32 Min, int32 Max);

	/** Returns the bot's assigned ELO if randomization is on, else 1400. Stable
	 *  per UniqueId across calls within the session. */
	int32 GetOrAssignBotElo(const FString& UniqueId);

	/** Compute balanced team assignments via Tron's TeamGlicko2::TeamBalancer
	 *  using cached human ratings + bot random ELOs. Caller iterates the result
	 *  Team0/Team1Indices to look up the controller for each slot in their own
	 *  parallel array and call MovePlayerToTeam. Server-only; no DB or
	 *  replicator side-effects. */
	FElimPlusBalanceResult ComputeBalancedTeams(const TArray<FElimPlusBalanceInput>& Players);

	/** Drop a player from the cache (e.g. on Logout, well after the match). */
	void Forget(const FString& UniqueId);

	/** Build the JSON payload pushed to ut4stats.com for global-ELO updates.
	 *  Call AFTER FlushAtMatchEnd so RatingCache holds final values (including
	 *  any bot-match clamp). Bots (UIds not in RatingCache) are filtered out.
	 *  Returns an empty string if no human players have cache entries. */
	FString BuildResultPayload(UWorld* World, const FNCElimPlusMatchInput& In) const;

private:
	TUniquePtr<FElimPlusRatingSystemImpl> Impl;
};
