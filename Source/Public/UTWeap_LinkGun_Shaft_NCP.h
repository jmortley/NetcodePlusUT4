#pragma once

#include "NetcodePlus.h"
#include "UTWeap_LinkGun_NCP.h"
#include "UTWeap_LinkGun_Shaft_NCP.generated.h"

/**
 * Beam-only parent for NCShaftArena content.
 *
 * This keeps AUTWeap_LinkGun_NCP's server-authoritative, rewind-capped beam
 * and installs it in both firing-state slots. Primary input therefore fires
 * the Shaft beam instead of plasma; neither input can spawn a projectile.
 * The class does not inherit the legacy CSHD Link Gun or expose its client
 * damage RPCs. Link pull and the stock primary-plasma overheat path are also
 * disabled.
 *
 * The class is intentionally content-free. Reparent an existing complete
 * Shaft Link Blueprint to it so the Blueprint retains its meshes, effects,
 * animations, sounds, and tuned defaults.
 */
UCLASS(Abstract)
class NETCODEPLUS_API AUTWeap_LinkGun_Shaft_NCP : public AUTWeap_LinkGun_NCP
{
	GENERATED_UCLASS_BODY()

public:
	/** Legacy Shaft BP compatibility only; authoritative NCP beam ignores these CSHD tunables. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Legacy Shaft Compatibility")
	float HighPingBeamWidthPadding;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Legacy Shaft Compatibility")
	float MaxHitDistanceTolerance;

	/** Beam states own damage; never fall back to AUTWeapon's projectile shot path. */
	virtual void StartFire(uint8 FireModeNum) override;
	virtual void StopFire(uint8 FireModeNum) override;
	virtual void FireShot() override;
	virtual AUTProjectile* FireProjectile() override;
	virtual void PostInitProperties() override;
	virtual void PostInitializeComponents() override;

	virtual bool SupportsLinkPull() const override { return false; }

protected:
	/** Repair firing-state arrays serialized by a Blueprint's previous parent. */
	void EnsureBeamFiringStates();

	virtual bool UsesPrimaryOverheat() const override { return false; }
};
