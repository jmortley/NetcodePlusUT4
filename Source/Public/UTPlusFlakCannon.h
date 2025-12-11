// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTPlusFlakCannon.generated.h"

/**
 * UTPlus Flak Cannon
 * * Implements standard Flak Cannon logic (Primary Shard Spread / Secondary Shell)
 * on top of the UTWeaponFix transactional networking system.
 */
UCLASS(abstract)
class NETCODEPLUS_API AUTPlusFlakCannon : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

	/** Number of projectiles to fire.
	* When firing multiple projectiles at once, main projectile will be fired at crosshair.
	* Remaining projectiles will be fired in a circle pattern */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	TArray<int32> MultiShotCount;

	/** Projectile class to use when firing multiple projectiles at once.
	* This is only for additional projectiles, main projectile will use ProjClass.
	* If not specified, ProjClass will be used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	TArray< TSubclassOf<AUTProjectile> > MultiShotProjClass;

	/** Firing cone angle in degrees.
	* Applies to individual projectiles when firing multiple at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	TArray<FRotator> MultiShotAngle;

	/** Firing location randomness, in unreal units.
	* Applies to individual projectiles when firing multiple at once */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	TArray<FVector> MultiShotLocationSpread;

	/** Firing direction randomness, in degrees.
	* Applies to individual projectiles when firing multiple at once */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	TArray<float> MultiShotRotationSpread;

	/** Returns projectile spawn location when firing multiple projectiles at once */
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Weapon")
	FVector GetFireLocationForMultiShot(int32 MultiShotIndex, const FVector& FireLocation, const FRotator& FireRotation);

	/** Returns projectile spawn rotation when firing multiple projectiles at once */
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "Weapon")
	FRotator GetFireRotationForMultiShot(int32 MultiShotIndex, const FVector& FireLocation, const FRotator& FireRotation);

	/** * Overridden to handle the multi-projectile loop. 
	 * Calls AUTWeaponFix::SpawnNetPredictedProjectile to handle server-side catchup.
	 */
	virtual AUTProjectile* FireProjectile() override;

	virtual float SuggestAttackStyle_Implementation() override;
	virtual float SuggestDefenseStyle_Implementation() override;
	virtual float GetAISelectRating_Implementation() override;
	virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;
};