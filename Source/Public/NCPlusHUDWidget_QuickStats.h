// Modernized minimal-typography health/armor widget for NetcodePlus modes.
// Replaces stock bpHW_QuickStats. Clean white numbers with green/yellow accent
// labels + underlines. Damage flash, pickup pulse, low-HP red tint.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
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

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;

private:
	// Damage / pickup tracking shared across all styles
	int32 LastHealth;
	int32 LastArmor;
	float HealthDamageFlashEnd;
	float ArmorDamageFlashEnd;
	float HealthPickupPulseEnd;
	float ArmorPickupPulseEnd;

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
	};

	void DrawMinimalTypography(int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawSegmentedBars   (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawRadialArcs      (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawHexChevrons     (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
	void DrawVerticalPills   (int32 Health, int32 Armor, bool bDrawArmor, const FStatColors& C);
};
