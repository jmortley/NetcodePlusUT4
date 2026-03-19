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
	// CLIENT FAKE PROJECTILE: Notify weapon that we hit an enemy
	// This runs BEFORE Super, which will Explode the fake (visual only, no damage).
	// The weapon sends an RPC so the server can validate with rewind.
	if (bFakeClientProjectile && OtherActor)
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
