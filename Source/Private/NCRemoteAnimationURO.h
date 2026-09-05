#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "NCRemoteAnimationPolicy.h"

class ATeamArenaCharacter;
class USkeletalMeshComponent;
class USkeletalMesh;
class UAnimInstance;
class UWorld;

namespace NCRemoteAnimationURO
{
	// AdditionalTime describes the previous update and may already have been
	// consumed. Only the negative pose offset represents time still owed.
	inline float GetPendingAnimationTime(float TickedPoseOffsetTime)
	{
		return FMath::IsFinite(TickedPoseOffsetTime)
			? FMath::Max(0.f, -TickedPoseOffsetTime) : 0.f;
	}
}

/** Owns a temporary client-body scheduling policy, never the animation tick.
 * Keep calling Update/Release while IsManaged(): disabling may await one normal
 * pose update to consume skipped animation time before restoring the settings. */
struct FNCRemoteAnimationUROState
{
	void Update(ATeamArenaCharacter& Owner, bool bEnabled, bool bThrottleCandidate,
		double Now, uint32 ViewRevision, float DemotionDelay);
	void Release(ATeamArenaCharacter& Owner, bool bTeardown = false);
	bool IsManaged() const { return bManaged; }
	bool IsThrottled() const { return bManaged && !bDraining && bThrottled; }

private:
	struct FAuthoredInputs
	{
		int32 NonRenderedRate = 4;
		TArray<float> Thresholds;
		bool bUseLodMap = false;
		TMap<int32, int32> LodMap;
		int32 InterpolationLimit = 4;
		EUpdateRateShiftBucket ShiftBucket = EUpdateRateShiftBucket::ShiftBucket0;

		void Read(const FAnimUpdateRateParameters& Params);
		void Restore(FAnimUpdateRateParameters& Params) const;
	};

	void ApplyPolicy(FAnimUpdateRateParameters& Params, bool bReduce);
	bool HasExpectedInputs(const FAnimUpdateRateParameters& Params) const;
	void Forget();
	void YieldToExternalPolicy();

	NCRemoteAnimationPolicy::FState Priority;
	FAuthoredInputs SavedInputs;
	TWeakObjectPtr<USkeletalMeshComponent> ManagedMesh;
	TWeakObjectPtr<USkeletalMesh> ManagedAsset;
	TWeakObjectPtr<UAnimInstance> ManagedAnimation;
	TWeakObjectPtr<UWorld> ManagedWorld;
	// Identity only. Always obtain a live pointer from ManagedMesh before use;
	// unregistering the last owned mesh destroys this engine-owned allocation.
	FAnimUpdateRateParameters* ParameterIdentity = nullptr;
	double LastUpdateTime = 0.0;
	uint32 LastViewRevision = 0;
	float DrainPoseTime = 0.f;
	float DrainWorldTime = 0.f;
	bool bManaged = false;
	bool bDraining = false;
	bool bThrottled = false;
	bool bSavedEnabled = false;
	bool bYielded = false;
};
