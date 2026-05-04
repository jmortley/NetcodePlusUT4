// Modernized ammo counter — implementation. Three visual styles dispatched off
// the layout's "style" extra (alias "ammo").
#include "NCPlusHUDWidget_AmmoCounter.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"

namespace NCPlusAC
{
	static const float PickupPulseDuration = 0.45f;
	static const float SwapFlashDuration   = 0.30f;

	// Default palette (overridable via Extras keys).
	static const FLinearColor NumWhite      = FLinearColor::White;
	static const FLinearColor MaxDim        (0.65f, 0.65f, 0.65f, 1.f);
	static const FLinearColor AccentFull    (0.40f, 0.95f, 0.48f, 1.f);
	static const FLinearColor AccentWarn    (1.00f, 0.85f, 0.30f, 1.f);
	static const FLinearColor AccentDanger  (1.00f, 0.32f, 0.28f, 1.f);
	static const FLinearColor PlateBg       (0.04f, 0.04f, 0.04f, 0.45f);
}

UNCPlusHUDWidget_AmmoCounter::UNCPlusHUDWidget_AmmoCounter(const FObjectInitializer& OI)
	: Super(OI)
	, WeaponIconAtlas(nullptr)
	, LastAmmo(-1)
	, PickupPulseEnd(0.f)
	, SwapFlashEnd(0.f)
{
	// Anchored bottom-right by default. Origin matches anchor so content extends
	// up-and-to-the-left from the corner — same idiom we use for the WeaponBar.
	Position           = FVector2D(-20.f, -20.f);
	Size               = FVector2D(220.f, 110.f);
	ScreenPosition     = FVector2D(1.0f, 1.0f);
	Origin             = FVector2D(1.0f, 1.0f);
	DesignedResolution = 1080.f;

	// Opt out of HUDImpulse — see NCPlusHUDWidget_WeaponBar for the full rationale.
	bShouldKickBack = false;

	static ConstructorHelpers::FObjectFinder<UTexture2D> WeaponAtlasFinder(TEXT("Texture2D'/Game/RestrictedAssets/UI/WeaponAtlas01.WeaponAtlas01'"));
	WeaponIconAtlas = WeaponAtlasFinder.Object;
}

bool UNCPlusHUDWidget_AmmoCounter::ShouldDraw_Implementation(bool bShowScores)
{
	if (!Super::ShouldDraw_Implementation(bShowScores)) return false;
	if (!UTHUDOwner || !UTHUDOwner->UTPlayerOwner) return false;
	if (UTHUDOwner->bShowComsMenu || UTHUDOwner->bShowWeaponWheel) return false;

	AUTCharacter* Char = Cast<AUTCharacter>(UTHUDOwner->UTPlayerOwner->GetViewTarget());
	if (!Char || Char->IsDead()) return false;

	AUTWeapon* W = Char->GetWeapon();
	return (W != nullptr) && W->NeedsAmmoDisplay();
}

