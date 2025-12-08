#include "UnrealTournament.h"
#include "UTWeap_LinkGun_Plus.h"
#include "UTWeaponStateFiringLinkBeam_Plus.h"
#include "UTPlayerController.h"

AUTWeap_LinkGun_Plus::AUTWeap_LinkGun_Plus(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Override the default firing state for the Beam (Mode 1)
    // We assume Mode 0 is Plasma (Projectile) and Mode 1 is Beam
	if (FiringState.IsValidIndex(1))
	{
        // Replace existing state with our Plus version
		FiringState[1] = ObjectInitializer.CreateDefaultSubobject<UUTWeaponStateFiringLinkBeam_Plus>(this, TEXT("StateFiringBeam_Plus"));
	}

    MaxHitDistanceTolerance = 300.0f; // Allow 3 meters of lag discrepancy
}

void AUTWeap_LinkGun_Plus::ProcessClientSideHit(float DeltaTime, AActor* HitActor, FVector HitLoc, const FInstantHitDamageInfo& DamageInfo)
{
    // --- CLIENT SIDE LOGIC ---
    UUTWeaponStateFiringLinkBeam_Plus* BeamState = Cast<UUTWeaponStateFiringLinkBeam_Plus>(GetCurrentState());
    if (!BeamState) return;

    float RefireTime = GetRefireTime(GetCurrentFireMode());
    float DamagePerSec = float(DamageInfo.Damage) / RefireTime;

    // Accumulate the fractional damage for this tick
    BeamState->ClientDamageAccumulator += DamagePerSec * DeltaTime;

    // Threshold: Send update when we have accumulated enough damage (e.g., > 5)
    // or if we switched targets.
    int32 PendingDamage = FMath::TruncToInt(BeamState->ClientDamageAccumulator);

    if (PendingDamage >= 5) // Batching threshold
    {
        ServerProcessBeamHit(HitActor, HitLoc, PendingDamage);
        BeamState->ClientDamageAccumulator -= PendingDamage;
    }
}

bool AUTWeap_LinkGun_Plus::ServerProcessBeamHit_Validate(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount)
{
    // Basic anti-cheat sanity checks
    if (!HitActor || DamageAmount > 200) return false; // Impossible damage spike
    return true;
}

void AUTWeap_LinkGun_Plus::ServerProcessBeamHit_Implementation(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount)
{
    if (!UTOwner || !InstantHitInfo.IsValidIndex(1)) return;

    // 1. VALIDATION CHECK (Are they actually aiming near the target?)
    // We use a loose check because this is Plus (trusting the client mostly)
    FVector FireStart = GetFireStartLoc();
    float DistSq = FVector::DistSquared(FireStart, HitLocation);
    float MaxDist = InstantHitInfo[1].TraceRange + MaxHitDistanceTolerance;
    
    if (DistSq > FMath::Square(MaxDist))
    {
        // Hit is too far away, ignore it.
        return;
    }

    // 2. Update Link Gun State (For Pull Mechanics)
    // Standard Link Gun relies on 'CurrentLinkedTarget' to know who to pull
    CurrentLinkedTarget = HitActor;
    bLinkBeamImpacting = true;
    bLinkCausingDamage = true;
    
    // Reset the "Start Time" logic that UT uses for the pull delay
    if (GetWorld()->GetTimeSeconds() - LinkStartTime > 0.5f) // If we lost tracking for a bit
    {
        LinkStartTime = GetWorld()->GetTimeSeconds();
    }

    // 3. DEAL DAMAGE
    // We trust the amount calculated by the client (clamped logic could go here)
    FVector FireDir = (HitLocation - FireStart).GetSafeNormal();
    
    FInstantHitDamageInfo& DamageInfo = InstantHitInfo[1];
    
    HitActor->TakeDamage(DamageAmount, 
        FUTPointDamageEvent(DamageAmount, FHitResult(HitActor, nullptr, HitLocation, -FireDir), FireDir, DamageInfo.DamageType, FireDir * 1000.f), 
        UTOwner->Controller, 
        this);

    // 4. Update Stats
    AUTPlayerState* PS = UTOwner->Controller ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : nullptr;
    if (PS && HitsStatsName != NAME_None)
    {
        PS->ModifyStatsValue(HitsStatsName, DamageAmount);
    }
}