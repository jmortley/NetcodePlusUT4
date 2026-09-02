#pragma once

#include "NetcodePlus.h"

class APlayerController;
class APlayerCameraManager;
class AActor;
class AUTCharacter;
class AUTPlayerState;

/**
 * Tiny client-presentation cache for AUTWeapon::ShouldDrawFFIndicator(). Stock UT
 * performs a 50,000-unit complex trace every rendered frame; at 500+ FPS that is
 * hundreds of identical visual-only probes per second. This cache never participates
 * in firing, aim, movement, hit validation, or replication.
 */
struct NETCODEPLUS_API FNCFriendlyTargetProbeCache
{
	bool TryReuse(APlayerController* Viewer, AUTCharacter* WeaponOwner,
		AUTPlayerState*& OutHitPlayerState, bool& bOutDrawIndicator) const;
	void Store(APlayerController* Viewer, AUTCharacter* WeaponOwner,
		AUTPlayerState* HitPlayerState, bool bDrawIndicator) const;

private:
	mutable TWeakObjectPtr<APlayerController> CachedViewer;
	mutable TWeakObjectPtr<APlayerCameraManager> CachedCameraManager;
	mutable TWeakObjectPtr<AUTCharacter> CachedWeaponOwner;
	mutable TWeakObjectPtr<AUTPlayerState> CachedHitPlayerState;
	mutable TWeakObjectPtr<AActor> CachedViewTarget;
	mutable FVector CachedViewLocation = FVector::ZeroVector;
	mutable FRotator CachedViewRotation = FRotator::ZeroRotator;
	mutable double NextProbeTimeSeconds = 0.0;
	mutable float CachedProbeIntervalSeconds = 0.0f;
	mutable uint64 CachedProbeFrame = MAX_uint64;
	mutable bool bCachedDrawIndicator = false;
	mutable bool bCachedHadHitPlayerState = false;
	mutable bool bCachedHadViewTarget = false;
	mutable bool bCachedViewPoseValid = false;
};
