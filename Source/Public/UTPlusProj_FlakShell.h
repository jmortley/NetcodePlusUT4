#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTProj_FlakShell.h"
#include "UTPlusProj_FlakShell.generated.h"

/**
 * Enhanced flak shell (alt-fire ball) for NetcodePlus.
 *
 * Adds client-notify projectile rewind: when the client's fake flak ball
 * hits an enemy, sends an RPC so the server can validate with rewind.
 *
 * Reparent your flak shell Blueprint to this class to enable projectile
 * lag compensation. No behavioral change until reparented.
 */
UCLASS(Abstract, meta = (ChildCanTick))
class NETCODEPLUS_API AUTPlusProj_FlakShell : public AUTProj_FlakShell
{
	GENERATED_BODY()

public:
	AUTPlusProj_FlakShell(const FObjectInitializer& ObjectInitializer);

	/** Stock accepts every gravity projectile candidate. Flak shells instead require an
	 * unpaired live fake from the same instigator and a compatible ballistic phase. */
	virtual bool CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const override;

	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
		const FVector& HitLocation, const FVector& HitNormal) override;

	// Server-first explosion visual (mirrors AUTPlusProj_Rocket): when the replicated real
	// resolves before its visible fake, drive the fake to play the authoritative explosion at
	// truth instead of vanishing mid-air. No soft-sync — the shell arcs under gravity, which the
	// rocket's straight-line phase correction does not model (and is gated off for it anyway).
	virtual void Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal,
		UPrimitiveComponent* HitComp = nullptr) override;
	virtual void ShutDown() override;

private:
	// Re-entrancy guard for the ShutDown->Explode fallback (see .cpp).
	bool bForcingShutdownExplosion;
};
