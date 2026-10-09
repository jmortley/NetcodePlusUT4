#pragma once

#include "NetcodePlus.h"
#include "UTHUD.h"
#include "NCAimTrainerHUD.generated.h"

class ANCAimTrainerPlayerController;
struct FNCAimTrainerProgress;

/** Asset-free trainer interface; the practice targets themselves are real UT characters. */
UCLASS()
class NETCODEPLUS_API ANCAimTrainerHUD : public AUTHUD
{
	GENERATED_BODY()

public:
	ANCAimTrainerHUD(const FObjectInitializer& ObjectInitializer);
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual bool ScoreboardIsUp() override;
	virtual bool ShouldDrawMinimap() override;
	virtual EInputMode::Type GetInputMode_Implementation() const override;
	virtual bool OverrideMouseClick(FKey Key, EInputEvent EventType) override;

private:
	struct FTrainerButton
	{
		FVector2D Min;
		FVector2D Max;
		int32 Action;
	};
	TArray<FTrainerButton> TrainerButtons;
	float TrainerScale = 1.f;
	FVector2D TrainerOrigin = FVector2D::ZeroVector;

	void Panel(float X, float Y, float W, float H, const FLinearColor& Color);
	void Label(const FString& Text, float X, float Y, float Size, const FLinearColor& Color,
		float MaxWidth = 0.f, bool bCentered = false);
	void Button(int32 Action, const FString& Text, float X, float Y, float W, float H, bool bSelected);
	bool IsHovered(float X, float Y, float W, float H) const;
	void DrawSessionHeader(const FNCAimTrainerProgress& Progress);
	void DrawModePicker(ANCAimTrainerPlayerController* PC);
	void DrawResults(ANCAimTrainerPlayerController* PC);
	void DrawLeaderboard(ANCAimTrainerPlayerController* PC, float Y);
};
