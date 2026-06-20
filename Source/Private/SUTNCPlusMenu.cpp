// SUTNCPlusMenu.cpp — NetcodePlus client settings implementation
#include "SUTNCPlusMenu.h"
#include "NCPlusHUDLayout.h"
#include "NCPlusForceModels.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SScrollBox.h"

// Mod.ini section (General tab)
static const TCHAR* NCPSection = TEXT("NetcodePlus");
// Gib/ragdoll death settings live under [InstagibCTF] — that's the section the iCTF damage type
// (NCPlusUTDmg_Instagib: ShouldGib reads bAllowGib, PlayDeathEffects reads RagdollTime) actually reads.
static const TCHAR* IGCTFSection = TEXT("InstagibCTF");

// Ragdoll-time semantics (the iCTF damage-type BP NCPlusUTDmg_Instagib::PlayDeathEffects passes RagdollTime
// straight into a "Set Timer by Function Name" / CleanUpRagdoll node). Engine rule, FTimerManager::SetTimer:
// rate <= 0 NEVER schedules the timer. So the full 0..10 range is INTENTIONAL and all of it is useful:
//   0           -> timer never fires -> ragdoll is KEPT (no auto-cleanup)
//   tiny (0.01) -> timer fires almost instantly -> ragdoll REMOVED (the "disable ragdolls" setting)
//   N           -> ragdoll despawns after N seconds
// Do NOT snap/clamp the low end away — both ends are features players want.

// Shared fonts
static FSlateFontInfo BoldFont(int32 Size)   { return FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), Size); }
static FSlateFontInfo RegularFont(int32 Size) { return FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), Size); }

void SUTNCPlusMenu::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;
	ActiveTab = InArgs._InitialTab;   // ContentArea below builds BuildTabContent(ActiveTab)

	// Take mouse input: show the cursor and switch to GameAndUI so Slate gets
	// mouse events. NCPlusHUDDragMode holds the HUD's per-tick GetInputMode poll
	// open (it returns GameOnly during a match and would otherwise re-capture the
	// cursor). Released in ClosePanel. Mirrors SNCPlusHUDDragOverlay.
	NCPlusHUDDragMode::SetActive(true);
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		APlayerController* MenuPC = PlayerOwner->PlayerController;
		MenuPC->bShowMouseCursor = true;

		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(SharedThis(this));
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		MenuPC->SetInputMode(InputMode);
	}

	// Semi-transparent dark background
	BackgroundBrush.TintColor = FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.7f));

	LoadSettings();

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(&BackgroundBrush)
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 20, 0, 10)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("NETCODEPLUS SETTINGS")))
				.Font(BoldFont(28))
				.ColorAndOpacity(FLinearColor(1.f, 0.6f, 0.f, 1.f))
			]

			// Tab strip
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeTabButton(TEXT("About"), ENCPMenuTab::About)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeTabButton(TEXT("iCTF"), ENCPMenuTab::General)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeTabButton(TEXT("Force Models"), ENCPMenuTab::ForceModels)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeLaunchButton(TEXT("Weapon Skins"), TEXT("weaponskins"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeLaunchButton(TEXT("HUD Editor"), TEXT("nchud"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					MakeLaunchButton(TEXT("Cosmetics"), TEXT("cosmetics"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					// Opens the BP hitsounds menu (the C++ hitsounds port isn't ready yet):
					// close this menu + run "mutate hitsounds" (routes to the BP mutator's Mutate handler).
					MakeLaunchButton(TEXT("Hitsounds"), TEXT("mutate hitsounds"))
				]
			]

			// Active tab content
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			[
				SAssignNew(ContentArea, SBox)
				[
					BuildTabContent(ActiveTab)
				]
			]

			// Buttons (always visible)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 25, 0, 20)
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 10, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Save")))
					.OnClicked(this, &SUTNCPlusMenu::OnSaveClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Close")))
					.OnClicked(this, &SUTNCPlusMenu::OnCloseClicked)
				]
			]
		]
	];
}

