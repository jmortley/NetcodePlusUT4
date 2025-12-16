#pragma once
#include "NetcodePlus.h"
#include "UTWeaponStateFiringLinkBeam.h"
#include "UTWeaponStateFiringLinkBeamPlus.generated.h"

UCLASS()
class NETCODEPLUS_API UUTWeaponStateFiringLinkBeamPlus : public UUTWeaponStateFiringLinkBeam
{
	GENERATED_UCLASS_BODY()

public:
	virtual void Tick(float DeltaTime) override;
    virtual void BeginState(const UUTWeaponState* Prev) override;
    // Accumulator for client-side damage batching
    float ClientDamageAccumulator;
    virtual void RefireCheckTimer() override;
    bool bPendingEndFire;
    bool bPendingStartFire;
    //bool bHasBegun;
    //float ClientDamageAccumulator;
    virtual void EndFiringSequence(uint8 FireModeNum) override;
    virtual void PendingFireStarted() override;
    virtual void EndState() override;
	virtual void FireShot() override;
protected:
    bool bHasBegun;

};