// Modernized HP/Armor widget for NetcodePlus modes. Five visual styles
// dispatched off the layout's "style" extra (see NCPlusHUDLayout).
#include "NCPlusHUDWidget_QuickStats.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"

namespace NCPlusQS
{
	static const float FlashDuration   = 0.30f;
	static const float PulseDuration   = 0.45f;
	static const int32 LowHealthCutoff = 30;

	// Visual caps for segmented/filled styles. UT4 stock max HP is 199 (rounded
	// to 200 = 4 segments of 50); max armor is 150 = 3 segments of 50.
	static const int32 MaxHealth      = 200;
	static const int32 MaxArmor       = 150;
	static const int32 SegmentSize    = 50;
	static const int32 HealthSegCount = MaxHealth / SegmentSize;   // 4
	static const int32 ArmorSegCount  = MaxArmor  / SegmentSize;   // 3

	// MinimalTypography tunables
	static const float NumberScale     = 1.40f;
	static const float LabelScale      = 0.80f;
	static const float CenterGap       = 70.f;
	static const float DividerY0       = 14.f;
	static const float DividerY1       = 76.f;
	static const float NumberY         = 8.f;
	static const float AccentLineGap   = 4.f;
	static const float LabelGap        = 5.f;
	static const float AccentLineW     = 70.f;
}

UNCPlusHUDWidget_QuickStats::UNCPlusHUDWidget_QuickStats(const FObjectInitializer& OI)
	: Super(OI)
{
	// Default: BottomCenter anchor, lifted 180px so HP/Armor sits clear of
	// the bottom edge without overlapping the ammo cluster on default layouts.
	// Origin matches the anchor so adjusting the layout offset shifts the
	// widget intuitively (negative Y = up). Mirrors the alias-table StockOffset
	// in NCPlusHUDLayout.cpp — keep them in sync so "no layout entry" renders
	// identically to a freshly-created entry.
	Position           = FVector2D(0.f, -180.f);
	Size               = FVector2D(480.f, 100.f);
	ScreenPosition     = FVector2D(0.5f, 1.0f);
	Origin             = FVector2D(0.5f, 1.0f);
	DesignedResolution = 1080.f;

	// Opt out of HUDImpulse — see NCPlusHUDWidget_WeaponBar for the full rationale.
	bShouldKickBack = false;

	LastHealth             = -1;
	LastArmor              = -1;
	HealthDamageFlashEnd   = 0.f;
	ArmorDamageFlashEnd    = 0.f;
	HealthPickupPulseEnd   = 0.f;
	ArmorPickupPulseEnd    = 0.f;
}

float UNCPlusHUDWidget_QuickStats::GetDrawScaleOverride()
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("hp_armor"));
	const float UserScale = E ? FMath::Clamp(E->Scale, 0.25f, 4.f) : 1.f;
	return Super::GetDrawScaleOverride() * UserScale;
}

bool UNCPlusHUDWidget_QuickStats::ShouldDraw_Implementation(bool bShowScores)
{
	return Super::ShouldDraw_Implementation(bShowScores)
		&& UTHUDOwner != nullptr
		&& UTHUDOwner->UTPlayerOwner != nullptr
		&& !UTHUDOwner->bShowComsMenu
		&& !UTHUDOwner->bShowWeaponWheel;
}

