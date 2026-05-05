// NCPlusHUDWidget_Speedometer — horizontal-speed readout under the crosshair.
// Default-hidden (no seeded layout entry). Add via the nchud editor to enable.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_Speedometer.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_Speedometer : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;
};
