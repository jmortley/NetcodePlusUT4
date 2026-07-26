// ElimPlusScoreboard — flag-row scoreboard for ElimPlus (TeamArena).
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

	/** Country flag used by the normal and Absolute Elim scoreboard rows. */
	void DrawPlayerFlag(AUTPlayerState* PlayerState, float XOffset, float YOffset,
		float FlagWidth, float FlagHeight, float Opacity = 1.f);

	/** Recovered Elimination 1.13 scoreboard path selected with the existing
	 *  Absolute Elim 113 top-panel toggle. */
	bool ShouldDrawAbsoluteElimScoreboard() const;
	void DrawAbsoluteTeamPanel(float RenderDelta, float& YOffset);
	void DrawAbsoluteScoreHeaders(float RenderDelta, float& YOffset);
	void DrawAbsolutePlayerScores(float RenderDelta, float& YOffset);
	void DrawAbsolutePlayer(AUTPlayerState* PlayerState, int32 TeamIndex,
		float XOffset, float YOffset, float AbsoluteScale);
};
