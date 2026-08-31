// NCPlusBetaTopBar - shared code-native tactical ribbon for Wipeout/ElimPlus.
#include "NCPlusBetaTopBar.h"
#include "UnrealTournament.h"
#include "NCPlusHUDLayout.h"
#include "UTHUD.h"
#include "ElimPlusHUD.h"
#include "WipeoutHUD.h"
#include "ClutchHUD.h"
#include "NCLeagueDuelHUD.h"
#include "NCShaftArenaHUD.h"
#include "ShockDomHUD.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"

namespace
{
	constexpr float BetaPortraitAspect = 320.f / 224.f;
	constexpr float BetaCoreHeight = 56.f;
	constexpr float BetaScoreWidth = 84.f;
	constexpr float BetaClockWidth = 168.f;
	constexpr float BetaPortraitGap = 4.f;
	constexpr float BetaCorePortraitGap = 12.f;
	TMap<int32, FString> BetaPlainNumberStrings;
	TMap<int32, FString> BetaClockStrings;

	const FString& ResolveBetaNumber(int32 Value, bool bClock)
	{
		TMap<int32, FString>& Cache = bClock ? BetaClockStrings : BetaPlainNumberStrings;
		if (const FString* Existing = Cache.Find(Value))
		{
			return *Existing;
		}
		if (Cache.Num() >= 256)
		{
			Cache.Reset();
		}

		FString& Result = Cache.FindOrAdd(Value);
		Result = bClock
			? FString::Printf(TEXT("%02d:%02d"), Value / 60, Value % 60)
			: FString::FromInt(Value);
		return Result;
	}

	void AddTriangle(TArray<FCanvasUVTri>& Tris, const FVector2D& A,
		const FVector2D& B, const FVector2D& C, const FLinearColor& Color)
	{
		FCanvasUVTri Tri;
		Tri.V0_Pos = A;
		Tri.V1_Pos = B;
		Tri.V2_Pos = C;
		Tri.V0_UV = Tri.V1_UV = Tri.V2_UV = FVector2D::ZeroVector;
		Tri.V0_Color = Tri.V1_Color = Tri.V2_Color = Color;
		Tris.Add(Tri);
	}

	void AddQuad(TArray<FCanvasUVTri>& Tris, const FVector2D& A,
		const FVector2D& B, const FVector2D& C, const FVector2D& D,
		const FLinearColor& Color)
	{
		AddTriangle(Tris, A, B, C, Color);
		AddTriangle(Tris, A, C, D, Color);
	}

	void AddConvexPolygon(TArray<FCanvasUVTri>& Tris, const FVector2D* Points,
		int32 PointCount, const FLinearColor& Color)
	{
		if (Points == nullptr || PointCount < 3) return;
		for (int32 Index = 1; Index + 1 < PointCount; ++Index)
		{
			AddTriangle(Tris, Points[0], Points[Index], Points[Index + 1], Color);
		}
	}

	void AddChamferedRect(TArray<FCanvasUVTri>& Tris, float Left, float Top,
		float Right, float Bottom, float Chamfer, const FLinearColor& Color)
	{
		const float SafeChamfer = FMath::Min(Chamfer,
			0.5f * FMath::Min(FMath::Max(0.f, Right - Left), FMath::Max(0.f, Bottom - Top)));
		const FVector2D Points[8] =
		{
			FVector2D(Left + SafeChamfer, Top),
			FVector2D(Right - SafeChamfer, Top),
			FVector2D(Right, Top + SafeChamfer),
			FVector2D(Right, Bottom - SafeChamfer),
			FVector2D(Right - SafeChamfer, Bottom),
			FVector2D(Left + SafeChamfer, Bottom),
			FVector2D(Left, Bottom - SafeChamfer),
			FVector2D(Left, Top + SafeChamfer),
		};
		AddConvexPolygon(Tris, Points, 8, Color);
	}

	FLinearColor WithOpacity(FLinearColor Color, float Opacity)
	{
		Color.A *= Opacity;
		return Color;
	}

	FLinearColor MutedTeamPlate(const FLinearColor& TeamColor, float Alpha)
	{
		return FLinearColor(
			FMath::Clamp(0.018f + 0.24f * TeamColor.R, 0.f, 1.f),
			FMath::Clamp(0.026f + 0.24f * TeamColor.G, 0.f, 1.f),
			FMath::Clamp(0.036f + 0.24f * TeamColor.B, 0.f, 1.f),
			Alpha);
	}

