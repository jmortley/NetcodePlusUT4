#include "UTWeaponStateFiringCharged_Transactional.h"
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTBot.h"

UUTWeaponStateFiringCharged_Transactional::UUTWeaponStateFiringCharged_Transactional(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

void UUTWeaponStateFiringCharged_Transactional::BeginState(const UUTWeaponState* PrevState)
{
    // 1. Call Super to start the loading animation/timers
    // NOTE: Super::BeginState in standard UT typically starts the 'GraceTimer' 
    // and handles the loading logic.
    Super::BeginState(PrevState);

    // 2. IMPORTANT: Do NOT call FireShot() here.
    // The standard Transactional state calls FireShot() immediately. 
    // We want to wait until the user releases the button or the rockets are full.
}

void UUTWeaponStateFiringCharged_Transactional::Tick(float DeltaTime)
{
    // Run standard charging logic (checking for auto-release if full)
    Super::Tick(DeltaTime);

    // CLIENT SIDE ONLY: Check for release
    if (GetOuterAUTWeapon()->GetNetMode() != NM_DedicatedServer && 
        GetOuterAUTWeapon()->GetUTOwner() && 
        GetOuterAUTWeapon()->GetUTOwner()->IsLocallyControlled())
    {
        // If we are NOT firing anymore (button released), and we haven't fired yet...
        // The parent class 'Tick' usually handles the auto-fire if full, 
        // but we need to catch the manual release.
        if (!GetOuterAUTWeapon()->IsFiring())
        {
             // Standard logic calls EndState -> FireShot.
             // We ensure we trigger the transactional path.
             // (Logic handled in Super::Tick -> EndState -> FireShot override below)
        }
    }
}

void UUTWeaponStateFiringCharged_Transactional::FireShot()
{
    // This function is called by the parent class when it decides it is time to fire 
    // (either button release or max rockets loaded).

    AUTWeaponFix* WeaponFix = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
    if (WeaponFix)
    {
        // 1. Force the Transactional RPC
        // We call FireShot on the weapon, which triggers ServerStartFireFixed
        WeaponFix->FireShot(); 
    }
    else
    {
        Super::FireShot();
    }
}