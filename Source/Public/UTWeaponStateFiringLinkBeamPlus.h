#pragma once

#include "UTWeaponStateFiringLinkBeam.h"
#include "UTWeaponStateFiringLinkBeamPlus.generated.h"

UCLASS()
class NETCODEPLUS_API UUTWeaponStateFiringLinkBeamPlus : public UUTWeaponStateFiringLinkBeam
{
	GENERATED_UCLASS_BODY()

public:
	virtual void Tick(float DeltaTime) override;
    
    // Accumulator for client-side damage batching
    float ClientDamageAccumulator;
};