void UNCPlusHUDWidget_AmmoCounter::Draw_Implementation(float DeltaTime)
{
	using namespace NCPlusAC;

	if (!UTHUDOwner || UTHUDOwner->IsPendingKill()) return;
	if (!UTHUDOwner->UTPlayerOwner || UTHUDOwner->UTPlayerOwner->IsPendingKill()) return;
	if (!Canvas) return;

	AActor* ViewTarget = UTHUDOwner->UTPlayerOwner->GetViewTarget();
	if (!ViewTarget || ViewTarget->IsPendingKill()) return;
	AUTCharacter* Char = Cast<AUTCharacter>(ViewTarget);
	if (!Char || Char->IsDead()) return;

	AUTWeapon* W = Char->GetWeapon();
	if (!W || W->IsPendingKill() || !W->NeedsAmmoDisplay()) return;

	// Lazy-load atlas if CDO finder failed (consistent with WeaponBar).
	if (!WeaponIconAtlas)
	{
		WeaponIconAtlas = LoadObject<UTexture2D>(nullptr, TEXT("/Game/RestrictedAssets/UI/WeaponAtlas01.WeaponAtlas01"));
	}

	const float Now = GetWorld()->GetTimeSeconds();

	// --- Pickup / swap detection ---
	if (LastWeapon.Get() != W)
	{
		// Weapon changed → swap flash. Reset ammo tracking so the first frame on
		// the new weapon doesn't read as a "pickup".
		SwapFlashEnd = Now + SwapFlashDuration;
		LastAmmo     = W->Ammo;
		LastWeapon   = W;
	}
	else if (LastAmmo >= 0 && W->Ammo > LastAmmo)
	{
		// Same weapon, ammo went up → pickup pulse.
		PickupPulseEnd = Now + PickupPulseDuration;
	}
	LastAmmo = W->Ammo;

	// --- Color palette + opacity ---
	const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(TEXT("ammo"));

	auto Col = [&](FName Key, const FLinearColor& Default) -> FLinearColor
	{
		return Elem ? Elem->GetExtraColor(Key, Default) : Default;
	};

	FAmmoColors C;
	C.Opacity = Elem ? FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;

	const FLinearColor BaseNum   = Col(TEXT("color_number"), NumWhite);
	const FLinearColor MaxText   = Col(TEXT("color_max"),    MaxDim);
	const FLinearColor Full      = Col(TEXT("color_full"),   AccentFull);
	const FLinearColor Warn      = Col(TEXT("color_warn"),   AccentWarn);
	const FLinearColor Danger    = Col(TEXT("color_danger"), AccentDanger);

	// Accent (state-dependent): danger → warn → full.
	FLinearColor Accent = Full;
	if (W->MaxAmmo > 0)
	{
		if (W->Ammo <= W->AmmoDangerAmount)       Accent = Danger;
		else if (W->Ammo <= W->AmmoWarningAmount) Accent = Warn;
	}

	// Number color: lerp toward danger when low, then toward white during swap flash.
	FLinearColor NumCol = BaseNum;
	if (W->MaxAmmo > 0 && W->Ammo <= W->AmmoDangerAmount)
	{
		NumCol = Danger;
	}
	if (Now < SwapFlashEnd)
	{
		const float t = (SwapFlashEnd - Now) / SwapFlashDuration;
		NumCol = FMath::Lerp(NumCol, FLinearColor::White, t);
	}

	C.NumColor    = NumCol;
	C.MaxColor    = MaxText;
	C.AccentColor = Accent;
	C.BgColor     = Col(TEXT("color_bg"), PlateBg);

	// Icon color follows UseWeaponColors setting (same as WeaponBar).
	C.IconColor = (UTHUDOwner->GetUseWeaponColors()) ? W->IconColor : FLinearColor::White;
	C.IconColor.A = 1.f;

	C.Pulse = (Now >= PickupPulseEnd) ? 0.f : (PickupPulseEnd - Now) / PickupPulseDuration;

	// --- Dispatch to chosen style ---
	const ENCPlusAmmoStyle Style = Elem
		? NCPlusAmmoStyle::Parse(Elem->GetExtra(TEXT("style")))
		: ENCPlusAmmoStyle::BigNumber;

	switch (Style)
	{
		case ENCPlusAmmoStyle::IconAndCount:  DrawIconAndCount (W, C); break;
		case ENCPlusAmmoStyle::VerticalGauge: DrawVerticalGauge(W, C); break;
		default:                              DrawBigNumber    (W, C); break;
	}
}

// =============================================================================
// Style 1 — BigNumber (default)
//   Large ammo number, slim accent underline (pulses on pickup), "/Max" subtext.
//   Right-aligned to the widget so it sits flush against the screen corner.
// =============================================================================

