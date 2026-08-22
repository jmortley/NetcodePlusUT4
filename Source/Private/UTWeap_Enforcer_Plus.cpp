// UTWeap_Enforcer_Plus.cpp
#include "UTWeap_Enforcer_Plus.h"
#include "UnrealTournament.h"
#include "UTWeaponFix.h"   // shared slide-posture validation rule (static)
#include "UTWeaponStateFiring_Enforcer.h"
#include "UTWeaponStateFiringBurstEnforcer.h"
#include "UTCharacter.h"
#include "UTCharacterMovement.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Components/CapsuleComponent.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "UObject/ConstructorHelpers.h"

AUTWeap_Enforcer_Plus::AUTWeap_Enforcer_Plus(const FObjectInitializer& OI)
	: Super(OI
		.SetDefaultSubobjectClass<UUTWeaponStateFiring_Enforcer>(TEXT("FiringState0"))
		.SetDefaultSubobjectClass<UUTWeaponStateFiringBurstEnforcer>(TEXT("FiringState1")))
{
	// The stock weapon's presentation is authored in Enforcer.uasset rather than
	// AUTWeap_Enforcer.  Supply the two SCS-only muzzle templates natively so a
	// direct Blueprint child never needs cross-class component references.
	Muzzle = OI.CreateDefaultSubobject<UParticleSystemComponent>(this, TEXT("Muzzle"));
	LeftMuzzle = OI.CreateDefaultSubobject<UParticleSystemComponent>(this, TEXT("LeftMuzzle"));

	static ConstructorHelpers::FObjectFinder<UParticleSystem> MuzzleTemplate(
		TEXT("ParticleSystem'/Game/RestrictedAssets/Weapons/Weapon_Effects/Particles/P_Enforcer_MF_01_1P.P_Enforcer_MF_01_1P'"));

	if (Muzzle != nullptr)
	{
		Muzzle->SetupAttachment(Mesh, FName(TEXT("MuzzleFlashSocket")));
		Muzzle->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
		Muzzle->SetAutoActivate(false);
		Muzzle->bReceivesDecals = false;
		Muzzle->CastShadow = false;
		if (MuzzleTemplate.Succeeded())
		{
			Muzzle->SetTemplate(MuzzleTemplate.Object);
		}
	}

	if (LeftMuzzle != nullptr)
	{
		LeftMuzzle->SetupAttachment(LeftMesh, FName(TEXT("MuzzleFlashSocket")));
		LeftMuzzle->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
		LeftMuzzle->SetRelativeScale3D(FVector(-1.f, 1.f, 1.f));
		LeftMuzzle->SetAutoActivate(false);
		LeftMuzzle->bReceivesDecals = false;
		LeftMuzzle->CastShadow = false;
		if (MuzzleTemplate.Succeeded())
		{
			LeftMuzzle->SetTemplate(MuzzleTemplate.Object);
		}
	}

	MuzzleFlash.Empty(4);
	MuzzleFlash.Add(Muzzle);
	MuzzleFlash.Add(Muzzle);
	MuzzleFlash.Add(LeftMuzzle);
	MuzzleFlash.Add(LeftMuzzle);

	// NetcodePlus tuning below.
	MaxRewindMs = 250.f;
	FudgeFactorMs = 20.f;
	MovingTargetPadding = 25.f;
	StationaryTargetPadding = 8.f;
}

float AUTWeap_Enforcer_Plus::GetRewindSeconds() const
{
	if (!UTOwner || !UTOwner->PlayerState)
	{
		return 0.f;
	}

	const AUTPlayerState* PS = Cast<AUTPlayerState>(UTOwner->PlayerState);
	if (!PS)
	{
		return 0.f;
	}

	const AUTPlayerController* ShooterPC =
		Cast<AUTPlayerController>(UTOwner->Controller);
	const bool bRemoteHuman =
		ShooterPC != nullptr && !ShooterPC->IsLocalController();
	float ObservedRTTMs = bRemoteHuman ? 0.f : PS->ExactPing;
	if (bRemoteHuman &&
		!AUTWeaponFix::GetServerObservedRTTMs(ShooterPC, ObservedRTTMs))
	{
		// Never fall back to client-writable ExactPing for a remote human.
		return 0.f;
	}

	// Full RTT minus jitter buffer, then halve for one-way rewind.
	const float AdjustedRTTms = FMath::Max(0.f, ObservedRTTMs - FudgeFactorMs);
	const float OneWayMs = FMath::Min(AdjustedRTTms * 0.5f, MaxRewindMs);
	return OneWayMs * 0.001f;
}

