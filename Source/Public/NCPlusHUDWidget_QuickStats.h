// Modernized minimal-typography health/armor widget for NetcodePlus modes.
// Replaces stock bpHW_QuickStats. Clean white numbers with green/yellow accent
// labels + underlines. Damage flash, pickup pulse, low-HP red tint.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_QuickStats.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_QuickStats : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;

private:
	int32 LastHealth;
	int32 LastArmor;
	float HealthDamageFlashEnd;
	float ArmorDamageFlashEnd;
	float HealthPickupPulseEnd;
	float ArmorPickupPulseEnd;
};