void UNCPlusHUDWidget_AmmoCounter::DrawBigNumber(AUTWeapon* W, const FAmmoColors& C)
{
	using namespace NCPlusAC;
	UFont* NumberFont = UTHUDOwner->LargeFont;
	UFont* MaxFont    = UTHUDOwner->TinyFont;
	if (!NumberFont || !MaxFont) return;

	// Anchor to the right edge of the widget; layout offset places the whole
	// thing wherever the user wants.
	const float RightX = Size.X - 8.f;
	const float TopY   = 12.f;

	// Big number, right-aligned.
	const float NumberScale = 1.60f;
	const FVector2D NumSize = DrawText(FText::AsNumber(W->Ammo),
		RightX, TopY, NumberFont,
		FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
		NumberScale, C.NumColor.A * C.Opacity, C.NumColor,
		ETextHorzPos::Right, ETextVertPos::Top);

	const float NumberW = NumSize.X * NumberScale;
	const float NumberH = NumSize.Y * NumberScale;
	const float UnderlineY = TopY + NumberH + 4.f;
	const float UnderlineW = FMath::Max(NumberW, 80.f);

	// Accent underline — pulses on pickup.
	if (Canvas && Canvas->DefaultTexture)
	{
		const float Alpha = (0.85f + C.Pulse * 0.15f) * C.Opacity;
		const float LineH = 1.5f + C.Pulse * 1.5f;
		DrawTexture(Canvas->DefaultTexture, RightX - UnderlineW, UnderlineY,
			UnderlineW, LineH, 0, 0, 1, 1, Alpha, C.AccentColor);
	}

	// "/Max" below the underline, right-aligned.
	if (W->MaxAmmo > 0)
	{
		const FString MaxStr = FString::Printf(TEXT("/ %d"), W->MaxAmmo);
		DrawText(FText::FromString(MaxStr),
			RightX, UnderlineY + 5.f, MaxFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			0.95f, C.MaxColor.A * C.Opacity, C.MaxColor,
			ETextHorzPos::Right, ETextVertPos::Top);
	}
}

// =============================================================================
// Style 2 — IconAndCount
//   Weapon icon on the left, "Ammo / Max" on the right, ammo bar underline.
//   Background plate behind both for contrast.
// =============================================================================

void UNCPlusHUDWidget_AmmoCounter::DrawIconAndCount(AUTWeapon* W, const FAmmoColors& C)
{
	using namespace NCPlusAC;
	UFont* NumberFont = UTHUDOwner->LargeFont;
	UFont* MaxFont    = UTHUDOwner->SmallFont;
	if (!NumberFont || !MaxFont || !Canvas || !Canvas->DefaultTexture) return;

	const float PlateW = Size.X - 8.f;
	const float PlateH = 56.f;
	const float PlateX = Size.X - PlateW - 4.f;
	const float PlateY = 12.f;
	const float Pad    = 8.f;

	// Background plate — DrawTexture overrides .A → pass through DrawOpacity.
	DrawTexture(Canvas->DefaultTexture, PlateX, PlateY, PlateW, PlateH,
		0, 0, 1, 1, C.BgColor.A * C.Opacity, C.BgColor);

	// Weapon icon (left side of plate). Same UV pattern as WeaponBar.
	const float IconBoxW = 64.f;
	const float IconBoxH = PlateH - Pad * 2.f;
	const float IconBoxX = PlateX + Pad;
	const float IconBoxY = PlateY + Pad;

	if (WeaponIconAtlas && !WeaponIconAtlas->IsPendingKill())
	{
		const FTextureUVs& UV = W->WeaponBarSelectedUVs;
		float IconW = UV.UL;
		float IconH = UV.VL;
		if (IconW > 0.f && IconH > 0.f)
		{
			const float Scale = FMath::Min(IconBoxW / IconW, IconBoxH / IconH);
			IconW *= Scale;
			IconH *= Scale;
		}
		else
		{
			IconW = IconBoxW;
			IconH = IconBoxH;
		}
		const float IconX = IconBoxX + (IconBoxW - IconW) * 0.5f;
		const float IconY = IconBoxY + (IconBoxH - IconH) * 0.5f;
		DrawTexture(WeaponIconAtlas, IconX, IconY, IconW, IconH,
			UV.U, UV.V, UV.UL, UV.VL,
			C.IconColor.A * C.Opacity, C.IconColor);
	}

	// "Ammo / Max" text — number large, "/Max" small, right-aligned to plate.
	const float TextRightX = PlateX + PlateW - Pad;
	const float NumScale   = 1.30f;
	const float NumY       = PlateY + Pad - 2.f;

	const FVector2D NumSize = DrawText(FText::AsNumber(W->Ammo),
		TextRightX, NumY, NumberFont,
		FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
		NumScale, C.NumColor.A * C.Opacity, C.NumColor,
		ETextHorzPos::Right, ETextVertPos::Top);

	if (W->MaxAmmo > 0)
	{
		const FString MaxStr = FString::Printf(TEXT("/ %d"), W->MaxAmmo);
		const float MaxY = NumY + NumSize.Y * NumScale - 6.f;
		DrawText(FText::FromString(MaxStr),
			TextRightX, MaxY, MaxFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			0.65f, C.MaxColor.A * C.Opacity, C.MaxColor,
			ETextHorzPos::Right, ETextVertPos::Top);
	}

	// Ammo bar at the bottom of the plate.
	if (W->MaxAmmo > 0)
	{
		const float BarH = 4.f;
		const float BarY = PlateY + PlateH - BarH - Pad * 0.5f;
		const float BarX = PlateX + Pad;
		const float BarW = PlateW - Pad * 2.f;

		// Track
		DrawTexture(Canvas->DefaultTexture, BarX, BarY, BarW, BarH,
			0, 0, 1, 1, 0.7f * C.Opacity, FLinearColor(0.05f, 0.05f, 0.05f, 1.f));

		// Fill
		const float Frac = FMath::Clamp(float(W->Ammo) / float(W->MaxAmmo), 0.f, 1.f);
		const float FillAlpha = (0.9f + C.Pulse * 0.1f) * C.Opacity;
		DrawTexture(Canvas->DefaultTexture, BarX, BarY, BarW * Frac, BarH,
			0, 0, 1, 1, FillAlpha, C.AccentColor);
	}
}

