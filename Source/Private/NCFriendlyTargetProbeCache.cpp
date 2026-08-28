#include "NCFriendlyTargetProbeCache.h"

#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"

static TAutoConsoleVariable<float> CVarFriendlyTargetProbeHz(
	TEXT("ncp.FriendlyTargetProbeHz"), 240.0f,
	TEXT("Client-only crosshair friendly/name probe rate. Stock UT performs one 50,000-unit complex trace per rendered frame. ")
	TEXT("Default 240 uses a 4.2ms cache window; the first rendered frame after expiry refreshes it, cutting duplicate work at 500+ FPS. ")
	TEXT("Range 30-1000 Hz; 0 restores stock every-frame probing. Does not affect firing, aim, input, or hit validation."),
	ECVF_Default);

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
	const double Now = FPlatformTime::Seconds();
	NextProbeTimeSeconds = AdvanceFriendlyProbeDeadline(Now, NextProbeTimeSeconds,
		GetFriendlyTargetProbeIntervalSeconds());
}