// =========================================================================
// FIRING STATE GUARD — prevent crash when fire RPC arrives after owner death
// =========================================================================
// Race: player dies, weapon teardown clears UTOwner via Removed()/DetachFromOwner(),
// but a replicated ServerUpdateFiringStates from the client was already in flight
// and arrives this frame. Stock AUTWeapon::ServerUpdateFiringStates_Implementation
// dereferences UTOwner without a null check (UTWeapon.cpp line ~576):
//     if ( FiringState[i] && (UTOwner->IsPendingFire(i) != bWantsFire) )
// → SIGSEGV.
//
// AUTWeaponFix has this same guard for the LinkGun_Plus / Minigun_Plus path.
// Enforcer_Plus inherits from stock AUTWeap_Enforcer (to keep AUTDualWeapon
// dual-wield infrastructure), so we reproduce the guard here.
void AUTWeap_Enforcer_Plus::ServerUpdateFiringStates_Implementation(uint8 FireSettings)
{
	if (!GetUTOwner() || GetUTOwner()->IsDead() || IsPendingKillPending())
	{
		return;
	}
	Super::ServerUpdateFiringStates_Implementation(FireSettings);
}

void AUTWeap_Enforcer_Plus::HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime)
{
	// Override caller's PredictionTime with our ping-based one. Mirrors
	// AUTWeaponFix::HitScanTrace semantics without touching transactional fire.
	const float ActualPredictionTime = GetRewindSeconds();

	// 1) World geometry trace (no character collision). Preserve stock's
	//    projectile-ignore behavior, but use an actually bounded retry loop.
	ECollisionChannel TraceChannel = COLLISION_TRACE_WEAPONNOCHARACTER;
	FCollisionQueryParams QueryParams(GetClass()->GetFName(), true, UTOwner);
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	int32 RemainingIgnoredHits = (TraceRadius <= 0.f) ? 3 : 2;

	while (true)
	{
		const bool bBlockingHit = (TraceRadius <= 0.f)
			? GetWorld()->LineTraceSingleByChannel(Hit, StartLocation, EndTrace, TraceChannel, QueryParams)
			: GetWorld()->SweepSingleByChannel(Hit, StartLocation, EndTrace, FQuat::Identity, TraceChannel, FCollisionShape::MakeSphere(TraceRadius), QueryParams);

		if (!bBlockingHit)
		{
			Hit.Location = EndTrace;
			break;
		}

		if (TraceRadius > 0.f)
		{
			// Keep the reported impact point on the target surface for swept traces.
			Hit.Location += (EndTrace - StartLocation).GetSafeNormal() * TraceRadius;
		}

		AUTCharacter* BlockingCharacter = Cast<AUTCharacter>(Hit.GetActor());
		const bool bNonLiveCharacterHit = BlockingCharacter != nullptr &&
			!AUTWeaponFix::IsLiveHitscanTarget(BlockingCharacter);
		if (RemainingIgnoredHits > 0 && Hit.Actor.IsValid() &&
			(ShouldTraceIgnore(Hit.GetActor()) || bNonLiveCharacterHit))
		{
			QueryParams.AddIgnoredActor(Hit.GetActor());
			--RemainingIgnoredHits;
			continue;
		}
		if (bNonLiveCharacterHit)
		{
			// Exhausting the bounded ignore budget must not hand a corpse back to
			// stock FireInstantHit, whose final gate would award accuracy before
			// the dead character's TakeDamage path discards the event. Keep the
			// blocking location, but make the endpoint non-damageable.
			Hit.Actor = nullptr;
			Hit.Component = nullptr;
		}

		break;
	}

	// 2) Pawn iteration with rewind. Find the closest pawn hit that's also
	//    closer than the world-geometry hit (which set Hit.Location above).
	AUTCharacter* BestTarget = nullptr;
	FVector BestPoint(0.f);
	FVector BestCapsulePoint(0.f);
	float BestCollisionRadius = 0.f;

	for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
	{
		AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
		if (Target == UTOwner || !AUTWeaponFix::IsLiveHitscanTarget(Target))
		{
			continue;
		}
		// Match stock: team projectiles enabled means teammates also block hitscan.
		if (!(bTeammatesBlockHitscan || !GS || GS->bTeamProjHits || !GS->OnSameTeam(UTOwner, Target)))
		{
			continue;
		}

		UCapsuleComponent* Capsule = Target->GetCapsuleComponent();
		if (!Capsule)
		{
			continue;
		}

		// Rewind target to its position at fire time (server only; client uses current).
		FVector TargetLocation = (ActualPredictionTime > 0.f && Role == ROLE_Authority)
			? Target->GetRewindLocation(ActualPredictionTime)
			: Target->GetActorLocation();

		float CollisionHeight = Capsule->GetScaledCapsuleHalfHeight();
		float CollisionRadius = Capsule->GetScaledCapsuleRadius();

		// Floor-slide posture: standing envelope inside the ncp.SlideGraceMs window
		// after slide start, classic slide shrink after (shared validation rule —
		// see AUTWeaponFix::ApplySlidePostureForValidation).
		// Qualified: this class extends AUTWeap_Enforcer, not AUTWeaponFix.
		AUTWeaponFix::ApplySlidePostureForValidation(Target,
			(ActualPredictionTime > 0.f && Role == ROLE_Authority) ? ActualPredictionTime : 0.f,
			TargetLocation, CollisionHeight);

		// Padding based on target movement state.
		const bool bIsMoving = !Target->GetVelocity().IsNearlyZero(1.0f);
		const float ExtraHitPadding = bIsMoving ? MovingTargetPadding : StationaryTargetPadding;

		// Distance between trace and capsule.
		bool bHitTarget = false;
		bool bNeedsPaddingLOS = false;
		FVector ClosestPoint(0.f);
		FVector ClosestCapsulePoint = TargetLocation;
		float CapsuleSurfaceRadius = CollisionRadius;

		if (CollisionRadius >= CollisionHeight)
		{
			CapsuleSurfaceRadius = CollisionHeight;
			// Short-stocky capsule — treat as sphere centered on TargetLocation.
			ClosestPoint = FMath::ClosestPointOnSegment(TargetLocation, StartLocation, Hit.Location);
			bHitTarget = ((ClosestPoint - TargetLocation).SizeSquared() < FMath::Square(CapsuleSurfaceRadius + TraceRadius));
			if (!bHitTarget && ExtraHitPadding > 0.f)
			{
				bHitTarget = ((ClosestPoint - TargetLocation).SizeSquared() < FMath::Square(CapsuleSurfaceRadius + TraceRadius + ExtraHitPadding));
				bNeedsPaddingLOS = bHitTarget;
			}
		}
		else
		{
			// Tall capsule — segment-to-segment distance between trace and capsule axis.
			const FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
			FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
			bHitTarget = ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CapsuleSurfaceRadius + TraceRadius));
			if (!bHitTarget && ExtraHitPadding > 0.f)
			{
				bHitTarget = ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CapsuleSurfaceRadius + TraceRadius + ExtraHitPadding));
				bNeedsPaddingLOS = bHitTarget;
			}
		}

		// Padding-only hits still need clear line of sight to the real capsule.
		if (bNeedsPaddingLOS)
		{
			const FVector CapsuleToTrace = (ClosestPoint - ClosestCapsulePoint).GetSafeNormal();
			const FVector PointToCheck = ClosestCapsulePoint + CapsuleSurfaceRadius * CapsuleToTrace;
			FCollisionQueryParams PaddingQueryParams = QueryParams;
			int32 RemainingPaddingIgnores = 3;
			bHitTarget = false;

			while (true)
			{
				FHitResult CheckHit;
				if (!GetWorld()->LineTraceSingleByChannel(CheckHit, StartLocation, PointToCheck, TraceChannel, PaddingQueryParams))
				{
					bHitTarget = true;
					break;
				}

				if (RemainingPaddingIgnores > 0 && CheckHit.Actor.IsValid() && ShouldTraceIgnore(CheckHit.GetActor()))
				{
					PaddingQueryParams.AddIgnoredActor(CheckHit.GetActor());
					--RemainingPaddingIgnores;
					continue;
				}

				break;
			}
		}

		// Keep closest pawn hit.
		if (bHitTarget && (!BestTarget || (ClosestPoint - StartLocation).SizeSquared() < (BestPoint - StartLocation).SizeSquared()))
		{
			BestTarget = Target;
			BestPoint = ClosestPoint;
			BestCapsulePoint = ClosestCapsulePoint;
			BestCollisionRadius = CapsuleSurfaceRadius;
		}
	}

	// 3) If we found a pawn closer than any world hit, update Hit.
	if (AUTWeaponFix::IsLiveHitscanTarget(BestTarget))
	{
		const float ClosestDistSq = (BestPoint - BestCapsulePoint).SizeSquared();
		const float BackDist = FMath::Sqrt(FMath::Max(0.f, BestCollisionRadius * BestCollisionRadius - ClosestDistSq));

		Hit.Location = BestPoint + BackDist * (StartLocation - EndTrace).GetSafeNormal();
		Hit.Normal = (Hit.Location - BestCapsulePoint).GetSafeNormal();
		Hit.ImpactNormal = Hit.Normal;
		Hit.Actor = BestTarget;
		Hit.bBlockingHit = true;
		Hit.Component = BestTarget->GetCapsuleComponent();
		Hit.ImpactPoint = BestPoint;
		Hit.Time = (BestPoint - StartLocation).Size() / FMath::Max(KINDA_SMALL_NUMBER, (EndTrace - StartLocation).Size());
	}
}
