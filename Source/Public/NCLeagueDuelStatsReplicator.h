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

/** Per-player snapshot — accuracy + armor pickup counts. The underlying
 *  AUTPlayerState::StatsData TMap is not replicated, so we mirror the
 *  values we want client-side here. */
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

	/** Per-armor-type pickup counts. Stock UT4 increments these via
	 *  AUTPickupInventory::GiveTo -> ModifyStatsValue(StatsNameCount, 1)
	 *  using NAME_ShieldBeltCount / NAME_ArmorVestCount / NAME_ArmorPadsCount
	 *  / NAME_HelmetCount. uint8 is enough — duel matches don't exceed 255
	 *  pickups of any single armor type. */
	UPROPERTY() uint8 BeltCount   = 0;
	UPROPERTY() uint8 VestCount   = 0;
	UPROPERTY() uint8 PadsCount   = 0;
	UPROPERTY() uint8 HelmetCount = 0;
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

	/** Client-safe: armor pickup counts. 0 if no entry yet. */
	uint8 GetBeltCountForPlayer  (const FString& UniqueIdStr) const;
	uint8 GetVestCountForPlayer  (const FString& UniqueIdStr) const;
	uint8 GetPadsCountForPlayer  (const FString& UniqueIdStr) const;
	uint8 GetHelmetCountForPlayer(const FString& UniqueIdStr) const;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
