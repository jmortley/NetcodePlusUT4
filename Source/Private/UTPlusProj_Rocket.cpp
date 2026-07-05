// UTPlusProj_Rocket.cpp
// Enhanced rocket with client-notify projectile rewind support

#include "UTPlusProj_Rocket.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"

// Diagnostic category (Warning survives the Shipping dedicated server). Separates
// "server never fired" (no SPAWNED line) from "fired but whiffed" (SPAWNED + HIT
// non-pawn/none) from "hit but no damage" (HIT a pawn).
DEFINE_LOG_CATEGORY_STATIC(LogRocketDbg, Log, All);

AUTPlusProj_Rocket::AUTPlusProj_Rocket(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void AUTPlusProj_Rocket::BeginPlay()
{
	Super::BeginPlay();
	// DIAGNOSTIC: one line per SERVER-spawned rocket. Count these against your primary taps:
	// 0 lines = the server never fired (client-only "blanks"); N lines = the server IS firing,
	// so a no-reg is a whiff / rescue failure, not a blank.
	if (Role == ROLE_Authority)
	{
		AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
		UE_LOG(LogRocketDbg, Warning, TEXT("[RocketDbg] server rocket SPAWNED by %s at %s"),
			OwnerChar ? *OwnerChar->GetName() : TEXT("?"),
			*GetActorLocation().ToString());
	}
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
			// Only the SHOOTER's own client can route the claim — the Server RPC needs the
			// weapon's owning connection. This same ProcessHit also runs on our client for a
			// bot's / remote player's replicated rocket, where GetWeapon() resolves a weapon we
			// don't own → the engine drops the RPC with "No owning connection" spam. Gate to the
			// locally-controlled shooter so only routable claims are sent.
			if (OwnerChar && OwnerChar->IsLocallyControlled())
			{
				AUTWeaponFix* Weapon = Cast<AUTWeaponFix>(OwnerChar->GetWeapon());
				if (Weapon)
				{
					Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 0); // FireMode 0 = primary (rockets)
				}
			}
		}
	}

	// SERVER-SIDE: snapshot final state into the weapon's grace buffer BEFORE Super explodes/
	// destroys this projectile, so a claim arriving after the rocket is gone (close-range timing
	// race) can still rewind-rescue. The pawn we directly hit (or null = geometry/whiff) is passed
	// so the grace path won't double-damage a target that already took the present-time hit.
	if (Role == ROLE_Authority)
	{
		// DIAGNOSTIC: what the SERVER rocket actually hit present-time — a pawn (the target),
		// world geometry, or nothing. Pairs with the SPAWNED line in BeginPlay to separate
		// "blanks" (no spawn) from "whiff" (spawned, hit non-pawn/none) from "hit, no damage".
		UE_LOG(LogRocketDbg, Warning, TEXT("[RocketDbg] server rocket HIT: %s (%s) at %s"),
			OtherActor ? *OtherActor->GetName() : TEXT("none/whiff"),
			Cast<APawn>(OtherActor) ? TEXT("PAWN") : TEXT("non-pawn"),
			*HitLocation.ToString());

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
	// rocket detonating against a wall inflates RocketHits. Zero the credit for non-pawn impacts so
	// only player hits count. Pawn hits keep the default credit; the radial/splash path in Explode
	// resets StatsHitCredit itself, so this affects only the buggy direct-impact line.
	if (Role == ROLE_Authority && Cast<APawn>(OtherActor) == nullptr)
	{
		StatsHitCredit = 0.f;
	}

	// Standard processing: damage (server only), explode, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