	float ResolveFittedStableText(UCanvas* Canvas, UFont* Font,
		const FString& Source, float DesiredScale, float MaxWidth, float MaxHeight,
		FText& OutText, float& OutWidth, float& OutHeight)
	{
		float DrawScale = DesiredScale;
		NCPlusHUDDrawCall::ResolveStableText(Canvas, Font, Source,
			DrawScale, DrawScale, OutText, OutWidth, OutHeight);

		float Fit = 1.f;
		if (OutWidth > MaxWidth && OutWidth > KINDA_SMALL_NUMBER)
		{
			Fit = FMath::Min(Fit, MaxWidth / OutWidth);
		}
		if (OutHeight > MaxHeight && OutHeight > KINDA_SMALL_NUMBER)
		{
			Fit = FMath::Min(Fit, MaxHeight / OutHeight);
		}
		if (Fit < 1.f)
		{
			DrawScale *= Fit;
			NCPlusHUDDrawCall::ResolveStableText(Canvas, Font, Source,
				DrawScale, DrawScale, OutText, OutWidth, OutHeight);
		}
		return DrawScale;
	}
}

FVector2D FNCPlusBetaTopBarGeometry::GetPortraitPosition(
	ENCPlusBetaTopBarSide Side, int32 Ordinal) const
{
	const float SafeOrdinal = float(FMath::Max(0, Ordinal));
	const float X = (Side == ENCPlusBetaTopBarSide::Left)
		? LeftPortraitAnchorX - PortraitWidth - SafeOrdinal * PortraitPitch
		: RightPortraitAnchorX + SafeOrdinal * PortraitPitch;
	return FVector2D(X, PortraitY);
}

bool NCPlusBetaTopBar::IsActiveForHUD(const AUTHUD* HUD)
{
	if (HUD == nullptr || !FNCPlusHUDLayout::WantsBetaTopBar())
	{
		return false;
	}

	if (HUD->IsA(AElimPlusHUD::StaticClass()))
	{
		return true;
	}

	return HUD->IsA(AWipeoutHUD::StaticClass())
		&& !HUD->IsA(AClutchHUD::StaticClass())
		&& !HUD->IsA(ANCLeagueDuelHUD::StaticClass())
		&& !HUD->IsA(ANCShaftArenaHUD::StaticClass())
		&& !HUD->IsA(AShockDomHUD::StaticClass());
}

bool NCPlusBetaTopBar::BuildGeometry(AUTHUD* HUD, UCanvas* Canvas,
	int32 LeftPortraitCount, int32 RightPortraitCount,
	FNCPlusBetaTopBarGeometry& OutGeometry)
{
	if (HUD == nullptr || Canvas == nullptr || Canvas->SizeX <= 0 || Canvas->SizeY <= 0)
	{
		return false;
	}

	const float ViewScale = FMath::Min(float(Canvas->SizeX) / 1920.f,
		float(Canvas->SizeY) / 1080.f);
	const float LayoutScale = NCPlusHUDDrawCall::GetScale(TEXT("scorebar"));
	const float HUDScale = HUD->GetHUDWidgetScaleOverride();
	const float UnitScale = FMath::Max(0.01f, ViewScale * LayoutScale * HUDScale);
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 2.f * ViewScale);
	const FVector2D Center = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, StockPos);

	OutGeometry = FNCPlusBetaTopBarGeometry();
	OutGeometry.Center = Center;
	OutGeometry.TopY = Center.Y;
	OutGeometry.UnitScale = UnitScale;
	OutGeometry.CoreHeight = BetaCoreHeight * UnitScale;
	const float CoreWidth = (2.f * BetaScoreWidth + BetaClockWidth) * UnitScale;
	OutGeometry.CoreLeft = Center.X - 0.5f * CoreWidth;
	OutGeometry.CoreRight = Center.X + 0.5f * CoreWidth;

	OutGeometry.PortraitHeight = BetaCoreHeight * UnitScale;
	OutGeometry.PortraitWidth = (BetaCoreHeight / BetaPortraitAspect) * UnitScale;
	OutGeometry.PortraitPitch = OutGeometry.PortraitWidth + BetaPortraitGap * UnitScale;
	OutGeometry.PortraitY = OutGeometry.TopY + 1.f * UnitScale;
	OutGeometry.LeftPortraitAnchorX = OutGeometry.CoreLeft
		- BetaCorePortraitGap * UnitScale;
	OutGeometry.RightPortraitAnchorX = OutGeometry.CoreRight
		+ BetaCorePortraitGap * UnitScale;
	OutGeometry.PortraitCount[int32(ENCPlusBetaTopBarSide::Left)] =
		FMath::Clamp(LeftPortraitCount, 0, 32);
	OutGeometry.PortraitCount[int32(ENCPlusBetaTopBarSide::Right)] =
		FMath::Clamp(RightPortraitCount, 0, 32);
	return true;
}

