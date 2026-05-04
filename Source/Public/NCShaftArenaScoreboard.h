// NCShaftArenaScoreboard — FFA scoreboard for NCShaftArena. Built on
// UWipeoutScoreboard purely to inherit its column-position fields and
// portrait/atlas plumbing; DrawTeamPanel is overridden to render FFA-flat
// (no team grouping). Columns: Kills, Accuracy %, Best Streak, Damage.
#pragma once

#include "NetcodePlus.h"
#include "WipeoutScoreboard.h"
#include "NCShaftArenaScoreboard.generated.h"

UCLASS()
class NETCODEPLUS_API UNCShaftArenaScoreboard : public UWipeoutScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Accuracy;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Streak;

protected:
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset,
		float YOffset, float Width, FLinearColor DrawColor) override;
};
