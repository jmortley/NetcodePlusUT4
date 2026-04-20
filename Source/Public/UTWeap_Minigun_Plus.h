// UTWeap_Minigun_Plus.h
#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTWeap_Minigun_Plus.generated.h"

/**
 * AUTWeap_Minigun_Plus
 *
 * NetcodePlus minigun. Inherits from AUTWeaponFix so the server-side
 * HitScanTrace override (with ping-based capsule rewind) applies automatically
 * via virtual dispatch. Firing flow is routed to the grandparent AUTWeapon
 * directly (bypassing AUTWeaponFix's transactional event / fake projectile
 * machinery), because the stock spin-up firing state handles refire timing
 * and the minigun doesn't need per-shot event indexing.
 *
 * Hit reg on high ping improves "for free" since every FireInstantHit call
 * eventually dispatches HitScanTrace through the vtable to AUTWeaponFix's
 * rewind-aware implementation.
 */
UCLASS(abstract)
class NETCODEPLUS_API AUTWeap_Minigun_Plus : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

	//~ Begin AUTWeapon Interface
	virtual void StartFire(uint8 FireModeNum) override;
	virtual void StopFire(uint8 FireModeNum) override;
	virtual void FireShot() override;
	virtual void PlayFiringEffects() override;
	virtual float GetAISelectRating_Implementation() override;
	virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;
	virtual bool HasAmmo(uint8 FireModeNum) override;
	//~ End AUTWeapon Interface
};
