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
 * via virtual dispatch.
 *
 * Firing flow is MODE-SPLIT (2026-07-21):
 *  - Mode 0 (spin-up hitscan) routes to the grandparent AUTWeapon directly,
 *    bypassing the transactional event machinery — the stock spin-up firing
 *    state owns refire timing server-side and there is no per-shot client
 *    message to transact. Rewind hit reg still applies via HitScanTrace.
 *    FireShot stamps LastFireTime[0] on the server (nothing on the stock
 *    route writes it) so AUTWeaponFix::Tick's stuck-fire watchdog sees a
 *    live stream instead of the -1 sentinel it would kill on sight.
 *  - Mode 1 (stinger shard projectile) routes THROUGH AUTWeaponFix's
 *    transactional path: per-shot fire events (ROF validation + anti-dup) and
 *    client-fake/server-auth projectile pairing with ExactPing-based catch-up,
 *    independent of the player's negotiated MaxPredictionPing.
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

	/** Primary is spread hitscan and never claims, so like the link beam its
	 *  only acceptance route was the raw validation rewind — same
	 *  aim-at-rendering vs raw-history offset, on a thinner trace. Opt into
	 *  the render-credit re-test (ncp.RenderCredit); the spread-adjusted ray
	 *  is server-owned, so spread composes with the credit pass unchanged. */
	virtual bool SupportsRenderCredit() const override { return true; }
};
