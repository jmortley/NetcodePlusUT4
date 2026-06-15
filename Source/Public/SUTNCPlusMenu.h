// SUTNCPlusMenu.h — NetcodePlus client settings panel
// Console command: ncpmenu (also bound to F5 by default)
// Tabs: General (gore/footsteps/screenshot) + Force Models
#pragma once

#include "NetcodePlus.h"     // PCH preamble (UT/core types first)
#include "SlateBasics.h"
#include "NCPlusForceModels.h"

class UUTLocalPlayer;

/** Which settings tab is showing. */
enum class ENCPMenuTab : uint8
{
	About,
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

	// ── Force Models settings ── working copy of the live config, edited in-place by the tab's
	// widgets via bool*/float* lambdas and written back on Save. The combo option lists are members
	// so each STextComboBox::OptionsSource can point at them for the panel's lifetime.
	FNCPlusForceModelsConfig FMConfig;
	TArray<NCPlusForceModels::FContentEntry> FMContentEntries; // installed characters (display -> path)
	TArray<TSharedPtr<FString>> FMModelOptions;                // "(none)" + each FMContentEntries display name
	TArray<TSharedPtr<FString>> FMStyleOptions;                // Team/Enemy, Red/Blue, Enemy Only
	TArray<TSharedPtr<FString>> FMArmourOptions;               // Match Skin, Complimentary

	// ── Tabs ──
	ENCPMenuTab ActiveTab = ENCPMenuTab::About;
	TSharedPtr<class SBox> ContentArea;
	TSharedRef<SWidget> BuildAboutTab();
	TSharedRef<SWidget> BuildGeneralTab();
	TSharedRef<SWidget> BuildForceModelsTab();
	TSharedRef<SWidget> BuildTabContent(ENCPMenuTab Tab);
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

	// Force Models tab builders/helpers
	TSharedRef<SWidget> BuildSideRow(const FString& Label, FNCPlusModelSettings* Side);
	TSharedRef<SWidget> MakeFlagCheck(const FString& Label, bool* Flag);
	TSharedRef<SWidget> MakeLabeledSpin(const FString& Label, float* Value, float Min, float Max, float Delta);

	FReply OnSaveClicked();
	FReply OnCloseClicked();

	// Background
	FSlateBrush BackgroundBrush;
};
