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

	UPROPERTY()
	int32 BeltPickups = 0;

	UPROPERTY()
	int32 AmpPickups = 0;

	UPROPERTY()
	int32 VestPickups = 0;

	/** Siphon powerup pickups. SiphonPowerup deliberately reuses the stock
	 *  Berserk stat names (SiphonPowerup.cpp) to avoid an engine edit, so this
	 *  reads NAME_BerserkCount. */
	UPROPERTY()
	int32 SiphonPickups = 0;

	/** Health given to teammates this match. Lives on the GameMode
	 *  (AUWipeoutGame::HealingDoneThisMatch, credited by CreditHealing) — server
	 *  only, hence the trip through this replicator like DamageDone. */
	UPROPERTY()
	int32 HealingDone = 0;
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

	/** Look up a player's full replicated snapshot by UniqueNetId string. */
	const FReplicatedDamageEntry* FindEntry(const FString& UniqueIdStr) const;

	/** Look up individual values. Missing players return 0. Safe on clients. */
	int32 GetDamageForPlayer(const FString& UniqueIdStr) const;
	int32 GetBeltsForPlayer(const FString& UniqueIdStr) const;
	int32 GetAmpsForPlayer(const FString& UniqueIdStr) const;
	int32 GetVestsForPlayer(const FString& UniqueIdStr) const;
	int32 GetSiphonsForPlayer(const FString& UniqueIdStr) const;
	int32 GetHealingForPlayer(const FString& UniqueIdStr) const;

	/** Server-only: refresh DamageEntries from all PlayerStates */
	void UpdateFromPlayerStates();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	/** How often to snapshot damage (seconds) */
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
