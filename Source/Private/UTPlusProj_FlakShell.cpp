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

	// Standard processing: damage, explode, spawn shards, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
