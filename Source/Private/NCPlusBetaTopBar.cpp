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
	constexpr float BetaScoreWidth = 95.f;
	constexpr float BetaClockWidth = 190.f;
	constexpr float BetaPortraitGap = 24.f;
	constexpr float BetaCorePortraitGap = 15.f;
	constexpr float BetaDefaultTopY = 22.f;
	constexpr float BetaOuterWing = 53.f;
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

	FLinearColor NeutralTacticalPanel(float Alpha)
	{
		return FLinearColor(0.0015f, 0.004f, 0.007f, Alpha);
	}

	FLinearColor TeamAccent(const FLinearColor& TeamColor, float Alpha)
	{
		const FLinearColor SafeColor = TeamColor.GetClamped(0.f, 1.f);
		return FLinearColor(
			FMath::Clamp(0.015f + 0.58f * SafeColor.R, 0.f, 1.f),
			FMath::Clamp(0.020f + 0.58f * SafeColor.G, 0.f, 1.f),
			FMath::Clamp(0.025f + 0.58f * SafeColor.B, 0.f, 1.f),
			Alpha);
	}

	FVector2D ResolvePortraitLayoutDelta(FName Alias, UCanvas* Canvas)
	{
		if (Alias == NAME_None || Canvas == nullptr)
		{
			return FVector2D::ZeroVector;
		}

		// Portrait X/Y remain useful in a connected ribbon as per-bank offsets.
		// Subtract the alias's stock position so untouched/default portrait rows
		// continue to follow a moved scorebar parent instead of pinning themselves
		// to an unrelated absolute screen coordinate.
		const ENCPlusHUDAnchor StockAnchor = NCPlusHUDAliases::GetStockAnchor(Alias);
		const FVector2D StockAnchorCoords = FNCPlusHUDLayout::AnchorToScreenCoords(StockAnchor);
		const float RenderScale = Canvas->ClipY / 1080.f;
		const FVector2D StockOffset = NCPlusHUDAliases::GetStockOffset(Alias);
		const FVector2D StockPosition(
			StockAnchorCoords.X * Canvas->ClipX + StockOffset.X * RenderScale,
			StockAnchorCoords.Y * Canvas->ClipY + StockOffset.Y * RenderScale);
		return NCPlusHUDDrawCall::ResolveScreenPos(Alias, Canvas, StockPosition)
			- StockPosition;
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
	const FNCPlusBetaTopBarPortraitGeometry& SideGeometry = GetPortraitGeometry(Side);
	const float SafeOrdinal = float(FMath::Max(0, Ordinal));
	const float X = (Side == ENCPlusBetaTopBarSide::Left)
		? SideGeometry.AnchorX - SideGeometry.Width - SafeOrdinal * SideGeometry.Pitch
		: SideGeometry.AnchorX + SafeOrdinal * SideGeometry.Pitch;
	return FVector2D(X, SideGeometry.Y);
}

