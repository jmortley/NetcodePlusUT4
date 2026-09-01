#include "NCFriendlyTargetProbeCache.h"
#include "NCFriendlyTargetProbeCacheInternal.h"

#include "Camera/PlayerCameraManager.h"
#include "EngineGlobals.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"

static TAutoConsoleVariable<float> CVarFriendlyTargetProbeHz(
	TEXT("ncp.FriendlyTargetProbeHz"), 240.0f,
	TEXT("Client-only crosshair friendly/name probe rate. Stock UT performs one 50,000-unit complex trace per rendered frame. ")
	TEXT("Default 240 uses a 4.2ms cache window during normal aiming; the first rendered frame after expiry refreshes it, cutting duplicate work at 500+ FPS. ")
	TEXT("Viewer/owner changes, camera cuts, view-target changes, and large camera discontinuities refresh immediately. ")
	TEXT("Range 30-1000 Hz; 0 restores stock every-frame probing. Does not affect firing, aim, input, or hit validation."),
	ECVF_Default);

namespace
{
	bool GetFriendlyProbeViewPose(APlayerController* Viewer,
		FVector& OutViewLocation, FRotator& OutViewRotation,
		AActor*& OutViewTarget, bool& bOutCameraCut)
	{
		APlayerCameraManager* CameraManager = Viewer ? Viewer->PlayerCameraManager : nullptr;
		if (!CameraManager || CameraManager->CameraCache.TimeStamp <= 0.f)
		{
			return false;
		}
		// This is the same cached POV used by APlayerController::GetPlayerViewPoint
		// for a normal local HUD, without recomputing the view after Super traced it.
		CameraManager->GetCameraViewPoint(OutViewLocation, OutViewRotation);
		OutViewTarget = CameraManager->GetViewTarget();
		bOutCameraCut = CameraManager->bGameCameraCutThisFrame;
		return true;
	}
}

static FORCEINLINE float GetFriendlyTargetProbeIntervalSeconds()
{
	const float ProbeHz = CVarFriendlyTargetProbeHz.GetValueOnGameThread();
	return ProbeHz > 0.0f ? (1.0f / FMath::Clamp(ProbeHz, 30.0f, 1000.0f)) : 0.0f;
}

bool FNCFriendlyTargetProbeCache::TryReuse(APlayerController* Viewer,
	AUTCharacter* WeaponOwner, AUTPlayerState*& OutHitPlayerState,
	bool& bOutDrawIndicator) const
{
	const float ProbeInterval = GetFriendlyTargetProbeIntervalSeconds();
	if (!NCFriendlyTargetProbeCachePrivate::IsProbeIntervalReusable(
			ProbeInterval, CachedProbeIntervalSeconds) ||
		Viewer == nullptr || WeaponOwner == nullptr ||
		CachedViewer.Get() != Viewer ||
		CachedCameraManager.Get() != Viewer->PlayerCameraManager ||
		CachedWeaponOwner.Get() != WeaponOwner ||
		(bCachedHadHitPlayerState && !CachedHitPlayerState.IsValid()) ||
		(bCachedHadViewTarget && !CachedViewTarget.IsValid()) ||
		FPlatformTime::Seconds() >= NextProbeTimeSeconds)
	{
		return false;
	}

	// Ordinary camera motion is expected inside the bounded cache window. Only
	// explicit camera cuts, view-target changes, or discontinuity-sized pose
	// changes bypass the cap and force an immediate presentation refresh.
	FVector ViewLocation;
	FRotator ViewRotation;
	AActor* ViewTarget = nullptr;
	bool bCameraCut = false;
	if (!bCachedViewPoseValid ||
		!GetFriendlyProbeViewPose(Viewer, ViewLocation, ViewRotation,
			ViewTarget, bCameraCut) ||
		(bCameraCut && CachedProbeFrame != GFrameCounter) ||
		ViewTarget != CachedViewTarget.Get() ||
		NCFriendlyTargetProbeCachePrivate::IsMeaningfulCameraDiscontinuity(
			CachedViewLocation, CachedViewRotation, ViewLocation, ViewRotation))
	{
		return false;
	}

	OutHitPlayerState = bCachedHadHitPlayerState ? CachedHitPlayerState.Get() : nullptr;
	bOutDrawIndicator = bCachedDrawIndicator;
	return true;
}

void FNCFriendlyTargetProbeCache::Store(APlayerController* Viewer,
	AUTCharacter* WeaponOwner, AUTPlayerState* HitPlayerState,
	bool bDrawIndicator) const
{
	CachedViewer = Viewer;
	CachedCameraManager = Viewer ? Viewer->PlayerCameraManager : nullptr;
	CachedWeaponOwner = WeaponOwner;
	CachedHitPlayerState = HitPlayerState;
	bCachedHadHitPlayerState = HitPlayerState != nullptr;
	bCachedDrawIndicator = bDrawIndicator;
	// Super::ShouldDrawFFIndicator() just traced from this cached POV. Retain its
	// camera identity so real cuts can bypass the otherwise hard time cap.
	AActor* ViewTarget = nullptr;
	bool bCameraCut = false;
	bCachedViewPoseValid = GetFriendlyProbeViewPose(Viewer,
		CachedViewLocation, CachedViewRotation, ViewTarget, bCameraCut);
	CachedViewTarget = ViewTarget;
	bCachedHadViewTarget = ViewTarget != nullptr;
	CachedProbeFrame = GFrameCounter;
	const double Now = FPlatformTime::Seconds();
	CachedProbeIntervalSeconds = GetFriendlyTargetProbeIntervalSeconds();
	NextProbeTimeSeconds = NCFriendlyTargetProbeCachePrivate::AdvanceProbeDeadline(
		Now, NextProbeTimeSeconds, CachedProbeIntervalSeconds);
}
