// ElimPlusRatingSystem — server-side wrapper around the vendored TeamGlicko2
// library. Handles Mods.db persistence and bridges between AUTPlayerState +
// our own AElimPlusStatsReplicator and Tron's PlayerRating / TeamGlicko2System.
//
// Design notes:
//   - ELO is computed per round (ProcessRound) for accuracy, but the values
//     replicated to clients are FROZEN at match-start. Only at HandleMatchHasEnded
//     does the new rating + total match delta get pushed to the replicator and
//     persisted to Mods.db. Avoids HUD ELO ping-ponging round to round.
//   - Lives outside any UObject hierarchy — gamemode owns a TUniquePtr<>.
//   - Server-only. No replication of this class itself; client display is via
//     AElimPlusStatsReplicator's Elo + EloDeltaThisMatch fields.
#pragma once

#include "CoreMinimal.h"
#include "TeamGlickoRating.h"  // Vendored: namespace TeamGlicko2

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
	/** UniqueIds of the players on the winning team this round. Empty if draw. */
	TArray<FElimPlusPlayerRoundPerf> WinnerTeam;
	TArray<FElimPlusPlayerRoundPerf> LoserTeam;
	bool bIsDraw = false;
};

class NETCODEPLUS_API FElimPlusRatingSystem
{
public:
	FElimPlusRatingSystem();

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

	/** Drop a player from the cache (e.g. on Logout, well after the match). */
	void Forget(const FString& UniqueId);

	/** Read-only access to cached PlayerRating for balancer / diagnostics. */
	const TeamGlicko2::PlayerRating* FindRating(const FString& UniqueId) const;

private:
	/** UniqueId -> mutable PlayerRating. Updated each ProcessRound. */
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;

	/** UniqueId -> rating-rounded-int captured by SnapshotMatchStart.
	 *  Used at FlushAtMatchEnd to compute (NewElo - StartElo) delta. */
	TMap<FString, int32> RatingAtMatchStart;

	/** Returns true if the player was loaded in this match (used to skip
	 *  late-joiners for ELO updates per spec — bSkipELO equivalent). */
	bool IsPlayerActive(const FString& UniqueId) const;
};
