// UTWeaponStateFiringLinkBeam_Plus.cpp
#include "UTWeaponStateFiringLinkBeamPlus.h"
#include "UnrealTournament.h"
#include "UTWeap_LinkGun_Plus.h"
#include "Animation/AnimInstance.h"


DEFINE_LOG_CATEGORY_STATIC(LogUTWeaponState, Log, All);

UUTWeaponStateFiringLinkBeamPlus::UUTWeaponStateFiringLinkBeamPlus(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ClientDamageAccumulator = 0.f;
}


void UUTWeaponStateFiringLinkBeamPlus::BeginState(const UUTWeaponState* Prev)
{
    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    UE_LOG(LogUTWeaponState, Warning, TEXT("LinkBeam BeginState Outer=%s (Role=%d)"),
        *GetNameSafe(GetOuter()), (LinkGun ? LinkGun->Role : -1));
    if (LinkGun)
    {
        LinkGun->CurrentLinkedTarget = nullptr;
        LinkGun->LinkStartTime = -100.f;
    }


    UUTWeaponStateFiringBeam::BeginState(Prev);
    bHasBegun = true;
}


void UUTWeaponStateFiringLinkBeamPlus::Tick(float DeltaTime)
{
    // Call parent to handle animation / delayed shot logic
    // But we are hijacking the damage logic.
    if (!bHasBegun) { return; }
    HandleDelayedShot();

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (!LinkGun) { return; }
    //HandleDelayedShot(); 


    // --- SERVER ---
    if (LinkGun->Role == ROLE_Authority && !LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        // Dedicated server only - wait for client RPC
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