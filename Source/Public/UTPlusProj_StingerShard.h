#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTProj_StingerShard2.h"
#include "UTPlusProj_StingerShard.generated.h"

/**
 * Enhanced stinger shard (minigun alt-fire projectile) for NetcodePlus.
 *
 * Wires the shard into AUTWeaponFix's validated client-notify projectile
 * rewind (NotifyFakeProjectileHit -> ServerProjectileHitClaim) — the same
 * lag-comp rockets (AUTPlusProj_Rocket) and flak shells (AUTPlusProj_FlakShell)
 * use. The server stays authoritative: a client claim only triggers a rewind
 * re-test against the server's own tracked projectile, then applies the shard's
 * normal ProcessHit damage if (and only if) the tracked shard actually passed
 * within the target's rewound capsule.
 *
 * KEY DIFFERENCE from rocket/flak: the shard's primary kill is a PROXIMITY
 * air-explode — it grows an overlap sphere and detonates (radially) at closest
 * approach to a target, a path that NEVER reaches ProcessHit. So the client
 * claim is routed from BOTH the direct impact (ProcessHit) and the air-explode
 * instant (Tick, mirroring the parent's detonation predicate), whichever fires
 * first, guarded to at most one claim per projectile.
 *
 * GRACE deliberately NOT armed (no OnTrackedProjectileResolved): the shard's
 * damage is radial, so there is no single present-time victim for the grace
 * buffer's double-damage guard. The shard therefore uses LIVE-projectile rescue
 * only (the common high-ping case); live rescue consumes the tracked shard
 * (bExploded), so it can never double-apply with the present-time hit.
 *
 * REQUIRES, to have any effect:
 *   1. bEnableProjectileRewind = true on the firing weapon (NCPMinigun CDO), and
 *   2. this class registered in AUTWeaponFix's bTrackForRewind type list
 *      (done in UTWeaponFix.cpp alongside rocket/flak).
 * Reparent your shard Blueprint to this class. No behavioral change until
 * reparented AND both requirements are met.
 */
UCLASS(Abstract, meta = (ChildCanTick))
class NETCODEPLUS_API AUTPlusProj_StingerShard : public AUTProj_StingerShard2
{
	GENERATED_BODY()

public:
	AUTPlusProj_StingerShard(const FObjectInitializer& ObjectInitializer);

	/** Direct impact path (point-blank / before the sphere air-explodes): route a
	 *  client rewind claim, then stock processing. */
	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
		const FVector& HitLocation, const FVector& HitNormal) override;

	/** Proximity air-explode path (the shard's primary kill): mirror the parent's
	 *  detonation predicate to route the claim an instant before Super detonates. */
	virtual void Tick(float DeltaTime) override;

private:
	/** Client-only: route exactly one rewind claim for HitChar. Self-gates on
	 *  role / fake / owning-connection; NotifyFakeProjectileHit self-gates on the
	 *  weapon's bEnableProjectileRewind, so nothing is sent when the feature is off. */
	void TrySendShardHitClaim(class AUTCharacter* HitChar, const FVector& HitLocation);

	/** Send-once guard — the shard both direct-hits and air-explodes, but the
	 *  server consumes only one tracking entry per claim, so cap at one claim. */
	bool bSentShardHitClaim;
};
