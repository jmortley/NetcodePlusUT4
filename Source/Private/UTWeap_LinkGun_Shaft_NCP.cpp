#include "UTWeap_LinkGun_Shaft_NCP.h"
#include "UTWeaponStateFiringLinkBeam_NCP.h"

AUTWeap_LinkGun_Shaft_NCP::AUTWeap_LinkGun_Shaft_NCP(const FObjectInitializer& OI)
	: Super(OI.SetDefaultSubobjectClass<UUTWeaponStateFiringLinkBeam_NCP>(TEXT("FiringState0")))
{
	// AUTWeap_LinkGun_NCP replaces FiringState1. Replacing FiringState0 here
	// converts primary from plasma to the same authoritative Shaft beam.
	OverheatFactor = 0.f;
	bIsInCoolDown = false;
	HighPingBeamWidthPadding = 0.f;
	MaxHitDistanceTolerance = 0.f;
}

void AUTWeap_LinkGun_Shaft_NCP::EnsureBeamFiringStates()
{
	// A reparented Blueprint serializes its inherited FiringState array. That
	// serialized data wins over SetDefaultSubobjectClass() and can leave the
	// legacy UUTWeaponStateFiringLinkBeamPlus objects in both slots. Those
	// states require AUTWeap_LinkGun_Plus as their owner, so with this NCP
	// parent they only toggle looping audio and never trace, damage, or render
	// the beam. Replace both slots after Blueprint defaults have been applied.
	for (int32 FireMode = 0; FireMode < 2 && FireMode < FiringState.Num(); ++FireMode)
	{
		if (FiringState[FireMode] == nullptr ||
			!FiringState[FireMode]->IsA(UUTWeaponStateFiringLinkBeam_NCP::StaticClass()))
		{
			FiringState[FireMode] = NewObject<UUTWeaponStateFiringLinkBeam_NCP>(
				this, UUTWeaponStateFiringLinkBeam_NCP::StaticClass());
		}
	}
}

void AUTWeap_LinkGun_Shaft_NCP::PostInitProperties()
{
	Super::PostInitProperties();
	EnsureBeamFiringStates();
}

void AUTWeap_LinkGun_Shaft_NCP::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// PostInitProperties repairs spawned instances; this second guarded pass
	// also covers loaded Blueprint actors whose serialized CDO data arrived
	// later in the UObject load lifecycle.
	EnsureBeamFiringStates();

	// Blueprint defaults are applied after the native constructor. Normalize
	// primary here so a reparented stock/legacy asset cannot retain plasma-era
	// mode-zero damage, cadence, ammo, or projectile data. Mode 1 is the proven
	// authored Shaft beam and remains the single source of those values.
	if (InstantHitInfo.IsValidIndex(0) && InstantHitInfo.IsValidIndex(1))
	{
		InstantHitInfo[0] = InstantHitInfo[1];
	}
	if (FireInterval.IsValidIndex(0) && FireInterval.IsValidIndex(1))
	{
		FireInterval[0] = FireInterval[1];
	}
	if (AmmoCost.IsValidIndex(0) && AmmoCost.IsValidIndex(1))
	{
		AmmoCost[0] = AmmoCost[1];
	}
	if (ProjClass.IsValidIndex(0))
	{
		ProjClass[0] = nullptr;
	}
}

void AUTWeap_LinkGun_Shaft_NCP::StartFire(uint8 FireModeNum)
{
	// The reparented Shaft Blueprint authored all persistent beam cosmetics in
	// mode 1. Treat either input as mode 1 so primary uses that same beam actor,
	// muzzle effect, looping sound, animations, and replicated effect mode.
	AUTWeap_LinkGun_NCP::StartFire(FireModeNum == 0 ? 1 : FireModeNum);
}

void AUTWeap_LinkGun_Shaft_NCP::StopFire(uint8 FireModeNum)
{
	// Match StartFire's canonical mode so pending-fire state and the stop RPC
	// release the beam that was actually started.
	AUTWeap_LinkGun_NCP::StopFire(FireModeNum == 0 ? 1 : FireModeNum);
}

void AUTWeap_LinkGun_Shaft_NCP::FireShot()
{
	// UUTWeaponStateFiringLinkBeam_NCP owns continuous beam damage, effects,
	// and ammo. A direct/stale FireShot call must not reach the stock plasma
	// path inherited from AUTWeap_LinkGun_NCP.
}

AUTProjectile* AUTWeap_LinkGun_Shaft_NCP::FireProjectile()
{
	return nullptr;
}