void UNCPlusHUDWidget_QuickStats::Draw_Implementation(float DeltaTime)
{
	using namespace NCPlusQS;

	if (!UTHUDOwner || !UTHUDOwner->UTPlayerOwner) return;
	AUTCharacter* Char = Cast<AUTCharacter>(UTHUDOwner->UTPlayerOwner->GetViewTarget());
	if (!Char || Char->IsDead())
	{
		LastHealth = -1;
		LastArmor  = -1;
		return;
	}

	const int32 Health = Char->Health;
	const int32 Armor  = Char->GetArmorAmount();
	const float Now    = GetWorld()->GetTimeSeconds();

	// --- Detect deltas → trigger flashes / pulses ---
	if (LastHealth >= 0)
	{
		if (Health < LastHealth)      HealthDamageFlashEnd = Now + FlashDuration;
		else if (Health > LastHealth) HealthPickupPulseEnd = Now + PulseDuration;
	}
	if (LastArmor >= 0)
	{
		if (Armor < LastArmor)      ArmorDamageFlashEnd = Now + FlashDuration;
		else if (Armor > LastArmor) ArmorPickupPulseEnd = Now + PulseDuration;
	}
	LastHealth = Health;
	LastArmor  = Armor;

	// --- Color palette (shared across styles).
	// Per-element overrides via Extras keys: color_health, color_armor,
	// color_low_hp, color_damage_flash, plus opacity multiplier.
	// NOTE: Colors carry their natural alpha — we DON'T pre-multiply Opacity in
	// because UUTHUDWidget::DrawTexture overwrites color.A. Each draw site must
	// pass `color.A * C.Opacity` through the explicit DrawOpacity argument.
	const FLinearColor White = FLinearColor::White;
	const FNCPlusHUDElement* ColorElem = FNCPlusHUDLayout::GetLive().Find(TEXT("hp_armor"));
	auto Col = [&](FName Key, const FLinearColor& Default) -> FLinearColor
	{
		return ColorElem ? ColorElem->GetExtraColor(Key, Default) : Default;
	};
	const FLinearColor LowHpRed    = Col(TEXT("color_low_hp"),       FLinearColor(1.f, 0.32f, 0.28f, 1.f));
	const FLinearColor DamageFlash = Col(TEXT("color_damage_flash"), FLinearColor(1.f, 0.45f, 0.30f, 1.f));

	FStatColors C;
	C.Opacity      = ColorElem ? FMath::Clamp(ColorElem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;
	C.HealthAccent = Col(TEXT("color_health"), FLinearColor(0.37f, 0.96f, 0.48f, 1.f));
	C.ArmorAccent  = Col(TEXT("color_armor"),  FLinearColor(0.95f, 0.83f, 0.34f, 1.f));

	// Base number colors are user-overridable; low-HP red + damage flash still
	// lerp over the top so behavior remains intact when colors are customized.
	const FLinearColor HealthNumBase = Col(TEXT("color_health_number"), White);
	const FLinearColor ArmorNumBase  = Col(TEXT("color_armor_number"),  White);

	C.HealthNumColor = HealthNumBase;
	if (Health <= LowHealthCutoff)
	{
		const float t = FMath::Clamp(float(Health) / float(LowHealthCutoff), 0.f, 1.f);
		C.HealthNumColor = FMath::Lerp(LowHpRed, HealthNumBase, t);
	}
	if (Now < HealthDamageFlashEnd)
	{
		const float t = (HealthDamageFlashEnd - Now) / FlashDuration;
		C.HealthNumColor = FMath::Lerp(C.HealthNumColor, DamageFlash, t);
	}

	C.ArmorNumColor = ArmorNumBase;
	if (Now < ArmorDamageFlashEnd)
	{
		const float t = (ArmorDamageFlashEnd - Now) / FlashDuration;
		C.ArmorNumColor = FMath::Lerp(C.ArmorNumColor, DamageFlash, t);
	}

	C.HealthPulse = (Now >= HealthPickupPulseEnd) ? 0.f : (HealthPickupPulseEnd - Now) / PulseDuration;
	C.ArmorPulse  = (Now >= ArmorPickupPulseEnd ) ? 0.f : (ArmorPickupPulseEnd  - Now) / PulseDuration;

	const bool bDrawArmor = (Armor > 0);

	// --- Dispatch to chosen style ---
	const FNCPlusHUDElement* LayoutElem = FNCPlusHUDLayout::GetLive().Find(TEXT("hp_armor"));
	const ENCPlusHPArmorStyle Style = LayoutElem
		? NCPlusHPArmorStyle::Parse(LayoutElem->GetExtra(TEXT("style")))
		: ENCPlusHPArmorStyle::MinimalTypography;

	switch (Style)
	{
		case ENCPlusHPArmorStyle::SegmentedBars:    DrawSegmentedBars   (Health, Armor, bDrawArmor, C); break;
		case ENCPlusHPArmorStyle::RadialArcs:       DrawRadialArcs      (Health, Armor, bDrawArmor, C); break;
		case ENCPlusHPArmorStyle::HexChevrons:      DrawHexChevrons     (Health, Armor, bDrawArmor, C); break;
		case ENCPlusHPArmorStyle::VerticalPills:    DrawVerticalPills   (Health, Armor, bDrawArmor, C); break;
		default:                                    DrawMinimalTypography(Health, Armor, bDrawArmor, C); break;
	}
}

// =============================================================================
// Style 1 — MinimalTypography (default)
//   Big white numbers, green/amber labels, slim accent underline.
// =============================================================================

void UNCPlusHUDWidget_QuickStats::DrawMinimalTypography(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C)
{
	using namespace NCPlusQS;

	// Per-element font override (Phase 3.8). Defaults to LargeFont when no override.
	UFont* NumberFont = NCPlusHUDFonts::Resolve(TEXT("hp_armor"), UTHUDOwner, UTHUDOwner->LargeFont);
	UFont* LabelFont  = UTHUDOwner->TinyFont;
	if (!NumberFont || !LabelFont) return;

	const float CenterX = Size.X * 0.5f;

	// --- HEALTH (right-aligned to center divider) ---
	{
		const float TextX = CenterX - CenterGap;
		const float TextY = NumberY;
		const FVector2D NumSize = DrawText(FText::AsNumber(Health),
			TextX, TextY, NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			NumberScale, C.HealthNumColor.A * C.Opacity, C.HealthNumColor,
			ETextHorzPos::Right, ETextVertPos::Top);

		const float NumberW       = NumSize.X * NumberScale;
		const float NumberBottomY = TextY + NumSize.Y * NumberScale;
		const float AccentLineY   = NumberBottomY + AccentLineGap;
		const float LabelY        = AccentLineY + LabelGap;
		const float LineW         = FMath::Max(NumberW, AccentLineW);
		const float LabelCenterX  = TextX - LineW * 0.5f;

		if (Canvas && Canvas->DefaultTexture)
		{
			FLinearColor LineColor = C.HealthAccent;
			const float LineAlpha = (0.85f + C.HealthPulse * 0.15f) * C.Opacity;
			const float LineH = 1.5f + C.HealthPulse * 1.5f;
			DrawTexture(Canvas->DefaultTexture, TextX - LineW, AccentLineY, LineW, LineH,
				0.f, 0.f, 1.f, 1.f, LineAlpha, LineColor);
		}

		DrawText(FText::FromString(TEXT("HEALTH")),
			LabelCenterX, LabelY, LabelFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			LabelScale, C.HealthAccent.A * C.Opacity, C.HealthAccent,
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	// --- ARMOR (left-aligned to center divider) ---
	if (bDrawArmor)
	{
		const float TextX = CenterX + CenterGap;
		const float TextY = NumberY;
		const FVector2D NumSize = DrawText(FText::AsNumber(Armor),
			TextX, TextY, NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			NumberScale, C.ArmorNumColor.A * C.Opacity, C.ArmorNumColor,
			ETextHorzPos::Left, ETextVertPos::Top);

		const float NumberW       = NumSize.X * NumberScale;
		const float NumberBottomY = TextY + NumSize.Y * NumberScale;
		const float AccentLineY   = NumberBottomY + AccentLineGap;
		const float LabelY        = AccentLineY + LabelGap;
		const float LineW         = FMath::Max(NumberW, AccentLineW);
		const float LabelCenterX  = TextX + LineW * 0.5f;

		if (Canvas && Canvas->DefaultTexture)
		{
			FLinearColor LineColor = C.ArmorAccent;
			const float LineAlpha = (0.85f + C.ArmorPulse * 0.15f) * C.Opacity;
			const float LineH = 1.5f + C.ArmorPulse * 1.5f;
			DrawTexture(Canvas->DefaultTexture, TextX, AccentLineY, LineW, LineH,
				0.f, 0.f, 1.f, 1.f, LineAlpha, LineColor);
		}

		DrawText(FText::FromString(TEXT("ARMOR")),
			LabelCenterX, LabelY, LabelFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			LabelScale, C.ArmorAccent.A * C.Opacity, C.ArmorAccent,
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	// Vertical divider
	if (Canvas && Canvas->DefaultTexture)
	{
		const FLinearColor DividerColor(0.7f, 0.55f, 0.30f, 0.45f);
		DrawTexture(Canvas->DefaultTexture, CenterX - 0.5f, DividerY0,
			1.f, DividerY1 - DividerY0, 0.f, 0.f, 1.f, 1.f, DividerColor.A * C.Opacity, DividerColor);
	}
}

// =============================================================================
// Style 2 — SegmentedBars
//   5 base segments per stat (0-100) + 1 brighter overflow segment (100-125).
// =============================================================================

void UNCPlusHUDWidget_QuickStats::DrawSegmentedBars(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C)
{
	using namespace NCPlusQS;
	// Per-element font override (Phase 3.8). Defaults to LargeFont when no override.
	UFont* NumberFont = NCPlusHUDFonts::Resolve(TEXT("hp_armor"), UTHUDOwner, UTHUDOwner->LargeFont);
	if (!NumberFont || !Canvas || !Canvas->DefaultTexture) return;

	// Segment math: each segment is SegmentSize (50) units. Full segments are
	// solid, the "active" segment partially fills proportional to remainder.
	const float SegW       = 50.f;   // wider since we have only 4 segments now
	const float SegH       = 18.f;
	const float SegGap     = 5.f;
	const float NumWidth   = 80.f;   // reserved column for the integer label on the left
	const float RowGap     = 18.f;
	const float RowY[2]    = { 14.f, 14.f + SegH + RowGap };
	const float StripStartX = NumWidth + 10.f;

	const float NumScale = 0.70f;   // small enough to vertically fit inside SegH
	const float NumNudgeY = -4.f;   // visual centering compensates for font asymmetry

	auto DrawRow = [&](int32 Value, int32 SegCount, float Y,
	                   const FLinearColor& FillColor, float Pulse)
	{
		// Number on the left, visually centered on the segment row's midpoint.
		DrawText(FText::AsNumber(Value), NumWidth, Y + SegH * 0.5f + NumNudgeY, NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			NumScale, FillColor.A * C.Opacity, FillColor,
			ETextHorzPos::Right, ETextVertPos::Center);

		const float FillAlpha   = (0.85f + Pulse * 0.15f) * C.Opacity;
		const FLinearColor EmptyOutline(0.18f, 0.20f, 0.18f, 1.f);
		const float EmptyAlpha  = 0.7f * C.Opacity;

		const int32 FullCount   = FMath::Clamp(Value / SegmentSize, 0, SegCount);
		const float ActiveFrac  = FMath::Clamp(float(Value % SegmentSize) / float(SegmentSize), 0.f, 1.f);

		for (int32 i = 0; i < SegCount; i++)
		{
			const float X = StripStartX + i * (SegW + SegGap);
			DrawTexture(Canvas->DefaultTexture, X, Y, SegW, SegH, 0, 0, 1, 1, EmptyAlpha, EmptyOutline);
			if (i < FullCount)
			{
				DrawTexture(Canvas->DefaultTexture, X, Y, SegW, SegH, 0, 0, 1, 1, FillAlpha, FillColor);
			}
			else if (i == FullCount && ActiveFrac > 0.f)
			{
				DrawTexture(Canvas->DefaultTexture, X, Y, SegW * ActiveFrac, SegH, 0, 0, 1, 1, FillAlpha, FillColor);
			}
		}
	};

	DrawRow(Health, HealthSegCount, RowY[0], C.HealthAccent, C.HealthPulse);
	if (bDrawArmor)
	{
		DrawRow(Armor, ArmorSegCount, RowY[1], C.ArmorAccent, C.ArmorPulse);
	}
}

// =============================================================================
// Style 3 — RadialArcs
//   Two concentric arcs (outer = HP, inner = armor) with centered numbers.
//   Arcs are tessellated into N triangle strips using DrawTri primitives.
// =============================================================================

namespace
{
	// Convert widget-local design pixels to screen pixels using the widget's
	// RenderPosition (post-Origin top-left in screen space) and RenderScale.
	// All shape helpers below take WIDGET-LOCAL coords and apply this transform
	// before submitting triangles, so the pipeline stays consistent with
	// DrawText / DrawTexture.
	FORCEINLINE FVector2D LocalToScreen(const FVector2D& Local, const FVector2D& RenderPos, float Scale)
	{
		return FVector2D(RenderPos.X + Local.X * Scale, RenderPos.Y + Local.Y * Scale);
	}

	// Helper: draw a filled arc as a triangle ring around widget-local (CX, CY)
	// with given inner/outer radius (design pixels), sweep angle (radians,
	// 0 = north, clockwise), N segments. Batches into one K2_DrawTriangle call.
	void DrawArc(UCanvas* Canvas, const FVector2D& RenderPos, float Scale,
	             float CX, float CY, float InnerR, float OuterR,
	             float StartRad, float EndRad, int32 N, const FLinearColor& Color)
	{
		if (!Canvas || N <= 0 || EndRad <= StartRad) return;
		const float Step = (EndRad - StartRad) / float(N);

		// 0 rad = pointing UP (negative Y), increasing CW. Returns SCREEN coords.
		auto PtAt = [&](float Rad, float R) -> FVector2D {
			const FVector2D Local(CX + FMath::Sin(Rad) * R, CY - FMath::Cos(Rad) * R);
			return LocalToScreen(Local, RenderPos, Scale);
		};

		TArray<FCanvasUVTri> Tris;
		Tris.Reserve(N * 2);

		for (int32 i = 0; i < N; i++)
		{
			const float A0 = StartRad + i * Step;
			const float A1 = StartRad + (i + 1) * Step;
			const FVector2D Inner0 = PtAt(A0, InnerR);
			const FVector2D Outer0 = PtAt(A0, OuterR);
			const FVector2D Inner1 = PtAt(A1, InnerR);
			const FVector2D Outer1 = PtAt(A1, OuterR);

			FCanvasUVTri T1;
			T1.V0_Color = T1.V1_Color = T1.V2_Color = Color;
			T1.V0_UV = T1.V1_UV = T1.V2_UV = FVector2D::ZeroVector;
			T1.V0_Pos = Inner0; T1.V1_Pos = Outer0; T1.V2_Pos = Outer1;
			Tris.Add(T1);

			FCanvasUVTri T2 = T1;
			T2.V0_Pos = Inner0; T2.V1_Pos = Outer1; T2.V2_Pos = Inner1;
			Tris.Add(T2);
		}

		Canvas->K2_DrawTriangle(Canvas->DefaultTexture, Tris);
	}
}

void UNCPlusHUDWidget_QuickStats::DrawRadialArcs(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C)
{
	using namespace NCPlusQS;
	// Per-element font override (Phase 3.8). Defaults to LargeFont when no override.
	UFont* NumberFont = NCPlusHUDFonts::Resolve(TEXT("hp_armor"), UTHUDOwner, UTHUDOwner->LargeFont);
	if (!NumberFont || !Canvas) return;

	const float CX = Size.X * 0.5f;
	const float CY = Size.Y * 0.5f;
	const float OuterR_HP = 46.f, OuterR_AR = 32.f;
	const float Thick = 6.f;

	// Track ring (dim background — full circle). DrawArc uses K2_DrawTriangle
	// which writes per-vertex color directly (no DrawOpacity override), so we
	// can pre-multiply Opacity into the color.A and it WILL render.
	FLinearColor TrackHP(0.10f, 0.16f, 0.12f, 0.85f * C.Opacity);
	FLinearColor TrackAR(0.16f, 0.14f, 0.10f, 0.85f * C.Opacity);
	DrawArc(Canvas, RenderPosition, RenderScale, CX, CY, OuterR_HP - Thick, OuterR_HP, 0.f, 2.f * PI, 48, TrackHP);
	if (bDrawArmor)
	{
		DrawArc(Canvas, RenderPosition, RenderScale, CX, CY, OuterR_AR - (Thick - 1.f), OuterR_AR, 0.f, 2.f * PI, 40, TrackAR);
	}

	// HP arc — full sweep maps to MaxHealth (200).
	const float HPFrac = FMath::Clamp(float(Health) / float(MaxHealth), 0.f, 1.f);
	FLinearColor HPColor = C.HealthAccent;
	HPColor.A = (0.9f + C.HealthPulse * 0.1f) * C.Opacity;
	DrawArc(Canvas, RenderPosition, RenderScale, CX, CY, OuterR_HP - Thick, OuterR_HP,
	        0.f, HPFrac * 2.f * PI, FMath::Max(8, FMath::FloorToInt(HPFrac * 48.f)), HPColor);

	// Armor arc — full sweep maps to MaxArmor (150).
	if (bDrawArmor)
	{
		const float ARFrac = FMath::Clamp(float(Armor) / float(MaxArmor), 0.f, 1.f);
		FLinearColor ARColor = C.ArmorAccent;
		ARColor.A = (0.9f + C.ArmorPulse * 0.1f) * C.Opacity;
		DrawArc(Canvas, RenderPosition, RenderScale, CX, CY, OuterR_AR - (Thick - 1.f), OuterR_AR,
		        0.f, ARFrac * 2.f * PI, FMath::Max(6, FMath::FloorToInt(ARFrac * 40.f)), ARColor);
	}

	// Numbers stacked in the center of the rings — DrawText needs DrawOpacity arg.
	DrawText(FText::AsNumber(Health), CX, CY - 14.f, NumberFont,
		FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
		0.95f, C.HealthNumColor.A * C.Opacity, C.HealthNumColor,
		ETextHorzPos::Center, ETextVertPos::Center);
	if (bDrawArmor)
	{
		DrawText(FText::AsNumber(Armor), CX, CY + 14.f, NumberFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			0.6f, C.ArmorNumColor.A * C.Opacity, C.ArmorNumColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}
}

// =============================================================================
// Style 4 — HexChevrons
//   Sci-fi angled segments (5 base + 1 overflow per stat). Each chevron is a
//   six-vertex polygon drawn as 4 triangles.
// =============================================================================

namespace
{
	// Chevron shape (rightward-pointing parallelogram with slanted edges):
	//   (S, 0)──(W,    0)
	//      \         \
	//       \         \─(W+S, H/2)
	//       /         /
	//      /         /
	//   (0,H/2)──(S,  H)──(W, H)
	void DrawChevron(UCanvas* Canvas, const FVector2D& RenderPos, float Scale,
	                 float X, float Y, float W, float H, float Slant, const FLinearColor& Color)
	{
		if (!Canvas) return;
		// Widget-local polygon points → transform to screen space.
		const FVector2D P0 = LocalToScreen({X + Slant,     Y          }, RenderPos, Scale); // top-left
		const FVector2D P1 = LocalToScreen({X + W,         Y          }, RenderPos, Scale); // top-right
		const FVector2D P2 = LocalToScreen({X + W + Slant, Y + H*0.5f }, RenderPos, Scale); // tip
		const FVector2D P3 = LocalToScreen({X + W,         Y + H      }, RenderPos, Scale); // bottom-right
		const FVector2D P4 = LocalToScreen({X + Slant,     Y + H      }, RenderPos, Scale); // bottom-left
		const FVector2D P5 = LocalToScreen({X,             Y + H*0.5f }, RenderPos, Scale); // back-tip

		// 4-triangle fan from P0
		TArray<FCanvasUVTri> Tris;
		Tris.SetNum(4);
		for (FCanvasUVTri& T : Tris)
		{
			T.V0_Color = T.V1_Color = T.V2_Color = Color;
			T.V0_UV = T.V1_UV = T.V2_UV = FVector2D::ZeroVector;
			T.V0_Pos = P0;
		}
		Tris[0].V1_Pos = P1; Tris[0].V2_Pos = P2;
		Tris[1].V1_Pos = P2; Tris[1].V2_Pos = P3;
		Tris[2].V1_Pos = P3; Tris[2].V2_Pos = P4;
		Tris[3].V1_Pos = P4; Tris[3].V2_Pos = P5;

		Canvas->K2_DrawTriangle(Canvas->DefaultTexture, Tris);
	}
}

void UNCPlusHUDWidget_QuickStats::DrawHexChevrons(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C)
{
	using namespace NCPlusQS;
	// Per-element font override (Phase 3.8). Defaults to LargeFont when no override.
	UFont* NumberFont = NCPlusHUDFonts::Resolve(TEXT("hp_armor"), UTHUDOwner, UTHUDOwner->LargeFont);
	if (!NumberFont || !Canvas) return;

	const float ChevW   = 44.f, ChevH = 22.f, ChevSlant = 10.f, ChevGap = 8.f;
	// Number column on the left, then 4 (HP) / 3 (Armor) chevrons. Use the
	// HP row to set the strip's start so both rows align on the same X.
	const float NumWidth = 80.f;
	const float StripX   = NumWidth + 10.f;
	const float RowY[2]  = { 14.f, 14.f + ChevH + 18.f };

	const float NumScale = 0.75f;   // small enough to fit inside ChevH visually
	const float NumNudgeY = -4.f;   // compensate for font glyph-box asymmetry

	auto DrawRow = [&](int32 Value, int32 SegCount, float Y,
	                   const FLinearColor& FillColor, float Pulse)
	{
		// Number on the left, visually centered on the chevron row.
		DrawText(FText::AsNumber(Value), NumWidth, Y + ChevH * 0.5f + NumNudgeY, NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			NumScale, FillColor.A * C.Opacity, FillColor,
			ETextHorzPos::Right, ETextVertPos::Center);

		// DrawChevron uses K2_DrawTriangle which preserves color.A → bake Opacity in.
		FLinearColor EmptyOutline(0.18f, 0.20f, 0.18f, 0.55f * C.Opacity);
		FLinearColor PulsedFill = FillColor;
		PulsedFill.A = (0.85f + Pulse * 0.15f) * C.Opacity;

		const int32 FullCount   = FMath::Clamp(Value / SegmentSize, 0, SegCount);
		const float ActiveFrac  = FMath::Clamp(float(Value % SegmentSize) / float(SegmentSize), 0.f, 1.f);

		for (int32 i = 0; i < SegCount; i++)
		{
			const float X = StripX + i * (ChevW + ChevGap);
			if (i < FullCount)
			{
				DrawChevron(Canvas, RenderPosition, RenderScale, X, Y, ChevW, ChevH, ChevSlant, PulsedFill);
			}
			else if (i == FullCount && ActiveFrac > 0.f)
			{
				// Partial: blend toward fill via alpha (chevron clipping is hard).
				FLinearColor PartialColor = FMath::Lerp(EmptyOutline, PulsedFill, ActiveFrac);
				DrawChevron(Canvas, RenderPosition, RenderScale, X, Y, ChevW, ChevH, ChevSlant, PartialColor);
			}
			else
			{
				DrawChevron(Canvas, RenderPosition, RenderScale, X, Y, ChevW, ChevH, ChevSlant, EmptyOutline);
			}
		}
	};

	DrawRow(Health, HealthSegCount, RowY[0], C.HealthAccent, C.HealthPulse);
	if (bDrawArmor)
	{
		DrawRow(Armor, ArmorSegCount, RowY[1], C.ArmorAccent, C.ArmorPulse);
	}
}

// =============================================================================
// Style 5 — VerticalPills
//   Two compact tall meters side-by-side. Numbers above each pill.
// =============================================================================

void UNCPlusHUDWidget_QuickStats::DrawVerticalPills(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C)
{
	using namespace NCPlusQS;
	// Per-element font override (Phase 3.8). Defaults to LargeFont when no override.
	UFont* NumberFont = NCPlusHUDFonts::Resolve(TEXT("hp_armor"), UTHUDOwner, UTHUDOwner->LargeFont);
	if (!NumberFont || !Canvas || !Canvas->DefaultTexture) return;

	const float PillW       = 22.f;
	const float PillH       = 64.f;
	const float GapBetween  = 56.f;   // wide enough so HP/Armor numbers don't collide
	const float CenterX     = Size.X * 0.5f;
	const float TopY        = 14.f;
	const float HPx         = CenterX - GapBetween * 0.5f - PillW;
	const float ARx         = CenterX + GapBetween * 0.5f;

	auto DrawPill = [&](float X, float Y, int32 Value, int32 MaxValue,
	                    const FLinearColor& FillColor, const FLinearColor& NumColor, float Pulse, const FText& Label)
	{
		// Track — DrawTexture overrides color.A → pass alpha through DrawOpacity.
		const FLinearColor Track(0.10f, 0.12f, 0.10f, 1.f);
		const float TrackAlpha = 0.92f * C.Opacity;
		DrawTexture(Canvas->DefaultTexture, X, Y, PillW, PillH, 0, 0, 1, 1, TrackAlpha, Track);

		// Fill scaled to MaxValue (200 for HP, 150 for armor).
		const float FillFrac = FMath::Clamp(float(Value) / float(MaxValue), 0.f, 1.f);
		const float FillH    = PillH * FillFrac;
		const float FillAlpha = (0.9f + Pulse * 0.1f) * C.Opacity;
		DrawTexture(Canvas->DefaultTexture,
			X + 2.f, Y + (PillH - FillH) + 2.f,
			PillW - 4.f, FMath::Max(0.f, FillH - 4.f),
			0, 0, 1, 1, FillAlpha, FillColor);

		// Number BELOW the pill so it doesn't compete with the team-name strip above.
		DrawText(FText::AsNumber(Value), X + PillW * 0.5f, Y + PillH + 4.f, NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			0.85f, NumColor.A * C.Opacity, NumColor,
			ETextHorzPos::Center, ETextVertPos::Top);
	};

	DrawPill(HPx, TopY, Health, MaxHealth, C.HealthAccent, C.HealthNumColor, C.HealthPulse, FText::FromString(TEXT("HP")));
	if (bDrawArmor)
	{
		DrawPill(ARx, TopY, Armor, MaxArmor, C.ArmorAccent, C.ArmorNumColor, C.ArmorPulse, FText::FromString(TEXT("AR")));
	}
}
