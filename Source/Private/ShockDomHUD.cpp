#include "ShockDomHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "ShockDomControlPoint.h"
#include "ShockDomScoreboard.h"
#include "NCPlusHUDLayout.h"
#include "EngineUtils.h"


AShockDomHUD::AShockDomHUD(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Replace WipeoutScoreboard with ShockDomScoreboard in widget list
	HudWidgetClasses.Remove(TEXT("/Script/NetcodePlus.WipeoutScoreboard"));
	HudWidgetClasses.Add(TEXT("/Script/NetcodePlus.ShockDomScoreboard"));
}


void AShockDomHUD::DrawHUD()
{
	// Re-apply live layout each frame for live preview (cheap when clean).
	// We bypass AWipeoutHUD::DrawHUD below, so the apply call has to live here.
	ApplyLayoutToWidgets(this, FNCPlusHUDLayout::GetLive());

	// Skip AWipeoutHUD::DrawHUD (portrait strip) — go straight to AUTHUD base
	AUTHUD::DrawHUD();

	if (!Canvas || !SmallFont) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	bool bScoreboardIsUp = ScoreboardIsUp();

	// Team score bar (reuse from Wipeout — respects TeamSkins colors)
	if (GS && !bScoreboardIsUp)
	{
		DrawTeamScoreBar(GS);
	}

	if (!bScoreboardIsUp && GS &&
		(GS->GetMatchState() == MatchState::InProgress ||
		 GS->GetMatchState() == MatchState::WaitingPostMatch))
	{
		// Cache the control-point list across frames. They're spawned at map
		// load, never destroyed mid-match (bAlwaysRelevant), and never change
		// PointIndex — so iterating + sorting every frame at 144Hz+ is wasted
		// work. Cache invalidates on world swap (PIE stop / map travel).
		static TWeakObjectPtr<UWorld> CachedWorld;
		static TArray<TWeakObjectPtr<AShockDomControlPoint>> CachedPointsWeak;
		UWorld* World = GetWorld();

		bool bRebuild = (CachedWorld.Get() != World);
		if (!bRebuild)
		{
			for (const TWeakObjectPtr<AShockDomControlPoint>& WP : CachedPointsWeak)
			{
				if (!WP.IsValid()) { bRebuild = true; break; }
			}
		}
		if (bRebuild)
		{
			CachedPointsWeak.Reset();
			TArray<AShockDomControlPoint*> Tmp;
			for (TActorIterator<AShockDomControlPoint> It(World); It; ++It)
			{
				Tmp.Add(*It);
			}
			Tmp.Sort([](const AShockDomControlPoint& A, const AShockDomControlPoint& B)
			{
				return A.PointIndex < B.PointIndex;
			});
			CachedWorld = World;
			for (AShockDomControlPoint* P : Tmp) CachedPointsWeak.Add(P);
		}

		TArray<AShockDomControlPoint*> Points;
		Points.Reserve(CachedPointsWeak.Num());
		for (const TWeakObjectPtr<AShockDomControlPoint>& WP : CachedPointsWeak)
		{
			if (AShockDomControlPoint* P = WP.Get()) Points.Add(P);
		}

		DrawControlPointIndicators(Points);
		DrawControlPointWorldMarkers(Points);
	}

	// Optional opt-in overlays (default OFF). DrawDamageFlash must be last so it
	// tints over every other HUD draw.
	NCPlusHUDDrawCall::DrawServerInfo(this, Canvas);
	NCPlusHUDDrawCall::DrawDamageFlash(this, Canvas);
}


// ============================================================================
// MATCH CLOCK (centered below team score bar — Wipeout/Blitz style)
// ============================================================================

