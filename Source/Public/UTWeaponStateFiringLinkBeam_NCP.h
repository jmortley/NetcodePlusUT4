#pragma once

#include "NetcodePlus.h"
#include "UTWeaponStateFiringBeam.h"
#include "UTWeaponStateFiringLinkBeam_NCP.generated.h"

/** Stock Link beam state retargeted to AUTWeap_LinkGun_NCP. */
UCLASS()
class NETCODEPLUS_API UUTWeaponStateFiringLinkBeam_NCP : public UUTWeaponStateFiringBeam
{
	GENERATED_UCLASS_BODY()

	UPROPERTY()
	float AccumulatedFiringTime;

	UPROPERTY()
	bool bPendingEndFire;

	UPROPERTY()
	bool bPendingStartFire;

	virtual void BeginState(const UUTWeaponState* PrevState) override;
	virtual void RefireCheckTimer() override;
	virtual void FireShot() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndState() override;
	virtual void EndFiringSequence(uint8 FireModeNum) override;
	virtual void PendingFireStarted() override;
};
