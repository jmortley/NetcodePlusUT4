// WipeoutScoreboard — Portrait-row scoreboard for Wipeout game mode
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
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
	FText CH_KD;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_BeltAmp;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Damage;

	/** Retained for subclasses only — Wipeout itself no longer draws an efficiency
	 *  column. NCShaftArena, NCLeagueDuel and ShockDom derive from this scoreboard
	 *  and reuse this text/slot (ShockDom as real efficiency; the others repurpose
	 *  the position for DMG and the armour icon row). Do not remove. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Efficiency;

	/** Vest / Siphon pickup counts, same "a/b" form as CH_BeltAmp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_VestSiphon;

	/** HP restored to teammates this match. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Heal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_DmgPerLife;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderKDX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderBeltAmpX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderDamageX;

	/** Retained for subclasses only — see CH_Efficiency above. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderEfficiencyX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderVestSiphonX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderHealX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderDmgPerLifeX;

protected:
	virtual void DrawTeamPanel(float RenderDelta, float& YOffset) override;
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScores(float RenderDelta, float& DrawY) override;
	virtual void DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;
	virtual void DrawReadyText(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width) override;

	/** Check if teams have non-standard colors (TeamSkins) */
	bool HasCustomTeamColors() const;

	/** Draw a small portrait pip at the given position. */
	void DrawPortraitPip(AUTPlayerState* PlayerState, float XOffset, float YOffset, float PipWidth, float PipHeight);
};
