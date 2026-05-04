// NCPlusHUDWidget_Accuracy — live shaft accuracy readout. Used by NCShaftArena
// (and trivially droppable into other modes via the nchud editor under the
// "accuracy" alias). Draws "84%" big and centered; reads NAME_LinkHits /
// NAME_LinkShots off the local AUTPlayerState every frame.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_Accuracy.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_Accuracy : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;
};
