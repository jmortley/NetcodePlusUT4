// NCPlusHUDWidget_Minimap — optional in-match minimap. Default-hidden.
// Wraps the engine's UTHUD::DrawMinimap so the existing world-bounds /
// render-to-texture pipeline is reused; this widget only owns position +
// size + visibility through the nchud editor.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_Minimap.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_Minimap : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;
};
