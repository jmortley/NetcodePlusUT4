// SUTWeaponSkinSelector.h — NetcodePlus weapon settings panel (skins + hide)
#pragma once

#include "NetcodePlus.h"     // PCH preamble — pulls UnrealTournament.h → Engine.h → UDataAsset.
#include "SlateBasics.h"
#include "UTWeaponSkin.h"

class UUTLocalPlayer;
class SColorBlock;

/** Free all cached skin assets (call on module shutdown or map change) */
void SUTWeaponSkinSelector_CleanupCache();

/** Info about a discovered NetcodePlus weapon */
struct FNetcodePlusWeaponInfo
{
	FName Tag;               // WeaponSkinCustomizationTag (used for skin matching)
	FName HideKey;           // Unique key for hide state (class name — avoids shared-tag collisions)
	FString DisplayName;     // Human-readable name
	UClass* WeaponClass;     // The UClass itself
	bool bHasSkins;          // Whether any skin data assets exist for this weapon
};

/**
 * Weapon settings panel for NetcodePlus.
 * Shows all weapons inheriting AUTWeaponFix.
 * Weapons with skins get a skin selector. All weapons get a hide checkbox.
 * Settings persist to Mod.ini.
 */
class SUTWeaponSkinSelector : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SUTWeaponSkinSelector) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;

	/** Discovered NetcodePlus weapons */
	TArray<FNetcodePlusWeaponInfo> Weapons;

	/** Skins grouped by weapon tag */
	TMap<FName, TArray<UUTWeaponSkin*>> SkinsByTag;

	/** Currently selected weapon index */
	int32 CurrentWeaponIndex;

	/** Currently selected skin index per weapon tag (0 = default) */
	TMap<FName, int32> SelectedSkinIndex;

	/** Per-weapon hide state (mirrors AUTWeaponFix::HiddenWeaponsByTag) */
	TMap<FName, bool> HideState;

	/** UI containers */
	TSharedPtr<SVerticalBox> WeaponListContainer;
	TSharedPtr<SVerticalBox> SkinListContainer;
	TSharedPtr<STextBlock> StatusText;

	/** Discover all weapon classes inheriting AUTWeaponFix */
	void GatherWeapons();

	/** Find matching weapon skin data assets */
	void GatherSkins();

	/** Load current settings from Mod.ini + AUTWeaponFix statics */
	void LoadSettings();

	/** Save settings to Mod.ini and apply */
	void SaveAndApply();

	/** Rebuild UI lists */
	void RebuildWeaponList();
	void RebuildSkinList();

	/** Handlers */
	FReply OnWeaponClicked(int32 Index);
	FReply OnSkinClicked(int32 Index);
	FReply OnApplyClicked();
	FReply OnCloseClicked();
	void OnHideCheckChanged(ECheckBoxState NewState, FName Tag);

	/** Hitscan choice (Sniper / LG) */
	FString CurrentHitscanChoice;
	TSharedPtr<STextBlock> HitscanValueText;
	FReply OnHitscanToggleClicked();

	/** Hidden-weapon beam origin offsets (read into AUTWeaponFix statics).
	 *  Live values shown in the spinner; LoadSettings seeds them from the
	 *  static (which was filled from Mod.ini); SaveAndApply pushes them back. */
	float HiddenBeamBack;
	float HiddenBeamDown;
	TOptional<float> GetHiddenBeamBack() const { return HiddenBeamBack; }
	TOptional<float> GetHiddenBeamDown() const { return HiddenBeamDown; }
	void OnHiddenBeamBackChanged(float NewVal) { HiddenBeamBack = NewVal; }
	void OnHiddenBeamDownChanged(float NewVal) { HiddenBeamDown = NewVal; }

	/** Beam (tracer) colors stored in Mod.ini [WeaponSkinsPlus] keys LGColor +
	 *  ShockBeam. Read by the UTNPLightningGun / UTNPSniper / UTNPShockRifle
	 *  blueprint defaults at spawn. LGColor is shared by Sniper + LG (unified
	 *  hitscan). String format = FLinearColor::ToString output "(R=...,G=...,
	 *  B=...,A=...)" so BP "Convert String to LinearColor" reads it directly. */
	FLinearColor HitscanBeamColor;
	FLinearColor ShockBeamColor;
	// SColorBlock has no SetColor in UE4.15 — bind via TAttribute getters so the
	// swatch re-polls each paint and reflects edits automatically.
	FLinearColor GetHitscanBeamColor() const { return HitscanBeamColor; }
	FLinearColor GetShockBeamColor()   const { return ShockBeamColor;   }
	FReply OnHitscanColorClicked();
	FReply OnShockColorClicked();
	/** Shared FSE-safe color picker. Mirrors the nchud OnSwatchClicked pattern
	 *  (snapshot mode, swap to borderless, restore on close). */
	void OpenBeamColorPicker(FLinearColor Initial, TFunction<void(FLinearColor)> OnCommit);
	void OnHitscanColorCommitted(FLinearColor NewColor);
	void OnShockColorCommitted(FLinearColor NewColor);

	FSlateBrush BackgroundBrush;
};
