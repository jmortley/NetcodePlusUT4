// Custom split WeaponBar for NetcodePlus. Two independent strips (Left + Right)
// each backed by a UClass so the layout alias map can target them separately.
//
// Each side filters the player's inventory by FNCPlusHUDLayout::GetWeaponSide()
// so users can split hitscan / projectile (or any other classification) across
// the two strips. Position, orientation, and hidden state come from the layout
// system; per-side weapon assignment is in the JSON's `weapon_groups` block.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_WeaponBar.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_WeaponBar : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

public:
	virtual void  Draw_Implementation(float DeltaTime) override;
	virtual bool  ShouldDraw_Implementation(bool bShowScores) override;
	// Scale override (Phase 3.11): multiply base by FNCPlusHUDElement::Scale
	// for the per-side alias so layout scale tweaks resize the strip.
	virtual float GetDrawScaleOverride() override;

	/** 0 = Left strip, 1 = Right strip. Set by subclass constructor. */
	int32 SideIndex;

	/** Texture atlas containing weapon HUD icons. WeaponBarSelectedUVs on each
	 *  AUTWeapon are pixel coordinates into THIS texture (not HUDAtlas). */
	UPROPERTY()
	class UTexture2D* WeaponIconAtlas;

private:
	// Parsed layout style. Extras are strings in the editable layout, but they
	// only need parsing when the live revision changes.
	uint32 CachedStyleRevision;
	bool bCachedVertical;
	float CachedOpacity;
	FLinearColor CachedSlotBgInactive;
	FLinearColor CachedSlotBgActive;
	FLinearColor CachedActiveOutline;
	FLinearColor CachedAmmoFillFull;
	FLinearColor CachedAmmoFillWarn;
	FLinearColor CachedAmmoFillDanger;
};

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_WeaponBar_Left : public UNCPlusHUDWidget_WeaponBar
{
	GENERATED_UCLASS_BODY()
};

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_WeaponBar_Right : public UNCPlusHUDWidget_WeaponBar
{
	GENERATED_UCLASS_BODY()
};
