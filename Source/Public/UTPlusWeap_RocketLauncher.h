#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h" // Inherit from your Fix class
#include "UTWeap_RocketLauncher.h" // Needed for some structs/enums if accessible, otherwise copy them
#include "UTPlusWeap_RocketLauncher.generated.h"



UCLASS(abstract)
class NETCODEPLUS_API AUTPlusWeap_RocketLauncher : public AUTWeaponFix
{
    GENERATED_UCLASS_BODY()

    // --- COPY PASTE ALL PROPERTIES FROM UTWeap_RocketLauncher.h ---
    // You need the LockOn variables, Loading Animations, etc.
    // ...
public:
    virtual void PostInitProperties() override;

    // Override FireProjectile to use your "Plus" projectiles with interp
    //virtual AUTProjectile* FireProjectile() override;

    // You will need to copy the functionality of:
    // BeginLoadRocket, EndLoadRocket, FireShot, etc.
    // from UTWeap_RocketLauncher.cpp
};