// =============================================================================
// Style 3 — VerticalGauge
//   Compact stacked layout: weapon icon on top, vertical fill gauge in the
//   middle, big ammo number below. Suits placements next to the crosshair.
// =============================================================================

void UNCPlusHUDWidget_AmmoCounter::DrawVerticalGauge(AUTWeapon* W, const FAmmoColors& C)
{
	using namespace NCPlusAC;
	UFont* NumberFont = UTHUDOwner->LargeFont;
	if (!NumberFont || !Canvas || !Canvas->DefaultTexture) return;

	// Centered column.
	const float CenterX = Size.X * 0.5f;
	const float TopY    = 6.f;

	// Icon
	const float IconBoxH = 28.f;
	if (WeaponIconAtlas && !WeaponIconAtlas->IsPendingKill())
	{
		const FTextureUVs& UV = W->WeaponBarSelectedUVs;
		float IconW = UV.UL;
		float IconH = UV.VL;
		if (IconW > 0.f && IconH > 0.f)
		{
			const float Scale = IconBoxH / IconH;
			IconW *= Scale;
			IconH *= Scale;
		}
		else
		{
			IconW = 50.f;
			IconH = IconBoxH;
		}
		DrawTexture(WeaponIconAtlas, CenterX - IconW * 0.5f, TopY,
			IconW, IconH, UV.U, UV.V, UV.UL, UV.VL,
			C.IconColor.A * C.Opacity, C.IconColor);
	}

	// Gauge — vertical bar, fills from bottom up.
	const float GaugeW = 16.f;
	const float GaugeH = 50.f;
	const float GaugeX = CenterX - GaugeW * 0.5f;
	const float GaugeY = TopY + IconBoxH + 4.f;

	// Track
	DrawTexture(Canvas->DefaultTexture, GaugeX, GaugeY, GaugeW, GaugeH,
		0, 0, 1, 1, C.BgColor.A * C.Opacity, C.BgColor);

	// Fill (rises from bottom).
	if (W->MaxAmmo > 0)
	{
		const float Frac = FMath::Clamp(float(W->Ammo) / float(W->MaxAmmo), 0.f, 1.f);
		const float FillH = GaugeH * Frac;
		const float FillAlpha = (0.9f + C.Pulse * 0.1f) * C.Opacity;
		DrawTexture(Canvas->DefaultTexture,
			GaugeX + 2.f, GaugeY + (GaugeH - FillH) + 1.f,
			GaugeW - 4.f, FMath::Max(0.f, FillH - 2.f),
			0, 0, 1, 1, FillAlpha, C.AccentColor);
	}

	// Number below
	const float NumY = GaugeY + GaugeH + 4.f;
	DrawText(FText::AsNumber(W->Ammo),
		CenterX, NumY, NumberFont,
		FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
		0.95f, C.NumColor.A * C.Opacity, C.NumColor,
		ETextHorzPos::Center, ETextVertPos::Top);
}
