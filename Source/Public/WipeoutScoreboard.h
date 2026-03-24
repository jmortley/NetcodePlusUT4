// WipeoutScoreboard — Portrait-row scoreboard for Wipeout game mode
#pragma once

#include "UTTeamScoreboard.h"
#include "WipeoutScoreboard.generated.h"

UCLASS()
class NETCODEPLUS_API UWipeoutScoreboard : public UUTTeamScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	// Portrait atlas icons — same UV coords as the HUD strip
	UPROPERTY()
	FCanvasIcon RedTeamIcon;

	UPROPERTY()
	FCanvasIcon BlueTeamIcon;

	UPROPERTY()
	FCanvasIcon RedTeamOverlay;

	UPROPERTY()
	FCanvasIcon BlueTeamOverlay;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Damage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderKillsX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderDamageX;

protected:
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;

	/** Draw a small portrait pip at the given position. */
	void DrawPortraitPip(AUTPlayerState* PlayerState, float XOffset, float YOffset, float PipWidth, float PipHeight);
};
