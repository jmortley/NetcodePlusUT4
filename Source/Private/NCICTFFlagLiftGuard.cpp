// NCICTFFlagLiftGuard.cpp - see the header for behavior and tick ordering.
#include "NCICTFFlagLiftGuard.h"

#include "UnrealTournament.h"
#include "UTCarriedObject.h"
#include "UTCTFFlagBase.h"
#include "UTCTFGameState.h"
#include "UTFlag.h"
#include "UTLift.h"
#include "Components/ActorComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCICTFFlagLiftGuard, Log, All);

namespace
{
	static TAutoConsoleVariable<int32> CVarICTFFlagLiftGuard(
		TEXT("ncp.ICTFFlagLiftGuard"), 1,
		TEXT("Server-side iCTF dropped-flag lift protection. 1 = sweep and safely relocate flags riding lifts (default); 0 = restore stock Epic lift behavior."));

	const float ActorRefreshInterval = 0.5f;
	const float ClearancePad = 3.f;
	const float SidePad = 24.f;
	const float EjectTraceDepth = 5000.f;
	const float ThreatProximityPad = 256.f;

	AUTLift* GetAttachedLift(const AUTFlag* Flag)
	{
		if (Flag == nullptr || Flag->GetRootComponent() == nullptr)
		{
			return nullptr;
		}
		USceneComponent* Parent = Flag->GetRootComponent()->GetAttachParent();
		return Parent != nullptr ? Cast<AUTLift>(Parent->GetOwner()) : nullptr;
	}
}

ANCICTFFlagLiftGuard::ANCICTFFlagLiftGuard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	bReplicates = false;
}

ANCICTFFlagLiftGuard* ANCICTFFlagLiftGuard::EnsureSpawned(UWorld* World)
{
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	for (TActorIterator<ANCICTFFlagLiftGuard> It(World); It; ++It)
	{
		return *It;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<ANCICTFFlagLiftGuard>(SpawnParams);
}

void ANCICTFFlagLiftGuard::BeginPlay()
{
	Super::BeginPlay();
	if (Role == ROLE_Authority && CVarICTFFlagLiftGuard.GetValueOnGameThread() != 0)
	{
		ActivateGuard();
	}
}

void ANCICTFFlagLiftGuard::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DeactivateGuard();
	Super::EndPlay(EndPlayReason);
}

void ANCICTFFlagLiftGuard::ActivateGuard()
{
	if (bGuardActive || Role != ROLE_Authority)
	{
		return;
	}

	bGuardActive = true;
	RefreshAccumulator = 0.f;
	RefreshActors();
	UE_LOG(LogNCICTFFlagLiftGuard, Log,
		TEXT("[ICTFFlagLiftGuard] enabled (ncp.ICTFFlagLiftGuard=0 restores stock behavior)"));
}

void ANCICTFFlagLiftGuard::DeactivateGuard()
{
	if (!bGuardActive)
	{
		return;
	}

	for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
	{
		AUTLift* Lift = LiftPtr.Get();
		if (Lift == nullptr)
		{
			continue;
		}

		RemoveLiftPrerequisites(Lift);
		UPrimitiveComponent* LiftComp = Lift->GetEncroachComponent();
		if (LiftComp != nullptr)
		{
			for (const TWeakObjectPtr<AUTFlag>& FlagPtr : Flags)
			{
				if (AUTFlag* Flag = FlagPtr.Get())
				{
					LiftComp->IgnoreActorWhenMoving(Flag, false);
				}
			}
		}
	}

	for (const TWeakObjectPtr<AUTFlag>& FlagPtr : Flags)
	{
		if (AUTFlag* Flag = FlagPtr.Get())
		{
			Flag->RemoveTickPrerequisiteActor(this);
		}
	}

	Lifts.Empty();
	Flags.Empty();
	FlagStates.Empty();
	PreviousLiftLocations.Empty();
	bGuardActive = false;
	UE_LOG(LogNCICTFFlagLiftGuard, Log, TEXT("[ICTFFlagLiftGuard] disabled; stock behavior restored"));
}

