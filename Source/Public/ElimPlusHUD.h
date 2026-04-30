// ElimPlusHUD — FlagRun-style portrait strip for ElimPlus (TeamArena) game mode.
// Mirror of AWipeoutHUD with elim-specific adaptations:
//   - No respawn countdown overlay on dead portraits (no mid-round respawns)
//   - No next-to-spawn gold border (irrelevant in elim)
//   - Always-dim overlay on dead portraits (no sweep animation)
//   - Optional ELO chip per portrait (read from ElimPlusStatsReplicator)
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "ElimPlusHUD.generated.h"

UCLASS()
class NETCODEPLUS_API AElimPlusHUD : public AUTHUD
{
	GENERATED_UCLASS_BODY()

	virtual void DrawHUD() override;
	virtual FLinearColor GetBaseHUDColor() override;

	// Portrait atlas icons — same UV coords as AUTFlagRunHUD / AWipeoutHUD
	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon RedTeamIcon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon BlueTeamIcon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon RedTeamOverlay;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon BlueTeamOverlay;

	int32 RedPlayerCount;
	int32 BluePlayerCount;

	virtual void DrawPlayerIcon(AUTPlayerState* PlayerState, bool bPlayerAlive, float XOffset, float YOffset, float IconSize);
	virtual void GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers);

	/** Custom team score bar — dynamic team colors via TeamSkins, plus round clock. */
	virtual void DrawTeamScoreBar(AUTGameState* GS);

	/** Force game-only input when dead but match in progress — prevents mouse escaping viewport */
	virtual EInputMode::Type GetInputMode_Implementation() const override;

	/** Take high-res screenshot when match ends (if enabled in NCP settings) */
	virtual void NotifyMatchStateChange() override;

private:
	bool bPostMatchScreenshotTaken = false;
	bool bNCPScreenshotEnabled = true;

	/** Per-player ELO chip animation state. Triggered the first frame the
	 *  replicator's EloDeltaThisMatch transitions from 0 to non-zero (= match end);
	 *  cleared automatically when Delta returns to 0 (next match's frame-1 push). */
	struct FElimPlusEloAnim
	{
		float StartTime  = 0.f;
		int32 FromElo    = 0;
		int32 ToElo      = 0;
		int32 FinalDelta = 0;
	};
	TMap<FString, FElimPlusEloAnim> EloAnimByPlayerId;
	static constexpr float EloAnimDurationSec = 4.0f;
};
