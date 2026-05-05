// NCShaftArenaHUD — 1v1 FFA HUD inheriting AWipeoutHUD. No team portraits
// (this is FFA, AUTDMGameMode has no teams). Override DrawTeamScoreBar to
// render an FFA-style "Name1 N - N Name2" scoreline. Adds the Accuracy widget
// to HudWidgetClasses for the live aim-trainer feedback the mode is built around.
#pragma once

#include "NetcodePlus.h"
#include "WipeoutHUD.h"
#include "NCShaftArenaHUD.generated.h"

UCLASS()
class NETCODEPLUS_API ANCShaftArenaHUD : public AWipeoutHUD
{
	GENERATED_UCLASS_BODY()

	virtual void BeginPlay() override;
	virtual void DrawTeamScoreBar(AUTGameState* GS) override;
};
