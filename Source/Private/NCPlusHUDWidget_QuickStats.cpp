// Modernized minimal-typography health/armor widget for NetcodePlus modes.
#include "NCPlusHUDWidget_QuickStats.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"

namespace NCPlusQS
{
	static const float FlashDuration = 0.30f;   // damage tint length
	static const float PulseDuration = 0.45f;   // accent-line pulse on pickup
	static const int32 LowHealthCutoff = 30;    // below this, number lerps to red
	static const float NumberScale     = 1.40f; // LargeFont * 1.4 ~ ~50pt visual
	static const float LabelScale      = 0.80f; // TinyFont * 0.8 — small caps
	static const float CenterGap       = 70.f;  // gap from divider to text edge
	static const float DividerY0       = 14.f;  // top of vertical divider
	static const float DividerY1       = 76.f;  // bottom of vertical divider
	static const float NumberY         = 8.f;   // top of big number
	static const float AccentLineGap   = 4.f;   // gap from number bottom to accent line
	static const float LabelGap        = 5.f;   // gap from accent line to label top
	static const float AccentLineW     = 70.f;  // minimum accent line length
}

UNCPlusHUDWidget_QuickStats::UNCPlusHUDWidget_QuickStats(const FObjectInitializer& OI)
	: Super(OI)
{
	// Pivot at top-center of widget, anchored at 78% down the screen.
	// (Just above the inventory bar, where the stock QuickStats normally sits.)
	Position           = FVector2D(0.f, 0.f);
	Size               = FVector2D(480.f, 100.f);
	ScreenPosition     = FVector2D(0.5f, 0.78f);
	Origin             = FVector2D(0.5f, 0.f);
	DesignedResolution = 1080.f;

	LastHealth             = -1;
	LastArmor              = -1;
	HealthDamageFlashEnd   = 0.f;
	ArmorDamageFlashEnd    = 0.f;
	HealthPickupPulseEnd   = 0.f;
	ArmorPickupPulseEnd    = 0.f;
}

