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

protected:
	virtual EInputMode::Type GetInputMode_Implementation() const override;

private:
	/** Draw the Wipeout-style team score bar at top center */
	void DrawTeamScoreBar();
};
