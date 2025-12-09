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


void UUTWeaponStateFiringLinkBeamPlus::RefireCheckTimer()
{
    UE_LOG(LogUTWeaponState, Warning, TEXT("=== RefireCheckTimer START ==="));

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());

    UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - LinkGun=%s"), *GetNameSafe(LinkGun));

    if (!LinkGun)
    {
        UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - NO LINKGUN, ABORT"));
        return;
    }

    UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - UTOwner=%s"), *GetNameSafe(LinkGun->GetUTOwner()));

    if (!LinkGun->GetUTOwner())
    {
        UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - NO OWNER, going to ActiveState"));
        LinkGun->GotoActiveState();
        return;
    }

    UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - IsPendingFire(1)=%d"),
        LinkGun->GetUTOwner()->IsPendingFire(1));

    // Beam-specific: just check if we should stop firing
    if (!LinkGun->GetUTOwner()->IsPendingFire(LinkGun->GetCurrentFireMode()))
    {
        UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - Not pending fire, ending sequence"));
        EndFiringSequence(LinkGun->GetCurrentFireMode());
        return;
    }

    // Check ammo
    if (!LinkGun->HasAmmo(LinkGun->GetCurrentFireMode()))
    {
        UE_LOG(LogUTWeaponState, Warning, TEXT("RefireCheck - No ammo, going to ActiveState"));
        LinkGun->GotoActiveState();
        return;
    }

    UE_LOG(LogUTWeaponState, Warning, TEXT("=== RefireCheckTimer END ==="));
}


void UUTWeaponStateFiringLinkBeamPlus::BeginState(const UUTWeaponState* Prev)
{
    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());

    if (LinkGun)
    {
        LinkGun->CurrentLinkedTarget = nullptr;
        LinkGun->LinkStartTime = -100.f;
    }

    // Do what UUTWeaponStateFiring::BeginState does, but SKIP FireShot()
    GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
        RefireCheckHandle,
        this,
        &UUTWeaponStateFiring::RefireCheckTimer,
        GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode()),
        true
    );
    ToggleLoopingEffects(true);
    PendingFireSequence = -1;
    bDelayShot = false;
    GetOuterAUTWeapon()->OnStartedFiring();
    // NO FireShot() - beam handles this in Tick()
    GetOuterAUTWeapon()->bNetDelayedShot = false;

    bHasBegun = true;
}





void UUTWeaponStateFiringLinkBeamPlus::Tick(float DeltaTime)
{
    UE_LOG(LogUTWeaponState, Warning, TEXT("=== Tick START ==="));

    if (!bHasBegun)
    {
        UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - not begun, skip"));
        return;
    }

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());

    UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - LinkGun=%s"), *GetNameSafe(LinkGun));

    if (!LinkGun) { return; }

    UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - UTOwner=%s"), *GetNameSafe(LinkGun->GetUTOwner()));

    if (!LinkGun->GetUTOwner()) { return; }

    UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - About to check Role"));
    UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - Role=%d"), (int32)LinkGun->Role);
    UE_LOG(LogUTWeaponState, Warning, TEXT("Tick - IsLocallyControlled=%d"), LinkGun->GetUTOwner()->IsLocallyControlled());


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


void UUTWeaponStateFiringLinkBeamPlus::PendingFireStarted()
{
    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (LinkGun && LinkGun->IsLinkPulsing())
    {
        bPendingStartFire = true;
    }
    else
    {
        bPendingEndFire = false;
    }
}


void UUTWeaponStateFiringLinkBeamPlus::EndState()
{
    bPendingStartFire = false;
    bPendingEndFire = false;
    bHasBegun = false;
    ClientDamageAccumulator = 0.f;

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (LinkGun)
    {
        LinkGun->bReadyToPull = false;
        LinkGun->CurrentLinkedTarget = nullptr;
        LinkGun->bLinkBeamImpacting = false;
        LinkGun->bLinkCausingDamage = false;
    }

    // Call grandparent - skip UUTWeaponStateFiringLinkBeam::EndState
    UUTWeaponStateFiringBeam::EndState();
}