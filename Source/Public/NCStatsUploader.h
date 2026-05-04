// NCStatsUploader — outbound match-result POSTs to ut4stats.com (Django) and
// the StatSQL backend. STUBBED: methods log payloads via UE_LOG with TODO
// markers for the future HTTP fill-in (see api_endpoints.md).
//
// Why two backends: ut4stats.com is the public scoreboard / season tracker;
// StatSQL is the internal Django app at github.com/jmortley/UTPugs_StatSQL.
// Both consume the same FNCMatchSummary; the uploader just formats per-endpoint
// payloads. Splitting into PostToUT4Stats / PostToStatSQL lets future work land
// one backend at a time without touching the gamemodes.
//
// Server-only. Free-function namespace; no UObject lifecycle to worry about.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class UWorld;

struct FNCPlayerSummary
{
	FString UniqueId;
	FString PlayerName;
	int32   Score    = 0;
	int32   Kills    = 0;
	int32   Deaths   = 0;
	int32   Ping     = 0;
	int32   Team     = 0;       // 0 = red / player1, 1 = blue / player2 (FFA: 0/1 still)
	int32   Damage   = 0;

	// Per-weapon (Shots=X, Hits=Y) — keyed by canonical weapon NAME.
	// FIntPoint instead of TPair<int32,int32> because UE4.15's TPair has no
	// (Key, Value) ctor (only the TPairInitializer one); FIntPoint has the
	// straightforward 2-arg ctor we need at call sites.
	TMap<FName, FIntPoint> WeaponAccuracy;

	int32   PreMatchElo  = 0;
	int32   PostMatchElo = 0;
};

struct FNCMatchSummary
{
	FString GameMode;            // "NCLeagueDuel" or "NCShaftArena"
	FString MapName;
	FString ServerName;
	FString GameOptions;         // URL-encoded "timelimit=15?goalscore=10?..."
	FString ReplayId;

	int32   RedScore   = 0;
	int32   BlueScore  = 0;
	int32   RedDamage  = 0;
	int32   BlueDamage = 0;

	TArray<FNCPlayerSummary> Players;
};

class NETCODEPLUS_API FNCStatsUploader
{
public:
	/** STUB: log the would-be ut4stats POST sequence. Future work: HTTP fill-in. */
	static void PostToUT4Stats(UWorld* World, const FNCMatchSummary& Summary);

	/** STUB: log the would-be StatSQL POST sequence. */
	static void PostToStatSQL(UWorld* World, const FNCMatchSummary& Summary);
};