void ANCICTFFlagLiftGuard::RefreshActors()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	TArray<TWeakObjectPtr<AUTLift>> NewLifts;
	for (TActorIterator<AUTLift> It(World); It; ++It)
	{
		if (!It->IsPendingKillPending() && It->GetEncroachComponent() != nullptr)
		{
			NewLifts.Add(*It);
		}
	}

	TArray<TWeakObjectPtr<AUTFlag>> NewFlags;
	AUTCTFGameState* CTFGameState = World->GetGameState<AUTCTFGameState>();
	if (CTFGameState != nullptr)
	{
		for (AUTCTFFlagBase* FlagBase : CTFGameState->FlagBases)
		{
			if (FlagBase != nullptr && FlagBase->MyFlag != nullptr
				&& !FlagBase->MyFlag->IsPendingKillPending())
			{
				NewFlags.AddUnique(FlagBase->MyFlag);
			}
		}
	}

	// Undo only relationships that disappeared. Rebuilding every relationship
	// while this actor is ticking needlessly mutates live tick prerequisite
	// arrays and briefly reopens the stock lift collision path.
	for (const TWeakObjectPtr<AUTLift>& OldLiftPtr : Lifts)
	{
		if (OldLiftPtr.IsValid() && !NewLifts.Contains(OldLiftPtr))
		{
			AUTLift* OldLift = OldLiftPtr.Get();
			RemoveLiftPrerequisites(OldLift);
			if (UPrimitiveComponent* LiftComp = OldLift->GetEncroachComponent())
			{
				for (const TWeakObjectPtr<AUTFlag>& OldFlagPtr : Flags)
				{
					if (AUTFlag* OldFlag = OldFlagPtr.Get())
					{
						LiftComp->IgnoreActorWhenMoving(OldFlag, false);
					}
				}
			}
		}
	}
	for (const TWeakObjectPtr<AUTFlag>& OldFlagPtr : Flags)
	{
		if (OldFlagPtr.IsValid() && !NewFlags.Contains(OldFlagPtr))
		{
			AUTFlag* OldFlag = OldFlagPtr.Get();
			OldFlag->RemoveTickPrerequisiteActor(this);
			for (const TWeakObjectPtr<AUTLift>& OldLiftPtr : Lifts)
			{
				AUTLift* OldLift = OldLiftPtr.Get();
				if (OldLift != nullptr && OldLift->GetEncroachComponent() != nullptr)
				{
					OldLift->GetEncroachComponent()->IgnoreActorWhenMoving(OldFlag, false);
				}
			}
		}
	}

	Lifts = MoveTemp(NewLifts);
	Flags = MoveTemp(NewFlags);
	ApplyRelationships();

	for (auto It = FlagStates.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid() || !Flags.Contains(It->Key))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = PreviousLiftLocations.CreateIterator(); It; ++It)
	{
		bool bStillTracked = false;
		if (It->Key.IsValid())
		{
			for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
			{
				AUTLift* Lift = LiftPtr.Get();
				if (Lift != nullptr && Lift->GetEncroachComponent() == It->Key.Get())
				{
					bStillTracked = true;
					break;
				}
			}
		}
		if (!bStillTracked)
		{
			It.RemoveCurrent();
		}
	}
}

void ANCICTFFlagLiftGuard::AddLiftPrerequisites(AUTLift* Lift)
{
	if (Lift == nullptr)
	{
		return;
	}

	// Generic_Lift movement may come from a Blueprint timeline component, not
	// AUTLift::Tick itself. Depending on every ticking component guarantees the
	// platform transform is final before the guard evaluates attached flags.
	AddTickPrerequisiteActor(Lift);
	TInlineComponentArray<UActorComponent*> Components(Lift);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->PrimaryComponentTick.bCanEverTick)
		{
			AddTickPrerequisiteComponent(Component);
		}
	}
}

void ANCICTFFlagLiftGuard::RemoveLiftPrerequisites(AUTLift* Lift)
{
	if (Lift == nullptr)
	{
		return;
	}

	RemoveTickPrerequisiteActor(Lift);
	TInlineComponentArray<UActorComponent*> Components(Lift);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr)
		{
			RemoveTickPrerequisiteComponent(Component);
		}
	}
}

