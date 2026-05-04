// NCLeagueDuelScoreboard — duel-specific columns. Replaces the BeltAmp column
// with combined hitscan accuracy (LG + Shock + Sniper) — duel-relevant signal.
#pragma once

#include "NetcodePlus.h"
#include "WipeoutScoreboard.h"
#include "NCLeagueDuelScoreboard.generated.h"

UCLASS()
class NETCODEPLUS_API UNCLeagueDuelScoreboard : public UWipeoutScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Accuracy;

protected:
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset,
		float YOffset, float Width, FLinearColor DrawColor) override;
};
