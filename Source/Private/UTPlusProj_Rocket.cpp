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
	// CLIENT-SIDE HIT: Notify weapon so server can validate with rewind.
	// This fires on the CLIENT when the replicated (real) rocket overlaps an enemy
	// on the client's local pawn positions. The server may disagree because its
	// capsule positions are different — the RPC gives it a second chance with rewind.
	// Role != ROLE_Authority means we're on the client viewing the replicated rocket.
	if (Role != ROLE_Authority && OtherActor && !bFakeClientProjectile)
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

	// Standard processing: damage (server only), explode, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