void ANCICTFFlagLiftGuard::ApplyRelationships()
{
	// Moving lifts ignore the flag so AUTLift::NotifyHit cannot execute its
	// hardcoded below-center SendHomeWithNotify branch. This is directional:
	// the flag's own projectile movement still collides with and lands on lifts.
	for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
	{
		AUTLift* Lift = LiftPtr.Get();
		if (Lift == nullptr)
		{
			continue;
		}

		AddLiftPrerequisites(Lift);
		if (UPrimitiveComponent* LiftComp = Lift->GetEncroachComponent())
		{
			for (const TWeakObjectPtr<AUTFlag>& FlagPtr : Flags)
			{
				if (AUTFlag* Flag = FlagPtr.Get())
				{
					LiftComp->IgnoreActorWhenMoving(Flag, true);
				}
			}
		}
	}

	// AUTFlag::Tick performs the second stock instant-return check. Ensure this
	// guard has repaired/ejected an encroaching flag before that check runs.
	for (const TWeakObjectPtr<AUTFlag>& FlagPtr : Flags)
	{
		if (AUTFlag* Flag = FlagPtr.Get())
		{
			Flag->AddTickPrerequisiteActor(this);
		}
	}
}

bool ANCICTFFlagLiftGuard::IsRidePathClear(AUTFlag* Flag, AUTLift* Lift,
	const FVector& Start, const FVector& End, FHitResult* OutHit) const
{
	if (Flag == nullptr || Flag->Collision == nullptr || GetWorld() == nullptr)
	{
		return false;
	}

	FCollisionQueryParams Params(FName(TEXT("ICTFFlagLiftRide")), false, Flag);
	Params.bFindInitialOverlaps = true;
	if (Lift != nullptr)
	{
		Params.AddIgnoredActor(Lift);
	}

	FHitResult LocalHit;
	FHitResult& Hit = OutHit != nullptr ? *OutHit : LocalHit;
	const FCollisionShape Shape = Flag->Collision->GetCollisionShape(-ClearancePad);
	const bool bBlocked = GetWorld()->SweepSingleByChannel(Hit, Start, End,
		Flag->GetActorQuat(), Flag->Collision->GetCollisionObjectType(), Shape,
		Params, Flag->Collision->GetCollisionResponseToChannels());
	return !bBlocked;
}

bool ANCICTFFlagLiftGuard::IsLocationClear(AUTFlag* Flag, const FVector& Location) const
{
	if (Flag == nullptr || Flag->Collision == nullptr || GetWorld() == nullptr)
	{
		return false;
	}

	FCollisionQueryParams Params(FName(TEXT("ICTFFlagLiftClearance")), false, Flag);
	Params.bFindInitialOverlaps = true;
	return !GetWorld()->OverlapBlockingTestByChannel(Location, Flag->GetActorQuat(),
		Flag->Collision->GetCollisionObjectType(), Flag->Collision->GetCollisionShape(),
		Params, Flag->Collision->GetCollisionResponseToChannels());
}

