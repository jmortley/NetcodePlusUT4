#pragma once

#include "NetcodePlus.h"
#include "UTWeaponStateZooming.h"
#include "UTWeaponStateZoomingFix.generated.h"

UCLASS()
class NETCODEPLUS_API UUTWeaponStateZoomingFix : public UUTWeaponStateZooming
{
	GENERATED_UCLASS_BODY()

    // Override the HUD drawing to fix the Ping calculation
	virtual bool DrawHUD(UUTHUDWidget* WeaponHudWidget) override;
};