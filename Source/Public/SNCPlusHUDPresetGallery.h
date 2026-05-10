// SNCPlusHUDPresetGallery.h - browse, apply, save, delete HUD layout presets.
//
// Hosted as an SOverlay slot inside SNCPlusHUDEditor's ChildSlot (toggled
// visible/collapsed by the "Presets..." footer button). Not a top-level
// Slate window - that path interacts poorly with UT4 fullscreen-exclusive
// (see feedback_fse_picker_restore.md). Not a viewport-pushed menu either -
// MenuBuilder.AddSubMenu asserts in UE 4.15 from this context (see
// feedback_no_addsubmenu_in_viewport_menus.md).
#pragma once

#include "NetcodePlus.h"
#include "SlateBasics.h"
#include "NCPlusHUDPresets.h"

// Apply-preset delegate: the gallery passes (Json, Source) up to the editor's
// ApplyJsonReplacingLive helper, which returns true on success so the gallery
// can dismiss itself on confirmation.
DECLARE_DELEGATE_RetVal_TwoParams(bool, FNCApplyPresetDelegate, const FString& /*Json*/, const FString& /*Source*/);

class SNCPlusHUDPresetGallery : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNCPlusHUDPresetGallery) {}
		SLATE_EVENT(FSimpleDelegate,         OnCloseRequested)
		SLATE_EVENT(FNCApplyPresetDelegate,  OnApplyPreset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Re-scan customs from disk + rebuild the cards list. Call after Save /
	 *  Delete or whenever the panel is shown. */
	void RefreshList();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool   SupportsKeyboardFocus() const override { return true; }

private:
	FSimpleDelegate           OnCloseRequested;
	FNCApplyPresetDelegate    OnApplyPreset;
	TArray<FNCPlusHUDPreset>  Presets;        // cached for the current view

	// Top-level layout pieces.
	TSharedPtr<class SVerticalBox> CardListBox;     // populated by RefreshList()
	TSharedPtr<class STextBlock>   StatusText;      // "Saved.", "Deleted.", etc.

	// Inline Save-Current dialog (sub-overlay; visible only while user is
	// entering name/description).
	TSharedPtr<class SBox>              SaveDialogWrapper;
	TSharedPtr<class SEditableTextBox>  SaveDialogNameBox;
	TSharedPtr<class SEditableTextBox>  SaveDialogDescBox;

	FSlateBrush BackgroundBrush;
	FSlateBrush CardBrush;
	FSlateBrush DialogBrush;
	FSlateBrush ScreenBrush;     // dark gray "screen" backdrop in thumbnails

	// Build helpers.
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildCard(const FNCPlusHUDPreset& P);
	TSharedRef<SWidget> BuildThumbnail(const FNCPlusHUDPreset& P);
	TSharedRef<SWidget> BuildSaveCurrentRow();
	TSharedRef<SWidget> BuildSaveDialog();

	// Button handlers.
	FReply OnApplyClicked(FString Json, FString DisplayName);
	FReply OnDeleteClicked(FString Id, FString DisplayName);
	FReply OnSaveCurrentClicked();
	FReply OnSaveDialogConfirmClicked();
	FReply OnSaveDialogCancelClicked();
	FReply OnCloseClicked();

	void   SetStatus(const FString& Msg);
	void   ShowSaveDialog(bool bShow);
};