const FNCPlusBetaTopBarPortraitGeometry&
FNCPlusBetaTopBarGeometry::GetPortraitGeometry(ENCPlusBetaTopBarSide Side) const
{
	return Portrait[int32(Side)];
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
	FName LeftPortraitAlias, FName RightPortraitAlias,
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
	const FVector2D StockPos(Canvas->ClipX * 0.5f, 0.f);
	const FVector2D ResolvedCenter = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("scorebar"), Canvas, StockPos);
	// The shared scorebar layout resolves TopCenter/Y=0 even when the fallback has
	// a margin, so make the mock's inset intrinsic to the Beta chassis. User drag
	// offsets still apply; they now move a complete, unclipped frame.
	const FVector2D Center(ResolvedCenter.X,
		ResolvedCenter.Y + BetaDefaultTopY * ViewScale);

	OutGeometry = FNCPlusBetaTopBarGeometry();
	OutGeometry.Center = Center;
	OutGeometry.TopY = Center.Y;
	OutGeometry.UnitScale = UnitScale;
	OutGeometry.CoreHeight = BetaCoreHeight * UnitScale;
	const float CoreWidth = (2.f * BetaScoreWidth + BetaClockWidth) * UnitScale;
	OutGeometry.CoreLeft = Center.X - 0.5f * CoreWidth;
	OutGeometry.CoreRight = Center.X + 0.5f * CoreWidth;

	const FName PortraitAliases[2] = { LeftPortraitAlias, RightPortraitAlias };
	const int32 PortraitCounts[2] = { LeftPortraitCount, RightPortraitCount };
	const float ParentAnchors[2] =
	{
		OutGeometry.CoreLeft - BetaCorePortraitGap * UnitScale,
		OutGeometry.CoreRight + BetaCorePortraitGap * UnitScale,
	};
	for (int32 SideIndex = 0; SideIndex < 2; ++SideIndex)
	{
		FNCPlusBetaTopBarPortraitGeometry& Side = OutGeometry.Portrait[SideIndex];
		Side.Alias = PortraitAliases[SideIndex];
		Side.UnitScale = UnitScale * NCPlusHUDDrawCall::GetScale(Side.Alias);
		Side.Height = BetaCoreHeight * Side.UnitScale;
		Side.Width = (BetaCoreHeight / BetaPortraitAspect) * Side.UnitScale;
		Side.Pitch = Side.Width + BetaPortraitGap * Side.UnitScale;
		const FVector2D LayoutDelta = ResolvePortraitLayoutDelta(Side.Alias, Canvas);
		Side.AnchorX = ParentAnchors[SideIndex] + LayoutDelta.X;
		Side.Y = OutGeometry.TopY + 1.f * UnitScale + LayoutDelta.Y;
		// Position and scale inherit from the connected scorebar transform, while
		// styling remains owned by the portrait row. This keeps the editor's two
		// opacity controls independent.
		Side.Opacity = NCPlusHUDDrawCall::GetOpacity(Side.Alias);
		// Keep the chassis/card-frame count identical to the owning HUD's render
		// loop. Large community servers can exceed 32 players on one side; clipping
		// only the frame count leaves later portraits floating outside the ribbon.
		Side.Count = FMath::Max(0, PortraitCounts[SideIndex]);
	}
	return true;
}

bool NCPlusBetaTopBar::BuildGeometry(AUTHUD* HUD, UCanvas* Canvas,
	int32 LeftPortraitCount, int32 RightPortraitCount,
	FNCPlusBetaTopBarGeometry& OutGeometry)
{
	return BuildGeometry(HUD, Canvas, LeftPortraitCount, RightPortraitCount,
		TEXT("portrait_red"), TEXT("portrait_blue"), OutGeometry);
}

