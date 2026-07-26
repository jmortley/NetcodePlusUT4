// UTPlusProj_StingerShard.cpp
// Minigun alt-fire shard wired into AUTWeaponFix client-notify projectile rewind.

#include "UTPlusProj_StingerShard.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "UTProjectileMovementComponent.h"
#include "UnrealTournament.h"   // COLLISION_TRACE_WEAPON

AUTPlusProj_StingerShard::AUTPlusProj_StingerShard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSentShardHitClaim = false;
}

void AUTPlusProj_StingerShard::TrySendShardHitClaim(AUTCharacter* HitChar, const FVector& HitLocation)
{
	// Only the SHOOTER's own client can route the claim: the Server RPC needs the
	// weapon's owning connection, so a bot's / remote player's replicated shard
	// would drop it ("No owning connection"). Also require the REAL replicated
	// shard, never the cosmetic client fake (matches rocket/flak). One claim per
	// projectile — the server consumes a single tracking entry and applies the
	// shard's full damage once. NotifyFakeProjectileHit self-gates on the weapon's
	// bEnableProjectileRewind, so nothing is sent when the feature is off.
	if (bSentShardHitClaim || Role == ROLE_Authority || bFakeClientProjectile || HitChar == nullptr)
	{
		return;
	}

	AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
	if (OwnerChar == nullptr || !OwnerChar->IsLocallyControlled())
	{
		return;
	}

	AUTWeaponFix* Weapon = Cast<AUTWeaponFix>(OwnerChar->GetWeapon());
	if (Weapon != nullptr)
	{
		bSentShardHitClaim = true;
		Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 1); // FireMode 1 = minigun alt (shard)
	}
}

void AUTPlusProj_StingerShard::ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
	const FVector& HitLocation, const FVector& HitNormal)
{
	// DIRECT impact on an enemy (point-blank, or before the growing sphere
	// air-explodes). Route a claim from the client's replicated real shard.
	// Deliberately NO OnTrackedProjectileResolved (unlike rocket/flak): the shard
	// deals RADIAL damage, which has no single present-time victim for the grace
	// buffer's double-damage guard, so the shard uses LIVE-projectile rescue only
	// (see class comment). Live rescue consumes the tracked shard, so there is no
	// present-time double-hit.
	TrySendShardHitClaim(Cast<AUTCharacter>(OtherActor), HitLocation);

	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}

void AUTPlusProj_StingerShard::Tick(float DeltaTime)
{
	// PROXIMITY air-explode is the shard's primary kill and never reaches
	// ProcessHit, so mirror AUTProj_StingerShard2::Tick's detonation predicate
	// here to route the claim for the target being air-exploded an instant before
	// Super::Tick detonates. Cheap: PotentialTargets is empty unless an enemy is
	// inside the (growing) overlap sphere, and it is already team/instigator
	// filtered by the parent's OnPawnSphereOverlapBegin, so these are enemies.
	if (!bSentShardHitClaim && Role != ROLE_Authority && !bFakeClientProjectile
		&& PotentialTargets.Num() > 0 && ProjectileMovement != nullptr
		&& !ProjectileMovement->Velocity.IsZero())
	{
		AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
		if (OwnerChar != nullptr && OwnerChar->IsLocallyControlled())
		{
			const FVector MovementDir = ProjectileMovement->Velocity.GetUnsafeNormal();
			const FVector MyLoc = GetActorLocation();
			for (APawn* P : PotentialTargets)
			{
				if (P == nullptr || P->bTearOff)
				{
					continue;
				}
				// Identical predicate to the parent's air-explode: velocity aligned
				// to the target (closest approach) AND a clear line of sight.
				const bool bAligned =
					((P->GetActorLocation() - MyLoc).GetSafeNormal() | MovementDir) <= AirExplodeAngle;
				if (bAligned
					&& !GetWorld()->LineTraceTestByChannel(MyLoc, P->GetActorLocation(),
						COLLISION_TRACE_WEAPON, FCollisionQueryParams(NAME_None, true, P)))
				{
					TrySendShardHitClaim(Cast<AUTCharacter>(P), P->GetActorLocation());
					break;
				}
			}
		}
	}

	Super::Tick(DeltaTime);
}