AUTLift* ANCICTFFlagLiftGuard::FindThreateningLift(AUTFlag* Flag,
	const FFlagRideState& State) const
{
	if (Flag == nullptr || Flag->Collision == nullptr || GetWorld() == nullptr)
	{
		return nullptr;
	}

	// Coarse proximity gate: the exact queries below run every server tick for
	// every dropped flag, but on most frames no lift is anywhere near it. The
	// current-frame component bounds (final for this frame — the guard ticks
	// after lift movement), padded by the flag capsule plus a full frame of
	// fast lift-and-flag travel, keep the gate conservative; anything inside
	// the pad still runs the exact overlap/sweep tests.
	const float NearPad = Flag->Collision->GetScaledCapsuleRadius()
		+ Flag->Collision->GetScaledCapsuleHalfHeight() + ThreatProximityPad;
	const FVector FlagLocation = Flag->GetActorLocation();
	bool bAnyLiftNear = false;
	for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
	{
		AUTLift* NearLift = LiftPtr.Get();
		UPrimitiveComponent* NearComp = NearLift != nullptr ? NearLift->GetEncroachComponent() : nullptr;
		if (NearComp != nullptr
			&& NearComp->Bounds.GetBox().ComputeSquaredDistanceToPoint(FlagLocation) <= FMath::Square(NearPad))
		{
			bAnyLiftNear = true;
			break;
		}
	}
	if (!bAnyLiftNear)
	{
		return nullptr;
	}

	FCollisionQueryParams Params(FName(TEXT("ICTFFlagLiftThreat")), false, Flag);
	Params.bFindInitialOverlaps = true;
	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByChannel(Overlaps, Flag->GetActorLocation(),
		Flag->GetActorQuat(), Flag->Collision->GetCollisionObjectType(),
		Flag->Collision->GetCollisionShape(), Params,
		Flag->Collision->GetCollisionResponseToChannels());
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AUTLift* OverlapLift = Cast<AUTLift>(Overlap.Actor.Get());
		if (OverlapLift != nullptr)
		{
			for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
			{
				if (LiftPtr.Get() == OverlapLift)
				{
					return OverlapLift;
				}
			}
		}
	}

	if (!State.bHasPreviousLocation)
	{
		return nullptr;
	}

	// Directional MoveIgnoreActors stops AUTLift's stock hit callback, so test
	// the same interaction explicitly. Transform the flag's previous location
	// into the lift's current frame; sweeping from there to the current flag
	// location captures relative motion by both the lift and a falling flag.
	const FCollisionShape Shape = Flag->Collision->GetCollisionShape(-ClearancePad);
	for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
	{
		AUTLift* Lift = LiftPtr.Get();
		UPrimitiveComponent* LiftComp = Lift != nullptr ? Lift->GetEncroachComponent() : nullptr;
		if (LiftComp == nullptr)
		{
			continue;
		}
		// Same coarse gate per lift: a distant moving lift cannot reach the flag
		// within one frame, so skip its relative sweep.
		if (LiftComp->Bounds.GetBox().ComputeSquaredDistanceToPoint(FlagLocation) > FMath::Square(NearPad))
		{
			continue;
		}
		const FVector* PreviousLiftLocation = PreviousLiftLocations.Find(LiftComp);
		if (PreviousLiftLocation == nullptr)
		{
			continue;
		}

		const FVector LiftDelta = LiftComp->GetComponentLocation() - *PreviousLiftLocation;
		const FVector RelativeStart = State.PreviousWorldLocation + LiftDelta;
		const FVector End = Flag->GetActorLocation();
		if (RelativeStart.Equals(End, KINDA_SMALL_NUMBER))
		{
			continue;
		}

		FHitResult Hit;
		if (GetWorld()->SweepSingleByChannel(Hit, RelativeStart, End,
			Flag->GetActorQuat(), Flag->Collision->GetCollisionObjectType(), Shape,
			Params, Flag->Collision->GetCollisionResponseToChannels())
			&& Hit.Actor.Get() == Lift)
		{
			return Lift;
		}
	}

	return nullptr;
}

