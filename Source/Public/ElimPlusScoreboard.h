// ElimPlusScoreboard — Portrait-row scoreboard for ElimPlus (TeamArena).
// Mirrors UWipeoutScoreboard structure but with the ElimPlus column set:
//   Name | Kills | Deaths | Damage | PPR(Cur) | PPR(Ovr) | ELO | LG_Acc | BestWpn | Ping
// Reads stats from AElimPlusStatsReplicator (replicated AInfo).
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTTeamScoreboard.h"
#include "ElimPlusScoreboard.generated.h"

UCLASS()
class NETCODEPLUS_API UElimPlusScoreboard : public UUTTeamScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	// Portrait atlas icons — same UV coords as the HUD strip (assigned at draw time)
	UPROPERTY()
	FCanvasIcon RedTeamIcon;

	UPROPERTY()
	FCanvasIcon BlueTeamIcon;

	UPROPERTY()
	FCanvasIcon RedTeamOverlay;

	UPROPERTY()
	FCanvasIcon BlueTeamOverlay;

	// Column header texts (CH_Kills, CH_Deaths, CH_PlayerName, CH_Score, CH_Skill,
	// CH_Ping are inherited from UUTScoreboard — set their values in the ctor.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Damage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_PPRCur;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Elo;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_LGAcc;

	// Column header X positions (fraction of CellWidth)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderKillsX;

	// ColumnHeaderDeathsX is inherited from UUTScoreboard — set value in ctor.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderDamageX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderPPRCurX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderEloX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	float ColumnHeaderLGAccX;

protected:
	virtual void DrawTeamPanel(float RenderDelta, float& YOffset) override;
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScores(float RenderDelta, float& DrawY) override;
	virtual void DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;

	/** True when team colors are non-standard (TeamSkins active). */
	bool HasCustomTeamColors() const;

	/** Small portrait pip on the scoreboard row. */
	void DrawPortraitPip(AUTPlayerState* PlayerState, float XOffset, float YOffset, float PipWidth, float PipHeight);
};
