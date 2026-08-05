// UTWeap_Enforcer_Plus.cpp
#include "UTWeap_Enforcer_Plus.h"
#include "UnrealTournament.h"
#include "UTWeaponFix.h"   // shared slide-posture validation rule (static)
#include "UTCharacter.h"
#include "UTCharacterMovement.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "Components/CapsuleComponent.h"

AUTWeap_Enforcer_Plus::AUTWeap_Enforcer_Plus(const FObjectInitializer& OI)
	: Super(OI)
{
	// Inherit all stock enforcer defaults; NetcodePlus tuning below.
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

	// ExactPing is round-trip. Subtract jitter buffer, halve for one-way, clamp.
	const float AdjustedRTTms = FMath::Max(0.f, PS->ExactPing - FudgeFactorMs);
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

	// 1) World geometry trace (no character collision). Same as stock.
	ECollisionChannel TraceChannel = COLLISION_TRACE_WEAPONNOCHARACTER;
	FCollisionQueryParams QueryParams(GetClass()->GetFName(), true, UTOwner);

	if (TraceRadius <= 0.f)
	{
		GetWorld()->LineTraceSingleByChannel(Hit, StartLocation, EndTrace, TraceChannel, QueryParams);
	}
	else
	{
		GetWorld()->SweepSingleByChannel(Hit, StartLocation, EndTrace, FQuat::Identity, TraceChannel, FCollisionShape::MakeSphere(TraceRadius), QueryParams);
	}

	if (!Hit.bBlockingHit)
	{
		Hit.Location = EndTrace;
	}

	// 2) Pawn iteration with rewind. Find the closest pawn hit that's also
	//    closer than the world-geometry hit (which set Hit.Location above).
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	AUTCharacter* BestTarget = nullptr;
	FVector BestPoint(0.f);
	FVector BestCapsulePoint(0.f);
	float BestCollisionRadius = 0.f;

	for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
	{
		AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
		if (!Target || Target == UTOwner)
		{
			continue;
		}
		if (Target->IsDead())
		{
			continue;
		}
		// Teammate check: skip if same team (unless bTeammatesBlockHitscan is set).
		if (!(bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target)))
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
		FVector ClosestPoint(0.f);
		FVector ClosestCapsulePoint = TargetLocation;

		if (CollisionRadius >= CollisionHeight)
		{
			// Short-stocky capsule — treat as sphere centered on TargetLocation.
			ClosestPoint = FMath::ClosestPointOnSegment(TargetLocation, StartLocation, Hit.Location);
			bHitTarget = ((ClosestPoint - TargetLocation).SizeSquared() < FMath::Square(CollisionHeight + TraceRadius + ExtraHitPadding));
		}
		else
		{
			// Tall capsule — segment-to-segment distance between trace and capsule axis.
			const FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
			FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
			bHitTarget = ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CollisionRadius + TraceRadius + ExtraHitPadding));
		}

		// Keep closest pawn hit.
		if (bHitTarget && (!BestTarget || (ClosestPoint - StartLocation).SizeSquared() < (BestPoint - StartLocation).SizeSquared()))
		{
			BestTarget = Target;
			BestPoint = ClosestPoint;
			BestCapsulePoint = ClosestCapsulePoint;
			BestCollisionRadius = CollisionRadius;
		}
	}

	// 3) If we found a pawn closer than any world hit, update Hit.
	if (BestTarget)
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
