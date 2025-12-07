#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTProj_ShockBall.h"
#include "UTPlusProj_ShockBall.generated.h"

/**
 * Custom shock ball projectile for UTPlusShockRifle.
 * Same as stock shock ball but references UTPlusShockRifle instead of UTWeap_ShockRifle.
 */
UCLASS()
class NETCODEPLUS_API AUTPlusProj_ShockBall : public AUTProj_ShockBall
{
	GENERATED_BODY()

public:
	AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer);

protected:
	/**
	 * Override to reference UTPlusShockRifle instead of UTWeap_ShockRifle.
	 * This ensures combo detection works with our custom weapon.
	 */
	virtual void PerformCombo(class AController* InstigatedBy, class AActor* DamageCauser) override;
};