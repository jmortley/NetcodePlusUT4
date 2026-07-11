// Modernized ammo counter widget for NetcodePlus modes. Replaces stock
// bpHW_WeaponInfo with a styleable, fully editable widget.
//
// Three visual styles dispatched off the layout's "style" extra (alias "ammo"):
//   - BigNumber     — large number + thin accent underline + "/Max" subtext
//   - IconAndCount  — weapon icon left, count right, ammo bar underline
//   - VerticalGauge — icon top, vertical fill gauge, number below
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_AmmoCounter.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_AmmoCounter : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void  Draw_Implementation(float DeltaTime) override;
	virtual bool  ShouldDraw_Implementation(bool bShowScores) override;
	// Scale override (Phase 3.11): multiply base by FNCPlusHUDElement::Scale
	// for the "ammo" alias so layout scale tweaks resize the whole widget.
	virtual float GetDrawScaleOverride() override;

	/** Same atlas WeaponBar uses — WeaponBarSelectedUVs are pixel coords into this. */
	UPROPERTY()
	class UTexture2D* WeaponIconAtlas;

private:
	// Pickup pulse / weapon-swap flash bookkeeping.
	TWeakObjectPtr<class AUTWeapon> LastWeapon;
	int32 LastAmmo;
	float PickupPulseEnd;
	float SwapFlashEnd;

	// Typed layout extras, refreshed only when live editing bumps the revision.
	uint32 CachedLayoutRevision;
	uint8 CachedStyle;
	float CachedOpacity;
	FLinearColor CachedNumColor;
	FLinearColor CachedMaxColor;
	FLinearColor CachedFullColor;
	FLinearColor CachedWarnColor;
	FLinearColor CachedDangerColor;
	FLinearColor CachedBgColor;

	struct FAmmoColors
	{
		FLinearColor NumColor;       // base number color
		FLinearColor MaxColor;       // dim "/Max" text
		FLinearColor AccentColor;    // underline / fill color (state-dependent)
		FLinearColor BgColor;        // background plate (used by IconAndCount / VerticalGauge)
		FLinearColor IconColor;      // weapon icon tint (passed through opacity)
		float Pulse;                 // 0..1 pickup pulse
		float Opacity;               // per-element opacity multiplier
	};

	void DrawBigNumber    (class AUTWeapon* W, const FAmmoColors& C);
	void DrawIconAndCount (class AUTWeapon* W, const FAmmoColors& C);
	void DrawVerticalGauge(class AUTWeapon* W, const FAmmoColors& C);
};
