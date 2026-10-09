#pragma once

#include "NetcodePlus.h"
#include "UTHUDWidget.h"
#include "UTScoreboard.h"
#include "UTHUDWidget_SpectatorSlideOut.h"
#include "NCPlusXTDMSpectator.generated.h"

/** Reuses only stock spectator-window/input lifecycle, never its two-team roster. */
UCLASS()
class NETCODEPLUS_API UNCPlusXTDMSpectator : public UUTHUDWidget_SpectatorSlideOut
{
	GENERATED_UCLASS_BODY()
public:
	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool MouseClick(FVector2D Position) override;
	virtual void TrackMouseMovement(FVector2D Position) override { PointerPosition = Position; }
	virtual void SetMouseInteractive(bool bInteractive) override { bPointerInteractive = bInteractive; }
private:
	TArray<FSelectionObject> PlayerHits;
	FVector2D PointerPosition;
	FVector4 ToggleBounds;
	bool bPointerInteractive = false;
};