void AShockDomHUD::DrawTeamScoreBar(AUTGameState* GS)
{
	// Draw parent's score bar (red/blue bars, scores, team names)
	Super::DrawTeamScoreBar(GS);

	if (!Canvas || !MediumFont || !GS) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"))) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	// Phase 3.11: scorebar Scale override sizes clock + bar offset uniformly.
	const float ScoreScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));

	// Phase 3.5 layout consult — same anchor as Super so the clock stays put.
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * RenderScale);
	const FVector2D ScoreBarPos = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("scorebar"), Canvas, StockPos);
	const float CenterX = ScoreBarPos.X;
	const float TopY    = ScoreBarPos.Y;
	const float BarHeight = 36.f * RenderScale * ScoreScale;
	const float ClockY = TopY + BarHeight + 2.f * RenderScale;

	int32 RemainingTime = GS->GetRemainingTime();
	if (RemainingTime < 0) return;

	int32 Mins = RemainingTime / 60;
	int32 Secs = RemainingTime % 60;
	FString ClockStr = FString::Printf(TEXT("%02d:%02d"), Mins, Secs);

	const float FontExtraScale = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	float ClockScale = RenderScale * 1.1f * ScoreScale * FontExtraScale;
	float XL, YL;
	Canvas->TextSize(MediumFont, ClockStr, XL, YL, ClockScale, ClockScale);

	// Flash red under 30s
	Canvas->DrawColor = (RemainingTime <= 30) ? FColor(255, 60, 60, 255) : FColor::White;
	Canvas->DrawText(MediumFont, ClockStr, CenterX - XL * 0.5f, ClockY, ClockScale, ClockScale);
}


// ============================================================================
// CONTROL POINT STATUS INDICATORS (below match clock)
// ============================================================================

void AShockDomHUD::DrawControlPointIndicators(const TArray<AShockDomControlPoint*>& Points)
{
	if (Points.Num() == 0) return;
	if (NCPlusHUDDrawCall::IsHidden(TEXT("shockdom_controls"))) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	// Phase 3.11: shockdom_controls Scale override sizes the whole indicator
	// strip (and its label font below) uniformly.
	const float CtrlScale = NCPlusHUDDrawCall::GetScale(TEXT("shockdom_controls"));
	const float IndicatorSize = 30.f * RenderScale * CtrlScale;
	const float Spacing = 10.f * RenderScale * CtrlScale;
	const float TotalWidth = Points.Num() * IndicatorSize + (Points.Num() - 1) * Spacing;

	// Phase 3.5+ layout consult — anchor + offset for the whole strip.
	// Stock placement: centered horizontally, 78 design-px below screen top.
	const FVector2D StockTopLeft((Canvas->ClipX - TotalWidth) * 0.5f, 78.f * RenderScale);
	const FVector2D ResolvedAnchor = NCPlusHUDDrawCall::ResolveScreenPos(TEXT("shockdom_controls"), Canvas, FVector2D(Canvas->ClipX * 0.5f, 78.f * RenderScale));

	// The layout anchor refers to the strip's logical center-top. To keep the
	// strip centered on that point, subtract half its width to land top-left.
	const float StartX = ResolvedAnchor.X - TotalWidth * 0.5f;
	const float YPos   = ResolvedAnchor.Y;

	// Per-element opacity — fades the whole indicator strip uniformly.
	const float Op = NCPlusHUDDrawCall::GetOpacity(TEXT("shockdom_controls"));
	auto Tinted = [Op](FLinearColor C) -> FLinearColor { C.A *= Op; return C; };

	for (int32 i = 0; i < Points.Num(); i++)
	{
		AShockDomControlPoint* CP = Points[i];
		if (!CP) continue;

		float XPos = StartX + i * (IndicatorSize + Spacing);
		FLinearColor Color = CP->GetOwnerColor();

		// Pulse animation on recent capture
		float TimeSinceCapture = GetWorld()->GetTimeSeconds() - CP->LastCaptureTime;
		if (TimeSinceCapture < 1.0f && CP->OwningTeamIndex != 255)
		{
			float Pulse = 0.5f + 0.5f * FMath::Cos(TimeSinceCapture * PI * 4.f);
			Color = FMath::Lerp(Color, FLinearColor::White, Pulse * 0.4f);
		}

		// Filled square indicator (color carries the per-element opacity).
		Canvas->SetLinearDrawColor(Tinted(Color));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XPos, YPos, IndicatorSize, IndicatorSize,
			0.f, 0.f, 1.f, 1.f);

		// Point label centered
		FString Label = CP->PointLabel;
		float TextW, TextH;
		float FontScale = RenderScale * 0.7f * CtrlScale;
		Canvas->TextSize(SmallFont, Label, TextW, TextH, FontScale, FontScale);

		float TextX = XPos + (IndicatorSize - TextW) * 0.5f;
		float TextY = YPos + (IndicatorSize - TextH) * 0.5f;

		FCanvasTextItem TextItem(FVector2D(TextX, TextY), FText::FromString(Label),
			SmallFont, Tinted(FLinearColor::White));
		TextItem.Scale = FVector2D(FontScale, FontScale);
		TextItem.bOutlined = true;
		TextItem.OutlineColor = Tinted(FLinearColor::Black);
		Canvas->DrawItem(TextItem);
	}
}


