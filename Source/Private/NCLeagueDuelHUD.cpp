// NCLeagueDuelHUD.cpp — 1v1 HUD: score bar + duel-specific scoreboard +
// engine respawn-choice widget. Portraits suppressed (1v1 doesn't benefit
// from a team-portrait row).

#include "NCLeagueDuelHUD.h"
#include "UnrealTournament.h"
#include "NCLeagueDuelScoreboard.h"

ANCLeagueDuelHUD::ANCLeagueDuelHUD(const FObjectInitializer& OI)
	: Super(OI)
{
	// 1v1 duel: skip the team-portrait strip entirely (parent's DrawHUD
	// honors this flag — see AWipeoutHUD::DrawHUD).
	bShouldDrawPortraits = false;

	// Mirror AWipeoutHUD's HudWidgetClasses list but swap the scoreboard for
	// our duel-specific variant. The other widgets (scorebar, crosshair, ammo,
	// NCPlus widgets, killfeed, etc.) are inherited as-is.
	for (int32 i = HudWidgetClasses.Num() - 1; i >= 0; --i)
	{
		if (HudWidgetClasses[i].Contains(TEXT("WipeoutScoreboard")))
		{
			HudWidgetClasses.RemoveAt(i);
		}
	}
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCLeagueDuelScoreboard"));

	// Spawn-choice picker — engine widget that captures both PlayerStart views
	// and lets the dead player toggle between A and B. Stock UTHUD_Duel registers
	// this via DefaultGame.ini config; we add it here directly because we don't
	// have a [/Script/NetcodePlus.NCLeagueDuelHUD] ini section.
	HudWidgetClasses.Add(TEXT("/Script/UnrealTournament.UTHUDWidget_RespawnChoice"));
}
