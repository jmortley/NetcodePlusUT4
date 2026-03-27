// SUTNCPlusMenu.h — NetcodePlus client settings panel
// Console command: ncpmenu (also bound to F5 by default)
// Settings: gore (gib/ragdoll), footstep volume, screenshot post-match
#pragma once

#include "SlateBasics.h"

class UUTLocalPlayer;

/**
 * NetcodePlus settings panel.
 * Opened via "ncpmenu" console command or F5 key (configurable).
 * Reads/writes to Mod.ini [NetcodePlus] section.
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

	// Settings
	bool bAllowGib;
	bool bShowRagdoll;
	float RagdollTime;
	float OwnFootstepVolume;
	bool bHighResScreenshotPostMatch;

	// Load from Mod.ini
	void LoadSettings();

	// Save to Mod.ini
	void SaveSettings();

	// Handlers
	void OnAllowGibChanged(ECheckBoxState NewState);
	void OnShowRagdollChanged(ECheckBoxState NewState);
	void OnRagdollTimeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnFootstepVolumeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnScreenshotChanged(ECheckBoxState NewState);
	FReply OnSaveClicked();
	FReply OnCloseClicked();

	// Background
	FSlateBrush BackgroundBrush;
};
