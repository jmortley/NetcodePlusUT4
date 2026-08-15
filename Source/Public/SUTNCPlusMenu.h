// SUTNCPlusMenu.h — NetcodePlus client settings panel
// Console command: ncpmenu (also bound to F5 by default)
// Tabs: Home (ready-up/about/announcer), iCTF, Force Models, and Hitsounds
#pragma once

#include "NetcodePlus.h"     // PCH preamble (UT/core types first)
#include "SlateBasics.h"
#include "NCPlusForceModels.h"
#include "NCPlusAnnouncer.h"
#include "ClientHitsounds.h"

class UUTLocalPlayer;

/** Which settings tab is showing. */
enum class ENCPMenuTab : uint8
{
	Home,
	ICTF,
	ForceModels,
	Hitsounds,
};

/**
 * NetcodePlus settings panel.
 * Opened via "ncpmenu" console command or F5 key (configurable).
 * iCTF tab reads/writes Mod.ini [NetcodePlus]/[InstagibCTF]; Force Models drives NCPlusForceModels.
 */
class SUTNCPlusMenu : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SUTNCPlusMenu)
		: _InitialTab(ENCPMenuTab::Home)
		{}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	/** Tab to show on open — ncpmenu's optional arg (bare F5 = Home default). */
	SLATE_ARGUMENT(ENCPMenuTab, InitialTab)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	/** Switch the visible tab in place (used when `ncpmenu <tab>` targets an
	 *  already-open panel — closing it instead would punish dc muscle memory
	 *  like typing "mutate hitsounds" with F5 open). */
	void SwitchTab(ENCPMenuTab Tab);

	/** RAII backstop: release a held NCPlusHUDDragMode count if this panel is torn
	 *  down without ClosePanel (e.g. viewport widgets dropped on a map load). */
	virtual ~SUTNCPlusMenu();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;
	mutable TWeakObjectPtr<class ANCReadyUpState> ReadyUpState;
	bool bHeldDragMode = false;  // true while this panel holds a NCPlusHUDDragMode refcount

	// ── iCTF settings ([NetcodePlus]/[InstagibCTF] in Mod.ini) ──
	bool bAllowGib;
	bool bShowRagdoll;
	float RagdollTime;
	bool bShowOwnBeam;
	float OwnFootstepVolume;
	bool bPlayFlagCarrierSound;
	bool bHighResScreenshotPostMatch;
	TArray<FNCPlusAnnouncerPackOption> AnnouncerPackEntries;
	TArray<TSharedPtr<FString>> AnnouncerPackOptions;
	FString SelectedAnnouncerPackId;
	bool bAnnouncerDataInitialized = false;

	// ── Home performance settings ── client-local working copy, applied on Save.
	float CharacterOverlayDistance = 6500.f;
	TArray<TSharedPtr<FString>> CharacterOverlayDistanceOptions;
	bool bShowClutchOverlay = true;

	// ── Force Models settings ── working copy of the live config, edited in-place by the tab's
	// widgets via bool*/float* lambdas and written back on Save. The combo option lists are members
	// so each STextComboBox::OptionsSource can point at them for the panel's lifetime.
	FNCPlusForceModelsConfig FMConfig;
	TArray<NCPlusForceModels::FContentEntry> FMContentEntries; // installed characters (display -> path)
	TArray<TSharedPtr<FString>> FMModelOptions;                // "(none)" + each FMContentEntries display name
	TArray<TSharedPtr<FString>> FMStyleOptions;                // Team/Enemy, Red/Blue, Enemy Only
	TArray<TSharedPtr<FString>> FMArmourOptions;               // Match Skin, Complimentary
	bool bForceModelDataInitialized = false;

	// ── Hitsounds settings ── working copy, edited in place and written to Mod.ini on Save.
	// Option lists are members so each STextComboBox::OptionsSource stays valid for the
	// panel's lifetime (same lifetime contract as the Force Models lists above).
	FHitsoundsConfig HSConfig;
	TArray<TSharedPtr<FString>> HSPresetOptions;   // every catalog preset, built-in then custom
	TArray<TSharedPtr<FString>> HSStyleOptions;    // Absolute / UTComp / Flat
	bool bHitsoundDataInitialized = false;

	// ── Tabs ──
	ENCPMenuTab ActiveTab = ENCPMenuTab::Home;
	TSharedPtr<class SBox> ContentArea;
	TSharedRef<SWidget> BuildHomeTab();
	TSharedRef<SWidget> BuildICTFTab();
	TSharedRef<SWidget> BuildForceModelsTab();
	TSharedRef<SWidget> BuildHitsoundsTab();
	TSharedRef<SWidget> BuildTabContent(ENCPMenuTab Tab);
	TSharedRef<SWidget> MakeTabButton(const FString& Label, ENCPMenuTab Tab);
	FReply OnTabClicked(ENCPMenuTab Tab);
	// "Launch" tabs: look like tabs but close this menu and run a tool's console command (weaponskins / nchud).
	TSharedRef<SWidget> MakeLaunchButton(const FString& Label, const FString& Command);
	FReply OnLaunchClicked(FString Command);

	void LoadSettings();
	void SaveSettings();
	void EnsureAnnouncerData();
	void EnsureForceModelData();
	void EnsureHitsoundData();

	// Home/ready-up handlers
	class AUTPlayerController* GetUTPlayerController() const;
	class AUTPlayerState* GetUTPlayerState() const;
	class ANCReadyUpState* GetReadyUpState() const;
	bool CanSetReady(bool bReady) const;
	FText GetReadyModeText() const;
	FText GetReadyCountText() const;
	FReply OnReadyClicked(bool bReady);
	FReply OnGitHubClicked();

	// iCTF handlers
	void OnAllowGibChanged(ECheckBoxState NewState);
	void OnShowRagdollChanged(ECheckBoxState NewState);
	void OnRagdollTimeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnShowOwnBeamChanged(ECheckBoxState NewState);
	void OnFootstepVolumeChanged(float NewValue, ETextCommit::Type CommitType);
	void OnPlayFlagCarrierSoundChanged(ECheckBoxState NewState);
	void OnScreenshotChanged(ECheckBoxState NewState);

	// Force Models tab builders/helpers
	/** One per-side settings row. bFixedColour (the Red/Blue rows): the style forces the colours
	 *  wholesale (zero-config red/blue), so the Model picker + H/S/V are collapsed and only Glow +
	 *  Armour mode show. */
	TSharedRef<SWidget> BuildSideRow(const FString& Label, FNCPlusModelSettings* Side, bool bFixedColour = false);
	TSharedRef<SWidget> MakeFlagCheck(const FString& Label, bool* Flag);
	TSharedRef<SWidget> MakeLabeledSpin(const FString& Label, float* Value, float Min, float Max, float Delta);

	FReply OnSaveClicked();
	FReply OnCloseClicked();

	// Background
	FSlateBrush BackgroundBrush;
};