TSharedRef<SWidget> SUTNCPlusMenu::MakeTabButton(const FString& Label, ENCPMenuTab Tab)
{
	return SNew(SButton)
		.OnClicked(this, &SUTNCPlusMenu::OnTabClicked, Tab)
		.ContentPadding(FMargin(18, 6))
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label))
			.Font(BoldFont(14))
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::MakeLaunchButton(const FString& Label, const FString& Command)
{
	// Looks like a tab, but instead of swapping content it closes this menu and runs the tool's
	// console command (weaponskins / nchud), which open their own full-screen Slate panels.
	return SNew(SButton)
		.OnClicked(this, &SUTNCPlusMenu::OnLaunchClicked, Command)
		.ContentPadding(FMargin(18, 6))
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label))
			.Font(BoldFont(14))
		];
}

FReply SUTNCPlusMenu::OnLaunchClicked(FString Command)
{
	// Capture the PC before ClosePanel (which removes this widget), then run the command.
	APlayerController* PC = PlayerOwner.IsValid() ? PlayerOwner->PlayerController : nullptr;
	ClosePanel();
	if (PC)
	{
		PC->ConsoleCommand(Command, false);
	}
	return FReply::Handled();
}

FReply SUTNCPlusMenu::OnTabClicked(ENCPMenuTab Tab)
{
	ActiveTab = Tab;
	if (ContentArea.IsValid())
	{
		ContentArea->SetContent(BuildTabContent(Tab));
	}
	return FReply::Handled();
}

TSharedRef<SWidget> SUTNCPlusMenu::BuildTabContent(ENCPMenuTab Tab)
{
	switch (Tab)
	{
		case ENCPMenuTab::ForceModels: return BuildForceModelsTab();
		case ENCPMenuTab::General:     return BuildGeneralTab();
		case ENCPMenuTab::About:
		default:                       return BuildAboutTab();
	}
}

