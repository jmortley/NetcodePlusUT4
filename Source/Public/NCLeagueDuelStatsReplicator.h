// NCLeagueDuelStatsReplicator — replicates per-player hitscan accuracy from
// server to clients for the NCLeagueDuel scoreboard.
//
// AUTPlayerState::StatsData (the TMap that holds NAME_LinkHits / NAME_ShockRifleHits
// / NAME_SniperHits etc.) is UPROPERTY() with no Replicated specifier — server-only.
// Client scoreboards calling PS->GetStatsValue(...) always get 0 in networked play.
//
// Pattern mirrors AElimPlusStatsReplicator: AInfo subclass, bAlwaysRelevant,
// Tick at 1Hz on the authority, snapshots PlayerArray's hits/shots into a
// replicated TArray. Clients call GetAccuracyForPlayer(UniqueId) to read.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCLeagueDuelStatsReplicator.generated.h"

/** Per-player accuracy snapshot. Combined hitscan: LG + Shock + Sniper. */
USTRUCT()
struct FNCLeagueDuelStatsEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	/** Hitscan hits/shots * 10000, packed as integer for compact replication.
	 *  Display: divide by 100 to get percentage with 2 decimals. */
	UPROPERTY()
	int32 HitscanAccuracyTimes100 = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCLeagueDuelStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(Replicated, Transient)
	TArray<FNCLeagueDuelStatsEntry> StatsEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** Server-only: refresh StatsEntries from current PlayerStates. */
	void UpdateFromPlayerStates();

	/** Client-safe: returns 0..100 accuracy percent for a player by UniqueId
	 *  string. 0 if no entry (e.g. before first replication). */
	float GetAccuracyForPlayer(const FString& UniqueIdStr) const;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
