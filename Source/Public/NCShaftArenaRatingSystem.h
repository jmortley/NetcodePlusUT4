// NCShaftArenaRatingSystem — server-side 1v1 Glicko2 + persistent awards for
// NCShaftArena. Single Mods.db table holds both rating columns and award
// columns since they share a load/flush lifecycle.
//
// Pimpl pattern (see NCDuelRatingSystem.h banner) — TeamGlicko2 vendored
// headers stay confined to the .cpp.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

struct FNCShaftArenaPlayerEntry
{
	FString UniqueId;
	// Glicko2
	double  Rating              = 1400.0;
	double  RD                  = 200.0;
	double  Sigma               = 0.06;
	// Per-mode awards
	float   BestSessionAccuracy = 0.f;
	int32   BestStreak          = 0;
	int32   TotalWins           = 0;
	int32   TotalLosses         = 0;
	int32   TotalKills          = 0;
	int32   GamesPlayed         = 0;
	int64   LastSeenUtc         = 0;
	int32   SchemaVersion       = 1;
};

struct FNCShaftArenaRatingSystemImpl;

class NETCODEPLUS_API FNCShaftArenaRatingSystem
{
public:
	FNCShaftArenaRatingSystem();
	~FNCShaftArenaRatingSystem();

	/** CREATE TABLE IF NOT EXISTS NCRatingShaftArena. Idempotent. */
	static bool InitDatabase(UWorld* World);

	/** Load (or insert default) row for the given UniqueId. Call from PostLogin. */
	void LoadPlayerFromDB(UWorld* World, const FString& UniqueId);

	/** Returns rounded current cached ELO, or default 1400 if not loaded. */
	int32 GetCachedElo(const FString& UniqueId) const;

	/** Snapshot ratings as they were at match start so the uploader can report
	 *  PreElo / PostElo deltas. Call from HandleMatchHasStarted. */
	void SnapshotMatchStart();

	/** Returns the snapshotted rating from match start, or current cached if
	 *  no snapshot exists. */
	int32 GetPreMatchElo(const FString& UniqueId) const;

	/** Returns the cached entry (post-update once RecordMatchResult ran). */
	const FNCShaftArenaPlayerEntry* GetEntry(const FString& UniqueId) const;

	/** Apply a single 1v1 match outcome plus award updates. Pure in-memory; call
	 *  Flush to persist. WinnerKills / LoserKills feed TotalKills accumulators;
	 *  Accuracy / BestStreak update the per-mode award columns. */
	void RecordMatchResult(const FString& WinnerId, const FString& LoserId,
	                       int32 WinnerKills, int32 LoserKills,
	                       float WinnerAccuracy, float LoserAccuracy,
	                       int32 WinnerBestStreak, int32 LoserBestStreak);

	/** Persist all cached entries to Mods.db. Call from HandleMatchHasEnded. */
	void Flush(UWorld* World);

	/** Drop a player from the cache (e.g. on Logout). */
	void Forget(const FString& UniqueId);

	/** Return top-N entries by Rating descending (live query — not cached). */
	TArray<FNCShaftArenaPlayerEntry> GetTopPlayers(UWorld* World, int32 N);

private:
	TUniquePtr<FNCShaftArenaRatingSystemImpl> Impl;
};
