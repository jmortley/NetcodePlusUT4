// NCShaftArenaHUD.cpp — Shaft Arena HUD wiring.

#include "NCShaftArenaHUD.h"
#include "UnrealTournament.h"
#include "NCShaftArenaScoreboard.h"

ANCShaftArenaHUD::ANCShaftArenaHUD(const FObjectInitializer& OI)
	: Super(OI)
{
	// Swap out AWipeoutHUD's scoreboard for our Shaft Arena variant. The accuracy
	// widget is already registered by the parent; visibility is layout-gated
	// (see ShouldDraw_Implementation) and BeginPlay below seeds the default
	// entry so it shows on this mode out of the box.
	for (int32 i = HudWidgetClasses.Num() - 1; i >= 0; --i)
	{
		if (HudWidgetClasses[i].Contains(TEXT("WipeoutScoreboard")))
		{
			HudWidgetClasses.RemoveAt(i);
		}
	}
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCShaftArenaScoreboard"));
	HudWidgetClasses.AddUnique(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_ReadyUp"));
}

void ANCShaftArenaHUD::BeginPlay()
{
	Super::BeginPlay();

	// NOTE: the old "seed a default accuracy entry" block is GONE (2026-07-01). It
	// wrote into the process-global live layout, and the nchud editor/drag overlay
	// auto-saves that whole map on close — so playing shaft once + touching the
	// editor baked a visible accuracy entry into HUDLayout.json for EVERY mode.
	// Shaft's accuracy-on-by-default now lives in the widget's own ShouldDraw
	// (mode check, no shared-state mutation) — see NCPlusHUDWidget_Accuracy.cpp.
}