bool ANCICTFFlagLiftGuard::TryPlaceOnLift(AUTFlag* Flag, AUTLift* Lift,
	const FVector& PathStart, FVector& OutLocation) const
{
	if (Flag == nullptr || Flag->Collision == nullptr || Lift == nullptr
		|| Lift->GetEncroachComponent() == nullptr || GetWorld() == nullptr)
	{
		return false;
	}

	UPrimitiveComponent* LiftComp = Lift->GetEncroachComponent();
	const FBox Bounds = LiftComp->Bounds.GetBox();
	const float Radius = Flag->Collision->GetScaledCapsuleRadius();
	const float HalfHeight = Flag->Collision->GetScaledCapsuleHalfHeight();
	const float MinX = Bounds.Min.X + Radius + ClearancePad;
	const float MaxX = Bounds.Max.X - Radius - ClearancePad;
	const float MinY = Bounds.Min.Y + Radius + ClearancePad;
	const float MaxY = Bounds.Max.Y - Radius - ClearancePad;
	const FVector Current = Flag->GetActorLocation();

	TArray<FVector2D> XYs;
	XYs.Add(FVector2D(
		(MinX <= MaxX) ? FMath::Clamp(Current.X, MinX, MaxX) : Bounds.GetCenter().X,
		(MinY <= MaxY) ? FMath::Clamp(Current.Y, MinY, MaxY) : Bounds.GetCenter().Y));
	XYs.AddUnique(FVector2D(Bounds.GetCenter().X, Bounds.GetCenter().Y));
	if (MinX <= MaxX && MinY <= MaxY)
	{
		XYs.AddUnique(FVector2D(MinX, MinY));
		XYs.AddUnique(FVector2D(MinX, MaxY));
		XYs.AddUnique(FVector2D(MaxX, MinY));
		XYs.AddUnique(FVector2D(MaxX, MaxY));
	}

	FCollisionQueryParams Params(FName(TEXT("ICTFFlagLiftTop")), false, Flag);
	Params.bFindInitialOverlaps = true;
	const FCollisionShape Shape = Flag->Collision->GetCollisionShape(-ClearancePad);
	for (const FVector2D& XY : XYs)
	{
		const FVector TraceStart(XY.X, XY.Y, Bounds.Max.Z + HalfHeight + 128.f);
		const FVector TraceEnd(XY.X, XY.Y, Bounds.Min.Z - HalfHeight - 32.f);
		FHitResult SupportHit;
		if (!GetWorld()->SweepSingleByChannel(SupportHit, TraceStart, TraceEnd,
			Flag->GetActorQuat(), Flag->Collision->GetCollisionObjectType(), Shape,
			Params, Flag->Collision->GetCollisionResponseToChannels()))
		{
			continue;
		}
		if (SupportHit.Actor.Get() != Lift || SupportHit.ImpactNormal.Z < 0.5f)
		{
			continue;
		}

		// The support sweep uses a slightly shrunken capsule. Add twice the
		// shrink so the final full-size capsule retains a real clearance gap
		// instead of resting on a floating-point contact boundary.
		const FVector Candidate = SupportHit.Location + SupportHit.ImpactNormal * (2.f * ClearancePad);
		if (IsLocationClear(Flag, Candidate)
			&& IsRidePathClear(Flag, Lift, PathStart, Candidate))
		{
			OutLocation = Candidate;
			return true;
		}
	}

	return false;
}

bool ANCICTFFlagLiftGuard::TryEjectBesideLift(AUTFlag* Flag, AUTLift* Lift,
	const FVector& PathStart, FVector& OutLocation) const
{
	if (Flag == nullptr || Flag->Collision == nullptr || Lift == nullptr
		|| Lift->GetEncroachComponent() == nullptr || GetWorld() == nullptr)
	{
		return false;
	}

	const FBox Bounds = Lift->GetEncroachComponent()->Bounds.GetBox();
	const FVector Center = Bounds.GetCenter();
	const float Radius = Flag->Collision->GetScaledCapsuleRadius();
	const float HalfHeight = Flag->Collision->GetScaledCapsuleHalfHeight();
	const float OffsetX = Bounds.GetExtent().X + Radius + SidePad;
	const float OffsetY = Bounds.GetExtent().Y + Radius + SidePad;
	const float TraceZ = FMath::Max(PathStart.Z + HalfHeight + 128.f,
		Bounds.Max.Z + HalfHeight + 256.f);

	TArray<FVector2D> XYs;
	XYs.Add(FVector2D(Center.X - OffsetX, PathStart.Y));
	XYs.Add(FVector2D(Center.X + OffsetX, PathStart.Y));
	XYs.Add(FVector2D(PathStart.X, Center.Y - OffsetY));
	XYs.Add(FVector2D(PathStart.X, Center.Y + OffsetY));
	XYs.Add(FVector2D(Center.X - OffsetX, Center.Y - OffsetY));
	XYs.Add(FVector2D(Center.X - OffsetX, Center.Y + OffsetY));
	XYs.Add(FVector2D(Center.X + OffsetX, Center.Y - OffsetY));
	XYs.Add(FVector2D(Center.X + OffsetX, Center.Y + OffsetY));

	// Nearest side first minimizes visible displacement and makes the direct
	// path-sweep test less likely to reject an otherwise recoverable exit.
	XYs.Sort([PathStart](const FVector2D& A, const FVector2D& B)
	{
		return FVector2D::DistSquared(A, FVector2D(PathStart.X, PathStart.Y))
			< FVector2D::DistSquared(B, FVector2D(PathStart.X, PathStart.Y));
	});

	FCollisionQueryParams Params(FName(TEXT("ICTFFlagLiftEject")), false, Flag);
	Params.bFindInitialOverlaps = true;
	Params.AddIgnoredActor(Lift);
	const FCollisionShape Shape = Flag->Collision->GetCollisionShape(-ClearancePad);
	for (const FVector2D& XY : XYs)
	{
		const FVector TraceStart(XY.X, XY.Y, TraceZ);
		const FVector TraceEnd(XY.X, XY.Y, TraceZ - EjectTraceDepth);
		FHitResult FloorHit;
		if (!GetWorld()->SweepSingleByChannel(FloorHit, TraceStart, TraceEnd,
			Flag->GetActorQuat(), Flag->Collision->GetCollisionObjectType(), Shape,
			Params, Flag->Collision->GetCollisionResponseToChannels())
			|| FloorHit.bStartPenetrating || FloorHit.ImpactNormal.Z < 0.5f)
		{
			continue;
		}

		const FVector Candidate = FloorHit.Location + FloorHit.ImpactNormal * (2.f * ClearancePad);
		if (IsLocationClear(Flag, Candidate)
			&& IsRidePathClear(Flag, Lift, PathStart, Candidate))
		{
			OutLocation = Candidate;
			return true;
		}
	}

	return false;
}

