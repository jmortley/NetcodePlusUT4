#include "NCFriendlyTargetProbeCache.h"

#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"

static TAutoConsoleVariable<float> CVarFriendlyTargetProbeHz(
	TEXT("ncp.FriendlyTargetProbeHz"), 240.0f,
	TEXT("Client-only crosshair friendly/name probe rate. Stock UT performs one 50,000-unit complex trace per rendered frame. ")
	TEXT("Default 240 uses a 4.2ms cache window while the camera pose is unchanged; the first rendered frame after expiry refreshes it, cutting duplicate work at 500+ FPS. ")
	TEXT("Range 30-1000 Hz; 0 restores stock every-frame probing. Does not affect firing, aim, input, or hit validation."),
	ECVF_Default);

namespace
{
	bool GetFriendlyProbeViewPose(APlayerController* Viewer,
		FVector& OutViewLocation, FRotator& OutViewRotation)
	{
		APlayerCameraManager* CameraManager = Viewer ? Viewer->PlayerCameraManager : nullptr;
		if (!CameraManager || CameraManager->CameraCache.TimeStamp <= 0.f)
		{
			return false;
		}
		// This is the same cached POV used by APlayerController::GetPlayerViewPoint
		// for a normal local HUD, without recomputing the view after Super traced it.
		CameraManager->GetCameraViewPoint(OutViewLocation, OutViewRotation);
		return true;
	}
}

static FORCEINLINE float GetFriendlyTargetProbeIntervalSeconds()
{
	const float ProbeHz = CVarFriendlyTargetProbeHz.GetValueOnGameThread();
	return ProbeHz > 0.0f ? (1.0f / FMath::Clamp(ProbeHz, 30.0f, 1000.0f)) : 0.0f;
}

static FORCEINLINE double AdvanceFriendlyProbeDeadline(double Now,
	double Deadline, double Interval)
{
	if (Interval <= 0.0) return Now;
	if (Deadline <= 0.0 || Deadline > Now || Now - Deadline > 4.0 * Interval)
	{
		return Now + Interval;
	}
	// Preserve the configured average rate at 500+ FPS without issuing a run of
	// catch-up traces after a hitch. Advancing from Now would alias 240 Hz down to
	// one trace every three 500 Hz frames (~167 Hz).
	const int32 PeriodsToSkip = FMath::FloorToInt(float((Now - Deadline) / Interval)) + 1;
	return Deadline + double(PeriodsToSkip) * Interval;
}

bool FNCFriendlyTargetProbeCache::TryReuse(APlayerController* Viewer,
	AUTCharacter* WeaponOwner, AUTPlayerState*& OutHitPlayerState,
	bool& bOutDrawIndicator) const
{
	const float ProbeInterval = GetFriendlyTargetProbeIntervalSeconds();
	if (ProbeInterval <= 0.0f || Viewer == nullptr || WeaponOwner == nullptr ||
		CachedViewer.Get() != Viewer || CachedWeaponOwner.Get() != WeaponOwner ||
		(bCachedHadHitPlayerState && !CachedHitPlayerState.IsValid()) ||
		FPlatformTime::Seconds() >= NextProbeTimeSeconds)
	{
		return false;
	}

	// Only inspect the cached POV on a frame that would otherwise reuse the
	// trace. A flick or camera translation must receive a fresh trace even
	// inside the 4.2 ms cache window.
	FVector ViewLocation;
	FRotator ViewRotation;
	if (!bCachedViewPoseValid ||
		!GetFriendlyProbeViewPose(Viewer, ViewLocation, ViewRotation) ||
		ViewLocation != CachedViewLocation ||
		!ViewRotation.Equals(CachedViewRotation, 0.f))
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
	CachedWeaponOwner = WeaponOwner;
	CachedHitPlayerState = HitPlayerState;
	bCachedHadHitPlayerState = HitPlayerState != nullptr;
	bCachedDrawIndicator = bDrawIndicator;
	// Super::ShouldDrawFFIndicator() just traced from this cached POV. Reuse is
	// valid only while the camera location and rotation remain exactly unchanged.
	bCachedViewPoseValid = GetFriendlyProbeViewPose(Viewer,
		CachedViewLocation, CachedViewRotation);
	const double Now = FPlatformTime::Seconds();
	NextProbeTimeSeconds = AdvanceFriendlyProbeDeadline(Now, NextProbeTimeSeconds,
		GetFriendlyTargetProbeIntervalSeconds());
}
