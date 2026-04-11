// NCPlusCTFScoreboard.h — Custom CTF scoreboard with K/D/Eff/Acc/C/G/R/Ping.
// Accuracy auto-detects instagib vs LG+Sniper.

#pragma once

#include "NetcodePlus.h"
#include "UTCTFScoreboard.h"
#include "NCPlusCTFScoreboard.generated.h"

class ACTFStatsReplicator;

UCLASS()
class NETCODEPLUS_API UNCPlusCTFScoreboard : public UUTCTFScoreboard
{
	GENERATED_UCLASS_BODY()

protected:
	// Column X positions (fraction of CellWidth)
	float ColumnHeaderKillsX;
	float ColumnHeaderDeathsX;
	float ColumnHeaderEffX;
	float ColumnHeaderAccX;
	float ColumnHeaderCapsX2;
	float ColumnHeaderGrabsX;
	float ColumnHeaderReturnsX2;
	float ColumnHeaderPingX;

	// Column header texts
	FText CH_Kills;
	FText CH_Deaths;
	FText CH_Eff;
	FText CH_Acc;
	FText CH_Grabs;

	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;
	virtual void DrawPlayerScores(float RenderDelta, float& YOffset) override;

private:
	/** Cached replicator reference (found once, reused) */
	UPROPERTY()
	ACTFStatsReplicator* CachedStatsRep = nullptr;

	ACTFStatsReplicator* FindStatsReplicator();
};
