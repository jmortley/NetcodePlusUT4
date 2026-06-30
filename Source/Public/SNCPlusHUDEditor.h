// SNCPlusHUDEditor.h — Slate panel for live-editing the ElimPlus HUD layout.
// Console command: nchud (also a toggle). Mutates FNCPlusHUDLayout::GetLive()
// directly, so changes appear next frame in the running PIE / game session.
#pragma once

#include "NetcodePlus.h"     // PCH preamble
#include "SlateBasics.h"
#include "NCPlusHUDLayout.h"

class UUTLocalPlayer;
class SNCPlusHUDPresetGallery;
class SBox;

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
	// No stored widget pointer — the swatch's color is attribute-bound to
	// GetCurrentColor() and re-read each paint, so it stays live without a
	// reference to update.
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

	// Optional per-row font picker (hp_armor + ammo).
	TSharedPtr<class STextComboBox> FontCombo;
	TArray<TSharedPtr<FString>> FontChoices;
	bool bHasFontPicker = false;

	// Optional per-row font display-size slider (Extras["font_scale"]). Only enable
	// for widgets whose draw site actually calls NCPlusHUDFonts::ResolveScale -
	// otherwise the slider would appear to do nothing. Today: scorebar, score_kda.
	bool bHasFontScale = false;

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

	/** RAII backstop: release a held NCPlusHUDDragMode count if torn down without
	 *  ClosePanel (e.g. viewport widgets dropped on a map load). */
	virtual ~SNCPlusHUDEditor();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;
	bool bHeldDragMode = false;  // true while this panel holds a NCPlusHUDDragMode refcount
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

	// Font combo (hp_armor + ammo)
	void OnFontSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias);

	// Font display-size multiplier (Extras["font_scale"], default 1.0). Lets the
	// user dial in apparent text size without bumping LegacyFontSize on the asset.
	// Consumed by widgets that call NCPlusHUDFonts::ResolveScale (scorebar / score_kda
	// today; other widgets need an explicit ResolveScale wire-up to honor it).
	TOptional<float> GetFontScale(FName Alias) const;
	void OnFontScaleChanged(float NewVal, FName Alias);
	void OnFontScaleCommitted(float NewVal, ETextCommit::Type, FName Alias);

	// Opacity (per-element multiplier 0..1)
	TOptional<float> GetOpacity(FName Alias) const;
	void OnOpacityChanged(float NewVal, FName Alias);
	void OnOpacityCommitted(float NewVal, ETextCommit::Type, FName Alias);

	// Scale (per-element size multiplier — 1.0 = stock, 0.5 = half, 2.0 = double)
	TOptional<float> GetScale(FName Alias) const;
	void OnScaleChanged(float NewVal, FName Alias);
	void OnScaleCommitted(float NewVal, ETextCommit::Type, FName Alias);

	// Font sub-row (hp_armor + ammo).
	TSharedRef<class SWidget> BuildFontRow(FNCHUDEditorRow& Row);

	// Color overrides — swatch button per slot, opens SColorPicker on click.
	TSharedRef<class SWidget> BuildColorRow(FNCHUDEditorRow& Row);
	FReply OnSwatchClicked(FName Alias, FName ColorKey, FLinearColor DefaultColor);
	void OnColorPickerCommitted(FLinearColor NewColor, FName Alias, FName ColorKey);
	FLinearColor GetCurrentColor(FName Alias, FName ColorKey, FLinearColor DefaultColor) const;

	// Team-color toggle (portraits + scorebar)
	ECheckBoxState GetTeamColorState(FName Alias) const;
	void OnTeamColorChanged(ECheckBoxState NewState, FName Alias);

	// Stock-vs-NCPlus bottom-bar toggle (global; moved here from the iCTF settings tab).
	// Applies immediately: persists [NetcodePlus] StockBottomBar + live-swaps the widget family.
	ECheckBoxState GetStockBottomBarState() const;
	void OnStockBottomBarChanged(ECheckBoxState NewState);

	// Stock top-left team roster (slanted name + HP plates) vs the NCPlus portrait strip.
	// Applies on the next HUD frame; persists [NetcodePlus] StockTeamPanel.
	ECheckBoxState GetStockTeamPanelState() const;
	void OnStockTeamPanelChanged(ECheckBoxState NewState);

	// Scoreboard background opacity (0.05..1.0). Global; persists [NetcodePlus] ScoreboardOpacity.
	TOptional<float> GetScoreboardOpacityValue() const;
	void OnScoreboardOpacityChanged(float NewValue);
	void OnScoreboardOpacityCommitted(float NewValue, ETextCommit::Type CommitType);

	// Per-row reset
	FReply OnResetRowClicked(FName Alias);

	// Footer buttons
	FReply OnSaveClicked();
	FReply OnReloadClicked();
	FReply OnResetAllClicked();
	FReply OnCloseClicked();
	FReply OnCopyToClipboardClicked();
	FReply OnPasteFromClipboardClicked();
	FReply OnPresetsClicked();

	// Preset gallery wiring (overlay on the editor body, toggled visible).
	TSharedPtr<SNCPlusHUDPresetGallery> PresetGallery;
	TSharedPtr<SBox>                    PresetGalleryWrapper;
	void ShowPresetGallery(bool bShow);

	// Replace GetLive() with the parsed contents of Json. Called by
	// OnPasteFromClipboardClicked and the preset gallery's Apply path.
	// Source is the user-facing description ("the clipboard layout",
	// "the 'Streamer Friendly' preset") interpolated into the confirm
	// dialog. Returns true on success (after user confirmation).
	bool ApplyJsonReplacingLive(const FString& Json, const FString& Source);

	// Refresh anchor/style/font combo selections to match GetLive().
	// Shared by Reload, Paste, and Apply Preset paths.
	void ResyncCombosToLive();

	// Status banner ("Saved.", "Reloaded.", etc.)
	TSharedPtr<class STextBlock> StatusText;
	void SetStatus(const FString& Msg);

	FSlateBrush BackgroundBrush;
};
