// ShockDomHUD — Wipeout-based HUD with control point indicators for Domination
#pragma once
#include "NetcodePlus.h"
#include "WipeoutHUD.h"
#include "ShockDomHUD.generated.h"

class AShockDomControlPoint;

UCLASS()
class NETCODEPLUS_API AShockDomHUD : public AWipeoutHUD
{
	GENERATED_UCLASS_BODY()

	virtual void DrawHUD() override;

protected:
	void DrawControlPointIndicators(const TArray<AShockDomControlPoint*>& Points);
	void DrawControlPointWorldMarkers(const TArray<AShockDomControlPoint*>& Points);
	void DrawWorldMarker(const FVector& WorldLoc, const FString& Label, const FLinearColor& Color, float RenderScale);
};
