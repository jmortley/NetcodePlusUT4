#pragma once

#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTPlusWeap_FlakCannon.generated.h"

UCLASS()
class AUTPlusWeap_FlakCannon : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

	/* * MULTI-SHOT CONFIGURATION
	 * These properties define the "Spread" logic for the Primary Fire (Shards).
	 * We must re-declare them here because we cannot inherit from UTWeap_FlakCannon.
	 */

	/** Spread of the spawn locations for each shard (random offset from center) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak Cannon")
	TArray<FVector> MultiShotLocationSpread;

	/** Random rotation spread added to each shard */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak Cannon")
	TArray<float> MultiShotRotationSpread;

	/** The base angle offset for the spread pattern (e.g. the smiley face curve) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak Cannon")
	TArray<FRotator> MultiShotAngle;

	/** How many projectiles to spawn per shot (e.g. 9 for shards, 1 for shell) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak Cannon")
	TArray<int32> MultiShotCount;

	/** Optional override class for the secondary shards (if different from main projectile) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak Cannon")
	TArray<TSubclassOf<AUTProjectile>> MultiShotProjClass;

public:
	/**
	 * Setup: Swaps the standard firing states for Transactional states.
	 * Flak Cannon uses UTWeaponStateFiring_Transactional for BOTH modes (Instant Fire).
	 */
	//virtual void PostInitProperties() override;

	/**
	 * Execution: Overrides the single-projectile logic to support the Flak Loop (9 shards).
	 * Calls SpawnNetPredictedProjectile multiple times for Mode 0.
	 */
	virtual AUTProjectile* FireProjectile() override;

	/**
	 * Helper: Calculates the spawn location for a specific shard in the loop.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintPure, Category = "Flak Cannon")
	FVector GetFireLocationForMultiShot(int32 MultiShotIndex, const FVector& FireLocation, const FRotator& FireRotation);

	/**
	 * Helper: Calculates the rotation (aim) for a specific shard in the loop.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintPure, Category = "Flak Cannon")
	FRotator GetFireRotationForMultiShot(int32 MultiShotIndex, const FVector& FireLocation, const FRotator& FireRotation);


	/*
	 * AI LOGIC
	 * Standard Bot support copied from the Flak Cannon to ensure bots know how to use it.
	 */
	virtual float SuggestAttackStyle_Implementation() override;
	virtual float SuggestDefenseStyle_Implementation() override;
	virtual float GetAISelectRating_Implementation() override;
	virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;
};