// UTPlusProj_FlakShell.cpp
// Enhanced flak shell with client-notify projectile rewind support

#include "UTPlusProj_FlakShell.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "UTProjectileMovementComponent.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFlakShellPair, Log, All);

static TAutoConsoleVariable<int32> CVarFlakShellPairDebug(
	TEXT("ncp.FlakShellPairDebug"), 0,
	TEXT("Flak-shell fake matching diagnostics. 0=off (default), 1=log every accepted/rejected candidate."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFlakShellMatchFakeMaxDist(
	TEXT("ncp.FlakShellMatchFakeMaxDist"), 1250.f,
	TEXT("Maximum real/fake Flak-shell separation considered for pairing (uu; clamped 100..5000)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFlakShellMatchFakeMaxPhase(
	TEXT("ncp.FlakShellMatchFakeMaxPhase"), 0.60f,
	TEXT("Maximum inferred ballistic phase separation considered for Flak-shell pairing (seconds; clamped 0.05..1.5)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFlakShellMatchFakeMinHorizDot(
	TEXT("ncp.FlakShellMatchFakeMinHorizDot"), 0.98f,
	TEXT("Minimum horizontal velocity-direction dot for Flak-shell pairing (clamped -1..1)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFlakShellMatchFakeMaxPosError(
	TEXT("ncp.FlakShellMatchFakeMaxPosError"), 256.f,
	TEXT("Maximum position residual after ballistic phase compensation (uu; clamped 16..2000)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarFlakShellMatchFakeMaxVelError(
	TEXT("ncp.FlakShellMatchFakeMaxVelError"), 200.f,
	TEXT("Maximum velocity residual after ballistic phase compensation (uu/s; clamped 16..2000)."),
	ECVF_Default);

static void AdvanceFlakShellBallisticState(const UProjectileMovementComponent* Movement,
	float DeltaTime, FVector& InOutLocation, FVector& InOutVelocity)
{
	// ComputeVelocity is public in UE4.15; ComputeMoveDelta is protected. This mirrors the
	// movement component's trapezoidal integration without advancing an actor or touching state.
	float Remaining = FMath::Max(0.f, DeltaTime);
	while (Remaining > KINDA_SMALL_NUMBER)
	{
		const float Step = FMath::Min(Remaining, 1.f / 120.f);
		const FVector NewVelocity = Movement->ComputeVelocity(InOutVelocity, Step);
		InOutLocation += (InOutVelocity + NewVelocity) * (0.5f * Step);
		InOutVelocity = NewVelocity;
		Remaining -= Step;
	}
}

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

bool AUTPlusProj_FlakShell::CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const
{
	(void)VelDir; // Stock's gravity path ignores this entirely; the gates below use full velocity.

	if (InFakeProjectile == nullptr)
	{
		if (CVarFlakShellPairDebug.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogFlakShellPair, Warning, TEXT("[FlakShellPair] reject real=%s fake=null reason=null-candidate"), *GetName());
		}
		return false;
	}

	const bool bExactClass = InFakeProjectile->GetClass() == GetClass();
	const bool bSameWorld = InFakeProjectile->GetWorld() == GetWorld();
	const bool bLiveReal = !IsPendingKillPending() && !bExploded && !bFakeClientProjectile
		&& bHasSpawnedFully && MyFakeProjectile == nullptr;
	const bool bLiveFake = !InFakeProjectile->IsPendingKillPending()
		&& InFakeProjectile->bFakeClientProjectile && InFakeProjectile->bHasSpawnedFully
		&& !InFakeProjectile->bExploded && InFakeProjectile->MasterProjectile == nullptr;
	const bool bSameInstigator = Instigator != nullptr
		&& InFakeProjectile->Instigator != nullptr
		&& InFakeProjectile->Instigator == Instigator;
	const bool bHasMovement = ProjectileMovement != nullptr
		&& InFakeProjectile->ProjectileMovement != nullptr;

	const FVector DeltaLocation = InFakeProjectile->GetActorLocation() - GetActorLocation();
	const float Distance = DeltaLocation.Size();
	const float MaxDistance = FMath::Clamp(CVarFlakShellMatchFakeMaxDist.GetValueOnGameThread(), 100.f, 5000.f);
	const bool bDistanceOK = Distance <= MaxDistance;

	float HorizontalDot = -1.f;
	float PhaseTime = BIG_NUMBER;
	float PositionError = BIG_NUMBER;
	float VelocityError = BIG_NUMBER;
	bool bDirectionOK = false;
	bool bPhaseOK = false;
	bool bTrajectoryOK = false;

	if (bExactClass && bSameWorld && bLiveReal && bLiveFake && bSameInstigator
		&& bHasMovement && bDistanceOK)
	{
		const FVector RealVelocity = ProjectileMovement->Velocity;
		const FVector FakeVelocity = InFakeProjectile->ProjectileMovement->Velocity;
		const FVector RealHorizontal(RealVelocity.X, RealVelocity.Y, 0.f);
		const FVector FakeHorizontal(FakeVelocity.X, FakeVelocity.Y, 0.f);
		const float RealHorizontalSpeed = RealHorizontal.Size();
		const float FakeHorizontalSpeed = FakeHorizontal.Size();
		const float DirectionThreshold = FMath::Clamp(
			CVarFlakShellMatchFakeMinHorizDot.GetValueOnGameThread(), -1.f, 1.f);

		if (RealHorizontalSpeed >= 10.f && FakeHorizontalSpeed >= 10.f)
		{
			HorizontalDot = RealHorizontal.GetSafeNormal() | FakeHorizontal.GetSafeNormal();
			bDirectionOK = HorizontalDot >= DirectionThreshold;
		}
		else
		{
			// A nearly vertical shot has no stable horizontal direction. Same-instigator,
			// bounded distance, phase, and the full ballistic residuals remain mandatory.
			HorizontalDot = 1.f;
			bDirectionOK = true;
		}

		const FVector AverageVelocity = (RealVelocity + FakeVelocity) * 0.5f;
		if (AverageVelocity.SizeSquared() > 100.f)
		{
			// For constant acceleration, displacement over a phase interval is exactly
			// average velocity * dt. Projecting also tolerates small replication error.
			PhaseTime = (DeltaLocation | AverageVelocity) / AverageVelocity.SizeSquared();
		}
		else
		{
			const FVector Acceleration(0.f, 0.f, ProjectileMovement->GetGravityZ());
			if (Acceleration.SizeSquared() > KINDA_SMALL_NUMBER)
			{
				PhaseTime = ((FakeVelocity - RealVelocity) | Acceleration) / Acceleration.SizeSquared();
			}
			else
			{
				PhaseTime = 0.f;
			}
		}

		const float MaxPhase = FMath::Clamp(CVarFlakShellMatchFakeMaxPhase.GetValueOnGameThread(), 0.05f, 1.5f);
		bPhaseOK = FMath::Abs(PhaseTime) <= MaxPhase;
		if (bDirectionOK && bPhaseOK)
		{
			FVector ProjectedLocation;
			FVector ProjectedVelocity;
			FVector TargetLocation;
			FVector TargetVelocity;
			const UProjectileMovementComponent* ProjectedMovement;

			if (PhaseTime >= 0.f)
			{
				ProjectedLocation = GetActorLocation();
				ProjectedVelocity = RealVelocity;
				TargetLocation = InFakeProjectile->GetActorLocation();
				TargetVelocity = FakeVelocity;
				ProjectedMovement = ProjectileMovement;
			}
			else
			{
				ProjectedLocation = InFakeProjectile->GetActorLocation();
				ProjectedVelocity = FakeVelocity;
				TargetLocation = GetActorLocation();
				TargetVelocity = RealVelocity;
				ProjectedMovement = InFakeProjectile->ProjectileMovement;
			}

			AdvanceFlakShellBallisticState(ProjectedMovement, FMath::Abs(PhaseTime),
				ProjectedLocation, ProjectedVelocity);
			PositionError = FVector::Dist(ProjectedLocation, TargetLocation);
			VelocityError = FVector::Dist(ProjectedVelocity, TargetVelocity);

			const float MaxPositionError = FMath::Clamp(
				CVarFlakShellMatchFakeMaxPosError.GetValueOnGameThread(), 16.f, 2000.f);
			const float MaxVelocityError = FMath::Clamp(
				CVarFlakShellMatchFakeMaxVelError.GetValueOnGameThread(), 16.f, 2000.f);
			bTrajectoryOK = PositionError <= MaxPositionError && VelocityError <= MaxVelocityError;
		}
	}

	const bool bMatch = bExactClass && bSameWorld && bLiveReal && bLiveFake
		&& bSameInstigator && bHasMovement && bDistanceOK && bDirectionOK && bPhaseOK && bTrajectoryOK;
	if (CVarFlakShellPairDebug.GetValueOnGameThread() > 0)
	{
		UE_LOG(LogFlakShellPair, Warning,
			TEXT("[FlakShellPair] %s real=%s fake=%s class=%d world=%d liveReal=%d liveFake=%d inst=%d move=%d dist=%.1f/%.1f hdot=%.4f dir=%d phase=%.4f phaseOK=%d posErr=%.1f velErr=%.1f traj=%d realInst=%s fakeInst=%s fakeAge=%.3f"),
			bMatch ? TEXT("ACCEPT") : TEXT("reject"), *GetName(), *InFakeProjectile->GetName(),
			bExactClass ? 1 : 0, bSameWorld ? 1 : 0, bLiveReal ? 1 : 0, bLiveFake ? 1 : 0,
			bSameInstigator ? 1 : 0, bHasMovement ? 1 : 0, Distance, MaxDistance,
			HorizontalDot, bDirectionOK ? 1 : 0, PhaseTime, bPhaseOK ? 1 : 0,
			PositionError, VelocityError, bTrajectoryOK ? 1 : 0,
			Instigator ? *Instigator->GetName() : TEXT("null"),
			InFakeProjectile->Instigator ? *InFakeProjectile->Instigator->GetName() : TEXT("null"),
			GetWorld() ? GetWorld()->GetTimeSeconds() - InFakeProjectile->CreationTime : -1.f);
	}

	return bMatch;
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
				// The flak cannon that FIRED this shell, not whatever is held now: a mid-flight
				// weapon switch would otherwise route the claim to a weapon that never tracked it.
				AUTWeaponFix* Weapon = AUTWeaponFix::FindFiringWeaponForProjectile(OwnerChar, this);
				if (Weapon)
				{
					Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 1, this); // FireMode 1 = alt-fire (flak shell)
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
		// Resolve the cannon that fired this shell, not the weapon held when it explodes.
		// The tracked grace entry lives on the firing weapon, matching the client claim route.
		AUTWeaponFix* Weapon = AUTWeaponFix::FindFiringWeaponForProjectile(OwnerChar, this);
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
