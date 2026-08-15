// NCShaftArenaHUD — 1v1 team-DM HUD inheriting AWipeoutHUD. Uses the inherited
// Duel/Wipeout team scorebar and swaps in Shaft Arena's scoreboard.
#pragma once

#include "NetcodePlus.h"
#include "WipeoutHUD.h"
#include "NCShaftArenaHUD.generated.h"

UCLASS()
class NETCODEPLUS_API ANCShaftArenaHUD : public AWipeoutHUD
{
	GENERATED_UCLASS_BODY()

	virtual void BeginPlay() override;
};
