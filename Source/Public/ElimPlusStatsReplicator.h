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

	/** Sniper / Lightning Gun accuracy: (hits/shots) as percentage * 100, packed
	 *  as an integer (0-10000). Display: divide by 100. -1 = no sniper shots
	 *  fired -> scoreboard prints "-" instead of a misleading 0%. */
	UPROPERTY()
	int32 LinkGunAccuracyTimes100 = -1;

	/** Global leaderboard rank (1-based) for this player's frozen ELO. 0 = unranked
	 *  / unknown / bot -> the scoreboard then shows no "(rank)". Set at match start + end. */
	UPROPERTY()
	int32 GlobalRank = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API AElimPlusStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** Replicated per-player stats array. Server writes, clients read. */
	UPROPERTY(Replicated, Transient)
	TArray<FElimPlusStatsEntry> StatsEntries;

	/** Mirror of AUTTeamGameMode::bBalanceTeams (parsed from the
	 *  ?BalanceTeams=true|false URL flag in InitGame). Replicated so the
	 *  client-side HUD can hide the pre-match team-balance preview overlay
	 *  when the admin disabled balancing — there's no point telegraphing an
	 *  ELO-balanced split that isn't going to happen. Server pushes this once
	 *  in HandleMatchHasStarted via SetBalanceTeamsActive. */
	UPROPERTY(Replicated, Transient)
	bool bBalanceTeamsActive = true;

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

	/** Server-only: gamemode / rating system pushes a player's global leaderboard
	 *  rank (1-based; 0 = unranked) at match start + end, alongside the ELO. */
	void SetPlayerGlobalRank(const FString& UniqueIdStr, int32 Rank);

	/** Server-only: gamemode pushes its bBalanceTeams flag here so it can
	 *  replicate to client HUDs. */
	void SetBalanceTeamsActive(bool bActive);

	/** Client-safe accessor for the replicated bBalanceTeamsActive flag. */
	bool IsBalanceTeamsActive() const { return bBalanceTeamsActive; }

private:
	/** Server-only side caches populated by setters. Not replicated; values
	 *  land in StatsEntries each Tick via UpdateFromPlayerStates. */
	TMap<FString, float> PPRCurrentCache;
	TMap<FString, int32> EloCache;
	TMap<FString, int32> EloDeltaCache;
	TMap<FString, int32> GlobalRankCache;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
