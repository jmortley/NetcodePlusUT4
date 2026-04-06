#include "ShockDomHUD.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "ShockDomControlPoint.h"
#include "ShockDomScoreboard.h"
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
		// Collect control points via TActorIterator (they're bAlwaysRelevant)
		TArray<AShockDomControlPoint*> Points;
		for (TActorIterator<AShockDomControlPoint> It(GetWorld()); It; ++It)
		{
			Points.Add(*It);
		}
		Points.Sort([](const AShockDomControlPoint& A, const AShockDomControlPoint& B)
		{
			return A.PointIndex < B.PointIndex;
		});

		DrawControlPointIndicators(Points);
		DrawControlPointWorldMarkers(Points);
	}
}


// ============================================================================
// CONTROL POINT STATUS INDICATORS (below team score bar)
// ============================================================================

void AShockDomHUD::DrawControlPointIndicators(const TArray<AShockDomControlPoint*>& Points)
{
	if (Points.Num() == 0) return;

	const float RenderScale = float(Canvas->SizeX) / 1920.0f;
	const float IndicatorSize = 30.f * RenderScale;
	const float Spacing = 10.f * RenderScale;
	const float TotalWidth = Points.Num() * IndicatorSize + (Points.Num() - 1) * Spacing;

	// Position: centered horizontally, below the team score bar
	const float StartX = (Canvas->ClipX - TotalWidth) * 0.5f;
	const float YPos = 52.f * RenderScale;

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

		// Filled square indicator
		Canvas->SetDrawColor(FColor(Color.ToFColor(true)));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XPos, YPos, IndicatorSize, IndicatorSize,
			0.f, 0.f, 1.f, 1.f);

		// Point label centered
		FString Label = CP->PointLabel;
		float TextW, TextH;
		float FontScale = RenderScale * 0.7f;
		Canvas->TextSize(SmallFont, Label, TextW, TextH, FontScale, FontScale);

		float TextX = XPos + (IndicatorSize - TextW) * 0.5f;
		float TextY = YPos + (IndicatorSize - TextH) * 0.5f;

		FCanvasTextItem TextItem(FVector2D(TextX, TextY), FText::FromString(Label),
			SmallFont, FLinearColor::White);
		TextItem.Scale = FVector2D(FontScale, FontScale);
		TextItem.bOutlined = true;
		TextItem.OutlineColor = FLinearColor::Black;
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
