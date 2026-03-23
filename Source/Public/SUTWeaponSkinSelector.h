// SUTWeaponSkinSelector.h — NetcodePlus weapon settings panel (skins + hide)
#pragma once

#include "SlateBasics.h"
#include "UTWeaponSkin.h"

class UUTLocalPlayer;

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

	FSlateBrush BackgroundBrush;
};