void NCPlusBetaTopBar::DrawChassisAndScoreCore(AUTHUD* HUD, UCanvas* Canvas,
	const FNCPlusBetaTopBarGeometry& G, const FNCPlusBetaTopBarCore& Core)
{
	if (HUD == nullptr || Canvas == nullptr || Canvas->DefaultTexture == nullptr
		|| NCPlusHUDDrawCall::IsHidden(TEXT("scorebar")))
	{
		return;
	}

	UFont* ScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), HUD, HUD->LargeFont);
	UFont* ClockFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), HUD, HUD->MediumFont);
	if (ScoreFont == nullptr) ScoreFont = HUD->LargeFont;
	if (ClockFont == nullptr) ClockFont = HUD->MediumFont;
	if (ScoreFont == nullptr || ClockFont == nullptr) return;

	const float U = G.UnitScale;
	const float Opacity = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	const int32 LeftCount = G.PortraitCount[int32(ENCPlusBetaTopBarSide::Left)];
	const int32 RightCount = G.PortraitCount[int32(ENCPlusBetaTopBarSide::Right)];
	const float LeftNearest = G.LeftPortraitAnchorX - G.PortraitWidth;
	const float RightNearest = G.RightPortraitAnchorX;
	const float LeftMost = LeftCount > 0
		? LeftNearest - float(LeftCount - 1) * G.PortraitPitch
		: G.CoreLeft - 18.f * U;
	const float RightMost = RightCount > 0
		? RightNearest + float(RightCount - 1) * G.PortraitPitch + G.PortraitWidth
		: G.CoreRight + 18.f * U;
	const float LeftOuter = LeftMost - 28.f * U;
	const float RightOuter = RightMost + 28.f * U;
	const float ShellTop = G.TopY - 5.f * U;
	const float ShellBottom = G.TopY + G.CoreHeight + 17.f * U;
	const float CoreBottom = G.TopY + G.CoreHeight;

	const FLinearColor Shell = WithOpacity(FLinearColor(0.008f, 0.018f, 0.028f, 0.88f), Opacity);
	const FLinearColor InnerShell = WithOpacity(FLinearColor(0.018f, 0.038f, 0.052f, 0.74f), Opacity);
	const FLinearColor Steel = WithOpacity(FLinearColor(0.20f, 0.36f, 0.43f, 0.62f), Opacity);
	const FLinearColor Cyan = WithOpacity(FLinearColor(0.04f, 0.70f, 0.82f, 0.58f), Opacity);
	const FLinearColor LeftRail = WithOpacity(FLinearColor(
		Core.LeftTeamColor.R, Core.LeftTeamColor.G, Core.LeftTeamColor.B, 0.72f), Opacity);
	const FLinearColor RightRail = WithOpacity(FLinearColor(
		Core.RightTeamColor.R, Core.RightTeamColor.G, Core.RightTeamColor.B, 0.72f), Opacity);

	TArray<FCanvasUVTri> Tris;
	Tris.Reserve(64 + 4 * (LeftCount + RightCount));

	// Three overlapping convex shells read as one connected chassis while keeping
	// the triangle fan simple and robust on the UE4.15 Canvas path.
	const FVector2D CenterShell[6] =
	{
		FVector2D(G.CoreLeft - 20.f * U, ShellTop),
		FVector2D(G.CoreRight + 20.f * U, ShellTop),
		FVector2D(G.CoreRight + 32.f * U, CoreBottom + 7.f * U),
		FVector2D(G.CoreRight + 22.f * U, ShellBottom),
		FVector2D(G.CoreLeft - 22.f * U, ShellBottom),
		FVector2D(G.CoreLeft - 32.f * U, CoreBottom + 7.f * U),
	};
	AddConvexPolygon(Tris, CenterShell, 6, Shell);
	const FVector2D LeftWing[5] =
	{
		FVector2D(LeftOuter + 14.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreLeft + 2.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreLeft + 18.f * U, ShellBottom - 3.f * U),
		FVector2D(LeftOuter + 30.f * U, ShellBottom),
		FVector2D(LeftOuter, ShellBottom - 17.f * U),
	};
	AddConvexPolygon(Tris, LeftWing, 5, InnerShell);
	const FVector2D RightWing[5] =
	{
		FVector2D(G.CoreRight - 2.f * U, ShellTop + 2.f * U),
		FVector2D(RightOuter - 14.f * U, ShellTop + 2.f * U),
		FVector2D(RightOuter, ShellBottom - 17.f * U),
		FVector2D(RightOuter - 30.f * U, ShellBottom),
		FVector2D(G.CoreRight - 18.f * U, ShellBottom - 3.f * U),
	};
	AddConvexPolygon(Tris, RightWing, 5, InnerShell);

	// Portrait slot backplates bridge the rectangular atlas crops into the angled
	// chassis. The actual portrait and gameplay ink are submitted later by the HUD.
	for (int32 Index = 0; Index < LeftCount; ++Index)
	{
		const FVector2D P = G.GetPortraitPosition(ENCPlusBetaTopBarSide::Left, Index);
		AddChamferedRect(Tris, P.X - 1.5f * U, P.Y - 1.5f * U,
			P.X + G.PortraitWidth + 1.5f * U,
			P.Y + G.PortraitHeight + 11.f * U, 3.f * U,
			WithOpacity(MutedTeamPlate(Core.LeftTeamColor, 0.76f), Opacity));
	}
	for (int32 Index = 0; Index < RightCount; ++Index)
	{
		const FVector2D P = G.GetPortraitPosition(ENCPlusBetaTopBarSide::Right, Index);
		AddChamferedRect(Tris, P.X - 1.5f * U, P.Y - 1.5f * U,
			P.X + G.PortraitWidth + 1.5f * U,
			P.Y + G.PortraitHeight + 11.f * U, 3.f * U,
			WithOpacity(MutedTeamPlate(Core.RightTeamColor, 0.76f), Opacity));
	}

	// Team identity lives in restrained upper rails; a shared cyan lower rail ties
	// both wings to the neutral clock core.
	AddQuad(Tris,
		FVector2D(LeftOuter + 15.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreLeft - 3.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreLeft + 1.f * U, ShellTop + 4.f * U),
		FVector2D(LeftOuter + 18.f * U, ShellTop + 4.f * U), LeftRail);
	AddQuad(Tris,
		FVector2D(G.CoreRight + 3.f * U, ShellTop + 2.f * U),
		FVector2D(RightOuter - 15.f * U, ShellTop + 2.f * U),
		FVector2D(RightOuter - 18.f * U, ShellTop + 4.f * U),
		FVector2D(G.CoreRight - 1.f * U, ShellTop + 4.f * U), RightRail);
	AddQuad(Tris,
		FVector2D(LeftOuter + 29.f * U, ShellBottom - 3.f * U),
		FVector2D(G.CoreLeft - 17.f * U, ShellBottom - 3.f * U),
		FVector2D(G.CoreLeft - 10.f * U, ShellBottom - 1.f * U),
		FVector2D(LeftOuter + 33.f * U, ShellBottom - 1.f * U), Cyan);
	AddQuad(Tris,
		FVector2D(G.CoreRight + 17.f * U, ShellBottom - 3.f * U),
		FVector2D(RightOuter - 29.f * U, ShellBottom - 3.f * U),
		FVector2D(RightOuter - 33.f * U, ShellBottom - 1.f * U),
		FVector2D(G.CoreRight + 10.f * U, ShellBottom - 1.f * U), Cyan);

	// Compact beveled score | clock | score core. An outer steel plate creates a
	// fine technical outline without a noisy full-saturation perimeter.
	AddChamferedRect(Tris, G.CoreLeft - 2.f * U, G.TopY - 2.f * U,
		G.CoreRight + 2.f * U, CoreBottom + 2.f * U, 10.f * U, Steel);
	const float ScoreWidth = BetaScoreWidth * U;
	const float ClockWidth = BetaClockWidth * U;
	const float ClockLeft = G.CoreLeft + ScoreWidth;
	const float ClockRight = ClockLeft + ClockWidth;
	AddChamferedRect(Tris, G.CoreLeft, G.TopY, ClockLeft + 2.f * U,
		CoreBottom, 9.f * U,
		WithOpacity(MutedTeamPlate(Core.LeftTeamColor, 0.95f), Opacity));
	AddChamferedRect(Tris, ClockLeft - 2.f * U, G.TopY, ClockRight + 2.f * U,
		CoreBottom, 8.f * U,
		WithOpacity(FLinearColor(0.006f, 0.016f, 0.025f, 0.96f), Opacity));
	AddChamferedRect(Tris, ClockRight - 2.f * U, G.TopY, G.CoreRight,
		CoreBottom, 9.f * U,
		WithOpacity(MutedTeamPlate(Core.RightTeamColor, 0.95f), Opacity));
	AddQuad(Tris, FVector2D(G.CoreLeft + 9.f * U, G.TopY),
		FVector2D(ClockLeft - 4.f * U, G.TopY),
		FVector2D(ClockLeft - 2.f * U, G.TopY + 1.6f * U),
		FVector2D(G.CoreLeft + 10.f * U, G.TopY + 1.6f * U), LeftRail);
	AddQuad(Tris, FVector2D(ClockRight + 4.f * U, G.TopY),
		FVector2D(G.CoreRight - 9.f * U, G.TopY),
		FVector2D(G.CoreRight - 10.f * U, G.TopY + 1.6f * U),
		FVector2D(ClockRight + 2.f * U, G.TopY + 1.6f * U), RightRail);

	FTexture* WhiteTexture = Canvas->DefaultTexture->Resource;
	if (WhiteTexture != nullptr && Tris.Num() > 0)
	{
		FCanvasTriangleItem Item(Tris, WhiteTexture);
		Item.BlendMode = ESimpleElementBlendMode::SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}

	// Deferred text sits over the single geometry batch.
	const float FontExtra = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	const float ScoreDesiredScale = 1.15f * U * FontExtra;
	const float CenterDesiredScale = (Core.ClockSeconds >= 0 ? 0.95f : 0.72f)
		* U * FontExtra;
	const float TextHeightLimit = FMath::Max(1.f, G.CoreHeight - 10.f * U);
	const float ScoreWidthLimit = FMath::Max(1.f, ScoreWidth - 16.f * U);
	const float ClockWidthLimit = FMath::Max(1.f, ClockWidth - 20.f * U);
	const uint8 TextAlpha = uint8(FMath::Clamp(FMath::RoundToInt(Opacity * 255.f), 0, 255));

	FText Text;
	float XL = 0.f, YL = 0.f;
	const FString& LeftScoreString = ResolveBetaNumber(Core.LeftScore, false);
	const float LeftScale = ResolveFittedStableText(Canvas, ScoreFont, LeftScoreString,
		ScoreDesiredScale, ScoreWidthLimit, TextHeightLimit, Text, XL, YL);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, ScoreFont, Text,
		G.CoreLeft + 0.5f * (ScoreWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), LeftScale, LeftScale,
		FColor(255, 255, 255, TextAlpha));

	const FString& RightScoreString = ResolveBetaNumber(Core.RightScore, false);
	const float RightScale = ResolveFittedStableText(Canvas, ScoreFont, RightScoreString,
		ScoreDesiredScale, ScoreWidthLimit, TextHeightLimit, Text, XL, YL);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, ScoreFont, Text,
		ClockRight + 0.5f * (ScoreWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), RightScale, RightScale,
		FColor(255, 255, 255, TextAlpha));

	static const FString VersusString(TEXT("VS"));
	const FString& CenterString = Core.ClockSeconds >= 0
		? ResolveBetaNumber(Core.ClockSeconds, true)
		: (Core.CenterFallback.IsEmpty() ? VersusString : Core.CenterFallback);
	const float CenterScale = ResolveFittedStableText(Canvas, ClockFont, CenterString,
		CenterDesiredScale, ClockWidthLimit, TextHeightLimit, Text, XL, YL);
	const FColor CenterColor = (Core.ClockSeconds >= 0 && Core.ClockSeconds <= 30)
		? FColor(255, 48, 48, TextAlpha)
		: (Core.ClockSeconds >= 0)
			? FColor(255, 255, 255, TextAlpha)
			: FColor(205, 218, 228, TextAlpha);
	NCPlusHUDDrawCall::DrawResolvedText(Canvas, ClockFont, Text,
		ClockLeft + 0.5f * (ClockWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), CenterScale, CenterScale,
		CenterColor);
}
