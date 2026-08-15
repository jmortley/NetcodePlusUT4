// Modernized minimal-typography health/armor widget for NetcodePlus modes.
// Replaces stock bpHW_QuickStats. Clean white numbers with green/yellow accent
// labels + underlines. Damage flash, pickup pulse, low-HP red tint.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "CanvasItem.h"
#include "NCPlusHUDWidget_QuickStats.generated.h"

/**
 * Modernized HP/Armor widget. Five visual variants (selectable per layout file
 * via the "style" extras key on the hp_armor element):
 *   - MinimalTypography (default) — clean white numbers + green/amber labels
 *   - SegmentedBars                — five segments + overflow indicator
 *   - RadialArcs                   — concentric arcs around screen pivot
 *   - HexChevrons                  — sci-fi angled segments
 *   - VerticalPills                — paired tall meters
 */
UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_QuickStats : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void  Draw_Implementation(float DeltaTime) override;
	virtual bool  ShouldDraw_Implementation(bool bShowScores) override;
	// Scale override (Phase 3.11): multiply base by FNCPlusHUDElement::Scale
	// for the "hp_armor" alias so layout scale tweaks resize the whole widget.
	virtual float GetDrawScaleOverride() override;

private:
	// Damage / pickup tracking shared across all styles
	int32 LastHealth;
	int32 LastArmor;
	float HealthDamageFlashEnd;
	float ArmorDamageFlashEnd;
	float HealthPickupPulseEnd;
	float ArmorPickupPulseEnd;

	// Parsed layout extras are immutable between live-layout revision bumps.
	// Keeping their typed forms here avoids reparsing colors/style/opacity at render rate.
	uint32 CachedLayoutRevision;
	uint8 CachedStyle;
	float CachedOpacity;
	FLinearColor CachedLowHpRed;
	FLinearColor CachedWarningHp;
	FLinearColor CachedDamageFlash;
	FLinearColor CachedHealthAccent;
	FLinearColor CachedArmorAccent;
	FLinearColor CachedHealthNumBase;
	FLinearColor CachedArmorNumBase;

	// RadialArcs is static between vital/layout changes. Retain the exact
	// tessellated vertices and the allocation; only vertex colors are refreshed
	// during pickup pulses. This removes render-rate trig without changing pixels.
	TArray<FCanvasUVTri> CachedRadialTris;
	FVector2D CachedRadialRenderPosition;
	FVector2D CachedRadialSize;
	float CachedRadialRenderScale;
	int32 CachedRadialHealth;
	int32 CachedRadialArmor;
	bool bCachedRadialDrawArmor;
	int32 RadialTrackHPStart;
	int32 RadialTrackHPCount;
	int32 RadialTrackARStart;
	int32 RadialTrackARCount;
	int32 RadialValueHPStart;
	int32 RadialValueHPCount;
	int32 RadialValueARStart;
	int32 RadialValueARCount;

	// Per-style draw functions. Health/Armor passed as ints so each function
	// can present them however it likes; the shared header in Draw_Implementation
	// computes flash/pulse colors and passes them in.
	struct FStatColors
	{
		FLinearColor HealthNumColor;
		FLinearColor ArmorNumColor;
		FLinearColor HealthAccent;
		FLinearColor ArmorAccent;
		float HealthPulse;
		float ArmorPulse;
		// Per-element opacity multiplier (0..1). Each draw call must multiply its
		// own alpha by this value and pass the result as DrawOpacity, because
		// UUTHUDWidget::DrawTexture overwrites the color's .A with its own calc.
		float Opacity;
	};

	void DrawMinimalTypography(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawSegmentedBars   (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawRadialArcs      (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawHexChevrons     (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawVerticalPills   (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
};
