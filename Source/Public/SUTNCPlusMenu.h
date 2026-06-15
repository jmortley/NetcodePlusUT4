// SUTNCPlusMenu.h — NetcodePlus client settings panel
// Console command: ncpmenu (also bound to F5 by default)
// Tabs: General (gore/footsteps/screenshot) + Force Models
#pragma once

#include "SlateBasics.h"

class UUTLocalPlayer;

/** Which settings tab is showing. */
enum class ENCPMenuTab : uint8
{
	General,
	ForceModels,
};

/**
 * NetcodePlus settings panel.
 * Opened via "ncpmenu" console command or F5 key (configurable).
 * General tab reads/writes Mod.ini [NetcodePlus]; Force Models tab drives NCPlusForceModels.
 */
class SUTNCPlusMenu : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SUTNCPlusMenu) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;

	// ── General settings ([NetcodePlus] in Mod.ini) ──
	bool bAllowGib;
	bool bShowRagdoll;
	float RagdollTime;
	float OwnFootstepVolume;
	bool bHighResScreenshotPostMatch;

	// ── Force Models settings (mirror of NCPlusForceModels config; Stage 1 = master toggle) ──
	bool bForceModelsEnabled;

	// ── Tabs ──
	ENCPMenuTab ActiveTab = ENCPMenuTab::General;
	TSharedPtr<class SBox> ContentArea;
	TSharedRef<SWidget> BuildGeneralTab();
	TSharedRef<SWidget> BuildForceModelsTab();
	TSharedRef<SWidget> MakeTabButton(const FString& Label, ENCPMenuTab Tab);
	FReply OnTabClicked(ENCPMenuTab Tab);

	void LoadSettings();
	void SaveSettings();

	// General handlers
	void OnAllowGibChanged(ECheckBoxState NewState);
	void OnShowRagdollChanged(ECheckBoxState NewState);
	void OnRagdollTimeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnFootstepVolumeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnScreenshotChanged(ECheckBoxState NewState);

	// Force Models handlers
	void OnForceModelsEnabledChanged(ECheckBoxState NewState);

	FReply OnSaveClicked();
	FReply OnCloseClicked();

	// Background
	FSlateBrush BackgroundBrush;
};
