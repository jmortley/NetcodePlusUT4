// SUTWeaponSkinSelector.h — Slate weapon skin picker for NetcodePlus
#pragma once

#include "SlateBasics.h"
#include "UTWeaponSkin.h"

class UUTLocalPlayer;

/**
 * Simple weapon skin selector overlay.
 * Opens via "weaponskins" console command, groups skins by weapon tag,
 * applies via the existing ServerReceiveWeaponSkin RPC.
 */
class SUTWeaponSkinSelector : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SUTWeaponSkinSelector) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Remove this widget from the viewport */
	void ClosePanel();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;

	/** All discovered weapon skin assets grouped by customization tag */
	TMap<FName, TArray<UUTWeaponSkin*>> SkinsByWeapon;

	/** Ordered list of weapon tags for display */
	TArray<FName> WeaponTags;

	/** Human-readable names for weapon tags */
	TMap<FName, FString> WeaponDisplayNames;

	/** Currently selected weapon tag index */
	int32 CurrentWeaponIndex;

	/** Currently selected skin index within the weapon's skin list */
	int32 CurrentSkinIndex;

	/** Container for skin buttons — rebuilt when weapon selection changes */
	TSharedPtr<SVerticalBox> SkinListContainer;

	/** Container for weapon buttons */
	TSharedPtr<SVerticalBox> WeaponListContainer;

	/** Preview text showing current selection */
	TSharedPtr<STextBlock> PreviewText;

	/** Gather all UUTWeaponSkin assets from the asset registry */
	void GatherWeaponSkins();

	/** Derive a display name from the weapon class path */
	FString GetWeaponDisplayName(const FString& WeaponClassPath);

	/** Rebuild the skin list for the currently selected weapon */
	void RebuildSkinList();

	/** Rebuild the weapon list (highlights current selection) */
	void RebuildWeaponList();

	/** Apply the currently selected skin */
	void ApplySelectedSkin();

	/** Button handlers */
	FReply OnWeaponClicked(int32 Index);
	FReply OnSkinClicked(int32 Index);
	FReply OnApplyClicked();
	FReply OnCloseClicked();

	/** Style helpers */
	FSlateBrush BackgroundBrush;
};
