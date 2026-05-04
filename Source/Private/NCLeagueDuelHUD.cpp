// NCLeagueDuelHUD.cpp — duel HUD: portrait + score bar + duel-specific scoreboard.

#include "NCLeagueDuelHUD.h"
#include "UnrealTournament.h"
#include "NCLeagueDuelScoreboard.h"

ANCLeagueDuelHUD::ANCLeagueDuelHUD(const FObjectInitializer& OI)
	: Super(OI)
{
	// Mirror AWipeoutHUD's HudWidgetClasses list but swap the scoreboard for
	// our duel-specific variant. The other widgets (portraits, scorebar,
	// crosshair, ammo, NCPlus widgets, killfeed, etc.) are inherited as-is.
	for (int32 i = HudWidgetClasses.Num() - 1; i >= 0; --i)
	{
		if (HudWidgetClasses[i].Contains(TEXT("WipeoutScoreboard")))
		{
			HudWidgetClasses.RemoveAt(i);
		}
	}
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCLeagueDuelScoreboard"));
}