// ============================================================================
// WORLD-SPACE CONTROL POINT MARKERS
// ============================================================================

void AShockDomHUD::DrawControlPointWorldMarkers(const TArray<AShockDomControlPoint*>& Points)
{
	if (!UTPlayerOwner) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;

	for (AShockDomControlPoint* CP : Points)
	{
		if (!CP) continue;
		DrawWorldMarker(CP->GetActorLocation() + FVector(0.f, 0.f, 100.f),
			CP->PointLabel, CP->GetOwnerColor(), RenderScale);
	}
}


void AShockDomHUD::DrawWorldMarker(const FVector& WorldLoc, const FString& Label,
	const FLinearColor& Color, float RenderScale)
{
	FVector ScreenLoc = Canvas->Project(WorldLoc);

	if (ScreenLoc.Z <= 0.f)
	{
		ScreenLoc.X = Canvas->ClipX - ScreenLoc.X;
		ScreenLoc.Y = Canvas->ClipY - ScreenLoc.Y;
	}

	const float Margin = 30.f * RenderScale;
	const float MinY = 60.f * RenderScale;
	bool bClamped = false;

	if (ScreenLoc.X < Margin) { ScreenLoc.X = Margin; bClamped = true; }
	if (ScreenLoc.X > Canvas->ClipX - Margin) { ScreenLoc.X = Canvas->ClipX - Margin; bClamped = true; }
	if (ScreenLoc.Y < MinY) { ScreenLoc.Y = MinY; bClamped = true; }
	if (ScreenLoc.Y > Canvas->ClipY - Margin) { ScreenLoc.Y = Canvas->ClipY - Margin; bClamped = true; }

	const float IconSize = 24.f * RenderScale;
	float CenterX = ScreenLoc.X;
	float CenterY = ScreenLoc.Y;

	Canvas->SetDrawColor(FColor(Color.ToFColor(true)));
	Canvas->DrawTile(Canvas->DefaultTexture,
		CenterX - IconSize * 0.5f, CenterY - IconSize * 0.5f,
		IconSize, IconSize,
		0.f, 0.f, 1.f, 1.f);

	float TextW, TextH;
	float FontScale = RenderScale * 0.6f;
	Canvas->TextSize(SmallFont, Label, TextW, TextH, FontScale, FontScale);

	FCanvasTextItem TextItem(
		FVector2D(CenterX - TextW * 0.5f, CenterY - TextH * 0.5f),
		FText::FromString(Label), SmallFont, FLinearColor::White);
	TextItem.Scale = FVector2D(FontScale, FontScale);
	TextItem.bOutlined = true;
	TextItem.OutlineColor = FLinearColor::Black;
	Canvas->DrawItem(TextItem);

	if (bClamped)
	{
		APawn* ViewPawn = UTPlayerOwner ? UTPlayerOwner->GetPawnOrSpectator() : nullptr;
		if (ViewPawn)
		{
			float Dist = (WorldLoc - ViewPawn->GetActorLocation()).Size();
			int32 DistMeters = FMath::RoundToInt(Dist / 100.f);
			FString DistText = FString::Printf(TEXT("%dm"), DistMeters);

			float DTextW, DTextH;
			Canvas->TextSize(SmallFont, DistText, DTextW, DTextH, FontScale, FontScale);

			FCanvasTextItem DistItem(
				FVector2D(CenterX - DTextW * 0.5f, CenterY + IconSize * 0.6f),
				FText::FromString(DistText), SmallFont, FLinearColor::White);
			DistItem.Scale = FVector2D(FontScale, FontScale);
			DistItem.bOutlined = true;
			DistItem.OutlineColor = FLinearColor::Black;
			Canvas->DrawItem(DistItem);
		}
	}
}
