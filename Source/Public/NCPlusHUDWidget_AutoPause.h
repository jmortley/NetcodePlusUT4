// NCPlusHUDWidget_AutoPause - pause-safe automatic-pause status and countdown.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "Containers/Ticker.h"
#include "NCPlusHUDWidget_AutoPause.generated.h"

class ANCAutoPauseState;
class USoundBase;

/**
 * Presents the authoritative automatic-pause snapshot without relying on world
 * time. It remains visible over the scoreboard and plays the stock female
 * CD7..CD1 cues directly so a frozen announcer queue cannot hide the countdown.
 */
UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_AutoPause : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void InitializeWidget(AUTHUD* Hud) override;
	virtual void BeginDestroy() override;
	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<ANCAutoPauseState> CachedAutoPauseState;

	/** Indexed by the spoken number; element zero is intentionally unused. */
	UPROPERTY(Transient)
	TArray<USoundBase*> CountdownSounds;

	int32 LastObservedStateRevision;
	int32 LastObservedCountdownSecond;
	float LastObservedCountdownStartRealTime;
	double LocalCountdownEndRealTime;
	FDelegateHandle AudioTicker;

	ANCAutoPauseState* GetAutoPauseState();
	void UpdateCountdownAudio(ANCAutoPauseState* State);
	bool TickPausePresentation(float DeltaTime);
};
