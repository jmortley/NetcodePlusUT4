#include "UTPlusWeap_RocketLauncher.h"
#include "UnrealTournament.h"
#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponStateFiringCharged_Transactional.h"
#include "StatNames.h"
#include "Core.h"
#include "Engine.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Net/UnrealNetwork.h"

AUTPlusWeap_RocketLauncher::AUTPlusWeap_RocketLauncher(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
    // ... Copy Constructor setup from UTWeap_RocketLauncher ...
}

void AUTPlusWeap_RocketLauncher::PostInitProperties()
{
    Super::PostInitProperties();

    // 1. SETUP FIRE MODE 0 (Primary - Single Rocket)
    // This is instant/repeating, so we use your standard Transactional State
    if (FiringState.Num() > 0)
    {
        FiringState[0] = NewObject<UUTWeaponStateFiring_Transactional>(this, UUTWeaponStateFiring_Transactional::StaticClass());
    }

    // 2. SETUP FIRE MODE 1 (Alt - Load 3 Rockets)
    // This is charged, so we use the NEW Charged Transactional State
    if (FiringState.Num() > 1)
    {
        // This handles: Hold -> Load -> Release -> Transactional RPC
        FiringState[1] = NewObject<UUTWeaponStateFiringCharged_Transactional>(this, UUTWeaponStateFiringCharged_Transactional::StaticClass());
    }
}

// ... Copy Paste the rest of the logic from UTWeap_RocketLauncher.cpp ...
// Make sure to change any Cast<UTWeap_RocketLauncher> to Cast<AUTPlusWeap_RocketLauncher>