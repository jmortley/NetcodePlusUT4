// SNCPlusHUDEditor.h — Slate panel for live-editing the ElimPlus HUD layout.
// Console command: nchud (also a toggle). Mutates FNCPlusHUDLayout::GetLive()
// directly, so changes appear next frame in the running PIE / game session.
#pragma once

#include "NetcodePlus.h"     // PCH preamble
#include "SlateBasics.h"
#include "NCPlusHUDLayout.h"

class UUTLocalPlayer;

/**
 * Per-row controls for one HUD element. The panel stores a parallel array of
 * these so we can update individual rows without rebuilding the whole VBox.
 */
/** A single editable color slot on a row (e.g. hp_armor's "color_health"). */
struct FNCHUDEditorColor
{
	FName  Key;            // extras key, e.g. "color_health"
	FText  Label;          // user-facing label, e.g. "Health"
	FLinearColor Default;  // default if no override present
	TSharedPtr<class SEditableTextBox> Input;
};

struct FNCHUDEditorRow
{
	FName Alias;
	FText DisplayName;
	TSharedPtr<class STextComboBox> AnchorCombo;
	TArray<TSharedPtr<FString>> AnchorChoices;   // same indices as ENCPlusHUDAnchor

	// Optional per-row style picker (currently only hp_armor uses one).
	TSharedPtr<class STextComboBox> StyleCombo;
	TArray<TSharedPtr<FString>> StyleChoices;
	bool bHasStylePicker = false;

	// Optional per-row color overrides (hp_armor + weapon_bar_*).
	TArray<FNCHUDEditorColor> Colors;

	// Optional "Use Team Color" checkbox (portraits + scorebar).
	bool bHasTeamColorToggle = false;
};

class SNCPlusHUDEditor : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNCPlusHUDEditor) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;
	TArray<FNCHUDEditorRow> Rows;

	// True while we're programmatically updating combo selections (Reset / Reload /
	// Reset All). Anchor and Style change callbacks honor it and skip MutateElement,
	// otherwise the SetSelectedItem call would fire OnAnchorSelected → re-create the
	// layout entry we just deleted in Reset.
	bool bSuppressComboCallbacks = false;

	// Weapon-group picker state — discovered at panel open.
	struct FNCHUDWeaponPickerEntry
	{
		FName    ClassKey;       // short class name, used for layout assignments
		FString  DisplayName;    // pretty-cleaned name shown in the editor
	};
	TArray<FNCHUDWeaponPickerEntry> WeaponPicker;
	void GatherWeaponsForPicker();
	TSharedRef<class SWidget> BuildWeaponPicker();
	TSharedRef<class SWidget> BuildWeaponPickerRow(const FNCHUDWeaponPickerEntry& W);
	void OnWeaponSideChanged(FName ClassKey, FName NewSide);
	ECheckBoxState GetWeaponSideState(FName ClassKey, FName Side) const;
	void OnWeaponSideCheckChanged(ECheckBoxState State, FName ClassKey, FName Side);

	// Build / rebuild handlers
	TSharedRef<class SWidget> BuildHeader();
	TSharedRef<class SWidget> BuildRow(FNCHUDEditorRow& Row);
	TSharedRef<class SWidget> BuildFooter();

	// Per-element accessors — read from the live layout, falling back to defaults.
	FNCPlusHUDElement& GetOrCreateElement(FName Alias);
	void MutateElement(FName Alias, TFunctionRef<void(FNCPlusHUDElement&)> Mutator);

	// Numeric callbacks
	TOptional<float> GetOffsetX(FName Alias) const;
	TOptional<float> GetOffsetY(FName Alias) const;
	void OnOffsetXChanged(float NewVal, FName Alias);
	void OnOffsetYChanged(float NewVal, FName Alias);
	void OnOffsetXCommitted(float NewVal, ETextCommit::Type, FName Alias);
	void OnOffsetYCommitted(float NewVal, ETextCommit::Type, FName Alias);

	// Hidden checkbox
	ECheckBoxState GetHiddenState(FName Alias) const;
	void OnHiddenChanged(ECheckBoxState NewState, FName Alias);

	// Anchor combo
	void OnAnchorSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias);

	// Style combo (hp_armor only)
	void OnStyleSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias);

	// Opacity (per-element multiplier 0..1)
	TOptional<float> GetOpacity(FName Alias) const;
	void OnOpacityChanged(float NewVal, FName Alias);
	void OnOpacityCommitted(float NewVal, ETextCommit::Type, FName Alias);

	// Color overrides
	TSharedRef<class SWidget> BuildColorRow(FNCHUDEditorRow& Row);
	void OnColorTextCommitted(const FText& NewText, ETextCommit::Type, FName Alias, FName ColorKey);

	// Team-color toggle (portraits + scorebar)
	ECheckBoxState GetTeamColorState(FName Alias) const;
	void OnTeamColorChanged(ECheckBoxState NewState, FName Alias);

	// Per-row reset
	FReply OnResetRowClicked(FName Alias);

	// Footer buttons
	FReply OnSaveClicked();
	FReply OnReloadClicked();
	FReply OnResetAllClicked();
	FReply OnCloseClicked();

	// Status banner ("Saved.", "Reloaded.", etc.)
	TSharedPtr<class STextBlock> StatusText;
	void SetStatus(const FString& Msg);

	FSlateBrush BackgroundBrush;
};