bool UNCPlusHUDWidget_QuickStats::ShouldDraw_Implementation(bool bShowScores)
{
	// Note: deliberately NOT gating on UTHUDOwner->GetQuickStatsHidden() — that's
	// the user setting that hides the STOCK QuickStats widget. Our widget is a
	// separate, modernized replacement and should remain visible regardless.
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
		// Reset trackers so respawn (0 → 100) doesn't trigger a stray pickup pulse.
		LastHealth = -1;
		LastArmor  = -1;
		return;
	}

	UFont* NumberFont = UTHUDOwner->LargeFont;
	UFont* LabelFont  = UTHUDOwner->TinyFont;
	if (!NumberFont || !LabelFont) return;

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

	// --- Color palette ---
	const FLinearColor HealthAccent(0.37f, 0.96f, 0.48f, 1.f); // bright green
	const FLinearColor ArmorAccent (0.95f, 0.83f, 0.34f, 1.f); // amber
	const FLinearColor White       = FLinearColor::White;
	const FLinearColor LowHpRed    (1.f, 0.32f, 0.28f, 1.f);
	const FLinearColor DamageFlash (1.f, 0.45f, 0.30f, 1.f);

	// --- Health number color: low-HP lerp + damage flash overlay ---
	FLinearColor HealthNumColor = White;
	if (Health <= LowHealthCutoff)
	{
		const float t = FMath::Clamp(float(Health) / float(LowHealthCutoff), 0.f, 1.f);
		HealthNumColor = FMath::Lerp(LowHpRed, White, t);
	}
	if (Now < HealthDamageFlashEnd)
	{
		const float t = (HealthDamageFlashEnd - Now) / FlashDuration;
		HealthNumColor = FMath::Lerp(HealthNumColor, DamageFlash, t);
	}

	FLinearColor ArmorNumColor = White;
	if (Now < ArmorDamageFlashEnd)
	{
		const float t = (ArmorDamageFlashEnd - Now) / FlashDuration;
		ArmorNumColor = FMath::Lerp(ArmorNumColor, DamageFlash, t);
	}

	// --- Pickup pulse strengths (0..1, decays over PulseDuration) ---
	auto PulseStrength = [&](float EndTime)->float
	{
		return (Now >= EndTime) ? 0.f : (EndTime - Now) / PulseDuration;
	};
	const float HealthPulse = PulseStrength(HealthPickupPulseEnd);
	const float ArmorPulse  = PulseStrength(ArmorPickupPulseEnd);

	// Don't render the armor block at all when there's no armor (avoids a stray "0" on respawn).
	const bool bDrawArmor = (Armor > 0);

	// Widget-local x=0 is the widget's LEFT edge; CenterX is the horizontal middle.
	const float CenterX = Size.X * 0.5f;

	// --- HEALTH block (right-aligned to centre divider) ---
	{
		const float TextX = CenterX - CenterGap;
		const float TextY = NumberY;
		const FVector2D NumSize = DrawText(FText::AsNumber(Health),
			TextX, TextY,
			NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f), // shadow
			NumberScale, 1.0f, HealthNumColor,
			ETextHorzPos::Right, ETextVertPos::Top);

		// Derive accent-line + label Y from the actual rendered number bottom so they
		// always sit cleanly below the digits, regardless of NumberScale or font.
		const float NumberW       = NumSize.X * NumberScale;
		const float NumberH       = NumSize.Y * NumberScale;
		const float NumberBottomY = TextY + NumberH;
		const float AccentLineY   = NumberBottomY + AccentLineGap;
		const float LabelY        = AccentLineY + LabelGap;
		const float NumberCenterX = TextX - NumberW * 0.5f;

		// Accent underline (between number and label) — width matches number, pulses on pickup.
		if (Canvas && Canvas->DefaultTexture)
		{
			FLinearColor LineColor = HealthAccent;
			LineColor.A = 0.85f + HealthPulse * 0.15f;
			const float LineH = 1.5f + HealthPulse * 1.5f;
			const float LineW = FMath::Max(NumberW, AccentLineW);
			DrawTexture(Canvas->DefaultTexture,
				TextX - LineW, AccentLineY,
				LineW, LineH,
				0.f, 0.f, 1.f, 1.f,
				1.0f, LineColor);
		}

		// "HEALTH" label centered under the number.
		DrawText(FText::FromString(TEXT("HEALTH")),
			NumberCenterX, LabelY,
			LabelFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			LabelScale, 1.0f, HealthAccent,
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	// --- ARMOR block (left-aligned to centre divider) ---
	if (bDrawArmor)
	{
		const float TextX = CenterX + CenterGap;
		const float TextY = NumberY;
		const FVector2D NumSize = DrawText(FText::AsNumber(Armor),
			TextX, TextY,
			NumberFont,
			FVector2D(2.f, 2.f), FLinearColor(0.f, 0.f, 0.f, 0.7f),
			NumberScale, 1.0f, ArmorNumColor,
			ETextHorzPos::Left, ETextVertPos::Top);

		const float NumberW       = NumSize.X * NumberScale;
		const float NumberH       = NumSize.Y * NumberScale;
		const float NumberBottomY = TextY + NumberH;
		const float AccentLineY   = NumberBottomY + AccentLineGap;
		const float LabelY        = AccentLineY + LabelGap;
		const float NumberCenterX = TextX + NumberW * 0.5f;

		if (Canvas && Canvas->DefaultTexture)
		{
			FLinearColor LineColor = ArmorAccent;
			LineColor.A = 0.85f + ArmorPulse * 0.15f;
			const float LineH = 1.5f + ArmorPulse * 1.5f;
			const float LineW = FMath::Max(NumberW, AccentLineW);
			DrawTexture(Canvas->DefaultTexture,
				TextX, AccentLineY,
				LineW, LineH,
				0.f, 0.f, 1.f, 1.f,
				1.0f, LineColor);
		}

		DrawText(FText::FromString(TEXT("ARMOR")),
			NumberCenterX, LabelY,
			LabelFont,
			FVector2D(1.f, 1.f), FLinearColor(0.f, 0.f, 0.f, 0.5f),
			LabelScale, 1.0f, ArmorAccent,
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	// --- Vertical divider (centered between blocks) ---
	if (Canvas && Canvas->DefaultTexture)
	{
		FLinearColor DividerColor(0.7f, 0.55f, 0.30f, 0.45f);
		DrawTexture(Canvas->DefaultTexture,
			CenterX - 0.5f, DividerY0,
			1.f, DividerY1 - DividerY0,
			0.f, 0.f, 1.f, 1.f,
			1.0f, DividerColor);
	}
}
