// UTWeap_ShaftLink.cpp — both fire modes route into the beam (shaft) path.

#include "UTWeap_ShaftLink.h"
#include "UnrealTournament.h"
#include "UTWeaponStateFiringLinkBeamPlus.h"

AUTWeap_ShaftLink::AUTWeap_ShaftLink(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Mirror Mode 1's beam tuning into Mode 0 so primary fires the same beam.
	// AUTWeap_LinkGun_Plus already sets FireInterval[1]=0.12 and
	// InstantHitInfo[0].Damage=9 / TraceRange=2200; we keep the same numbers
	// across modes for consistent shaft feel.
	if (FireInterval.Num() >= 2)
	{
		FireInterval[0] = FireInterval[1];
	}
	if (Spread.Num() >= 2)
	{
		Spread[0] = Spread[1];
	}
	if (AmmoCost.Num() >= 2)
	{
		AmmoCost[0] = AmmoCost[1];
	}
}

void AUTWeap_ShaftLink::BeginPlay()
{
	Super::BeginPlay();

	// Replace the primary-fire state (default plasma) with a beam state so
	// LMB enters the same firing-state path as RMB. The CSHD damage pipeline
	// in AUTWeap_LinkGun_Plus::ProcessClientSideHit /
	// ServerProcessBeamHit operates on UUTWeaponStateFiringLinkBeamPlus, so
	// pointing both modes at this state keeps the validated-hit path live.
	if (FiringState.IsValidIndex(0) && FiringState.IsValidIndex(1) && FiringState[1])
	{
		UUTWeaponStateFiringLinkBeamPlus* PrimaryBeam = NewObject<UUTWeaponStateFiringLinkBeamPlus>(
			this, UUTWeaponStateFiringLinkBeamPlus::StaticClass(),
			TEXT("ShaftLinkPrimaryBeamState"));
		if (PrimaryBeam)
		{
			FiringState[0] = PrimaryBeam;
		}
	}
}

void AUTWeap_ShaftLink::FireShot()
{
	// Both modes route through the AUTWeapon::FireShot path (skipping
	// AUTWeaponFix's gatekeeper for projectile-style transactional firing,
	// matching what AUTWeap_LinkGun_Plus does for mode 1).
	AUTWeapon::FireShot();
}

void AUTWeap_ShaftLink::StartFire(uint8 FireModeNum)
{
	// Defer to parent — with FiringState[0] now pointing at LinkBeamPlus, the
	// state machine handles both modes uniformly.
	Super::StartFire(FireModeNum);
}

AUTProjectile* AUTWeap_ShaftLink::FireProjectile()
{
	// Hard-block plasma. Even if some path leaks back into FireProjectile
	// (e.g. bot logic or alt firing-state quirks), nothing spawns.
	return nullptr;
}