void ANCICTFFlagLiftGuard::PlaceFlag(AUTFlag* Flag, const FVector& Location,
	AUTLift* AttachToLift) const
{
	if (Flag == nullptr)
	{
		return;
	}

	if (Flag->MovementComponent != nullptr)
	{
		Flag->MovementComponent->StopMovementImmediately();
		Flag->MovementComponent->SetUpdatedComponent(nullptr);
	}
	if (Flag->GetRootComponent() != nullptr
		&& Flag->GetRootComponent()->GetAttachParent() != nullptr)
	{
		Flag->DetachRootComponentFromParent(true);
	}
	Flag->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	if (AttachToLift != nullptr && AttachToLift->GetEncroachComponent() != nullptr)
	{
		Flag->AttachToComponent(AttachToLift->GetEncroachComponent(),
			FAttachmentTransformRules::KeepWorldTransform, NAME_None);
	}
	if (Flag->Collision != nullptr)
	{
		Flag->Collision->bShouldUpdatePhysicsVolume = true;
		Flag->Collision->UpdateOverlaps();
	}
	Flag->ForceNetUpdate();
}

void ANCICTFFlagLiftGuard::RescueFlag(AUTFlag* Flag, AUTLift* Lift,
	FFlagRideState& State, const TCHAR* Reason)
{
	const FVector PathStart = State.bHasClearLocation
		? State.LastClearWorldLocation
		: State.PreviousWorldLocation;
	FVector SafeLocation;
	if (TryPlaceOnLift(Flag, Lift, PathStart, SafeLocation))
	{
		PlaceFlag(Flag, SafeLocation, Lift);
		State.PreviousWorldLocation = SafeLocation;
		State.LastClearWorldLocation = SafeLocation;
		State.bHasPreviousLocation = true;
		State.bHasClearLocation = true;
		UE_LOG(LogNCICTFFlagLiftGuard, Verbose,
			TEXT("[ICTFFlagLiftGuard] kept %s on %s after %s"),
			*Flag->GetName(), *Lift->GetName(), Reason);
		return;
	}

	if (TryEjectBesideLift(Flag, Lift, PathStart, SafeLocation))
	{
		PlaceFlag(Flag, SafeLocation, nullptr);
		State.Lift.Reset();
		State.PreviousWorldLocation = SafeLocation;
		State.LastClearWorldLocation = SafeLocation;
		State.bHasPreviousLocation = true;
		State.bHasClearLocation = true;
		UE_LOG(LogNCICTFFlagLiftGuard, Log,
			TEXT("[ICTFFlagLiftGuard] ejected %s beside %s after %s"),
			*Flag->GetName(), *Lift->GetName(), Reason);
		return;
	}

	UE_LOG(LogNCICTFFlagLiftGuard, Warning,
		TEXT("[ICTFFlagLiftGuard] no safe recovery for %s at %s after %s; returning home"),
		*Flag->GetName(), *Flag->GetActorLocation().ToString(), Reason);
	Flag->SendHomeWithNotify();
	FlagStates.Remove(Flag);
}