TSharedRef<SWidget> SUTNCPlusMenu::BuildAboutTab()
{
	const FString VersionLine = FString::Printf(TEXT("Version %d"), NETCODE_PLUGIN_VERSION);

	return SNew(SBox)
		.WidthOverride(560.f)
		[
			SNew(SVerticalBox)

			// Heading
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 12, 0, 2)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("About NetcodePlus")))
				.Font(BoldFont(18))
				.ColorAndOpacity(FLinearColor::White)
			]

			// Version
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 12)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(VersionLine))
				.Font(RegularFont(12))
				.ColorAndOpacity(FLinearColor(1.f, 0.6f, 0.f, 1.f))
			]

			// Blurb
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(24, 0, 24, 12)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("A community client and server enhancement plugin for UT4: improved netcode and hit registration, extra game modes (Elimination+, Wipeout, Duel, Shaft Arena, Shock Domination), a configurable HUD, weapon skins, and team-model overrides. Unofficial and community-maintained.")))
				.Font(RegularFont(12))
				.ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f, 1.f))
				.AutoWrapText(true)
			]

			// Console commands heading
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 6, 0, 4)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Console Commands")))
				.Font(BoldFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]

			// Console commands list
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(24, 0, 24, 4)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("F5  or  ncpmenu  -  open this menu\nweaponskins  -  weapon skin selector\ncosmetics  -  hats, eyewear, characters, taunts\nnchud  -  HUD layout editor\nnchud_drag  -  drag HUD elements\nforcemodels_list / forcemodels_dumpmats  -  team-model diagnostics")))
				.Font(RegularFont(12))
				.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.f))
				.AutoWrapText(true)
			]

			// Footer hint
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(24, 12, 24, 6)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Use the tabs above for General and Force Models settings.")))
				.Font(RegularFont(11))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.f))
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::BuildGeneralTab()
{
	return SNew(SVerticalBox)

		// ── Gore Settings ──
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 10, 0, 5)
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Gore Settings")))
			.Font(BoldFont(18))
			.ColorAndOpacity(FLinearColor::White)
		]

		// Allow Gib
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(40, 4, 40, 4)
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked(bAllowGib ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged(this, &SUTNCPlusMenu::OnAllowGibChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Allow Gib")))
				.Font(RegularFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]
		]

		// Show Ragdoll
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(40, 4, 40, 4)
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked(bShowRagdoll ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged(this, &SUTNCPlusMenu::OnShowRagdollChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Show Ragdoll")))
				.Font(RegularFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]
		]

		// Ragdoll Time
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(40, 4, 40, 4)
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Ragdoll Time")))
				.Font(RegularFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(12, 0, 0, 0)
			[
				SNew(SSpinBox<float>)
				.MinValue(0.f)
				.MaxValue(10.f)
				.Value(RagdollTime)
				.OnValueCommitted(this, &SUTNCPlusMenu::OnRagdollTimeChanged)
				.MinDesiredWidth(80.f)
				.ToolTipText(FText::FromString(TEXT("Seconds before a ragdoll despawns. 0 = keep ragdolls forever; a tiny value (e.g. 0.01) removes them almost instantly; e.g. 3 = despawn after 3s.")))
			]
		]

		// ── Footstep Settings ──
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 15, 0, 5)
		.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Footstep Settings")))
			.Font(BoldFont(18))
			.ColorAndOpacity(FLinearColor::White)
		]

		// Own Footstep Volume
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(40, 4, 40, 4)
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Own Footstep Volume")))
				.Font(RegularFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(12, 0, 0, 0)
			[
				SNew(SSpinBox<float>)
				.MinValue(0.f)
				.MaxValue(1.f)
				.Delta(0.1f)
				.Value(OwnFootstepVolume)
				.OnValueCommitted(this, &SUTNCPlusMenu::OnFootstepVolumeChanged)
				.MinDesiredWidth(80.f)
			]
		]

		// ── Screenshot ──
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(40, 15, 40, 4)
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked(bHighResScreenshotPostMatch ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged(this, &SUTNCPlusMenu::OnScreenshotChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8, 0, 0, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("High Res Screenshot PostMatch")))
				.Font(RegularFont(14))
				.ColorAndOpacity(FLinearColor::White)
			]
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::MakeFlagCheck(const FString& Label, bool* Flag)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([Flag] { return (*Flag) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Flag](ECheckBoxState S) { *Flag = (S == ECheckBoxState::Checked); })
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6, 0, 0, 0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label))
			.Font(RegularFont(12))
			.ColorAndOpacity(FLinearColor::White)
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::MakeLabeledSpin(const FString& Label, float* Value, float Min, float Max, float Delta)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 4, 0)
		[
			SNew(SBox)
			.MinDesiredWidth(34.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.Font(RegularFont(12))
				.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.f))
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SSpinBox<float>)
			.MinValue(Min)
			.MaxValue(Max)
			.Delta(Delta)
			.MinDesiredWidth(64.f)
			.Value_Lambda([Value] { return *Value; })
			.OnValueChanged_Lambda([Value](float V) { *Value = V; })
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::BuildSideRow(const FString& Label, FNCPlusModelSettings* Side)
{
	// Resolve the model combo's initial selection from the stored class path ("(none)" if empty).
	// Match against VariantPaths too, so a saved variant that got coalesced (e.g. SkaarjMale03 -> the
	// "SkaarjMale" entry, which applies SkaarjMale01) still shows its family as the current selection
	// even though the stored path isn't the representative. (The model itself still applies regardless,
	// since the applier loads by path; this only fixes what the dropdown displays.)
	TSharedPtr<FString> InitialModel = (FMModelOptions.Num() > 0) ? FMModelOptions[0] : nullptr;
	if (!Side->ContentPath.IsEmpty())
	{
		for (int32 i = 0; i < FMContentEntries.Num(); ++i)
		{
			const NCPlusForceModels::FContentEntry& E = FMContentEntries[i];
			const bool bMatch = (E.ClassPath == Side->ContentPath) || E.VariantPaths.Contains(Side->ContentPath);
			if (bMatch && FMModelOptions.IsValidIndex(i + 1))
			{
				InitialModel = FMModelOptions[i + 1];
				break;
			}
		}
	}

	return SNew(SVerticalBox)

		// Side label
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 6, 0, 2)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Label))
			.Font(BoldFont(13))
			.ColorAndOpacity(FLinearColor(1.f, 0.6f, 0.f, 1.f))
		]

		// Model picker
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 2, 0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Model")))
				.Font(RegularFont(12))
				// Live colour preview: tint the "Model" label with this side's current skin colour
				// (from its H/S/V), re-evaluated each paint so it tracks the sliders as you drag them.
				.ColorAndOpacity_Lambda([Side] { return FSlateColor(NCPlusForceModels::GetSkinColour(*Side)); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			.VAlign(VAlign_Center)
			[
				SNew(STextComboBox)
				.OptionsSource(&FMModelOptions)
				.InitiallySelectedItem(InitialModel)
				.OnSelectionChanged_Lambda([this, Side](TSharedPtr<FString> NewSel, ESelectInfo::Type)
				{
					if (!NewSel.IsValid()) { return; }
					const FString Sel = *NewSel;
					if (Sel == TEXT("(none)")) { Side->ContentPath.Empty(); return; }
					for (const NCPlusForceModels::FContentEntry& E : FMContentEntries)
					{
						if (E.DisplayName == Sel) { Side->ContentPath = E.ClassPath; return; }
					}
				})
			]
		]

		// Hue / Saturation / Value
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 2, 0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0) [ MakeLabeledSpin(TEXT("H"), &Side->H, 0.f, 360.f, 1.f) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0) [ MakeLabeledSpin(TEXT("S"), &Side->S, 0.f, 1.f, 0.02f) ]
			+ SHorizontalBox::Slot().AutoWidth()                      [ MakeLabeledSpin(TEXT("V"), &Side->V, 0.f, 1.f, 0.02f) ]
		]

		// Glow + Armour mode
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0, 2, 0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0) [ MakeLabeledSpin(TEXT("Glow"), &Side->Brightness, 1.f, 5.f, 0.25f) ]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 6, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Armour")))
				.Font(RegularFont(12))
				.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.f))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(150.f)
				[
					SNew(STextComboBox)
					.OptionsSource(&FMArmourOptions)
					.InitiallySelectedItem(FMArmourOptions.IsValidIndex((int32)Side->ArmourMode) ? FMArmourOptions[(int32)Side->ArmourMode] : nullptr)
					.OnSelectionChanged_Lambda([this, Side](TSharedPtr<FString> NewSel, ESelectInfo::Type)
					{
						if (!NewSel.IsValid()) { return; }
						const int32 Idx = FMArmourOptions.IndexOfByPredicate([&](const TSharedPtr<FString>& P) { return P.IsValid() && *P == *NewSel; });
						if (Idx != INDEX_NONE) { Side->ArmourMode = (ENCPlusArmourMode)Idx; }
					})
				]
			]
		];
}

