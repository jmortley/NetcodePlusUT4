// NCPlusCTFHUD.h — Custom CTF HUD for NetcodePlus.
// Extends AUTHUD_CTF to preserve all flag UI (status widget, minimap icons).
// Adds: Wipeout-style team score bar, GetInputMode mouse focus fix.

#pragma once

#include "NetcodePlus.h"
#include "UTHUD_CTF.h"
#include "NCPlusCTFHUD.generated.h"

UCLASS()
class NETCODEPLUS_API ANCPlusCTFHUD : public AUTHUD_CTF
{
	GENERATED_BODY()

public:
	ANCPlusCTFHUD(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

	/** Swap the stock spectator slide-out for UNCPlusSpectatorSlideOut so the
	 *  weapon-stats panel shows only the instagib rifle (iCTF) with replicated
	 *  accuracy. Non-instagib CTF keeps the stock map-pickup behaviour. */
	virtual void AddSpectatorWidgets() override;

protected:
	virtual EInputMode::Type GetInputMode_Implementation() const override;

private:
	/** Draw the Wipeout-style team score bar at top center */
	void DrawTeamScoreBar();

	/** Draw "NOW WATCHING / PlayerName" bottom-right when viewing another player */
	void DrawSpectatorTarget();

	/** Auto post-match high-res screenshot (client opt-in via F5 "High Res Screenshot PostMatch").
	 *  Fires once, after the decisive-cap replay is over AND the scoreboard is up, so it captures the
	 *  final scoreboard rather than the replay. Mirrors AElimPlusHUD/AWipeoutHUD but gated on the
	 *  replay finishing (iCTF holds the match open ncp.CTFReplayHoldSec before MatchEnded). */
	void MaybeTakePostMatchScreenshot();

	/** One-shot guard so the screenshot fires a single time. */
	bool bPostMatchScreenshotTaken = false;
};
