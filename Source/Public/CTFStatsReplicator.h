// CTFStatsReplicator.h — Replicates per-player CTF stats that
// GetStatsValue() only exposes server-side: flag grabs and hitscan accuracy.
// Same pattern as WipeoutDamageReplicator.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "CTFStatsReplicator.generated.h"

USTRUCT()
struct FCTFReplicatedStatsEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	int32 FlagGrabs = 0;

	UPROPERTY()
	int32 HitscanHits = 0;

	UPROPERTY()
	int32 HitscanShots = 0;

	// Armor pickup counts. Stock UT4 increments these via
	// AUTPickupInventory::GiveTo → ModifyStatsValue using
	// NAME_ShieldBeltCount / NAME_ArmorVestCount / NAME_ArmorPadsCount /
	// NAME_HelmetCount. uint8 is enough for a CTF match.
	UPROPERTY() uint8 BeltCount   = 0;
	UPROPERTY() uint8 VestCount   = 0;
	UPROPERTY() uint8 PadsCount   = 0;
	UPROPERTY() uint8 HelmetCount = 0;

	// UDamage / Amp pickup count + total time held (seconds, integer-truncated).
	// NAME_UDamageCount and NAME_UDamageTime are server-only on AUTPlayerState
	// (StatsData TMap, no Replicated specifier).
	UPROPERTY() uint8 AmpCount  = 0;
	UPROPERTY() int32 AmpTimeS  = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API ACTFStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:

	UPROPERTY(Replicated, Transient)
	TArray<FCTFReplicatedStatsEntry> StatsEntries;

	/** True if any player on the server has accumulated InstagibShots.
	 *  Lets the scoreboard switch between non-instagib (armors + amp visible)
	 *  and instagib (no armors / no amp because they don't apply) layouts. */
	UPROPERTY(Replicated, Transient)
	bool bIsInstagibMatch = false;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	const FCTFReplicatedStatsEntry* FindEntry(const FString& UniqueIdStr) const;
	int32 GetGrabsForPlayer(const FString& UniqueIdStr) const;
	void GetAccuracyForPlayer(const FString& UniqueIdStr, int32& OutHits, int32& OutShots) const;
	void GetArmorCountsForPlayer(const FString& UniqueIdStr, uint8 OutCounts[4]) const;
	void GetAmpForPlayer(const FString& UniqueIdStr, uint8& OutCount, int32& OutTimeSeconds) const;

	void UpdateFromPlayerStates();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
