// UTPlusProj_FlakShell.cpp
// Enhanced flak shell with client-notify projectile rewind support

#include "UTPlusProj_FlakShell.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"

AUTPlusProj_FlakShell::AUTPlusProj_FlakShell(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void AUTPlusProj_FlakShell::ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
	const FVector& HitLocation, const FVector& HitNormal)
{
	// CLIENT-SIDE HIT: Notify weapon so server can validate with rewind.
	// This fires on the CLIENT when the replicated (real) flak shell overlaps an enemy
	// on the client's local pawn positions. The server may disagree because its
	// capsule positions are different — the RPC gives it a second chance with rewind.
	if (Role != ROLE_Authority && OtherActor && !bFakeClientProjectile)
	{
		AUTCharacter* HitChar = Cast<AUTCharacter>(OtherActor);
		if (HitChar)
		{
			AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
			AUTWeaponFix* Weapon = OwnerChar ? Cast<AUTWeaponFix>(OwnerChar->GetWeapon()) : nullptr;
			if (Weapon)
			{
				Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 1); // FireMode 1 = alt-fire (flak shell)
			}
		}
	}

	// SERVER-SIDE: snapshot final state into the weapon's grace buffer BEFORE Super explodes/
	// destroys this shell, so a claim arriving after the shell is gone (close-range timing race)
	// can still rewind-rescue. The pawn we directly hit (or null = geometry/whiff) is passed so
	// the grace path won't double-damage a target that already took the present-time hit.
	if (Role == ROLE_Authority)
	{
		AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
		AUTWeaponFix* Weapon = OwnerChar ? Cast<AUTWeaponFix>(OwnerChar->GetWeapon()) : nullptr;
		if (Weapon)
		{
			Weapon->OnTrackedProjectileResolved(this, Cast<AUTCharacter>(OtherActor));
		}
	}

	// ACCURACY FIX (server-authoritative): a direct impact on world geometry (a static-mesh actor;
	// BSP reports a null OtherActor and is already exempt) credits a full accuracy hit in
	// AUTProjectile::DamageImpactedActor — StatsHitCredit defaults to 1.0 with no pawn check, so a
	// flak shell detonating against a wall inflates FlakHits. Zero the credit for non-pawn impacts so
	// only player hits count. Pawn hits keep the default credit; the radial/splash path in Explode
	// resets StatsHitCredit itself, so this affects only the buggy direct-impact line. NOTE: the
	// flak-PRIMARY shards (stock AUTProj_FlakShard) are a separate, unsubclassed projectile and are
	// not covered by this fix.
	if (Role == ROLE_Authority && Cast<APawn>(OtherActor) == nullptr)
	{
		StatsHitCredit = 0.f;
	}

	// Standard processing: damage, explode, spawn shards, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
