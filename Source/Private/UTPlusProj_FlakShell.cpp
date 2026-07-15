// UTPlusProj_FlakShell.cpp
// Enhanced flak shell with client-notify projectile rewind support

#include "UTPlusProj_FlakShell.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "UTProjectileMovementComponent.h"
#include "HAL/IConsoleManager.h"

// Mirror of ncp.RocketServerFirstExplosionVisual for the flak alt-fire shell. 1=on (default),
// 0=stock silent shutdown. Server-only presentation; no replication/schema impact.
static TAutoConsoleVariable<int32> CVarFlakShellServerFirstExplosionVisual(
	TEXT("ncp.FlakShellServerFirstExplosionVisual"), 1,
	TEXT("Guarantee a paired flak shell fake plays the authoritative explosion when its server real resolves first, instead of vanishing mid-air. 1=on (default), 0=stock silent shutdown."),
	ECVF_Default);

AUTPlusProj_FlakShell::AUTPlusProj_FlakShell(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bForcingShutdownExplosion = false;
}

void AUTPlusProj_FlakShell::Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal,
	UPrimitiveComponent* HitComp)
{
	// A fake NEVER spawns gameplay shards (they would be client-only, with no server counterpart).
	// Route the fake through the BASE projectile explosion for the cosmetic effect only, skipping
	// AUTProj_FlakShell's authority shard-spawn. This also handles the server-first fake call below,
	// which re-enters here on the fake.
	if (bFakeClientProjectile)
	{
		AUTProjectile::Explode_Implementation(HitLocation, HitNormal, HitComp);
		return;
	}

	// Real (or listen-server host) resolved before its visible fake caught up: drive the fake to
	// play exactly one authoritative explosion at truth so it doesn't silently disappear.
	if (CVarFlakShellServerFirstExplosionVisual.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client
		&& MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending()
		&& !MyFakeProjectile->bExploded)
	{
		AUTProjectile* VisualFake = MyFakeProjectile;
		VisualFake->SetActorLocation(HitLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (VisualFake->ProjectileMovement)
		{
			VisualFake->ProjectileMovement->Velocity = FVector::ZeroVector;
			VisualFake->ProjectileMovement->UpdateComponentVelocity();
		}
		// Cosmetic only — fake explosion applies no client-side damage (DamageImpactedActor early-
		// outs for fakes) and the bFakeClientProjectile branch above suppresses shard spawning.
		VisualFake->Explode(HitLocation, HitNormal, HitComp);
	}

	Super::Explode_Implementation(HitLocation, HitNormal, HitComp);
}

void AUTPlusProj_FlakShell::ShutDown()
{
	// Final visual guarantee. Stock AUTProjectile::Explode suppresses the hidden real's effect
	// whenever MyFakeProjectile exists, then delegates only ShutDown() to that fake. If this is the
	// still-unexploded visible fake and its paired master already exploded, play exactly one cosmetic
	// explosion now instead of disappearing mid-air.
	if (CVarFlakShellServerFirstExplosionVisual.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && bFakeClientProjectile && !bExploded && !bForcingShutdownExplosion
		&& MasterProjectile != nullptr && !MasterProjectile->IsPendingKillPending()
		&& MasterProjectile->bExploded)
	{
		const FVector ExplosionLocation = MasterProjectile->GetActorLocation();
		FVector ImpactNormal = -GetVelocity().GetSafeNormal();
		if (ImpactNormal.IsNearlyZero())
		{
			ImpactNormal = FVector(0.f, 0.f, 1.f);
		}
		SetActorLocation(ExplosionLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (ProjectileMovement)
		{
			ProjectileMovement->Velocity = FVector::ZeroVector;
			ProjectileMovement->UpdateComponentVelocity();
		}
		bForcingShutdownExplosion = true;
		Explode(ExplosionLocation, ImpactNormal, nullptr);   // -> bFakeClientProjectile branch -> cosmetic only
		bForcingShutdownExplosion = false;
		return;
	}
	Super::ShutDown();
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
			// Only the SHOOTER's own client can route the claim (the Server RPC needs the weapon's
			// owning connection). A bot's / remote player's replicated shell also runs this here,
			// where GetWeapon() resolves a weapon we don't own → "No owning connection" RPC drop.
			if (OwnerChar && OwnerChar->IsLocallyControlled())
			{
				AUTWeaponFix* Weapon = Cast<AUTWeaponFix>(OwnerChar->GetWeapon());
				if (Weapon)
				{
					Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 1); // FireMode 1 = alt-fire (flak shell)
				}
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
