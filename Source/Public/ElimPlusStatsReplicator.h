// ElimPlusStatsReplicator — Replicates per-player stats (PPR, ELO, damage, LG accuracy,
// best weapon) from server to all clients for the ElimPlus HUD/Scoreboard.
// Pattern mirrors AWipeoutDamageReplicator. Server snapshots periodically; clients read.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "ElimPlusStatsReplicator.generated.h"

/** Per-player snapshot. Keyed by UniqueId string (Epic ID). */
USTRUCT()
struct FElimPlusStatsEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	int32 DamageDone = 0;

	/** Match-running PPR mean: sum(per-round PPR for completed rounds) / rounds_played_so_far.
	 *  Excludes the in-progress round. */
	UPROPERTY()
	float PPRCurrent = 0.f;

	/** Frozen ELO displayed throughout the match. Set at match-start to the
	 *  player's stored rating; only updated to the new value at match end (so
	 *  the HUD doesn't ping-pong round-to-round). */
	UPROPERTY()
	int32 Elo = 1400;

	/** Total ELO change for THIS match (NewElo - StartElo). Stays 0 during the
	 *  match; populated at match end alongside the new Elo value. */
	UPROPERTY()
	int32 EloDeltaThisMatch = 0;

	/** Link Gun primary hits / shots * 10000, packed as integer for replication compactness.
	 *  Display: divide by 100 to get percentage with 2 decimals. */
	UPROPERTY()
	int32 LinkGunAccuracyTimes100 = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API AElimPlusStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** Replicated per-player stats array. Server writes, clients read. */
	UPROPERTY(Replicated, Transient)
	TArray<FElimPlusStatsEntry> StatsEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** Server-only: refresh StatsEntries from current PlayerStates + game-mode caches.
	 *  Called periodically and at end-of-round. */
	void UpdateFromPlayerStates();

	/** Lookup helpers — safe on clients. Return defaults if not found. */
	const FElimPlusStatsEntry* FindEntry(const FString& UniqueIdStr) const;
	int32 GetDamageForPlayer(const FString& UniqueIdStr) const;
	float GetPPRCurrentForPlayer(const FString& UniqueIdStr) const;
	int32 GetEloForPlayer(const FString& UniqueIdStr) const;
	int32 GetEloDeltaForPlayer(const FString& UniqueIdStr) const;
	float GetLinkGunAccuracyForPlayer(const FString& UniqueIdStr) const;

	/** Server-only: gamemode pushes the match-running-mean PPR per player here at
	 *  end-of-round. UpdateFromPlayerStates copies this into the replicated entries. */
	void SetPlayerPPRCurrent(const FString& UniqueIdStr, float Value);

	/** Server-only: rating system pushes a player's ELO + match-delta here at
	 *  HandleMatchHasEnded (NOT per round — display stays frozen until then). */
	void SetPlayerEloAndDelta(const FString& UniqueIdStr, int32 NewElo, int32 DeltaThisMatch);

private:
	/** Server-only side caches populated by setters. Not replicated; values
	 *  land in StatsEntries each Tick via UpdateFromPlayerStates. */
	TMap<FString, float> PPRCurrentCache;
	TMap<FString, int32> EloCache;
	TMap<FString, int32> EloDeltaCache;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
