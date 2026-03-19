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

	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
		const FVector& HitLocation, const FVector& HitNormal) override;
};
