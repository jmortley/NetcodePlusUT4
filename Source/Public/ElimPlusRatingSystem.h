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

	/** Drop a player from the cache (e.g. on Logout, well after the match). */
	void Forget(const FString& UniqueId);

private:
	TUniquePtr<FElimPlusRatingSystemImpl> Impl;
};
