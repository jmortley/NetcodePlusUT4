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
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API ACTFStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:

	UPROPERTY(Replicated, Transient)
	TArray<FCTFReplicatedStatsEntry> StatsEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	int32 GetGrabsForPlayer(const FString& UniqueIdStr) const;
	void GetAccuracyForPlayer(const FString& UniqueIdStr, int32& OutHits, int32& OutShots) const;

	void UpdateFromPlayerStates();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