void NCPlusBetaTopBar::DrawChassisAndScoreCore(AUTHUD* HUD, UCanvas* Canvas,
	const FNCPlusBetaTopBarGeometry& G, const FNCPlusBetaTopBarCore& Core)
{
	if (HUD == nullptr || Canvas == nullptr || Canvas->DefaultTexture == nullptr)
	{
		return;
	}

	const float U = G.UnitScale;
	const bool bDrawCore = !NCPlusHUDDrawCall::IsHidden(TEXT("scorebar"));
	const float Opacity = NCPlusHUDDrawCall::GetOpacity(TEXT("scorebar"));
	const FNCPlusBetaTopBarPortraitGeometry& LeftPortrait =
		G.GetPortraitGeometry(ENCPlusBetaTopBarSide::Left);
	const FNCPlusBetaTopBarPortraitGeometry& RightPortrait =
		G.GetPortraitGeometry(ENCPlusBetaTopBarSide::Right);
	const int32 LeftCount = LeftPortrait.Count;
	const int32 RightCount = RightPortrait.Count;
	const float LeftNearest = LeftPortrait.AnchorX - LeftPortrait.Width;
	const float RightNearest = RightPortrait.AnchorX;
	const float LeftMost = LeftCount > 0
		? LeftNearest - float(LeftCount - 1) * LeftPortrait.Pitch
		: G.CoreLeft - 18.f * U;
	const float RightMost = RightCount > 0
		? RightNearest + float(RightCount - 1) * RightPortrait.Pitch + RightPortrait.Width
		: G.CoreRight + 18.f * U;
	const float LeftOuter = LeftMost - BetaOuterWing * U;
	const float RightOuter = RightMost + BetaOuterWing * U;
	const float LeftCardTop = LeftCount > 0 ? LeftPortrait.Y - 5.f * LeftPortrait.UnitScale : G.TopY;
	const float RightCardTop = RightCount > 0 ? RightPortrait.Y - 5.f * RightPortrait.UnitScale : G.TopY;
	const float LeftCardBottom = LeftCount > 0
		? LeftPortrait.Y + LeftPortrait.Height + 18.f * LeftPortrait.UnitScale : G.TopY;
	const float RightCardBottom = RightCount > 0
		? RightPortrait.Y + RightPortrait.Height + 18.f * RightPortrait.UnitScale : G.TopY;
	const float ShellTop = G.TopY - 5.f * U;
	const float ShellBottom = G.TopY + G.CoreHeight + 18.f * U;
	const float LeftShellTop = LeftCount > 0 ? FMath::Min(ShellTop, LeftCardTop) : ShellTop;
	const float LeftShellBottom = LeftCount > 0
		? FMath::Max(ShellBottom, LeftCardBottom) : ShellBottom;
	const float RightShellTop = RightCount > 0 ? FMath::Min(ShellTop, RightCardTop) : ShellTop;
	const float RightShellBottom = RightCount > 0
		? FMath::Max(ShellBottom, RightCardBottom) : ShellBottom;
	const float CoreBottom = G.TopY + G.CoreHeight;

	const FLinearColor Shadow = WithOpacity(FLinearColor(0.f, 0.f, 0.f, 0.56f), Opacity);
	const FLinearColor Shell = WithOpacity(FLinearColor(0.0015f, 0.004f, 0.007f, 0.98f), Opacity);
	const FLinearColor Panel = WithOpacity(FLinearColor(0.0015f, 0.003f, 0.006f, 0.99f), Opacity);
	const FLinearColor Steel = WithOpacity(FLinearColor(0.025f, 0.105f, 0.145f, 0.82f), Opacity);
	const FLinearColor LeftWingShadowColor = WithOpacity(
		FLinearColor(0.f, 0.f, 0.f, 0.56f), LeftPortrait.Opacity);
	const FLinearColor RightWingShadowColor = WithOpacity(
		FLinearColor(0.f, 0.f, 0.f, 0.56f), RightPortrait.Opacity);
	const FLinearColor LeftWingSteel = WithOpacity(
		FLinearColor(0.025f, 0.105f, 0.145f, 0.82f), LeftPortrait.Opacity);
	const FLinearColor RightWingSteel = WithOpacity(
		FLinearColor(0.025f, 0.105f, 0.145f, 0.82f), RightPortrait.Opacity);
	const FLinearColor LeftWingFill = WithOpacity(
		FLinearColor(0.003f, 0.008f, 0.013f, 0.96f), LeftPortrait.Opacity);
	const FLinearColor RightWingFill = WithOpacity(
		FLinearColor(0.003f, 0.008f, 0.013f, 0.96f), RightPortrait.Opacity);
	const FLinearColor LeftCyanGlow = WithOpacity(
		FLinearColor(0.015f, 0.26f, 0.34f, 0.28f), LeftPortrait.Opacity);
	const FLinearColor RightCyanGlow = WithOpacity(
		FLinearColor(0.015f, 0.26f, 0.34f, 0.28f), RightPortrait.Opacity);
	const FLinearColor LeftCyan = WithOpacity(
		FLinearColor(0.025f, 0.52f, 0.66f, 0.72f), LeftPortrait.Opacity);
	const FLinearColor RightCyan = WithOpacity(
		FLinearColor(0.025f, 0.52f, 0.66f, 0.72f), RightPortrait.Opacity);
	const FLinearColor LeftScoreRail = WithOpacity(TeamAccent(Core.LeftTeamColor, 0.84f), Opacity);
	const FLinearColor RightScoreRail = WithOpacity(TeamAccent(Core.RightTeamColor, 0.84f), Opacity);
	const FLinearColor LeftPortraitRail = WithOpacity(
		TeamAccent(Core.LeftPortraitColor, 0.84f), LeftPortrait.Opacity);
	const FLinearColor RightPortraitRail = WithOpacity(
		TeamAccent(Core.RightPortraitColor, 0.84f), RightPortrait.Opacity);

	TArray<FCanvasUVTri> Tris;
	Tris.Reserve(192 + 12 * (LeftCount + RightCount));

	// Layer 1: a black extrusion beneath the whole chassis. Keeping it in the same
	// triangle item gives the mock's depth without another Canvas submission.
	const FVector2D CenterShadow[6] =
	{
		FVector2D(G.CoreLeft - 24.f * U, ShellTop + 3.f * U),
		FVector2D(G.CoreRight + 24.f * U, ShellTop + 3.f * U),
		FVector2D(G.CoreRight + 36.f * U, CoreBottom + 10.f * U),
		FVector2D(G.CoreRight + 25.f * U, ShellBottom + 3.f * U),
		FVector2D(G.CoreLeft - 25.f * U, ShellBottom + 3.f * U),
		FVector2D(G.CoreLeft - 36.f * U, CoreBottom + 10.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, CenterShadow, 6, Shadow);
	const FVector2D LeftWingShadow[5] =
	{
		FVector2D(LeftOuter + 14.f * U, LeftShellTop + 5.f * U),
		FVector2D(G.CoreLeft + 4.f * U, LeftShellTop + 5.f * U),
		FVector2D(G.CoreLeft + 22.f * U, LeftShellBottom),
		FVector2D(LeftOuter + 33.f * U, LeftShellBottom + 3.f * U),
		FVector2D(LeftOuter - 2.f * U, LeftShellBottom - 14.f * U),
	};
	if (LeftCount > 0) AddConvexPolygon(Tris, LeftWingShadow, 5, LeftWingShadowColor);
	const FVector2D RightWingShadow[5] =
	{
		FVector2D(G.CoreRight - 4.f * U, RightShellTop + 5.f * U),
		FVector2D(RightOuter - 14.f * U, RightShellTop + 5.f * U),
		FVector2D(RightOuter + 2.f * U, RightShellBottom - 14.f * U),
		FVector2D(RightOuter - 33.f * U, RightShellBottom + 3.f * U),
		FVector2D(G.CoreRight - 22.f * U, RightShellBottom),
	};
	if (RightCount > 0) AddConvexPolygon(Tris, RightWingShadow, 5, RightWingShadowColor);

	// Layer 2: cyan/steel outer bevel, then an almost-opaque neutral interior.
	const FVector2D CenterBevel[6] =
	{
		FVector2D(G.CoreLeft - 22.f * U, ShellTop),
		FVector2D(G.CoreRight + 22.f * U, ShellTop),
		FVector2D(G.CoreRight + 34.f * U, CoreBottom + 8.f * U),
		FVector2D(G.CoreRight + 23.f * U, ShellBottom),
		FVector2D(G.CoreLeft - 23.f * U, ShellBottom),
		FVector2D(G.CoreLeft - 34.f * U, CoreBottom + 8.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, CenterBevel, 6, Steel);
	const FVector2D CenterShell[6] =
	{
		FVector2D(G.CoreLeft - 19.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreRight + 19.f * U, ShellTop + 2.f * U),
		FVector2D(G.CoreRight + 30.f * U, CoreBottom + 8.f * U),
		FVector2D(G.CoreRight + 20.f * U, ShellBottom - 2.f * U),
		FVector2D(G.CoreLeft - 20.f * U, ShellBottom - 2.f * U),
		FVector2D(G.CoreLeft - 30.f * U, CoreBottom + 8.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, CenterShell, 6, Shell);

	const FVector2D LeftWingBevel[5] =
	{
		FVector2D(LeftOuter + 14.f * U, LeftShellTop + 2.f * U),
		FVector2D(G.CoreLeft + 2.f * U, LeftShellTop + 2.f * U),
		FVector2D(G.CoreLeft + 19.f * U, LeftShellBottom - 3.f * U),
		FVector2D(LeftOuter + 30.f * U, LeftShellBottom),
		FVector2D(LeftOuter, LeftShellBottom - 17.f * U),
	};
	if (LeftCount > 0) AddConvexPolygon(Tris, LeftWingBevel, 5, LeftWingSteel);
	const FVector2D LeftWing[5] =
	{
		FVector2D(LeftOuter + 17.f * U, LeftShellTop + 4.f * U),
		FVector2D(G.CoreLeft - 1.f * U, LeftShellTop + 4.f * U),
		FVector2D(G.CoreLeft + 15.f * U, LeftShellBottom - 5.f * U),
		FVector2D(LeftOuter + 31.f * U, LeftShellBottom - 2.f * U),
		FVector2D(LeftOuter + 5.f * U, LeftShellBottom - 18.f * U),
	};
	if (LeftCount > 0) AddConvexPolygon(Tris, LeftWing, 5, LeftWingFill);

	const FVector2D RightWingBevel[5] =
	{
		FVector2D(G.CoreRight - 2.f * U, RightShellTop + 2.f * U),
		FVector2D(RightOuter - 14.f * U, RightShellTop + 2.f * U),
		FVector2D(RightOuter, RightShellBottom - 17.f * U),
		FVector2D(RightOuter - 30.f * U, RightShellBottom),
		FVector2D(G.CoreRight - 19.f * U, RightShellBottom - 3.f * U),
	};
	if (RightCount > 0) AddConvexPolygon(Tris, RightWingBevel, 5, RightWingSteel);
	const FVector2D RightWing[5] =
	{
		FVector2D(G.CoreRight + 1.f * U, RightShellTop + 4.f * U),
		FVector2D(RightOuter - 17.f * U, RightShellTop + 4.f * U),
		FVector2D(RightOuter - 5.f * U, RightShellBottom - 18.f * U),
		FVector2D(RightOuter - 31.f * U, RightShellBottom - 2.f * U),
		FVector2D(G.CoreRight - 15.f * U, RightShellBottom - 5.f * U),
	};
	if (RightCount > 0) AddConvexPolygon(Tris, RightWing, 5, RightWingFill);

	// Layer 3: wide angular card frames around the unchanged portrait crop. The
	// 6px side bleed plus the larger pitch produces the mock's airy player banks
	// without stretching the 224:320 atlas image.
	for (int32 Index = 0; Index < LeftCount; ++Index)
	{
		const FVector2D P = G.GetPortraitPosition(ENCPlusBetaTopBarSide::Left, Index);
		const float SideU = LeftPortrait.UnitScale;
		AddChamferedRect(Tris, P.X - 6.f * SideU, P.Y - 2.f * SideU,
			P.X + LeftPortrait.Width + 6.f * SideU,
			P.Y + LeftPortrait.Height + 15.f * SideU, 4.f * SideU, LeftPortraitRail);
		AddChamferedRect(Tris, P.X - 4.f * SideU, P.Y,
			P.X + LeftPortrait.Width + 4.f * SideU,
			P.Y + LeftPortrait.Height + 13.f * SideU, 3.f * SideU,
			WithOpacity(NeutralTacticalPanel(0.99f), LeftPortrait.Opacity));
	}
	for (int32 Index = 0; Index < RightCount; ++Index)
	{
		const FVector2D P = G.GetPortraitPosition(ENCPlusBetaTopBarSide::Right, Index);
		const float SideU = RightPortrait.UnitScale;
		AddChamferedRect(Tris, P.X - 6.f * SideU, P.Y - 2.f * SideU,
			P.X + RightPortrait.Width + 6.f * SideU,
			P.Y + RightPortrait.Height + 15.f * SideU, 4.f * SideU, RightPortraitRail);
		AddChamferedRect(Tris, P.X - 4.f * SideU, P.Y,
			P.X + RightPortrait.Width + 4.f * SideU,
			P.Y + RightPortrait.Height + 13.f * SideU, 3.f * SideU,
			WithOpacity(NeutralTacticalPanel(0.99f), RightPortrait.Opacity));
	}

	// Layer 4: team identity is confined to thin rails. A broad low-alpha cyan
	// pass under the 1px bright line fakes the mock's glow without a material.
	if (LeftCount > 0)
	{
		AddQuad(Tris,
			FVector2D(LeftOuter + 17.f * U, LeftShellTop + 3.f * U),
			FVector2D(G.CoreLeft - 4.f * U, LeftShellTop + 3.f * U),
			FVector2D(G.CoreLeft - 2.f * U, LeftShellTop + 4.5f * U),
			FVector2D(LeftOuter + 19.f * U, LeftShellTop + 4.5f * U), LeftPortraitRail);
		AddQuad(Tris,
			FVector2D(LeftOuter + 29.f * U, LeftShellBottom - 6.f * U),
			FVector2D(G.CoreLeft - 16.f * U, LeftShellBottom - 6.f * U),
			FVector2D(G.CoreLeft - 10.f * U, LeftShellBottom - 1.f * U),
			FVector2D(LeftOuter + 34.f * U, LeftShellBottom - 1.f * U), LeftCyanGlow);
		AddQuad(Tris,
			FVector2D(LeftOuter + 32.f * U, LeftShellBottom - 3.f * U),
			FVector2D(G.CoreLeft - 14.f * U, LeftShellBottom - 3.f * U),
			FVector2D(G.CoreLeft - 11.f * U, LeftShellBottom - 1.5f * U),
			FVector2D(LeftOuter + 35.f * U, LeftShellBottom - 1.5f * U), LeftCyan);
	}
	if (RightCount > 0)
	{
		AddQuad(Tris,
			FVector2D(G.CoreRight + 4.f * U, RightShellTop + 3.f * U),
			FVector2D(RightOuter - 17.f * U, RightShellTop + 3.f * U),
			FVector2D(RightOuter - 19.f * U, RightShellTop + 4.5f * U),
			FVector2D(G.CoreRight + 2.f * U, RightShellTop + 4.5f * U), RightPortraitRail);
		AddQuad(Tris,
			FVector2D(G.CoreRight + 16.f * U, RightShellBottom - 6.f * U),
			FVector2D(RightOuter - 29.f * U, RightShellBottom - 6.f * U),
			FVector2D(RightOuter - 34.f * U, RightShellBottom - 1.f * U),
			FVector2D(G.CoreRight + 10.f * U, RightShellBottom - 1.f * U), RightCyanGlow);
		AddQuad(Tris,
			FVector2D(G.CoreRight + 14.f * U, RightShellBottom - 3.f * U),
			FVector2D(RightOuter - 32.f * U, RightShellBottom - 3.f * U),
			FVector2D(RightOuter - 35.f * U, RightShellBottom - 1.5f * U),
			FVector2D(G.CoreRight + 11.f * U, RightShellBottom - 1.5f * U), RightCyan);
	}

	// Layer 5: mirrored score trapezoids and a central clock hex. Each panel is
	// drawn accent-first and inset-black second, so team color reads as an edge
	// rather than the large pastel blocks in the first Beta pass.
	const float ScoreWidth = BetaScoreWidth * U;
	const float ClockWidth = BetaClockWidth * U;
	const float ClockLeft = G.CoreLeft + ScoreWidth;
	const float ClockRight = ClockLeft + ClockWidth;
	const FVector2D LeftScoreOuter[8] =
	{
		FVector2D(G.CoreLeft + 10.f * U, G.TopY),
		FVector2D(ClockLeft - 5.f * U, G.TopY),
		FVector2D(ClockLeft + 4.f * U, G.TopY + 9.f * U),
		FVector2D(ClockLeft + 4.f * U, CoreBottom - 9.f * U),
		FVector2D(ClockLeft - 5.f * U, CoreBottom),
		FVector2D(G.CoreLeft + 16.f * U, CoreBottom),
		FVector2D(G.CoreLeft, CoreBottom - 15.f * U),
		FVector2D(G.CoreLeft, G.TopY + 13.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, LeftScoreOuter, 8, LeftScoreRail);
	const FVector2D LeftScoreInner[8] =
	{
		FVector2D(G.CoreLeft + 11.f * U, G.TopY + 2.f * U),
		FVector2D(ClockLeft - 6.f * U, G.TopY + 2.f * U),
		FVector2D(ClockLeft + 1.f * U, G.TopY + 10.f * U),
		FVector2D(ClockLeft + 1.f * U, CoreBottom - 10.f * U),
		FVector2D(ClockLeft - 6.f * U, CoreBottom - 2.f * U),
		FVector2D(G.CoreLeft + 17.f * U, CoreBottom - 2.f * U),
		FVector2D(G.CoreLeft + 3.f * U, CoreBottom - 16.f * U),
		FVector2D(G.CoreLeft + 3.f * U, G.TopY + 14.f * U),
	};
	if (bDrawCore)
	{
		AddConvexPolygon(Tris, LeftScoreInner, 8,
			WithOpacity(NeutralTacticalPanel(0.99f), Opacity));
	}

	const FVector2D RightScoreOuter[8] =
	{
		FVector2D(ClockRight + 5.f * U, G.TopY),
		FVector2D(G.CoreRight - 10.f * U, G.TopY),
		FVector2D(G.CoreRight, G.TopY + 13.f * U),
		FVector2D(G.CoreRight, CoreBottom - 15.f * U),
		FVector2D(G.CoreRight - 16.f * U, CoreBottom),
		FVector2D(ClockRight + 5.f * U, CoreBottom),
		FVector2D(ClockRight - 4.f * U, CoreBottom - 9.f * U),
		FVector2D(ClockRight - 4.f * U, G.TopY + 9.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, RightScoreOuter, 8, RightScoreRail);
	const FVector2D RightScoreInner[8] =
	{
		FVector2D(ClockRight + 6.f * U, G.TopY + 2.f * U),
		FVector2D(G.CoreRight - 11.f * U, G.TopY + 2.f * U),
		FVector2D(G.CoreRight - 3.f * U, G.TopY + 14.f * U),
		FVector2D(G.CoreRight - 3.f * U, CoreBottom - 16.f * U),
		FVector2D(G.CoreRight - 17.f * U, CoreBottom - 2.f * U),
		FVector2D(ClockRight + 6.f * U, CoreBottom - 2.f * U),
		FVector2D(ClockRight - 1.f * U, CoreBottom - 10.f * U),
		FVector2D(ClockRight - 1.f * U, G.TopY + 10.f * U),
	};
	if (bDrawCore)
	{
		AddConvexPolygon(Tris, RightScoreInner, 8,
			WithOpacity(NeutralTacticalPanel(0.99f), Opacity));
	}

	const FVector2D ClockOuter[8] =
	{
		FVector2D(ClockLeft + 8.f * U, G.TopY),
		FVector2D(ClockRight - 8.f * U, G.TopY),
		FVector2D(ClockRight + 3.f * U, G.TopY + 9.f * U),
		FVector2D(ClockRight + 3.f * U, CoreBottom - 9.f * U),
		FVector2D(ClockRight - 8.f * U, CoreBottom),
		FVector2D(ClockLeft + 8.f * U, CoreBottom),
		FVector2D(ClockLeft - 3.f * U, CoreBottom - 9.f * U),
		FVector2D(ClockLeft - 3.f * U, G.TopY + 9.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, ClockOuter, 8, Steel);
	const FVector2D ClockInner[8] =
	{
		FVector2D(ClockLeft + 9.f * U, G.TopY + 2.f * U),
		FVector2D(ClockRight - 9.f * U, G.TopY + 2.f * U),
		FVector2D(ClockRight, G.TopY + 10.f * U),
		FVector2D(ClockRight, CoreBottom - 10.f * U),
		FVector2D(ClockRight - 9.f * U, CoreBottom - 2.f * U),
		FVector2D(ClockLeft + 9.f * U, CoreBottom - 2.f * U),
		FVector2D(ClockLeft, CoreBottom - 10.f * U),
		FVector2D(ClockLeft, G.TopY + 10.f * U),
	};
	if (bDrawCore) AddConvexPolygon(Tris, ClockInner, 8, Panel);

	FTexture* WhiteTexture = Canvas->DefaultTexture->Resource;
	if (WhiteTexture != nullptr && Tris.Num() > 0)
	{
		FCanvasTriangleItem Item(Tris, WhiteTexture);
		Item.BlendMode = ESimpleElementBlendMode::SE_BLEND_Translucent;
		Canvas->DrawItem(Item);
	}
	if (!bDrawCore)
	{
		return;
	}

	UFont* ScoreFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), HUD, HUD->LargeFont);
	UFont* ClockFont = NCPlusHUDFonts::Resolve(TEXT("scorebar"), HUD, HUD->MediumFont);
	if (ScoreFont == nullptr) ScoreFont = HUD->LargeFont;
	if (ClockFont == nullptr) ClockFont = HUD->MediumFont;
	if (ScoreFont == nullptr || ClockFont == nullptr) return;

	// Deferred text sits over the single geometry batch.
	const float FontExtra = NCPlusHUDFonts::ResolveScale(TEXT("scorebar"), 1.f);
	const float ScoreDesiredScale = 1.15f * U * FontExtra;
	const float CenterDesiredScale = (Core.ClockSeconds >= 0 ? 0.95f : 0.72f)
		* U * FontExtra;
	const float TextHeightLimit = FMath::Max(1.f, G.CoreHeight - 10.f * U);
	const float ScoreWidthLimit = FMath::Max(1.f, ScoreWidth - 16.f * U);
	const float ClockWidthLimit = FMath::Max(1.f, ClockWidth - 20.f * U);

	FText Text;
	float XL = 0.f, YL = 0.f;
	const FString& LeftScoreString = ResolveBetaNumber(Core.LeftScore, false);
	const float LeftScale = ResolveFittedStableText(Canvas, ScoreFont, LeftScoreString,
		ScoreDesiredScale, ScoreWidthLimit, TextHeightLimit, Text, XL, YL);
	NCPlusHUDDrawCall::DrawOutlinedText(Canvas, ScoreFont, Text,
		G.CoreLeft + 0.5f * (ScoreWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), LeftScale,
		FLinearColor::White, FLinearColor::Black, Opacity);

	const FString& RightScoreString = ResolveBetaNumber(Core.RightScore, false);
	const float RightScale = ResolveFittedStableText(Canvas, ScoreFont, RightScoreString,
		ScoreDesiredScale, ScoreWidthLimit, TextHeightLimit, Text, XL, YL);
	NCPlusHUDDrawCall::DrawOutlinedText(Canvas, ScoreFont, Text,
		ClockRight + 0.5f * (ScoreWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), RightScale,
		FLinearColor::White, FLinearColor::Black, Opacity);

	static const FString VersusString(TEXT("VS"));
	const FString& CenterString = Core.ClockSeconds >= 0
		? ResolveBetaNumber(Core.ClockSeconds, true)
		: (Core.CenterFallback.IsEmpty() ? VersusString : Core.CenterFallback);
	const float CenterScale = ResolveFittedStableText(Canvas, ClockFont, CenterString,
		CenterDesiredScale, ClockWidthLimit, TextHeightLimit, Text, XL, YL);
	const FLinearColor CenterColor = (Core.ClockSeconds >= 0 && Core.ClockSeconds <= 30)
		? FLinearColor(1.f, 0.0025f, 0.0025f, 1.f)
		: (Core.ClockSeconds >= 0)
			? FLinearColor::White
			: FLinearColor(0.61f, 0.70f, 0.78f, 1.f);
	NCPlusHUDDrawCall::DrawOutlinedText(Canvas, ClockFont, Text,
		ClockLeft + 0.5f * (ClockWidth - XL),
		G.TopY + 0.5f * (G.CoreHeight - YL), CenterScale,
		CenterColor, FLinearColor::Black, Opacity);
}
