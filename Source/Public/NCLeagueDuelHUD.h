// NCLeagueDuelHUD — 1v1 portrait + scorebar HUD inheriting AWipeoutHUD.
// No control-point indicators (unlike ShockDom). Reuses Wipeout's portrait
// strips, score bar, GetInputMode override. Adds a custom scoreboard with
// per-weapon accuracy columns relevant to duel.
#pragma once

#include "NetcodePlus.h"
#include "WipeoutHUD.h"
#include "NCLeagueDuelHUD.generated.h"

UCLASS()
class NETCODEPLUS_API ANCLeagueDuelHUD : public AWipeoutHUD
{
	GENERATED_UCLASS_BODY()
};
