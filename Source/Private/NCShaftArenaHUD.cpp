// NCShaftArenaHUD.cpp — FFA scoreline override + accuracy widget added.

#include "NCShaftArenaHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "Engine/Canvas.h"
#include "NCShaftArenaScoreboard.h"

ANCShaftArenaHUD::ANCShaftArenaHUD(const FObjectInitializer& OI)
	: Super(OI)
{
	// Drop AWipeoutHUD's scoreboard reference and add our FFA scoreboard +
	// accuracy widget.
	for (int32 i = HudWidgetClasses.Num() - 1; i >= 0; --i)
	{
		if (HudWidgetClasses[i].Contains(TEXT("WipeoutScoreboard")))
		{
			HudWidgetClasses.RemoveAt(i);
		}
	}
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.NCShaftArenaScoreboard"));
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
