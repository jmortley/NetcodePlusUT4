// NCPlusCTFHUD.h — Custom CTF HUD for NetcodePlus.
// Extends AUTHUD_CTF to preserve all flag UI (status widget, minimap icons).
// Adds: Wipeout-style team score bar, GetInputMode mouse focus fix.

#pragma once

#include "NetcodePlus.h"
#include "UTHUD_CTF.h"
#include "NCPlusCTFHUD.generated.h"

class ANCPlusCTFOTInfo;

UCLASS()
class NETCODEPLUS_API ANCPlusCTFHUD : public AUTHUD_CTF
{
	GENERATED_BODY()

public:
	ANCPlusCTFHUD(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual bool ShouldDrawMinimap() override;

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

	/** Cached OT actor; misses are throttled because regulation matches spend
	 *  nearly their entire lifetime without one on a newly joined client. */
	TWeakObjectPtr<UWorld> CachedOTInfoWorld;
	TWeakObjectPtr<ANCPlusCTFOTInfo> CachedOTInfo;
	float NextOTInfoSearchTime = 0.f;
	ANCPlusCTFOTInfo* FindOTInfo();

	/** Post-match high-res screenshot state — serviced each frame by
	 *  NCPlusHUDDrawCall::ServicePostMatchScreenshot (fires after the replay ends + scoreboard settles). */
	bool bPostMatchScreenshotTaken = false;
	float PostMatchScreenshotStable = -1.f;
};
