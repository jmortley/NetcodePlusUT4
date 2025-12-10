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
    bPendingEndFire = false;
    bPendingStartFire = false;
    bHasBegun = false;
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




/*old working tick
void UUTWeaponStateFiringLinkBeamPlus::Tick(float DeltaTime)
{
    if (!bHasBegun) { return; }

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (!LinkGun || !LinkGun->GetUTOwner()) { return; }

    // --- Handle Pending End Fire (Pull Logic) ---
    if (bPendingEndFire)
    {
        if (LinkGun->IsLinkPulsing())
        {
            // Currently pulsing - update beam endpoint and wait
            LinkGun->GetUTOwner()->SetFlashLocation(LinkGun->PulseLoc, LinkGun->GetCurrentFireMode());
            return;
        }
        else if (LinkGun->bReadyToPull && LinkGun->CurrentLinkedTarget)
        {
            // Ready to pull - execute it!
            LinkGun->StartLinkPull();
            return;
        }

        if (bPendingStartFire)
        {
            // Player pressed fire again during pulse
            bPendingEndFire = false;
        }
        else
        {
            // Actually end now
            EndFiringSequence(LinkGun->GetCurrentFireMode());
            return;
        }
    }
    bPendingStartFire = false;

    // Reset beam state flags each tick (server)
    if (LinkGun->Role == ROLE_Authority)
    {
        LinkGun->bLinkCausingDamage = false;
    }

    // --- SERVER (Dedicated only) ---
    if (LinkGun->Role == ROLE_Authority && !LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        LinkGun->ConsumeAmmo(LinkGun->GetCurrentFireMode());

        return;
    }

    // --- CLIENT (and Listen Server Host) ---
    if (LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        FHitResult Hit;
        LinkGun->FireInstantHit(false, &Hit);

        // Update visual beam location immediately
        LinkGun->GetUTOwner()->SetFlashLocation(Hit.Location, LinkGun->GetCurrentFireMode());

        // Track beam impact state
        LinkGun->bLinkBeamImpacting = (Hit.Time < 1.f);

        // Track previous target for link timing
        AActor* OldLinkedTarget = LinkGun->CurrentLinkedTarget;
        LinkGun->CurrentLinkedTarget = nullptr;

        if (Hit.Actor.IsValid() && Hit.Actor->bCanBeDamaged)
        {
            // Check if valid link target (for pull mechanic)
            if (LinkGun->IsValidLinkTarget(Hit.Actor.Get()))
            {
                LinkGun->CurrentLinkedTarget = Hit.Actor.Get();
            }

            LinkGun->bLinkCausingDamage = true;
            LinkGun->ProcessClientSideHit(DeltaTime, Hit.Actor.Get(), Hit.Location, LinkGun->InstantHitInfo[LinkGun->GetCurrentFireMode()]);
        }
        else
        {
            // Missed - clear accumulator
            ClientDamageAccumulator = 0.f;
        }

        // --- Pull Warmup Logic ---
        if (OldLinkedTarget != LinkGun->CurrentLinkedTarget)
        {
            // Target changed - reset link timer
            LinkGun->LinkStartTime = GetWorld()->GetTimeSeconds();
            LinkGun->bReadyToPull = false;
        }
        else if (LinkGun->CurrentLinkedTarget && !LinkGun->IsLinkPulsing())
        {
            // Same target - check if we've held long enough to pull
            LinkGun->bReadyToPull = (GetWorld()->GetTimeSeconds() - LinkGun->LinkStartTime > LinkGun->PullWarmupTime);
        }
    }
}
*/



