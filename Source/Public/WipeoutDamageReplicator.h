// WipeoutDamageReplicator.h — Lightweight replicated actor that broadcasts
// per-player damage totals from server to all clients.
// DamageDone on AUTPlayerState is not replicated by Epic, so this actor
// periodically snapshots it and replicates via a parallel array.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "WipeoutDamageReplicator.generated.h"

/** Single entry: maps a player's UniqueId string to their total damage */
USTRUCT()
struct FReplicatedDamageEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	int32 DamageDone = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API AWipeoutDamageReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:

	/** Replicated damage snapshot — server writes, clients read */
	UPROPERTY(Replicated, Transient)
	TArray<FReplicatedDamageEntry> DamageEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Look up a player's replicated damage by their UniqueNetId string.
	 *  Returns 0 if not found. Safe to call on clients. */
	int32 GetDamageForPlayer(const FString& UniqueIdStr) const;

	/** Server-only: refresh DamageEntries from all PlayerStates */
	void UpdateFromPlayerStates();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	/** How often to snapshot damage (seconds) */
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