TSharedRef<SWidget> SUTNCPlusMenu::BuildForceModelsTab()
{
	return SNew(SBox)
		.WidthOverride(620.f)
		[
			SNew(SVerticalBox)

			// Header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 10, 0, 4)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Force Team Models")))
				.Font(BoldFont(18))
				.ColorAndOpacity(FLinearColor::White)
			]

			// Master enable
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20, 4, 20, 8)
			.HAlign(HAlign_Center)
			[
				MakeFlagCheck(TEXT("Enable Force Models"), &FMConfig.bEnabled)
			]

			// Feature flags (two rows of three)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20, 2, 20, 2)
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0) [ MakeFlagCheck(TEXT("Models"), &FMConfig.bModels) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0) [ MakeFlagCheck(TEXT("HUD"),    &FMConfig.bHUD) ]
				+ SHorizontalBox::Slot().AutoWidth()                      [ MakeFlagCheck(TEXT("Armour"), &FMConfig.bArmour) ]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20, 2, 20, 8)
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0) [ MakeFlagCheck(TEXT("Flags"),     &FMConfig.bFlags) ]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0) [ MakeFlagCheck(TEXT("Darken"),    &FMConfig.bDarkenBodies) ]
				+ SHorizontalBox::Slot().AutoWidth()                      [ MakeFlagCheck(TEXT("Cosmetics"), &FMConfig.bCosmetics) ]
			]

			// Style selector
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20, 4, 20, 8)
			.HAlign(HAlign_Center)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 8, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Style")))
					.Font(RegularFont(13))
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(160.f)
					[
						SNew(STextComboBox)
						.OptionsSource(&FMStyleOptions)
						.InitiallySelectedItem(FMStyleOptions.IsValidIndex((int32)FMConfig.Style) ? FMStyleOptions[(int32)FMConfig.Style] : nullptr)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewSel, ESelectInfo::Type)
						{
							if (!NewSel.IsValid()) { return; }
							const int32 Idx = FMStyleOptions.IndexOfByPredicate([&](const TSharedPtr<FString>& P) { return P.IsValid() && *P == *NewSel; });
							if (Idx != INDEX_NONE) { FMConfig.Style = (ENCPlusSkinStyle)Idx; }
						})
					]
				]
			]

			// Per-side rows (scrollable: all four sides always shown; Style decides which apply)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(10, 0, 10, 4)
			.HAlign(HAlign_Center)
			[
				SNew(SBox)
				.HeightOverride(300.f)
				.WidthOverride(600.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot().Padding(8, 0) [ BuildSideRow(TEXT("Enemy"),           &FMConfig.Enemy) ]
					+ SScrollBox::Slot().Padding(8, 0) [ BuildSideRow(TEXT("Team (friendly)"), &FMConfig.Team) ]
					+ SScrollBox::Slot().Padding(8, 0) [ BuildSideRow(TEXT("Red team"),        &FMConfig.Red) ]
					+ SScrollBox::Slot().Padding(8, 0) [ BuildSideRow(TEXT("Blue team"),       &FMConfig.Blue) ]
				]
			]

			// Footnote
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20, 2, 20, 4)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Which sides apply depends on Style. Recolour works on UT-material models; baked-texture models keep their own skin. Save applies live.")))
				.Font(RegularFont(10))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.f))
				.AutoWrapText(true)
			]
		];
}

