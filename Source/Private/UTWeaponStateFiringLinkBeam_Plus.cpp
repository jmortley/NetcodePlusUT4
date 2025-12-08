// UTWeaponStateFiringLinkBeam_Plus.cpp

#include "UnrealTournament.h"
#include "UTWeaponStateFiringLinkBeam_Plus.h"
#include "UTWeap_LinkGun_Plus.h"

UUTWeaponStateFiringLinkBeam_Plus::UUTWeaponStateFiringLinkBeam_Plus(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
    ClientDamageAccumulator = 0.f;
}

void UUTWeaponStateFiringLinkBeam_Plus::Tick(float DeltaTime)
{
    // Call parent to handle animation / delayed shot logic
    // But we are hijacking the damage logic.
    HandleDelayedShot(); 

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (!LinkGun) return;

    // --- SERVER ---
    if (LinkGun->Role == ROLE_Authority)
    {
        // On Server, we DO NOT TRACE. 
        // We only consume ammo and wait for the RPC from the client.
        // We must tick ammo consumption here or infinite ammo cheats are easy.
        LinkGun->ConsumeAmmo(LinkGun->GetCurrentFireMode());
        return;
    }

    // --- CLIENT (and Listen Server Host) ---
    // We are the authority on "Did we hit?"
    if (LinkGun->GetUTOwner() && LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        // Perform the trace locally
        FHitResult Hit;
        LinkGun->FireInstantHit(false, &Hit); // bDealDamage = false, we handle it manually

        // Update visual beam location immediately so it looks responsive
        LinkGun->GetUTOwner()->SetFlashLocation(Hit.Location, LinkGun->GetCurrentFireMode());

        if (Hit.Actor.IsValid() && Hit.Actor->bCanBeDamaged)
        {
            // We hit something! Process it.
            LinkGun->ProcessClientSideHit(DeltaTime, Hit.Actor.Get(), Hit.Location, LinkGun->InstantHitInfo[LinkGun->GetCurrentFireMode()]);
        }
        else
        {
            // We missed. Clear the accumulator so we don't store damage while aiming at a wall
            ClientDamageAccumulator = 0.f;
            
            // Tell server we missed (optional, but good for resetting Link Pull state)
            // For bandwidth, we usually just don't send hits.
        }
    }
}