// UTWeap_Enforcer_Plus.h
#pragma once
#include "NetcodePlus.h"
#include "UTWeap_Enforcer.h"
#include "UTWeap_Enforcer_Plus.generated.h"

/**
 * AUTWeap_Enforcer_Plus
 *
 * NetcodePlus enforcer. Inherits directly from stock AUTWeap_Enforcer so all
 * dual-wield (AUTDualWeapon left-mesh / BecomeDual), burst firing, spread
 * accumulation, and reload-clip logic are preserved unchanged.
 *
 * Does NOT inherit from AUTWeaponFix — that would fight AUTDualWeapon's mesh
 * infrastructure. Instead, we override HitScanTrace locally to perform the
 * same ping-based capsule rewind that AUTWeaponFix does. Net effect: every
 * FireInstantHit on this weapon (stock AUTWeap_Enforcer path) gets rewind
 * validation on the server, so high-ping players' shots register against
 * where targets actually were at fire time.
 *
 * Does not include ReceivedHitScanHitChar / claimed-target padding (no
 * matching client RPC plumbed). Adds a flat bonus capsule padding for moving
 * targets instead — simpler and covers the high-ping feel case.
 */
UCLASS(abstract)
class NETCODEPLUS_API AUTWeap_Enforcer_Plus : public AUTWeap_Enforcer
{
	GENERATED_UCLASS_BODY()

public:
	/** Max one-way rewind window (ms). Mirrors AUTWeaponFix::MaxRewindMs default. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NetcodePlus")
	float MaxRewindMs;

	/** Ping jitter buffer (ms) subtracted from ExactPing before computing rewind. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NetcodePlus")
	float FudgeFactorMs;

	/** Extra capsule-trace padding (units) for moving targets during rewind check.
	 *  UT4 hitboxes are tighter than the visible mesh silhouette; this compensates
	 *  for tiny sync gaps and movement jitter. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NetcodePlus")
	float MovingTargetPadding;

	/** Extra padding for stationary targets. Less is needed since no movement lead. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NetcodePlus")
	float StationaryTargetPadding;

	/** Returns one-way rewind time (seconds) based on owner's ExactPing. */
	virtual float GetRewindSeconds() const;

	//~ Begin AUTWeapon Interface
	virtual void HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime) override;
	//~ End AUTWeapon Interface
};
