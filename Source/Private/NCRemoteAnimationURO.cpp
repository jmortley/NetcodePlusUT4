#include "NetcodePlus.h"
#include "NCRemoteAnimationURO.h"
#include "TeamArenaCharacter.h"
#include "TeamArenaCharacterMovement.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
	bool IsEngineSchedulingAvailable()
	{
		static IConsoleVariable* Enable = IConsoleManager::Get().FindConsoleVariable(TEXT("a.URO.Enable"));
		static IConsoleVariable* ForceRate = IConsoleManager::Get().FindConsoleVariable(TEXT("a.URO.ForceAnimRate"));
		return Enable != nullptr && Enable->GetInt() > 0
			&& ForceRate != nullptr && ForceRate->GetInt() == 0;
	}

	bool HasDefaultInputs(const FAnimUpdateRateParameters& Params)
	{
		return Params.BaseNonRenderedUpdateRate == 4
			&& !Params.bShouldUseLodMap && Params.LODToFrameSkipMap.Num() == 0
			&& Params.MaxEvalRateForInterpolation == 4
			&& Params.ShiftBucket == EUpdateRateShiftBucket::ShiftBucket0
			&& Params.BaseVisibleDistanceFactorThesholds.Num() == 2
			&& Params.BaseVisibleDistanceFactorThesholds[0] == 0.4f
			&& Params.BaseVisibleDistanceFactorThesholds[1] == 0.2f;
	}

	void ClearTiming(FAnimUpdateRateParameters& Params)
	{
		Params.OptimizeMode = FAnimUpdateRateParameters::TrailMode;
		Params.UpdateRate = Params.EvaluationRate = 1;
		Params.bSkipUpdate = Params.bSkipEvaluation = false;
		Params.bInterpolateSkippedFrames = false;
		Params.TickedPoseOffestTime = 0.f;
		Params.AdditionalTime = 0.f;
		Params.ThisTickDelta = 0.f;
	}

	bool IsDormantStockFirstPersonMesh(ATeamArenaCharacter& Owner, USkeletalMeshComponent& Mesh)
	{
		UWorld* World = Owner.GetWorld();
		if (&Mesh != Owner.FirstPersonMesh || Mesh.GetClass() != USkeletalMeshComponent::StaticClass()
			|| World == nullptr || Owner.IsLocallyControlled()
			|| !FMath::IsFinite(World->TimeSeconds) || !FMath::IsFinite(Mesh.LastRenderTime)
			|| !Mesh.bOnlyOwnerSee
			|| Mesh.MeshComponentUpdateFlag != EMeshComponentUpdateFlag::OnlyTickPoseWhenRendered
			|| Mesh.bRecentlyRendered || Mesh.LastRenderTime > World->TimeSeconds - 1.f
			|| Mesh.bIsAutonomousTickPose || Mesh.bOnlyAllowAutonomousTickPose
			|| Mesh.PrimaryComponentTick.bTickEvenWhenPaused
			|| Mesh.IsPlayingRootMotion() || Mesh.IsPlayingRootMotionFromEverything())
		{
			return false;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			if (PC != nullptr && PC->IsLocalPlayerController() && PC->GetViewTarget() == &Owner)
			{
				return false;
			}
		}
		// This exception relies on TeamArenaCharacter releasing before BOTH
		// BecomeViewTarget and BehindViewChange: either can wake the stock 1P mesh
		// after our pre-movement check. Arbitrary authored secondary meshes get no
		// such exception, even when currently hidden, paused or master-pose driven.
		return true;
	}

	bool HasSupportedSiblings(ATeamArenaCharacter& Owner, USkeletalMeshComponent& Body)
	{
		// UE4.15 shares parameters by actor owner, including components whose own
		// URO flag is false. Reject another pose consumer: it would also receive
		// AdditionalTime during a catch-up frame. In UE4.15 even a master-pose
		// slave can reach TickPose if it still has its own animation instance.
		TInlineComponentArray<USkinnedMeshComponent*> Components(&Owner);
		for (USkinnedMeshComponent* Component : Components)
		{
			if (Component == nullptr || Component == &Body || !Component->IsRegistered())
			{
				continue;
			}
			if (Component->bEnableUpdateRateOptimizations
				|| Component->OnAnimUpdateRateParamsCreated.IsBound())
			{
				return false;
			}
			USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component);
			if (Skeletal != nullptr && (Skeletal->GetAnimInstance() != nullptr
				|| Skeletal->GetPostProcessInstance() != nullptr))
			{
				if (!IsDormantStockFirstPersonMesh(Owner, *Skeletal))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool IsSupportedBody(ATeamArenaCharacter& Owner, USkeletalMeshComponent* Body)
	{
		UTeamArenaCharacterMovement* Movement = Cast<UTeamArenaCharacterMovement>(Owner.GetCharacterMovement());
		return Body != nullptr && Owner.GetNetMode() == NM_Client && Owner.Role == ROLE_SimulatedProxy
			&& !Owner.IsLocallyControlled() && !Owner.IsDead() && !Owner.IsRagdoll()
			&& Movement != nullptr && Movement->IsRegistered() && Movement->IsActive()
			&& Movement->IsComponentTickEnabled()
			&& Body->IsRegistered() && Body->IsComponentTickEnabled()
			&& Body->GetClass() == USkeletalMeshComponent::StaticClass()
			&& Body->SkeletalMesh != nullptr && Body->GetAnimInstance() != nullptr
			&& !Body->MasterPoseComponent.IsValid() && !Body->IsSimulatingPhysics()
			&& !Body->bPauseAnims && !Body->bNoSkeletonUpdate
			&& !Body->PrimaryComponentTick.bTickEvenWhenPaused
			&& !Body->bOnlyAllowAutonomousTickPose
			&& !Body->IsPlayingRootMotion() && !Body->IsPlayingRootMotionFromEverything()
			&& !Body->OnAnimUpdateRateParamsCreated.IsBound();
	}
}

void FNCRemoteAnimationUROState::FAuthoredInputs::Read(const FAnimUpdateRateParameters& Params)
{
	NonRenderedRate = Params.BaseNonRenderedUpdateRate;
	Thresholds = Params.BaseVisibleDistanceFactorThesholds;
	bUseLodMap = Params.bShouldUseLodMap;
	LodMap = Params.LODToFrameSkipMap;
	InterpolationLimit = Params.MaxEvalRateForInterpolation;
	ShiftBucket = Params.ShiftBucket;
}

void FNCRemoteAnimationUROState::FAuthoredInputs::Restore(FAnimUpdateRateParameters& Params) const
{
	Params.BaseNonRenderedUpdateRate = NonRenderedRate;
	Params.BaseVisibleDistanceFactorThesholds = Thresholds;
	Params.bShouldUseLodMap = bUseLodMap;
	Params.LODToFrameSkipMap = LodMap;
	Params.MaxEvalRateForInterpolation = InterpolationLimit;
	Params.ShiftBucket = ShiftBucket;
}

void FNCRemoteAnimationUROState::ApplyPolicy(FAnimUpdateRateParameters& Params, bool bReduce)
{
	Params.BaseNonRenderedUpdateRate = 1;
	Params.bShouldUseLodMap = false;
	Params.LODToFrameSkipMap.Reset();
	Params.MaxEvalRateForInterpolation = SavedInputs.InterpolationLimit;
	Params.ShiftBucket = SavedInputs.ShiftBucket;
	Params.BaseVisibleDistanceFactorThesholds.Reset();
	if (bReduce)
	{
		// The caller already classifies screen size and aim relevance. One
		// unreachable threshold selects rate 2 and cannot select rate 3 or 4.
		Params.BaseVisibleDistanceFactorThesholds.Add(TNumericLimits<float>::Max());
	}
	bThrottled = bReduce;
}

bool FNCRemoteAnimationUROState::HasExpectedInputs(const FAnimUpdateRateParameters& Params) const
{
	return Params.BaseNonRenderedUpdateRate == 1 && !Params.bShouldUseLodMap
		&& Params.LODToFrameSkipMap.Num() == 0
		&& Params.MaxEvalRateForInterpolation == SavedInputs.InterpolationLimit
		&& Params.ShiftBucket == SavedInputs.ShiftBucket
		&& (bThrottled
			? (Params.BaseVisibleDistanceFactorThesholds.Num() == 1
				&& Params.BaseVisibleDistanceFactorThesholds[0] == TNumericLimits<float>::Max())
			: Params.BaseVisibleDistanceFactorThesholds.Num() == 0);
}

void FNCRemoteAnimationUROState::Forget()
{
	ManagedMesh.Reset();
	ManagedAsset.Reset();
	ManagedAnimation.Reset();
	ManagedWorld.Reset();
	ParameterIdentity = nullptr;
	bManaged = bDraining = bThrottled = false;
	Priority.Reset();
}

void FNCRemoteAnimationUROState::YieldToExternalPolicy()
{
	// Another author now owns the shared inputs. Do not restore over that author.
	// A deliberate off/on request is required before attempting to acquire again.
	bYielded = true;
	Forget();
}

void FNCRemoteAnimationUROState::Update(ATeamArenaCharacter& Owner, bool bEnabled,
	bool bThrottleCandidate, double Now, uint32 ViewRevision, float DemotionDelay)
{
	if (!bEnabled)
	{
		bYielded = false;
		Release(Owner);
		return;
	}
	if (bDraining)
	{
		Release(Owner);
		return;
	}
	if (!bManaged && !bThrottleCandidate)
	{
		// A nearby/aim-relevant/low-FPS body needs no manager or sibling scan.
		// Once acquired, retain the policy across full-rate promotions instead
		// of repeatedly toggling the component flag.
		Priority.Reset();
		return;
	}

	USkeletalMeshComponent* Body = Owner.GetMesh();
	if (bManaged && (Body != ManagedMesh.Get() || Body == nullptr
		|| Body->AnimUpdateRateParams != ParameterIdentity
		|| ManagedWorld.Get() != Owner.GetWorld()
		|| ManagedAsset.Get() != Body->SkeletalMesh
		|| ManagedAnimation.Get() != Body->GetAnimInstance()
		|| !NCRemoteAnimationPolicy::IsFiniteTime(Now) || Now < LastUpdateTime))
	{
		// Old-animation time must not be applied to a new mesh, animation instance,
		// registration or world clock. This discontinuity discards pending pose time.
		Release(Owner, true);
		return;
	}
	if (!IsEngineSchedulingAvailable() || !IsSupportedBody(Owner, Body))
	{
		Release(Owner);
		return;
	}

	FAnimUpdateRateParameters* Params = Body->AnimUpdateRateParams;
	if (Params == nullptr || !NCRemoteAnimationPolicy::IsFiniteTime(Now))
	{
		Release(Owner, true);
		return;
	}
	if (bManaged && (!Body->bEnableUpdateRateOptimizations || !HasExpectedInputs(*Params)))
	{
		// Release distinguishes an external flag-off (our remaining time still
		// needs draining) from another author replacing the shared input policy.
		Release(Owner);
		bYielded = true;
		return;
	}
	if (!HasSupportedSiblings(Owner, *Body))
	{
		// A newly active second animation consumer cannot share a one-shot drain.
		Release(Owner, true);
		return;
	}
	if (!bManaged)
	{
		if (bYielded || Body->bEnableUpdateRateOptimizations || !HasDefaultInputs(*Params)
			|| Params->AdditionalTime != 0.f || Params->TickedPoseOffestTime != 0.f
			|| Body->PoseTickedThisFrame())
		{
			return;
		}
		SavedInputs.Read(*Params);
		bSavedEnabled = Body->bEnableUpdateRateOptimizations;
		ManagedMesh = Body;
		ManagedAsset = Body->SkeletalMesh;
		ManagedAnimation = Body->GetAnimInstance();
		ManagedWorld = Owner.GetWorld();
		ParameterIdentity = Params;
		LastViewRevision = ViewRevision;
		Priority.Reset();
		bManaged = true;
		ApplyPolicy(*Params, false);
		Body->bEnableUpdateRateOptimizations = true;
	}
	if (LastViewRevision != ViewRevision)
	{
		Priority.Reset();
		LastViewRevision = ViewRevision;
		bThrottleCandidate = false;
	}
	if (Owner.GetCharacterMovement()->bJustTeleported)
	{
		// This is checked before Super::TickComponent can clear the movement
		// flag. A target teleport promotes immediately and restarts demotion.
		Priority.Reset();
		bThrottleCandidate = false;
	}
	const bool bReduce = Priority.Update(bThrottleCandidate, Now, DemotionDelay);
	if (bReduce != bThrottled)
	{
		ApplyPolicy(*Params, bReduce);
	}
	LastUpdateTime = Now;
}

void FNCRemoteAnimationUROState::Release(ATeamArenaCharacter& Owner, bool bTeardown)
{
	if (!bManaged)
	{
		return;
	}
	USkeletalMeshComponent* Body = ManagedMesh.Get();
	FAnimUpdateRateParameters* Params = Body != nullptr ? Body->AnimUpdateRateParams : nullptr;
	if (Body == nullptr || Params == nullptr || Params != ParameterIdentity)
	{
		// The old allocation may be freed. Restore only our component flag; never
		// copy the old allocation's policy into its replacement.
		if (Body != nullptr && Body->bEnableUpdateRateOptimizations == !bDraining)
		{
			Body->bEnableUpdateRateOptimizations = bSavedEnabled;
		}
		Forget();
		return;
	}
	if (!HasExpectedInputs(*Params))
	{
		// A registration can allocate its new default tracker at the old address.
		// Defaults are not our policy: relinquish our flag without restoring the
		// prior tracker's settings into this allocation.
		if (HasDefaultInputs(*Params) && Body->bEnableUpdateRateOptimizations == !bDraining)
		{
			ClearTiming(*Params);
			Body->bEnableUpdateRateOptimizations = bSavedEnabled;
			Forget();
			return;
		}
		// Retire NCP's outstanding animation time, but preserve the replacement
		// author's inputs and flag. Carrying it into that policy is ambiguous,
		// especially if that author disabled URO or the global switch is zero.
		ClearTiming(*Params);
		YieldToExternalPolicy();
		return;
	}
	if (Body->bEnableUpdateRateOptimizations != !bDraining)
	{
		if (bDraining)
		{
			ClearTiming(*Params);
			YieldToExternalPolicy();
			return;
		}
		// Another system disabled our flag. Respect that choice and settle our
		// animation time once; do not leave AdditionalTime repeating indefinitely.
		bYielded = true;
	}
	UWorld* World = Owner.GetWorld();
	const bool bPoseReplaced = ManagedAsset.Get() != Body->SkeletalMesh
		|| ManagedAnimation.Get() != Body->GetAnimInstance();
	const bool bClockReset = ManagedWorld.Get() != World
		|| (bDraining && World != nullptr && World->TimeSeconds < DrainWorldTime);
	const bool bNewSimulation = Owner.GetNetMode() != NM_Client
		|| Owner.Role != ROLE_SimulatedProxy || Owner.IsLocallyControlled()
		|| Owner.IsDead() || Owner.GetMesh() == nullptr || Owner.IsRagdoll() || Body->IsSimulatingPhysics()
		|| Body->IsPlayingRootMotion() || Body->IsPlayingRootMotionFromEverything();
	if (bTeardown || !Body->IsRegistered() || bPoseReplaced || bClockReset
		|| bNewSimulation
		|| !HasSupportedSiblings(Owner, *Body)
		|| (bDraining && Body->LastPoseTickTime != DrainPoseTime))
	{
		ClearTiming(*Params);
		SavedInputs.Restore(*Params);
		Body->bEnableUpdateRateOptimizations = bSavedEnabled;
		Forget();
		return;
	}
	if (bDraining)
	{
		return;
	}

	const float PendingTime = NCRemoteAnimationURO::GetPendingAnimationTime(Params->TickedPoseOffestTime);
	ApplyPolicy(*Params, false);
	ClearTiming(*Params);
	Body->bEnableUpdateRateOptimizations = false;
	if (PendingTime <= 0.f)
	{
		SavedInputs.Restore(*Params);
		Body->bEnableUpdateRateOptimizations = bSavedEnabled;
		Forget();
		return;
	}
	// In this UE4.15 fork TickPose adds AdditionalTime even with URO disabled.
	// Bypass its manager while lending the pending time to ONE ordinary pose tick.
	// The next pre-movement call observes LastPoseTickTime and clears the loan.
	// This works unchanged when a.URO.Enable is zero or a force-rate is active.
	Params->AdditionalTime = PendingTime;
	DrainPoseTime = Body->LastPoseTickTime;
	DrainWorldTime = World != nullptr ? World->TimeSeconds : 0.f;
	bDraining = true;
	Priority.Reset();
}
