// NCShaftArenaHUD.cpp — FFA scoreline override + accuracy widget added.

#include "NCShaftArenaHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "Engine/Canvas.h"
#include "NCShaftArenaScoreboard.h"
#include "NCPlusHUDLayout.h"

ANCShaftArenaHUD::ANCShaftArenaHUD(const FObjectInitializer& OI)
	: Super(OI)
{
	// Swap out AWipeoutHUD's scoreboard for our FFA variant. The accuracy
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
}

void ANCShaftArenaHUD::BeginPlay()
{
	Super::BeginPlay();

	// Seed a default accuracy entry so the widget is visible by default in
	// shaft arena. Other HUDs leave the entry empty (widget hidden) so users
	// must opt in via the nchud editor. The seed only writes if no entry
	// exists yet — explicit user customization (drag, hide, weapon override)
	// is preserved across launches.
	FNCPlusHUDLayout& Live = FNCPlusHUDLayout::GetLive();
	if (!Live.Elements.Contains(TEXT("accuracy")))
	{
		FNCPlusHUDElement Entry;
		Entry.Anchor = NCPlusHUDAliases::GetStockAnchor(TEXT("accuracy"));
		Entry.Offset = NCPlusHUDAliases::GetStockOffset(TEXT("accuracy"));
		// "current" is the default behavior (held weapon) — record it
		// explicitly so the editor's weapon dropdown shows the right choice.
		Entry.Extras.Add(TEXT("weapon"), TEXT("current"));
		Live.Elements.Add(TEXT("accuracy"), Entry);
		FNCPlusHUDLayout::MarkLiveDirty();
	}
}

void ANCShaftArenaHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !GS || !MediumFont) return;

	// Find the two FFA players (top by score). AUTDMGameMode is teamless, so
	// AWipeoutHUD::DrawTeamScoreBar's team-based logic doesn't apply.
	AUTPlayerState* P1 = nullptr;
	AUTPlayerState* P2 = nullptr;
	for (APlayerState* APS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		if (!P1 || UTPS->Score > P1->Score)      { P2 = P1; P1 = UTPS; }
		else if (!P2 || UTPS->Score > P2->Score) { P2 = UTPS; }
	}
	if (!P1 || !P2) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	const float CenterX = Canvas->ClipX * 0.5f;
	const float TopY    = 16.f * RenderScale;

	const FString Line = FString::Printf(TEXT("%s   %d  -  %d   %s"),
		*P1->PlayerName, int32(P1->Score), int32(P2->Score), *P2->PlayerName);

	FCanvasTextItem TextItem(FVector2D(CenterX, TopY), FText::FromString(Line),
		MediumFont, FLinearColor::White);
	TextItem.bCentreX = true;
	TextItem.EnableShadow(FLinearColor::Black);
	TextItem.Scale = FVector2D(RenderScale, RenderScale);
	Canvas->DrawItem(TextItem);
}
