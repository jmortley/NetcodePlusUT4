// UTWeap_ShaftLink — link gun where BOTH fire modes act as the beam (shaft).
// Used by NCShaftArena. Inherits AUTWeap_LinkGun_Plus, so all the CSHD
// (client-side hit detection) plumbing for the beam comes along for free.
//
// Key change: we point FiringState[0] at a UUTWeaponStateFiringLinkBeamPlus
// instance so primary fire enters the same beam state as secondary. The plasma
// projectile path is suppressed (FireProjectile returns nullptr in mode 0
// just in case).
//
// IMPORTANT — fire-mode mapping in AUTWeap_LinkGun_Plus:
//   Mode 0 = plasma projectile (default behavior we override here)
//   Mode 1 = beam / shaft       (default behavior we keep)
#pragma once

#include "NetcodePlus.h"
#include "UTWeap_LinkGun_Plus.h"
#include "UTWeap_ShaftLink.generated.h"

UCLASS(Blueprintable)
class NETCODEPLUS_API AUTWeap_ShaftLink : public AUTWeap_LinkGun_Plus
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void FireShot() override;
	virtual void StartFire(uint8 FireModeNum) override;
	virtual class AUTProjectile* FireProjectile() override;
};
