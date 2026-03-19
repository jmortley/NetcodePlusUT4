// UTPlusProj_Rocket.cpp
// Enhanced rocket with client-notify projectile rewind support

#include "UTPlusProj_Rocket.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"

AUTPlusProj_Rocket::AUTPlusProj_Rocket(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void AUTPlusProj_Rocket::ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
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
				Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 0); // FireMode 0 = primary (rockets)
			}
		}
	}

	// Standard processing: damage, explode, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
