#pragma once

#include "CoreMinimal.h"

class AUTWeaponFix;

/**
 * Client-only, observational shock-primary input trace.
 *
 * The implementation correlates a Windows legacy LMB message, UE PlayerInput,
 * AUTPlayerController's action/deferred queue, AUTWeaponFix::StartFire, and the
 * eventual FireShot. It never consumes input and never changes weapon state.
 */
namespace NCShockInputTrace
{
	int32 GetMode();
	void Start(AUTWeaponFix* Weapon);
	void Stop(AUTWeaponFix* Weapon);
	void Tick(AUTWeaponFix* Weapon);

	void RecordPlayerInput(AUTWeaponFix* Weapon, bool bPressed);
	void RecordAction(AUTWeaponFix* Weapon, bool bPressed, bool bQueueTailMatched,
		bool bQueueEvidenceConclusive, int32 QueueDepth);
	void RecordWeaponStart(AUTWeaponFix* Weapon, bool bRetry,
		bool bExpectedImmediateShot, float ReadyInMs, FName StateName);
	void RecordWeaponStop(AUTWeaponFix* Weapon, bool bInternal, FName StateName);
	void RecordFireShot(AUTWeaponFix* Weapon, FName StateName);
}