void SUTNCPlusMenu::LoadSettings()
{
	FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	FString Val;
	// Death gib/ragdoll settings: [InstagibCTF] with the iCTF damage type's exact keys (bAllowGib /
	// RagdollTime). ShowRagdoll kept here too for now — see note in Save (consumer unconfirmed).
	if (GConfig->GetString(IGCTFSection, TEXT("bAllowGib"), Val, ConfigPath))
		bAllowGib = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	else
		bAllowGib = false;

	if (GConfig->GetString(IGCTFSection, TEXT("bShowRagdoll"), Val, ConfigPath))
		bShowRagdoll = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	else
		bShowRagdoll = true;

	if (GConfig->GetString(IGCTFSection, TEXT("RagdollTime"), Val, ConfigPath))
		RagdollTime = FMath::Clamp(FCString::Atof(*Val), 0.f, 10.f);   // full range intentional (0=keep, tiny=remove)
	else
		RagdollTime = 3.0f;

	if (GConfig->GetString(NCPSection, TEXT("OwnFootstepVolume"), Val, ConfigPath))
		OwnFootstepVolume = FCString::Atof(*Val);
	else
		OwnFootstepVolume = 0.0f;

	if (GConfig->GetString(NCPSection, TEXT("HighResScreenshotPostMatch"), Val, ConfigPath))
		bHighResScreenshotPostMatch = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	else
		bHighResScreenshotPostMatch = true;

	// Force Models — take a working copy of the live config (loads Mod.ini [ForceModels] on first access).
	FMConfig = NCPlusForceModels::Get();

	// Build the combo option lists once. Installed characters first ("(none)" lets a side be cleared),
	// then the fixed Style / Armour lists (index order matches the ENCPlusSkinStyle / ENCPlusArmourMode ints).
	FMContentEntries.Reset();
	NCPlusForceModels::EnumerateContent(FMContentEntries);
	FMModelOptions.Reset();
	FMModelOptions.Add(MakeShareable(new FString(TEXT("(none)"))));
	for (const NCPlusForceModels::FContentEntry& E : FMContentEntries)
	{
		FMModelOptions.Add(MakeShareable(new FString(E.DisplayName)));
	}
	FMStyleOptions.Reset();
	FMStyleOptions.Add(MakeShareable(new FString(TEXT("Team / Enemy"))));
	FMStyleOptions.Add(MakeShareable(new FString(TEXT("Red / Blue"))));
	FMStyleOptions.Add(MakeShareable(new FString(TEXT("Enemy Only"))));
	FMArmourOptions.Reset();
	FMArmourOptions.Add(MakeShareable(new FString(TEXT("Match Skin"))));
	FMArmourOptions.Add(MakeShareable(new FString(TEXT("Complimentary"))));
}

