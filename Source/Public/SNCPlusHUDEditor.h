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
struct FNCHUDEditorRow
{
	FName Alias;
	FText DisplayName;
	TSharedPtr<class STextComboBox> AnchorCombo;
	TArray<TSharedPtr<FString>> AnchorChoices;   // same indices as ENCPlusHUDAnchor
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
