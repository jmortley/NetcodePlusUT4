// NCDuelRatingSystem — server-side 1v1 Glicko2 wrapper for NCLeagueDuel.
// Mirrors FElimPlusRatingSystem's Pimpl pattern: TeamGlicko2's vendored headers
// (vector / cmath) must NEVER leak into a consumer compilation unit because
// UE4 4.15's unity build poisons engine UT headers downstream. Pimpl keeps
// the .h dependency surface UE4-only.
//
// 1v1 specifics: each match is one TeamGlicko2::MatchResult with teamA={winner}
// and teamB={loser} (each a 1-player vector). The algorithm collapses to the
// textbook 1v1 Glicko-2 update — no team-strength aggregation needed.
//
// Lives outside any UObject hierarchy — the gamemode owns a TUniquePtr<>.
// Server-only.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

struct FNCDuelPlayerEntry
{
	FString UniqueId;
	double  Rating       = 1400.0;
	double  RD           = 200.0;
	double  Sigma        = 0.06;
	int32   GamesPlayed  = 0;
	int32   Wins         = 0;
	int32   Losses       = 0;
	int32   Draws        = 0;
	int64   LastSeenUtc  = 0;
	int32   SchemaVersion = 1;
};

/** Per-match payload input — gamemode supplies winner/loser identity + scores;
 *  rating system fills in pre/post/RD/sigma from its caches. */
struct FNCDuelMatchInput
{
	FString WinnerId;
	FString WinnerName;
	int32   WinnerScore = 0;
	FString LoserId;
	FString LoserName;
	int32   LoserScore  = 0;
	bool    bDraw       = false;
};

struct FNCDuelRatingSystemImpl;

class NETCODEPLUS_API FNCDuelRatingSystem
{
public:
	FNCDuelRatingSystem();
	~FNCDuelRatingSystem();   // out-of-line — TUniquePtr<Impl> needs complete type

	/** CREATE TABLE IF NOT EXISTS NCRatingDuel. Idempotent. Server only. */
	static bool InitDatabase(UWorld* World);

	/** Pull rating from Mods.db into the in-memory cache. Inserts default row if
	 *  no record exists. Call from PostLogin. */
	void LoadPlayerFromDB(UWorld* World, const FString& UniqueId);

	/** Returns rounded current cached ELO. Returns default 1400 if not loaded. */
	int32 GetCachedElo(const FString& UniqueId) const;

	/** Apply a single 1v1 match outcome to the cached ratings. Pure in-memory
	 *  update — call Flush to persist. */
	void ProcessMatchResult(const FString& WinnerId, const FString& LoserId, bool bDraw);

	/** Persist all cached ratings to Mods.db. Call from HandleMatchHasEnded. */
	void Flush(UWorld* World);

	/** Drop a player from the cache (e.g. on Logout). */
	void Forget(const FString& UniqueId);

	/** Return the rating snapshotted before ProcessMatchResult was called this
	 *  match. Used by uploader payload to compute (PostElo - PreElo) delta.
	 *  Returns the cached "before" value or current cached value if no snapshot. */
	int32 GetPreMatchElo(const FString& UniqueId) const;

	/** Capture each loaded player's rating as the "match start" reference for
	 *  PreMatchElo lookups. Call from HandleMatchHasStarted. */
	void SnapshotMatchStart();

	/** Build the JSON payload pushed to ut4stats.com for global-ELO updates.
	 *  Call AFTER ProcessMatchResult+Flush so RatingCache holds post-match values.
	 *  Returns an empty string if the cache lookup fails (logs a warning). */
	FString BuildResultPayload(UWorld* World, const FNCDuelMatchInput& In) const;

private:
	TUniquePtr<FNCDuelRatingSystemImpl> Impl;
};
