// ShockDomReplicator — Replicated actor for DOM-specific data.
// Plugin cannot subclass AUTGameState (ABI mismatch with engine DLL).
// This actor replicates control point ownership, capture stats, and score goal.
#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "WipeoutDamageReplicator.h"
#include "ShockDomReplicator.generated.h"

/** Per-player capture count, replicated to clients for scoreboard */
USTRUCT()
struct FShockDomCaptureEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	int32 Captures = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API AShockDomReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:

	/** Score needed to win (replicated for client HUD) */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "DOM")
	int32 DomScoreGoal;

	/** Per-player capture counts */
	UPROPERTY(Replicated, Transient)
	TArray<FShockDomCaptureEntry> PlayerCaptureStats;

	/** Per-player damage (same pattern as WipeoutDamageReplicator) */
	UPROPERTY(Replicated, Transient)
	TArray<FReplicatedDamageEntry> DamageEntries;

	/** Look up a player's capture count by unique ID */
	int32 GetCapturesForPlayer(const FString& UniqueIdStr) const;

	/** Server-only: increment capture count for a player */
	void IncrementCaptures(const FString& UniqueIdStr);

	/** Server-only: refresh DamageEntries from all PlayerStates */
	void UpdateFromPlayerStates();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	// Damage accessors (mirror WipeoutDamageReplicator API)
	int32 GetDamageForPlayer(const FString& UniqueIdStr) const;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
