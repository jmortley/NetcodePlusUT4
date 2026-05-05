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

// Mirrors AWipeoutHUD::DrawTeamScoreBar's boxed-bars + score boxes + center
// divider + clock layout, but FFA-flavoured: two grey bars (no team colors,
// no Liandri/Phayder), populated with the top two players' names where the
// team variant prints faction names. Same `scorebar` layout extras (anchor,
// offset, scale, hidden) so drag-drop works identically across modes.
void ANCShaftArenaHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	if (!Canvas || !GS || !SmallFont || !MediumFont || !LargeFont) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

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

	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;

	// FFA has no team colors — neutral mid-grey both sides. Same * 0.7f
	// darkening for the score box that the team variant uses.
	const FLinearColor BarColor(0.35f, 0.35f, 0.35f, 1.f);
	const FLinearColor BoxColor(BarColor.R * 0.7f, BarColor.G * 0.7f, BarColor.B * 0.7f, 1.f);

	const FString P1Name = P1->PlayerName;
	const FString P2Name = P2->PlayerName;
	const int32 Score1 = int32(P1->Score);
	const int32 Score2 = int32(P2->Score);

	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float BarWidth = 220.f * RenderScale * ScoreScale;
	const float BarHeight = 36.f * RenderScale * ScoreScale;
	const float ScoreBoxWidth = 50.f * RenderScale * ScoreScale;
	const float GapWidth = 8.f * RenderScale;

	const float LeftBarX = CenterX - GapWidth - ScoreBoxWidth - BarWidth;
	Canvas->SetLinearDrawColor(BarColor);
	Canvas->DrawTile(Canvas->DefaultTexture, LeftBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX0 = CenterX - GapWidth - ScoreBoxWidth;
	Canvas->SetLinearDrawColor(BoxColor);
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float ScoreBoxX1 = CenterX + GapWidth;
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY, ScoreBoxWidth, BarHeight, 0, 0, 1, 1);

	const float RightBarX = CenterX + GapWidth + ScoreBoxWidth;
	Canvas->SetLinearDrawColor(BarColor);
	Canvas->DrawTile(Canvas->DefaultTexture, RightBarX, TopY, BarWidth, BarHeight, 0, 0, 1, 1);

	const float TailHeight = 14.f * RenderScale * ScoreScale;
	Canvas->SetLinearDrawColor(BoxColor);
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX0, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);
	Canvas->DrawTile(Canvas->DefaultTexture, ScoreBoxX1, TopY + BarHeight, ScoreBoxWidth, TailHeight, 0, 0, 1, 1);

	Canvas->SetLinearDrawColor(FLinearColor::White);
	Canvas->DrawTile(Canvas->DefaultTexture, CenterX - 1.f * RenderScale, TopY, 2.f * RenderScale, BarHeight + TailHeight, 0, 0, 1, 1);

	const float FontScale = RenderScale * 0.85f * ScoreScale;
	const float LargeFontScale = RenderScale * 1.2f * ScoreScale;
	float XL, YL;

	// Per-element font override (Phase 3.8). FFA uses player names instead
	// of team names but the same scorebar alias drives the typography.
	UFont* PlayerNameFont  = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, SmallFont);
	UFont* PlayerScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), this, LargeFont);
	if (!PlayerNameFont)  PlayerNameFont  = SmallFont;
	if (!PlayerScoreFont) PlayerScoreFont = LargeFont;

	Canvas->TextSize(PlayerNameFont, P1Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(PlayerNameFont, P1Name, LeftBarX + BarWidth - XL - 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	const FString Score1Str = FString::Printf(TEXT("%d"), Score1);
	Canvas->TextSize(PlayerScoreFont, Score1Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(PlayerScoreFont, Score1Str, ScoreBoxX0 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	const FString Score2Str = FString::Printf(TEXT("%d"), Score2);
	Canvas->TextSize(PlayerScoreFont, Score2Str, XL, YL, LargeFontScale, LargeFontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(PlayerScoreFont, Score2Str, ScoreBoxX1 + (ScoreBoxWidth - XL) * 0.5f,
		TopY + (BarHeight - YL) * 0.5f, LargeFontScale, LargeFontScale);

	Canvas->TextSize(PlayerNameFont, P2Name, XL, YL, FontScale, FontScale);
	Canvas->DrawColor = FColor::White;
	Canvas->DrawText(PlayerNameFont, P2Name, RightBarX + 8.f * RenderScale,
		TopY + (BarHeight - YL) * 0.5f, FontScale, FontScale);

	// NCShaftArena is time-limited (DM with TimeLimit). Stock RemainingTime is
	// protected, so reach via reflection — same pattern Wipeout uses for
	// non-round modes.
	const float ClockY = TopY + BarHeight + 2.f * RenderScale;
	int32 ClockSeconds = -1;
	{
		// Static cache: see WipeoutHUD/ElimPlusHUD for rationale (FindField was
		// hitting the class-hierarchy walk every frame).
		static UClass* CachedCls = nullptr;
		static UIntProperty* CachedProp = nullptr;
		UClass* GSCls = GS->GetClass();
		if (CachedCls != GSCls)
		{
			CachedCls  = GSCls;
			CachedProp = FindField<UIntProperty>(GSCls, TEXT("RemainingTime"));
		}
		if (CachedProp)
		{
			const int32 RT = CachedProp->GetPropertyValue_InContainer(GS);
			if (RT > 0) ClockSeconds = RT;
		}
	}

	const float RoundClockScale = RenderScale * 1.1f * ScoreScale;
	if (ClockSeconds >= 0)
	{
		const int32 RMins = ClockSeconds / 60;
		const int32 RSecs = ClockSeconds % 60;
		FString RoundClockStr = FString::Printf(TEXT("%02d:%02d"), RMins, RSecs);
		Canvas->TextSize(MediumFont, RoundClockStr, XL, YL, RoundClockScale, RoundClockScale);
		Canvas->DrawColor = (ClockSeconds <= 30) ? FColor(255, 60, 60, 255) : FColor::White;
		Canvas->DrawText(MediumFont, RoundClockStr, CenterX - XL * 0.5f, ClockY, RoundClockScale, RoundClockScale);
	}
}
