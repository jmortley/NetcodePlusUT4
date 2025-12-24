
#include "UTPlusProj_ShockBall.h"
#include "UTPlusShockRifle.h"
#include "Particles/ParticleSystemComponent.h"


AUTPlusProj_ShockBall::AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FlightEffectVisual = nullptr;
	bVisualInitialized = false; // Start as false
	VisualInterpSpeed = 100.0f;
}

void AUTPlusProj_ShockBall::NotifyClientSideHit(AUTPlayerController* InstigatedBy, FVector HitLocation, AActor* DamageCauser, int32 Damage)
{
	// BARE MINIMUM VALIDATION
	// If the core is marked for deletion or has already exploded on the server, deny the combo.
	if (IsPendingKillPending() || bExploded)
	{
		return;
	}

	// If it's still alive, pass control back to Epic's standard logic
	Super::NotifyClientSideHit(InstigatedBy, HitLocation, DamageCauser, Damage);
}


void AUTPlusProj_ShockBall::PerformCombo(class AController* InstigatedBy, class AActor* DamageCauser)
{
	// Consume extra ammo for the combo
	if (Role == ROLE_Authority)
	{
		AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
		AUTWeapon* Weapon = Cast<AUTWeapon>(DamageCauser);
		if (Weapon && (!GameMode || GameMode->bAmmoIsLimited || (Weapon->Ammo > 9)))
		{
			Weapon->AddAmmo(-ComboAmmoCost);
		}

		// This gets called before server startfire(). bPlayComboEffects = true will send the FireExtra when fired
		AUTCharacter* UTC = (InstigatedBy != nullptr) ? Cast<AUTCharacter>(InstigatedBy->GetPawn()) : nullptr;

		// CHANGED: Look for UTPlusShockRifle instead of UTWeap_ShockRifle
		AUTPlusShockRifle* ShockRifle = (UTC != nullptr) ? Cast<AUTPlusShockRifle>(UTC->GetWeapon()) : nullptr;
		if (ShockRifle != nullptr)
		{
			ShockRifle->bPlayComboEffects = true;
		}
	}

	// The player who combos gets the credit
	InstigatorController = InstigatedBy;

	// Replicate combo and execute locally
	bComboExplosion = true;
	OnRep_ComboExplosion();
	Explode(GetActorLocation(), FVector(1.0f, 0.0f, 0.0f));
}





void AUTPlusProj_ShockBall::BeginPlay()
{
	Super::BeginPlay();


}



void AUTPlusProj_ShockBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}


// 1. SHIELD / SLOWMO FIX (Replicated Variable)
void AUTPlusProj_ShockBall::OnRep_Slomo()
{
	Super::OnRep_Slomo();

	if (GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
	{
		MyFakeProjectile->Slomo = Slomo;
		MyFakeProjectile->OnRep_Slomo();
	}
}



// 2. WALL / STOP FIX (Velocity Replication)
void AUTPlusProj_ShockBall::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	Super::PostNetReceiveVelocity(NewVelocity);

	if (GetNetMode() != NM_Client || bFakeClientProjectile || !MyFakeProjectile || MyFakeProjectile->IsPendingKillPending())
	{
		return;
	}

	const float StopThresh = 2.0f;

	// Server says "I stopped"
	if (NewVelocity.IsNearlyZero(StopThresh))
	{
		// Snap + stop fake
		MyFakeProjectile->SetActorLocation(GetActorLocation(), false, nullptr, ETeleportType::TeleportPhysics);

		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->StopMovementImmediately();
			MyFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
		return;
	}

	// Optional fail-safe: only if drift is huge (don't fight prediction)
	const float MaxDrift = 120.f;
	if (FVector::DistSquared(MyFakeProjectile->GetActorLocation(), GetActorLocation()) > FMath::Square(MaxDrift))
	{
		MyFakeProjectile->SetActorLocation(GetActorLocation(), false, nullptr, ETeleportType::TeleportPhysics);
		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->Velocity = NewVelocity;
			MyFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
	}
}

// 3. MID-AIR COLLISION / CANCELLATION FIX (Explosion Replication)
void AUTPlusProj_ShockBall::Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal, UPrimitiveComponent* HitComp)
{
	// CRITICAL FIX: If this is a combo, DO NOT touch the fake projectile.
	bool bIsCombo = bComboExplosion || (MyDamageType && MyDamageType->GetName().Contains(TEXT("Combo")));

	if (!bIsCombo && GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
	{
		MyFakeProjectile->SetActorLocation(HitLocation, false, nullptr, ETeleportType::TeleportPhysics);

		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->Velocity = FVector::ZeroVector;
		}
	}

	Super::Explode_Implementation(HitLocation, HitNormal, HitComp);
}
