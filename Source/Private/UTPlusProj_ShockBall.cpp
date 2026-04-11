
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


bool AUTPlusProj_ShockBall::ShouldIgnoreHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp)
{
	// Fake projectiles should NEVER process hits locally — the server handles
	// all collision. Without this, the fake shock core hits a wall on the client,
	// stops/embeds without exploding (fakes don't have authority to explode),
	// and remains alive + combo-able. The combo RPC reaches the server where
	// the real projectile may still be in flight (due to latency), giving a
	// free wall-combo that shouldn't exist.
	if (bFakeClientProjectile)
	{
		return true;
	}
	return Super::ShouldIgnoreHit_Implementation(OtherActor, OtherComp);
}

void AUTPlusProj_ShockBall::SetOriginalFireDirection(const FVector& Dir)
{
	OriginalFireDirection = Dir;
	bHasCachedFireDirection = true;
}

void AUTPlusProj_ShockBall::BeginPlay()
{
	Super::BeginPlay();

	// Cache the original fire direction for drift correction at high fps
	bHasCachedFireDirection = false;
	StuckTime = 0.f;
	if (ProjectileMovement && !ProjectileMovement->Velocity.IsNearlyZero())
	{
		OriginalFireDirection = ProjectileMovement->Velocity.GetSafeNormal();
		bHasCachedFireDirection = true;
	}

	if (Role == ROLE_Authority)
	{
		// Server: Fixed 240Hz
		PrimaryActorTick.TickInterval = 1.f / 240.f;
		if (ProjectileMovement) ProjectileMovement->PrimaryComponentTick.TickInterval = 1.f / 240.f;
	}
	else if (GetNetMode() != NM_DedicatedServer)
	{
		// CLIENT: Ask the Weapon Class for the rate
		int32 TargetHz = AUTWeaponFix::GetTargetProjectileTickRate();

		float ClientInterval = 1.f / static_cast<float>(TargetHz);

		PrimaryActorTick.TickInterval = ClientInterval;
		if (ProjectileMovement)
		{
			ProjectileMovement->PrimaryComponentTick.TickInterval = ClientInterval;
			// Force update immediately
			ProjectileMovement->SetComponentTickInterval(ClientInterval);
		}
	}
}



void AUTPlusProj_ShockBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// High-FPS drift correction: at 480+ fps, floating point accumulation in
	// ProjectileMovementComponent causes the velocity direction to drift slightly
	// per tick. Over 1000+ ticks the shock ball visibly curves upward/sideways.
	// Fix: snap velocity back to original fire direction, preserving speed.
	// Only applies to zero-gravity projectiles (shock balls have no arc).
	if (bHasCachedFireDirection && ProjectileMovement
		&& !ProjectileMovement->Velocity.IsNearlyZero()
		&& FMath::IsNearlyZero(ProjectileMovement->ProjectileGravityScale))
	{
		float Speed = ProjectileMovement->Velocity.Size();
		FVector CurrentDir = ProjectileMovement->Velocity / Speed;
		// Only correct if drift is small (< 1 degree) — larger changes are intentional (bounces, etc.)
		if ((CurrentDir | OriginalFireDirection) > 0.9998f) // ~1 degree
		{
			ProjectileMovement->Velocity = OriginalFireDirection * Speed;
		}
	}

	// Stuck-ball detection: if the server-side projectile has near-zero velocity
	// and is embedded in world geometry, force-explode. This catches edge cases
	// where the movement component bleeds velocity at a shallow angle without
	// triggering a clean OnStop → ProcessHit → Explode chain.
	if (Role == ROLE_Authority && ProjectileMovement
		&& ProjectileMovement->Velocity.IsNearlyZero(5.0f))
	{
		StuckTime += DeltaTime;
		if (StuckTime >= StuckExplodeDelay && CollisionComp)
		{
			// Zero-length sweep at current location — returns true only if inside
			// static world geometry (BSP, static meshes), not actors like bio globs.
			FHitResult Hit;
			bool bBlocked = GetWorld()->SweepSingleByChannel(Hit, GetActorLocation(), GetActorLocation(),
				FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(1.f),
				FCollisionQueryParams(TEXT("StuckCheck"), false, this));

			if (bBlocked)
			{
				Explode(GetActorLocation(), Hit.ImpactNormal.IsNearlyZero() ? FVector(0.f, 0.f, 1.f) : Hit.ImpactNormal);
				return;
			}
		}
	}
	else
	{
		StuckTime = 0.f;
	}

	// Stuck-ball handoff: when the real stops (bio goo, wall) but the fake is
	// still the rendering authority (bMoveFakeToReplicatedPos = false), the
	// fake's flight particle stops emitting → invisible to the shooter.
	// Detect the stop and hand rendering back to the real so the shooter
	// sees the same "stuck ball" visual as everyone else.
	if (GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile
		&& !MyFakeProjectile->IsPendingKillPending()
		&& ProjectileMovement && ProjectileMovement->Velocity.IsNearlyZero(2.0f))
	{
		UE_LOG(LogTemp, Verbose, TEXT("[ShockBall] HANDOFF: Real stopped, unhiding real and destroying fake"));
		SetActorHiddenInGame(false);
		// BeginFakeProjectileSynch also set every USceneComponent visibility to false —
		// SetActorHiddenInGame alone doesn't undo that.
		TArray<USceneComponent*> Components;
		GetComponents<USceneComponent>(Components);
		for (int32 i = 0; i < Components.Num(); i++)
		{
			Components[i]->SetVisibility(true);
		}
		MyFakeProjectile->Destroy();
		MyFakeProjectile = nullptr;
	}
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

bool AUTPlusProj_ShockBall::CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const
{
	// Relaxed direction check for shock balls. The stock 0.95 dot product threshold
	// is too strict when the cached transactional rotation differs slightly from the
	// server's (sub-frame mouse jitter, network quantization). At 2415 u/s with
	// typically one core in flight, 0.5 (~60 degrees) prevents double-core visuals
	// while still rejecting obviously wrong matches.
	return (InFakeProjectile->GetVelocity().GetSafeNormal() | VelDir) > 0.5f;
}
