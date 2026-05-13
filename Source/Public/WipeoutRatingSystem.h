// WipeoutRatingSystem — server-side wrapper around the vendored TeamGlicko2
// library for the Wipeout game mode. Round-based team elimination, same shape
// as ElimPlusRatingSystem (per-round ProcessMatch + match-end flush).
//
// Pimpl is intentional — vendored TeamGlicko2 headers transitively pull
// <vector>/<cmath> which UE4 4.15's unity build does not tolerate at the
// engine PCH preamble (see feedback_pimpl_for_vendored_libs.md,
// feedback_unity_iostream_pollution.md).
//
// Persistence: Mods.db table NCRatingWipeout. Independent ladder from ElimPlus.
//
// Lifecycle (mirrors ElimPlus):
//   BeginPlay          -> InitDatabase
//   PostLogin          -> LoadPlayerFromDB
//   HandleMatchHasStarted -> SnapshotMatchStart
//   EndRoundForTeam    -> ProcessRound (in-memory only)
//   HandleMatchHasEnded -> FlushAtMatchEnd + BuildResultPayload + uploader
//
// Server-only.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

/** Per-player perf score input to a round update. Tron's library expects
 *  `Kills*1 + Deaths*(-1) + Damage*(1/220)`. */
struct FWipeoutPlayerRoundPerf
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
struct FWipeoutRoundResult
{
	TArray<FWipeoutPlayerRoundPerf> WinnerTeam;
	TArray<FWipeoutPlayerRoundPerf> LoserTeam;
	bool bIsDraw = false;
};

/** Per-player audit info supplied by the gamemode at match end. UniqueId must
 *  match the rating system's cache key (FString from AUTPlayerState::UniqueId,
 *  NOT a synthetic bot key — bots are filtered out before the payload is
 *  built). */
struct FNCWipeoutPlayerInput
{
	FString UniqueId;
	FString PlayerName;
	int32   TeamIndex = 0;        // 0 = red, 1 = blue
	int32   Kills     = 0;        // match-aggregate
	int32   Deaths    = 0;
	int32   Damage    = 0;
};

/** Match-end payload input. WinnerTeamIndex is 0/1; -1 means draw. */
struct FNCWipeoutMatchInput
{
	int32 WinnerTeamIndex = 0;
	int32 RedScore        = 0;
	int32 BlueScore       = 0;
	TArray<FNCWipeoutPlayerInput> Players;
};

/** Pimpl: opaque forward decl of the implementation struct. */
struct FWipeoutRatingSystemImpl;

class NETCODEPLUS_API FWipeoutRatingSystem
{
public:
	FWipeoutRatingSystem();
	~FWipeoutRatingSystem();   // Out-of-line: TUniquePtr<Impl> dtor needs the complete type.

	/** Run the CREATE TABLE IF NOT EXISTS on Mods.db. Call once from gamemode
	 *  BeginPlay (server only). Idempotent. */
	static bool InitDatabase(UWorld* World);

	/** Pull this player's rating row from Mods.db into the in-memory cache.
	 *  If no row exists, creates an INSERT with defaults. Call from PostLogin. */
	void LoadPlayerFromDB(UWorld* World, const FString& UniqueId);

	/** Snapshot current cached ratings as the "match-start" frozen values.
	 *  Call from HandleMatchHasStarted, after all players are loaded. */
	void SnapshotMatchStart();

	/** Apply a single round's outcome to the cached PlayerRatings. Internal
	 *  in-memory update only; persistence happens in FlushAtMatchEnd. */
	void ProcessRound(const FWipeoutRoundResult& Result);

	/** Persist all cached ratings to Mods.db. Call from HandleMatchHasEnded. */
	void FlushAtMatchEnd(UWorld* World);

	/** Server-side accessor for current cached rating (rounded int). Returns
	 *  1400 if not loaded. */
	int32 GetCachedElo(const FString& UniqueId) const;

	/** Drop a player from the cache (e.g. on Logout). */
	void Forget(const FString& UniqueId);

	/** Build the JSON payload pushed to ut4stats.com for global-ELO updates.
	 *  Call AFTER FlushAtMatchEnd so RatingCache holds final values. Bots
	 *  (UIds not in RatingCache) are filtered out. Returns empty string when
	 *  there are no human players in cache. */
	FString BuildResultPayload(UWorld* World, const FNCWipeoutMatchInput& In) const;

private:
	TUniquePtr<FWipeoutRatingSystemImpl> Impl;
};