void SUTNCPlusMenu::SaveSettings()
{
	FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	// [InstagibCTF] so the iCTF damage type (NCPlusUTDmg_Instagib) actually reads them — was wrongly under
	// [NetcodePlus] with key "AllowGib" (vs the BP's "bAllowGib"), so the menu never drove the damage type.
	GConfig->SetString(IGCTFSection, TEXT("bAllowGib"), bAllowGib ? TEXT("True") : TEXT("False"), ConfigPath);
	GConfig->SetString(IGCTFSection, TEXT("bShowRagdoll"), bShowRagdoll ? TEXT("True") : TEXT("False"), ConfigPath);
	GConfig->SetString(IGCTFSection, TEXT("RagdollTime"), *FString::SanitizeFloat(RagdollTime), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("OwnFootstepVolume"), *FString::SanitizeFloat(OwnFootstepVolume), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("HighResScreenshotPostMatch"), bHighResScreenshotPostMatch ? TEXT("True") : TEXT("False"), ConfigPath);

	GConfig->Flush(false, ConfigPath);

	// Force Models — write the working copy through to the live config + Mod.ini [ForceModels].
	NCPlusForceModels::Mutable() = FMConfig;
	NCPlusForceModels::Save();

	// Live re-apply so changes show immediately without a respawn / rejoin.
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		NCPlusForceModels::ReapplyAll(PlayerOwner->PlayerController->GetWorld());
	}

	UE_LOG(LogTemp, Log, TEXT("NCPlus settings saved to Mod.ini"));
}

void SUTNCPlusMenu::ClosePanel()
{
	// Release the mouse capture taken in Construct (see NCPlusHUDDragMode).
	NCPlusHUDDragMode::SetActive(false);
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		APlayerController* MenuPC = PlayerOwner->PlayerController;
		MenuPC->bShowMouseCursor = false;
		FInputModeGameOnly InputMode;
		MenuPC->SetInputMode(InputMode);
	}

	if (PlayerOwner.IsValid())
	{
		UGameViewportClient* ViewportClient = PlayerOwner->ViewportClient;
		if (ViewportClient)
		{
			ViewportClient->RemoveViewportWidgetContent(SharedThis(this));
		}
	}

	// Return focus to game
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

FReply SUTNCPlusMenu::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape || InKeyEvent.GetKey() == EKeys::F5)
	{
		ClosePanel();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SUTNCPlusMenu::OnAllowGibChanged(ECheckBoxState NewState)
{
	bAllowGib = (NewState == ECheckBoxState::Checked);
}

void SUTNCPlusMenu::OnShowRagdollChanged(ECheckBoxState NewState)
{
	bShowRagdoll = (NewState == ECheckBoxState::Checked);
}

void SUTNCPlusMenu::OnRagdollTimeChanged(float NewValue, ETextCommit::Type CommitType)
{
	// Full range is intentional: 0 = keep ragdolls forever (SetTimer rate<=0 never fires); a tiny value
	// removes them almost instantly (the "disable ragdolls" setting); otherwise seconds before despawn.
	RagdollTime = FMath::Clamp(NewValue, 0.f, 10.f);
}

void SUTNCPlusMenu::OnFootstepVolumeChanged(float NewValue, ETextCommit::Type CommitType)
{
	OwnFootstepVolume = FMath::Clamp(NewValue, 0.f, 1.f);
}

void SUTNCPlusMenu::OnScreenshotChanged(ECheckBoxState NewState)
{
	bHighResScreenshotPostMatch = (NewState == ECheckBoxState::Checked);
}

FReply SUTNCPlusMenu::OnSaveClicked()
{
	SaveSettings();
	ClosePanel();
	return FReply::Handled();
}

FReply SUTNCPlusMenu::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
}
