#pragma once
#include "NetcodePlus.h"
#include "UTWeaponStateFiringChargedRocket.h"
#include "UTWeaponStateFiringCharged_Transactional.generated.h"

UCLASS()
class UUTWeaponStateFiringCharged_Transactional : public UUTWeaponStateFiringChargedRocket
{
    GENERATED_UCLASS_BODY()
    UPROPERTY()
    AUTWeap_RocketLauncher* RocketLauncher;
    // Override to prevent standard RPCs
    virtual void BeginState(const UUTWeaponState* PrevState) override;
    
    // Override to use Transactional RPCs on release
    virtual void FireShot() override;
    
    // Override to handle the "Release Button" check locally
    virtual void Tick(float DeltaTime) override;
};