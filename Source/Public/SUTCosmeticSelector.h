// SUTCosmeticSelector.h — Cosmetic selector panel for NetcodePlus
// Enumerates ALL cosmetics (including entitlement-gated) via GetAllBlueprintAssetData(false)
// and applies them via the standard ServerReceive* RPCs.
#pragma once

#include "SlateBasics.h"

class UUTLocalPlayer;

/** A cosmetic slot (hat, eyewear, character, taunt, etc.) */
enum class ECosmeticSlot : uint8
{
	Hat,
	LeaderHat,
	Eyewear,
	Taunt1,
	Taunt2,
	GroupTaunt,
	Character,
	MAX
};

/** Info about a single cosmetic item */
struct FCosmeticItemInfo
{
	FString DisplayName;
	FString ClassPath;    // Full path with _C suffix for LoadClass
	FAssetData AssetData;
};

/**
 * Cosmetic selector panel. Bypasses Epic's dead entitlement system by
 * calling GetAllBlueprintAssetData with bRequireEntitlements=false.
 * Selections are sent via ServerReceiveHatClass/etc. RPCs and saved to Mod.ini.
 */
class SUTCosmeticSelector : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SUTCosmeticSelector) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	/** RAII backstop: release a held NCPlusHUDDragMode count if torn down without
	 *  ClosePanel (e.g. viewport widgets dropped on a map load). */
	virtual ~SUTCosmeticSelector();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;
	bool bHeldDragMode = false;  // true while this panel holds a NCPlusHUDDragMode refcount

	/** All discovered items per slot */
	TMap<ECosmeticSlot, TArray<FCosmeticItemInfo>> ItemsBySlot;

	/** Currently selected slot tab */
	ECosmeticSlot CurrentSlot;

	/** Currently selected item index per slot (-1 = none/default) */
	TMap<ECosmeticSlot, int32> SelectedIndex;

	/** UI containers */
	TSharedPtr<SVerticalBox> ItemListContainer;
	TSharedPtr<STextBlock> StatusText;

	/** Enumerate all cosmetics for all slots */
	void GatherAllCosmetics();

	/** Rebuild the item list for the current slot */
	void RebuildItemList();

	/** Load saved selections from Mod.ini */
	void LoadSettings();

	/** Save selections to Mod.ini and apply via RPCs */
	void SaveAndApply();

	/** Apply a single slot's selection via the appropriate server RPC */
	void ApplySlot(ECosmeticSlot Slot, const FString& ClassPath);

	/** Get display name for a slot */
	static FString GetSlotName(ECosmeticSlot Slot);

	/** Handlers */
	FReply OnSlotTabClicked(ECosmeticSlot Slot);
	FReply OnItemClicked(int32 Index);
	FReply OnApplyClicked();
	FReply OnCloseClicked();

	FSlateBrush BackgroundBrush;
};