void ANCICTFFlagLiftGuard::UpdateFlag(AUTFlag* Flag)
{
	if (Flag == nullptr || Flag->Collision == nullptr
		|| Flag->ObjectState != CarriedObjectState::Dropped)
	{
		FlagStates.Remove(Flag);
		return;
	}

	FFlagRideState& State = FlagStates.FindOrAdd(Flag);
	const FVector CurrentLocation = Flag->GetActorLocation();
	if (!State.bHasPreviousLocation)
	{
		State.PreviousWorldLocation = CurrentLocation;
		State.bHasPreviousLocation = true;
	}
	AUTLift* AttachedLift = GetAttachedLift(Flag);
	if (AttachedLift == nullptr)
	{
		if (AUTLift* ThreateningLift = FindThreateningLift(Flag, State))
		{
			State.Lift = ThreateningLift;
			RescueFlag(Flag, ThreateningLift, State, TEXT("moving lift intersection"));
			return;
		}
		State.Lift.Reset();
		State.PreviousWorldLocation = CurrentLocation;
		State.bHasPreviousLocation = true;
		if (IsLocationClear(Flag, CurrentLocation))
		{
			State.LastClearWorldLocation = CurrentLocation;
			State.bHasClearLocation = true;
		}
		return;
	}

	if (State.Lift.Get() != AttachedLift)
	{
		State.Lift = AttachedLift;
		State.PreviousWorldLocation = CurrentLocation;
		State.bHasPreviousLocation = true;
	}

	FHitResult RideHit;
	const bool bPathClear = IsRidePathClear(Flag, AttachedLift,
		State.PreviousWorldLocation, CurrentLocation, &RideHit);
	FVector EncroachAdjust = FVector::ZeroVector;
	const bool bEncroaching = GetWorld()->EncroachingBlockingGeometry(Flag,
		CurrentLocation, Flag->GetActorRotation(), &EncroachAdjust);
	if (!bPathClear || bEncroaching)
	{
		RescueFlag(Flag, AttachedLift, State,
			!bPathClear ? TEXT("blocked capsule sweep") : TEXT("blocking overlap"));
		return;
	}

	State.Lift = AttachedLift;
	State.PreviousWorldLocation = CurrentLocation;
	State.LastClearWorldLocation = CurrentLocation;
	State.bHasPreviousLocation = true;
	State.bHasClearLocation = true;
}

void ANCICTFFlagLiftGuard::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Role != ROLE_Authority)
	{
		return;
	}

	const bool bWanted = CVarICTFFlagLiftGuard.GetValueOnGameThread() != 0;
	if (bWanted != bGuardActive)
	{
		if (bWanted)
		{
			ActivateGuard();
		}
		else
		{
			DeactivateGuard();
		}
	}
	if (!bGuardActive)
	{
		return;
	}

	RefreshAccumulator += DeltaTime;
	if (RefreshAccumulator >= ActorRefreshInterval)
	{
		RefreshAccumulator = 0.f;
		RefreshActors();
	}

	// Liftless map (much of the iCTF rotation): nothing can threaten a flag,
	// and UpdateFlag would still pay a physics query per dropped flag per tick.
	// RefreshActors above keeps rescanning, so the guard re-arms if that changes.
	if (Lifts.Num() == 0)
	{
		return;
	}

	for (const TWeakObjectPtr<AUTFlag>& FlagPtr : Flags)
	{
		if (AUTFlag* Flag = FlagPtr.Get())
		{
			UpdateFlag(Flag);
		}
	}

	// Snapshot only after every flag used the previous-frame lift positions.
	for (const TWeakObjectPtr<AUTLift>& LiftPtr : Lifts)
	{
		AUTLift* Lift = LiftPtr.Get();
		if (Lift != nullptr && Lift->GetEncroachComponent() != nullptr)
		{
			PreviousLiftLocations.Add(Lift->GetEncroachComponent(),
				Lift->GetEncroachComponent()->GetComponentLocation());
		}
	}
}
