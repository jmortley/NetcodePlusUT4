// NCICTFFlagLiftGuard - iCTF-only protection for dropped flags riding lifts.
//
// Stock AUTLift returns a carried object immediately when a descending lift
// hits it below the lift's center of mass. AUTFlag also returns itself when an
// attached lift pushes it into blocking geometry. Both are deliberately blunt
// anti-stuck fallbacks.
//
// This server-side guard keeps stock attachment (and therefore smooth client
// lift motion), but gives the flag's own capsule the sweep that attached child
// components do not receive. Tick prerequisites enforce:
//
//     lift movement -> this guard -> AUTFlag::Tick
//
// A blocked ride is moved to a clear point on the platform, or ejected onto
// nearby valid floor. The normal auto-return timer remains untouched. If no
// safe recoverable point exists, the stock notified return is used.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCICTFFlagLiftGuard.generated.h"

class AUTFlag;
class AUTLift;
class UPrimitiveComponent;

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCICTFFlagLiftGuard : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaTime) override;

	/** Find or spawn the world's guard. Server only; safe across halftime. */
	static ANCICTFFlagLiftGuard* EnsureSpawned(UWorld* World);

private:
	struct FFlagRideState
	{
		TWeakObjectPtr<AUTLift> Lift;
		FVector PreviousWorldLocation = FVector::ZeroVector;
		FVector LastClearWorldLocation = FVector::ZeroVector;
		bool bHasPreviousLocation = false;
		bool bHasClearLocation = false;
	};

	void ActivateGuard();
	void DeactivateGuard();
	void RefreshActors();
	void ApplyRelationships();
	void AddLiftPrerequisites(AUTLift* Lift);
	void RemoveLiftPrerequisites(AUTLift* Lift);
	void UpdateFlag(AUTFlag* Flag);
	AUTLift* FindThreateningLift(AUTFlag* Flag, const FFlagRideState& State) const;

	bool IsRidePathClear(AUTFlag* Flag, AUTLift* Lift,
		const FVector& Start, const FVector& End, FHitResult* OutHit = nullptr) const;
	bool IsLocationClear(AUTFlag* Flag, const FVector& Location) const;
	bool TryPlaceOnLift(AUTFlag* Flag, AUTLift* Lift,
		const FVector& PathStart, FVector& OutLocation) const;
	bool TryEjectBesideLift(AUTFlag* Flag, AUTLift* Lift,
		const FVector& PathStart, FVector& OutLocation) const;
	void PlaceFlag(AUTFlag* Flag, const FVector& Location, AUTLift* AttachToLift) const;
	void RescueFlag(AUTFlag* Flag, AUTLift* Lift, FFlagRideState& State,
		const TCHAR* Reason);

	TArray<TWeakObjectPtr<AUTLift>> Lifts;
	TArray<TWeakObjectPtr<AUTFlag>> Flags;
	TMap<TWeakObjectPtr<AUTFlag>, FFlagRideState> FlagStates;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FVector> PreviousLiftLocations;

	float RefreshAccumulator = 0.f;
	bool bGuardActive = false;
};
