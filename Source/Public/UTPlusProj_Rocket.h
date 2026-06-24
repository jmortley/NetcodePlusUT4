#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTProj_Rocket.h"
#include "UTPlusProj_Rocket.generated.h"

/**
 * Enhanced rocket projectile for NetcodePlus.
 *
 * Adds client-notify projectile rewind: when the client's fake rocket
 * hits an enemy, sends an RPC so the server can validate with rewind.
 *
 * Reparent your rocket Blueprint to this class to enable projectile
 * lag compensation. No behavioral change until reparented.
 */
UCLASS()
class NETCODEPLUS_API AUTPlusProj_Rocket : public AUTProj_Rocket
{
	GENERATED_BODY()

public:
	AUTPlusProj_Rocket(const FObjectInitializer& ObjectInitializer);

	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
		const FVector& HitLocation, const FVector& HitNormal) override;

	// Diagnostic: logs each SERVER-spawned rocket (see UTPlusProj_Rocket.cpp).
	virtual void BeginPlay() override;
};
