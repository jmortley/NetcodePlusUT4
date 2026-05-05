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

	/** Override DrawPlayer to skip the parent's Skill/Ping draw at the row's
	 *  right edge. Duel doesn't show ping/skill - the rightmost data column
	 *  is the armor-pickup row instead, drawn from DrawPlayerScore. */
	virtual void DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
		float RenderDelta, float XOffset, float YOffset) override;

	/** Draw a 4-icon row (Belt / Vest / Pads / Helmet) centered at CenterX.
	 *  If Counts is non-null, the corresponding count is drawn below each
	 *  icon (per-player row). If null, only the icons render (column header).
	 *  Counts ordering: [Belt, Vest, Pads, Helmet]. */
	void DrawArmorIconRow(float CenterX, float CenterY, const uint8* Counts, FLinearColor IconTint);
};