void UUTWeaponStateFiringLinkBeamPlus::Tick(float DeltaTime)
{
    if (!bHasBegun)
    {
        return;
    }

    // Keep base firing state timing (delay shot logic, etc.)
    HandleDelayedShot();

    AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());
    if (!LinkGun || !LinkGun->GetUTOwner())
    {
        return;
    }

    // Server: clear damage flag each tick
    if (LinkGun->Role == ROLE_Authority)
    {
        LinkGun->bLinkCausingDamage = false;
    }

    // -------------------------
    // Handle Pending End Fire / Pull Logic (same as your version)
    // -------------------------
    if (bPendingEndFire)
    {
        // Still pulsing: just keep drawing to PulseLoc
        if (LinkGun->IsLinkPulsing())
        {
            LinkGun->GetUTOwner()->SetFlashLocation(LinkGun->PulseLoc, LinkGun->GetCurrentFireMode());
            return;
        }
        // Ready to pull and still linked  fire the yoink
        else if (LinkGun->bReadyToPull && LinkGun->CurrentLinkedTarget)
        {
            LinkGun->StartLinkPull();
            return;
        }

        if (bPendingStartFire)
        {
            // Player pressed fire again during pulse
            bPendingEndFire = false;
        }
        else
        {
            // Actually end now
            EndFiringSequence(LinkGun->GetCurrentFireMode());
            return;
        }
    }
    bPendingStartFire = false;

    // --------------------------------------------------------
    // 1) SERVER: dedicated / remote simulation for visuals only
    // --------------------------------------------------------
    if (LinkGun->Role == ROLE_Authority && !LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        // We DO NOT consume ammo here – the owning server context
        // already handled ammo and damage. This branch is purely
        // to drive replicated beam state for other clients.

        FHitResult Hit;
        const uint8 FireMode = LinkGun->GetCurrentFireMode();

        // Suppress stats so beam traces don't spam accuracy
        FName RealShots = LinkGun->ShotsStatsName;
        FName RealHits = LinkGun->HitsStatsName;
        LinkGun->ShotsStatsName = NAME_None;
        LinkGun->HitsStatsName = NAME_None;

        LinkGun->FireInstantHit(false, &Hit);

        LinkGun->ShotsStatsName = RealShots;
        LinkGun->HitsStatsName = RealHits;

        // Replicated beam is hitting flag
        LinkGun->bLinkBeamImpacting = (Hit.Time < 1.f);

        // Replicated linked target (for HUD / SFX on others)
        AActor* OldLinked = LinkGun->CurrentLinkedTarget;
        LinkGun->CurrentLinkedTarget = nullptr;

        if (Hit.Actor.IsValid() && Hit.Actor->bCanBeDamaged && LinkGun->IsValidLinkTarget(Hit.Actor.Get()))
        {
            LinkGun->CurrentLinkedTarget = Hit.Actor.Get();
        }

        // For other clients’ audio/HUD we still want to know if beam is hitting something
        LinkGun->bLinkCausingDamage = Hit.Actor.IsValid() && Hit.Actor->bCanBeDamaged;

        // OPTIONAL: you can mirror the warmup timer here if you ever
        // need server-auth decisions about pull readiness for spectators.
        if (OldLinked != LinkGun->CurrentLinkedTarget)
        {
            LinkGun->LinkStartTime = GetWorld()->GetTimeSeconds();
            LinkGun->bReadyToPull = false;
        }
        else if (LinkGun->CurrentLinkedTarget && !LinkGun->IsLinkPulsing())
        {
            LinkGun->bReadyToPull = (GetWorld()->GetTimeSeconds() - LinkGun->LinkStartTime > LinkGun->PullWarmupTime);
        }

        return;
    }

    // --------------------------------------------------------
    // 2) CLIENT (and listen server owner): real aim + CSHD damage
    // --------------------------------------------------------
    if (LinkGun->GetUTOwner()->IsLocallyControlled())
    {
        FHitResult Hit;
        const uint8 FireMode = LinkGun->GetCurrentFireMode();

        // Same stats suppression as Epic
        FName RealShots = LinkGun->ShotsStatsName;
        FName RealHits = LinkGun->HitsStatsName;
        LinkGun->ShotsStatsName = NAME_None;
        LinkGun->HitsStatsName = NAME_None;

        LinkGun->FireInstantHit(false, &Hit);

        LinkGun->ShotsStatsName = RealShots;
        LinkGun->HitsStatsName = RealHits;

        // 2a) Update visual beam location immediately (owner)
        LinkGun->GetUTOwner()->SetFlashLocation(Hit.Location, FireMode);

        // 2b) Track beam impact + linked target for pull logic
        LinkGun->bLinkBeamImpacting = (Hit.Time < 1.f);

        AActor* OldLinkedTarget = LinkGun->CurrentLinkedTarget;
        LinkGun->CurrentLinkedTarget = nullptr;

        if (Hit.Actor.IsValid() && Hit.Actor->bCanBeDamaged)
        {
            // Check if valid link target (for pull + reward)
            if (LinkGun->IsValidLinkTarget(Hit.Actor.Get()))
            {
                LinkGun->CurrentLinkedTarget = Hit.Actor.Get();
            }

            LinkGun->bLinkCausingDamage = true;

            // 2c) Your client-side damage batching
            LinkGun->ProcessClientSideHit(
                DeltaTime,
                Hit.Actor.Get(),
                Hit.Location,
                LinkGun->InstantHitInfo[FireMode]);
        }
        else
        {
            // Miss: clear accumulator so we don’t store damage off-target
            ClientDamageAccumulator = 0.f;
        }

        // 2d) Pull warmup logic (same semantics as stock Link)
        if (OldLinkedTarget != LinkGun->CurrentLinkedTarget)
        {
            // Target changed – reset timer
            LinkGun->LinkStartTime = GetWorld()->GetTimeSeconds();
            LinkGun->bReadyToPull = false;
        }
        else if (LinkGun->CurrentLinkedTarget && !LinkGun->IsLinkPulsing())
        {
            LinkGun->bReadyToPull =
                (GetWorld()->GetTimeSeconds() - LinkGun->LinkStartTime > LinkGun->PullWarmupTime);
        }
    }
}




void UUTWeaponStateFiringLinkBeamPlus::EndFiringSequence(uint8 FireModeNum)
{
    if (FireModeNum == GetFireMode())
    {
        AUTWeap_LinkGun_Plus* LinkGun = Cast<AUTWeap_LinkGun_Plus>(GetOuterAUTWeapon());

        if (!LinkGun || (!LinkGun->bReadyToPull && !LinkGun->IsLinkPulsing()))
        {
            // Normal end - not pulling
            // Skip parent classes, just go to active state
            if (FireModeNum == GetOuterAUTWeapon()->GetCurrentFireMode())
            {
                GetOuterAUTWeapon()->GotoActiveState();
            }
        }
        else
        {
            // Ready to pull or currently pulsing - delay end
            bPendingEndFire = true;
            bPendingStartFire = false;
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