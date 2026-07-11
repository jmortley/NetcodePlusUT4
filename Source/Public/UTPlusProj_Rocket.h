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
 * Also prevents replicated primary rockets from adopting another player's
 * client fake by requiring matching instigators and bounded separation.
 *
 * Reparent your rocket Blueprint to this class to enable projectile
 * lag compensation and protected fake pairing.
 */
UCLASS()
class NETCODEPLUS_API AUTPlusProj_Rocket : public AUTProj_Rocket
{
	GENERATED_BODY()

public:
	AUTPlusProj_Rocket(const FObjectInitializer& ObjectInitializer);

	virtual void ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
		const FVector& HitLocation, const FVector& HitNormal) override;
	virtual bool CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const override;
	virtual void BeginFakeProjectileSynch(AUTProjectile* InFakeProjectile) override;
	virtual void Tick(float DeltaTime) override;
	virtual void Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal,
		UPrimitiveComponent* HitComp = nullptr) override;
	virtual void ShutDown() override;
	virtual void Destroyed() override;

	// Diagnostic: logs each authority-role spawn with an explicit SRV/FAKE side label.
	virtual void BeginPlay() override;

private:
	// Owning-client presentation only. Captured from the caught-up replicated real before stock
	// pairing teleports the fake, then advanced collision-free for straight primary/spread rockets.
	bool bPrimarySoftSyncActive;
	FVector PrimarySyncEstimateLocation;
	FVector PrimarySyncEstimateVelocity;
	float PrimarySyncCorrectionSpeed;
};
