// ElimPlusScoreboard — flag-row scoreboard for ElimPlus (TeamArena).
// Mirrors UWipeoutScoreboard structure but with the ElimPlus column set:
//   Name | Kills | Deaths | Damage | PPR(Cur) | PPR(Ovr) | ELO | LG_Acc | BestWpn | Ping
// Reads stats from AElimPlusStatsReplicator (replicated AInfo).
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTTeamScoreboard.h"
#include "ElimPlusScoreboard.generated.h"

class AElimPlusStatsReplicator;

UCLASS()
class NETCODEPLUS_API UElimPlusScoreboard : public UUTTeamScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	/** Decode and upload the recovered Absolute Elim scoreboard textures before
	 *  the scoreboard's draw path needs them. Safe to call repeatedly. */
	static void PreloadAbsoluteTextures();
	/** Release rooted transient artwork during a live module unload. */
	static void ReleaseAbsoluteTextures();

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
	virtual void DrawReadyText(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width) override;

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

private:
	struct FCachedRosterEntry
	{
		TWeakObjectPtr<AUTPlayerState> PlayerState;
		int32 TeamIndex = INDEX_NONE;
		int32 DamageDone = 0;
		int32 KillsAndAssists = 0;
		float Score = 0.f;
		float PPRCurrent = 0.f;
		int32 Elo = 1400;
		int32 EloDeltaThisMatch = 0;
		int32 LinkGunAccuracyTimes100 = -1;
		int32 GlobalRank = 0;

		bool HasSameSortState(const FCachedRosterEntry& Other) const
		{
			return PlayerState.Get() == Other.PlayerState.Get()
				&& TeamIndex == Other.TeamIndex
				&& DamageDone == Other.DamageDone
				&& KillsAndAssists == Other.KillsAndAssists
				&& Score == Other.Score;
		}

		bool HasSameData(const FCachedRosterEntry& Other) const
		{
			return HasSameSortState(Other)
				&& PPRCurrent == Other.PPRCurrent
				&& Elo == Other.Elo
				&& EloDeltaThisMatch == Other.EloDeltaThisMatch
				&& LinkGunAccuracyTimes100 == Other.LinkGunAccuracyTimes100
				&& GlobalRank == Other.GlobalRank;
		}
	};

	UPROPERTY(Transient)
	TWeakObjectPtr<AElimPlusStatsReplicator> CachedStatsReplicator;
	TWeakObjectPtr<UWorld> CachedStatsReplicatorWorld;
	float NextStatsReplicatorSearchTime = 0.f;

	TArray<FCachedRosterEntry> CachedRoster;
	TArray<FCachedRosterEntry> RosterScratch;
	TArray<int32> CachedTeamRosterIndices[2];
	TMap<const AUTPlayerState*, int32> CachedRosterIndexByPlayer;
	TArray<FString> CachedSpectatorNames;

	AElimPlusStatsReplicator* FindStatsReplicator();
	void UpdateCachedRoster(AElimPlusStatsReplicator* StatsReplicator);
	const FCachedRosterEntry* FindCachedRosterEntry(const AUTPlayerState* PlayerState) const;
	void ResetCachedRoster();
};
