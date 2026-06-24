// SNCPlusHUDEditor.cpp - implementation of the live HUD layout editor.
#include "SNCPlusHUDEditor.h"
#include "NCPlusHUDLayout.h"
#include "SNCPlusHUDPresetGallery.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTInventory.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/SWindow.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/PlatformMisc.h"     // FPlatformMisc::ClipboardCopy/Paste (UE 4.15)
#include "Misc/MessageDialog.h"

namespace NCHUDEdit
{
	static const float RowHeightPx        = 28.f;
	static const float NumericInputWidth  = 80.f;
	static const float ComboWidth         = 130.f;
	static const float DisplayNameWidth   = 180.f;

	// Section grouping for the editor's collapsible UI (Phase 3.6).
	static FString GetSectionForAlias(FName Alias)
	{
		if (Alias == TEXT("hp_armor"))
		{
			return TEXT("HP / Armor");
		}
		if (Alias == TEXT("ammo"))
		{
			return TEXT("Ammo");
		}
		if (Alias == TEXT("weapon_bar_left") || Alias == TEXT("weapon_bar_right"))
		{
			return TEXT("Weapon Bars");
		}
		if (Alias == TEXT("portrait_red") || Alias == TEXT("portrait_blue") || Alias == TEXT("scorebar") || Alias == TEXT("score_kda"))
		{
			return TEXT("Top Bar (Portraits + Scorebar)");
		}
		if (Alias == TEXT("shockdom_controls") || Alias == TEXT("accuracy") || Alias == TEXT("heal_ability") || Alias == TEXT("candy_marker"))
		{
			return TEXT("Game Mode");
		}
		if (Alias == TEXT("ctf_flag_status") || Alias == TEXT("ctf_carrier_indicator")
			|| Alias == TEXT("ctf_you_have_flag") || Alias == TEXT("ctf_enemy_has_flag")
			|| Alias == TEXT("crosshair_flag_grab"))
		{
			return TEXT("CTF");
		}
		if (Alias == TEXT("speedometer") || Alias == TEXT("minimap")
			|| Alias == TEXT("damage_flash") || Alias == TEXT("server_info")
			|| Alias == TEXT("powerups"))   // held-pickup status (DrawHeldPowerups) — a repositionable C++ draw element
		{
			return TEXT("Optional Overlays");
		}
		return TEXT("Stock Widgets");
	}

	static const TArray<FString>& GetSectionOrder()
	{
		static const TArray<FString> Order = {
			TEXT("HP / Armor"),
			TEXT("Ammo"),
			TEXT("Weapon Bars"),
			TEXT("Top Bar (Portraits + Scorebar)"),
			TEXT("Game Mode"),
			TEXT("CTF"),
			TEXT("Optional Overlays"),
			TEXT("Stock Widgets"),
		};
		return Order;
	}

	// Per-alias style enum dispatch - keeps the row code generic so multiple
	// widgets can have their own style picker without if-laddering everywhere.
	// Returns the integer index into Row.StyleChoices that matches StyleStr.
	static int32 ParseStyleIndex(FName Alias, const FString& StyleStr)
	{
		if (Alias == TEXT("hp_armor")) return (int32)NCPlusHPArmorStyle::Parse(StyleStr);
		if (Alias == TEXT("ammo"))     return (int32)NCPlusAmmoStyle::Parse(StyleStr);
		return 0;
	}

	static TArray<TSharedPtr<FString>> BuildAnchorChoices()
	{
		TArray<TSharedPtr<FString>> Out;
		Out.Add(MakeShareable(new FString(TEXT("TopLeft"))));
		Out.Add(MakeShareable(new FString(TEXT("TopCenter"))));
		Out.Add(MakeShareable(new FString(TEXT("TopRight"))));
		Out.Add(MakeShareable(new FString(TEXT("CenterLeft"))));
		Out.Add(MakeShareable(new FString(TEXT("Center"))));
		Out.Add(MakeShareable(new FString(TEXT("CenterRight"))));
		Out.Add(MakeShareable(new FString(TEXT("BottomLeft"))));
		Out.Add(MakeShareable(new FString(TEXT("BottomCenter"))));
		Out.Add(MakeShareable(new FString(TEXT("BottomRight"))));
		return Out;
	}
}

void SNCPlusHUDEditor::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;

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

	// Conditionally reload layout from disk so the editor reflects current
	// state without clobbering unsaved in-memory work.
	//
	//   Empty Live  → main-menu invocation, no HUD has run BeginPlay yet.
	//                 Reload so users see their saved settings (the original
	//                 reload-on-construct fix from commit 8b3bdaf).
	//   Non-empty   → either an in-game HUD already loaded the layout OR the
	//                 user just made edits in nchud_drag (orientation flip,
	//                 drag, scale change) and is opening the editor to fine-
	//                 tune. Preserve those unsaved edits — reloading would
	//                 silently drop them. The user can hit Reload manually
	//                 if they want disk state.
	if (FNCPlusHUDLayout::GetLive().Elements.Num() == 0)
	{
		FNCPlusHUDLayout::ReloadLive();
	}

	BackgroundBrush.TintColor = FLinearColor(0.05f, 0.05f, 0.05f, 0.92f);

	// Build per-row state
	for (FName Alias : NCPlusHUDAliases::GetAllAliases())
	{
		FNCHUDEditorRow Row;
		Row.Alias         = Alias;
		Row.DisplayName   = NCPlusHUDAliases::GetDisplayName(Alias);
		Row.AnchorChoices = NCHUDEdit::BuildAnchorChoices();

		// hp_armor: style picker + font picker + color overrides.
		if (Alias == TEXT("hp_armor"))
		{
			Row.bHasStylePicker = true;
			Row.bHasFontPicker  = true;
			Row.FontChoices     = NCPlusHUDFonts::GetChoices();
			Row.StyleChoices    = NCPlusHPArmorStyle::GetChoices();
			Row.Colors.Add({ TEXT("color_health_number"), FText::FromString(TEXT("HP #")),       FLinearColor::White                    });
			Row.Colors.Add({ TEXT("color_armor_number"),  FText::FromString(TEXT("AR #")),       FLinearColor::White                    });
			Row.Colors.Add({ TEXT("color_health"),        FText::FromString(TEXT("HP Accent")),  FLinearColor(0.37f, 0.96f, 0.48f, 1.f) });
			Row.Colors.Add({ TEXT("color_armor"),         FText::FromString(TEXT("AR Accent")),  FLinearColor(0.95f, 0.83f, 0.34f, 1.f) });
			Row.Colors.Add({ TEXT("color_warning_hp"),    FText::FromString(TEXT("Warn HP")),    FLinearColor(1.f,   0.78f, 0.20f, 1.f) });
			Row.Colors.Add({ TEXT("color_low_hp"),        FText::FromString(TEXT("Low HP")),     FLinearColor(1.f,   0.32f, 0.28f, 1.f) });
			Row.Colors.Add({ TEXT("color_damage_flash"),  FText::FromString(TEXT("Damage")),     FLinearColor(1.f,   0.45f, 0.30f, 1.f) });
		}
		// ammo: style picker + font picker + color overrides (3 styles - see NCPlusHUDWidget_AmmoCounter).
		if (Alias == TEXT("ammo"))
		{
			Row.bHasStylePicker = true;
			Row.bHasFontPicker  = true;
			Row.FontChoices     = NCPlusHUDFonts::GetChoices();
			Row.StyleChoices    = NCPlusAmmoStyle::GetChoices();
			Row.Colors.Add({ TEXT("color_number"), FText::FromString(TEXT("Number")),  FLinearColor::White                    });
			Row.Colors.Add({ TEXT("color_max"),    FText::FromString(TEXT("/Max")),    FLinearColor(0.65f, 0.65f, 0.65f, 1.f) });
			Row.Colors.Add({ TEXT("color_full"),   FText::FromString(TEXT("Full")),    FLinearColor(0.40f, 0.95f, 0.48f, 1.f) });
			Row.Colors.Add({ TEXT("color_warn"),   FText::FromString(TEXT("Warn")),    FLinearColor(1.0f,  0.85f, 0.30f, 1.f) });
			Row.Colors.Add({ TEXT("color_danger"), FText::FromString(TEXT("Low")),     FLinearColor(1.0f,  0.32f, 0.28f, 1.f) });
			Row.Colors.Add({ TEXT("color_bg"),     FText::FromString(TEXT("Plate BG")), FLinearColor(0.04f, 0.04f, 0.04f, 0.45f) });
		}
		// Portraits + scorebar opt-in to the use_team_color checkbox.
		if (Alias == TEXT("portrait_red") || Alias == TEXT("portrait_blue") || Alias == TEXT("scorebar"))
		{
			Row.bHasTeamColorToggle = true;
		}
		// Portrait pip text knobs: a Font dropdown (HP/Armor + respawn timer text)
		// and a FontSz slider so 4K users can right-size the numbers without
		// touching pip dimensions. DrawPlayerIcon (Wipeout + Elim) reads both
		// from the same alias so each team's strip is independent.
		if (Alias == TEXT("portrait_red") || Alias == TEXT("portrait_blue"))
		{
			Row.bHasFontPicker = true;
			Row.bHasFontScale  = true;
			Row.FontChoices    = NCPlusHUDFonts::GetChoices();
		}
		// Damage flash: opt-in opacity multiplier on the existing Op slider PLUS a
		// flash_duration float + a color_text picker. Default OFF (no layout entry
		// => no draw); enable by adding the entry and unchecking Hide.
		if (Alias == TEXT("damage_flash"))
		{
			Row.Colors.Add({ TEXT("color_text"), FText::FromString(TEXT("Tint")), FLinearColor(1.f, 0.f, 0.f, 1.f) });
		}
		// Server name plate: text element, so a font dropdown + size slider + color.
		// Default OFF; same convention as the CTF Grab Flash row (Hide reads
		// CHECKED when no entry exists).
		if (Alias == TEXT("server_info"))
		{
			Row.bHasFontPicker = true;
			Row.bHasFontScale  = true;
			Row.FontChoices    = NCPlusHUDFonts::GetChoices();
			Row.Colors.Add({ TEXT("color_text"), FText::FromString(TEXT("Text")), FLinearColor(0.85f, 0.85f, 0.85f, 1.f) });
		}
		// Scorebar font picker - drives both team-name and score-number text
		// in WipeoutHUD/ElimPlusHUD/NCPlusCTFHUD/NCShaftArenaHUD scorebars
		// (single alias, single font for typographic coherence). Also exposes a
		// FontSz slider (font_scale extras) so the user can dial in apparent text
		// size at 4K without re-importing the UFont at a different LegacyFontSize.
		if (Alias == TEXT("scorebar"))
		{
			Row.bHasFontPicker = true;
			Row.bHasFontScale  = true;
			Row.FontChoices    = NCPlusHUDFonts::GetChoices();
		}
		// score_kda mini panel (top-right) — its draw site (ElimPlusHUD::DrawHUD)
		// already calls NCPlusHUDFonts::Resolve + ResolveScale; the editor was
		// just missing the UI surface. Add the picker + size slider here.
		if (Alias == TEXT("score_kda"))
		{
			Row.bHasFontPicker = true;
			Row.bHasFontScale  = true;
			Row.FontChoices    = NCPlusHUDFonts::GetChoices();
		}
		// Other text-rendering opt-in widgets get font selection too. The CTF
		// banners (you-have-flag / enemy-has-flag) are text, so they opt in here;
		// DrawOneBanner honors the choice via NCPlusHUDFonts::Resolve.
		if (Alias == TEXT("accuracy") || Alias == TEXT("heal_ability") || Alias == TEXT("speedometer")
			|| Alias == TEXT("ctf_you_have_flag") || Alias == TEXT("ctf_enemy_has_flag"))
		{
			Row.bHasFontPicker = true;
			Row.FontChoices    = NCPlusHUDFonts::GetChoices();
		}
		// WeaponBar (both sides): bg/outline/ammo color overrides.
		if (Alias == TEXT("weapon_bar_left") || Alias == TEXT("weapon_bar_right"))
		{
			Row.Colors.Add({ TEXT("color_slot_bg_inactive"), FText::FromString(TEXT("Slot BG")),     FLinearColor(0.04f, 0.04f, 0.04f, 0.30f) });
			Row.Colors.Add({ TEXT("color_slot_bg_active"),   FText::FromString(TEXT("Slot Active")), FLinearColor(0.10f, 0.10f, 0.10f, 0.55f) });
			Row.Colors.Add({ TEXT("color_outline"),          FText::FromString(TEXT("Outline")),     FLinearColor(0.95f, 0.83f, 0.34f, 1.f)   });
			Row.Colors.Add({ TEXT("color_ammo_full"),        FText::FromString(TEXT("Ammo Full")),   FLinearColor(0.4f,  0.95f, 0.48f, 1.f)   });
			Row.Colors.Add({ TEXT("color_ammo_warn"),        FText::FromString(TEXT("Ammo Warn")),   FLinearColor(1.0f,  0.85f, 0.30f, 1.f)   });
			Row.Colors.Add({ TEXT("color_ammo_danger"),      FText::FromString(TEXT("Ammo Low")),    FLinearColor(1.0f,  0.32f, 0.28f, 1.f)   });
		}
		// CTF banner aliases: single color_text override (default yellow / red).
		if (Alias == TEXT("ctf_you_have_flag"))
		{
			Row.Colors.Add({ TEXT("color_text"), FText::FromString(TEXT("Text")), FLinearColor::Yellow });
		}
		if (Alias == TEXT("ctf_enemy_has_flag"))
		{
			Row.Colors.Add({ TEXT("color_text"), FText::FromString(TEXT("Text")), FLinearColor(1.f, 0.18f, 0.18f, 1.f) });
		}
		Rows.Add(Row);
	}

	// Group rows by section (Phase 3.6 - SExpandableArea-based UI).
	TMap<FString, TArray<int32>> SectionRows;
	for (int32 i = 0; i < Rows.Num(); i++)
	{
		const FString Section = NCHUDEdit::GetSectionForAlias(Rows[i].Alias);
		SectionRows.FindOrAdd(Section).Add(i);
	}

	TSharedPtr<SVerticalBox> RowList;
	SAssignNew(RowList, SVerticalBox);

	for (const FString& Section : NCHUDEdit::GetSectionOrder())
	{
		const TArray<int32>* Indices = SectionRows.Find(Section);
		if (!Indices || Indices->Num() == 0) continue;

		TSharedPtr<SVerticalBox> SectionBody;
		SAssignNew(SectionBody, SVerticalBox);
		for (int32 Idx : *Indices)
		{
			FNCHUDEditorRow& Row = Rows[Idx];
			TSharedPtr<SVerticalBox> RowBox;
			SAssignNew(RowBox, SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight() [ BuildRow(Row) ];
			if (Row.bHasFontPicker || Row.bHasFontScale)
			{
				RowBox->AddSlot().AutoHeight().Padding(8.f, 1.f, 0.f, 0.f) [ BuildFontRow(Row) ];
			}
			if (Row.Colors.Num() > 0)
			{
				RowBox->AddSlot().AutoHeight().Padding(8.f, 1.f, 0.f, 0.f) [ BuildColorRow(Row) ];
			}
			SectionBody->AddSlot().AutoHeight().Padding(2.f) [ RowBox.ToSharedRef() ];
		}

		// Default-open: HP/Armor (the headline element) + Game Mode (so the
		// optional accuracy widget and any mode-specific draw calls are
		// discoverable without hunting through collapsed areas). Other
		// sections collapsed for a compact panel at lower resolutions.
		const bool bCollapsed = (Section != TEXT("HP / Armor")) && (Section != TEXT("Game Mode"));
		RowList->AddSlot().AutoHeight().Padding(0, 4)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(bCollapsed)
			.AreaTitle(FText::FromString(Section))
			.BodyContent()
			[
				SectionBody.ToSharedRef()
			]
		];
	}

	GatherWeaponsForPicker();

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(940.f)
			[
				SNew(SBorder)
				.BorderImage(&BackgroundBrush)
				.Padding(FMargin(14.f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8) [ BuildHeader() ]
					+ SVerticalBox::Slot().FillHeight(1.f).MaxHeight(540.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot() [ RowList.ToSharedRef() ]
						+ SScrollBox::Slot()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
							[
								SNew(SExpandableArea)
								.InitiallyCollapsed(true)
								.AreaTitle(FText::FromString(TEXT("Weapon Group Assignments")))
								.BodyContent()
								[ BuildWeaponPicker() ]
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0) [ BuildFooter() ]
				]
			]
		]
		// Preset gallery overlay - covers the entire editor when shown.
		// Toggled via OnPresetsClicked / ShowPresetGallery. Lives in the same
		// viewport widget tree (no new top-level Slate window), per
		// feedback_fse_picker_restore.md and feedback_no_addsubmenu_in_viewport_menus.md.
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
		[
			SAssignNew(PresetGalleryWrapper, SBox)
			.Visibility(EVisibility::Collapsed)
			[
				SAssignNew(PresetGallery, SNCPlusHUDPresetGallery)
				.OnCloseRequested(FSimpleDelegate::CreateSP(this, &SNCPlusHUDEditor::ShowPresetGallery, false))
				.OnApplyPreset(FNCApplyPresetDelegate::CreateSP(this, &SNCPlusHUDEditor::ApplyJsonReplacingLive))
			]
		]
	];
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildHeader()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			// ASCII hyphen instead of em-dash. This file has no UTF-8 BOM
			// and MSVC defaults to Windows-1252 here, so a raw em-dash in
			// the source gets read as three garbled bytes and shows up as
			// mojibake in Slate. Hyphen sidesteps the encoding question
			// entirely.
			.Text(FText::FromString(TEXT("NetcodePlus HUD Editor - ElimPlus")))
			.ColorAndOpacity(FLinearColor(0.95f, 0.95f, 0.95f, 1.f))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Edits apply live. Click Save to persist to disk.")))
			.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.f))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
		[
			SAssignNew(StatusText, STextBlock)
			.Text(FText::GetEmpty())
			.ColorAndOpacity(FLinearColor(0.4f, 0.95f, 0.48f, 1.f))
		]
		// Stock vs NCPlus bottom bar (weapon/ammo/health). Lives in the HUD editor (moved
		// out of the iCTF settings tab). Applies immediately: persists [NetcodePlus]
		// StockBottomBar + live-swaps the bottom-bar widget family on the running HUD.
		+ SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)
		[
			SNew(SCheckBox)
			.IsChecked(this, &SNCPlusHUDEditor::GetStockBottomBarState)
			.OnCheckStateChanged(this, &SNCPlusHUDEditor::OnStockBottomBarChanged)
			.Content()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Stock bottom bar (weapon / ammo / health)")))
				.ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f, 1.f))
			]
		];
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildRow(FNCHUDEditorRow& Row)
{
	const FName Alias = Row.Alias;
	using namespace NCHUDEdit;

	// Helper for the optional team-color checkbox.
	TSharedRef<SWidget> TeamColorSlot = SNullWidget::NullWidget;
	if (Row.bHasTeamColorToggle)
	{
		TeamColorSlot = SNew(SCheckBox)
			.IsChecked(this, &SNCPlusHUDEditor::GetTeamColorState, Alias)
			.OnCheckStateChanged(this, &SNCPlusHUDEditor::OnTeamColorChanged, Alias)
			.Content()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Team Color")))
				.ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f, 1.f))
			];
	}

	// Helper for the optional style picker - only shown if Row.bHasStylePicker.
	TSharedRef<SWidget> StyleSlot = SNullWidget::NullWidget;
	if (Row.bHasStylePicker && Row.StyleChoices.Num() > 0)
	{
		// Resolve currently-selected style from live layout's extras.
		const FNCPlusHUDElement* Cur = FNCPlusHUDLayout::GetLive().Find(Alias);
		const FString CurStr = Cur ? Cur->GetExtra(TEXT("style")) : FString();
		const int32 InitialIdx = FMath::Clamp(NCHUDEdit::ParseStyleIndex(Alias, CurStr), 0, Row.StyleChoices.Num() - 1);

		StyleSlot = SNew(SBox).WidthOverride(150.f)
		[
			SAssignNew(Row.StyleCombo, STextComboBox)
			.OptionsSource(&Row.StyleChoices)
			.InitiallySelectedItem(Row.StyleChoices[InitialIdx])
			.OnSelectionChanged(this, &SNCPlusHUDEditor::OnStyleSelected, Alias)
		];
	}

	return SNew(SHorizontalBox)
		// Display name
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(DisplayNameWidth)
			[
				SNew(STextBlock)
				.Text(Row.DisplayName)
				.ColorAndOpacity(FLinearColor::White)
			]
		]
		// Anchor combo - show the layout entry's anchor if one exists, otherwise
		// the alias's stock anchor (so the dropdown reflects what's actually on screen).
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(ComboWidth)
			[
				SAssignNew(Row.AnchorCombo, STextComboBox)
				.OptionsSource(&Row.AnchorChoices)
				.InitiallySelectedItem(Row.AnchorChoices[(int32)(
					FNCPlusHUDLayout::GetLive().Find(Alias)
						? FNCPlusHUDLayout::GetLive().Find(Alias)->Anchor
						: NCPlusHUDAliases::GetStockAnchor(Alias))])
				.OnSelectionChanged(this, &SNCPlusHUDEditor::OnAnchorSelected, Alias)
			]
		]
		// Optional style picker (hp_armor only)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[ StyleSlot ]
		// Offset X
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(NumericInputWidth)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SNCPlusHUDEditor::GetOffsetX, Alias)
				.OnValueChanged(this, &SNCPlusHUDEditor::OnOffsetXChanged, Alias)
				.OnValueCommitted(this, &SNCPlusHUDEditor::OnOffsetXCommitted, Alias)
				.AllowSpin(true)
				.MinSliderValue(-4000.f)
				.MaxSliderValue( 4000.f)
				.Delta(1.f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("X "))) ]
			]
		]
		// Offset Y
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(NumericInputWidth)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SNCPlusHUDEditor::GetOffsetY, Alias)
				.OnValueChanged(this, &SNCPlusHUDEditor::OnOffsetYChanged, Alias)
				.OnValueCommitted(this, &SNCPlusHUDEditor::OnOffsetYCommitted, Alias)
				.AllowSpin(true)
				.MinSliderValue(-2400.f)
				.MaxSliderValue( 2400.f)
				.Delta(1.f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("Y "))) ]
			]
		]
		// Scale (per-element size multiplier - 1.0 = stock, 0.5 = half, 2.0 = double).
		// For widget-backed elements, drives UUTHUDWidget::GetDrawScaleOverride()
		// which the engine reads in PreDraw to scale RenderSize uniformly.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(NumericInputWidth)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SNCPlusHUDEditor::GetScale, Alias)
				.OnValueChanged(this, &SNCPlusHUDEditor::OnScaleChanged, Alias)
				.OnValueCommitted(this, &SNCPlusHUDEditor::OnScaleCommitted, Alias)
				.AllowSpin(true)
				.MinSliderValue(0.5f)
				.MaxSliderValue(2.0f)
				.MinValue(0.25f)
				.MaxValue(4.0f)
				.Delta(0.05f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("Sc "))) ]
			]
		]
		// Opacity (per-element multiplier 0..1)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(NumericInputWidth)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SNCPlusHUDEditor::GetOpacity, Alias)
				.OnValueChanged(this, &SNCPlusHUDEditor::OnOpacityChanged, Alias)
				.OnValueCommitted(this, &SNCPlusHUDEditor::OnOpacityCommitted, Alias)
				.AllowSpin(true)
				.MinSliderValue(0.f)
				.MaxSliderValue(1.f)
				.MinValue(0.f)
				.MaxValue(1.f)
				.Delta(0.05f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("Op "))) ]
			]
		]
		// Hidden checkbox - explicit width so the label never truncates to "Hid".
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(60.f)
			[
				SNew(SCheckBox)
				.IsChecked(this, &SNCPlusHUDEditor::GetHiddenState, Alias)
				.OnCheckStateChanged(this, &SNCPlusHUDEditor::OnHiddenChanged, Alias)
				.Content()
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Hide")))
					.ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f, 1.f))
				]
			]
		]
		// "Use Team Color" checkbox - only populated for portrait/scorebar rows.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[ TeamColorSlot ]
		// Per-row reset
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Reset")))
			.OnClicked(this, &SNCPlusHUDEditor::OnResetRowClicked, Alias)
		];
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildFooter()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Save to Disk")))
			.OnClicked(this, &SNCPlusHUDEditor::OnSaveClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Reload from Disk")))
			.OnClicked(this, &SNCPlusHUDEditor::OnReloadClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Reset All")))
			.OnClicked(this, &SNCPlusHUDEditor::OnResetAllClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Presets...")))
			.ToolTipText(FText::FromString(TEXT("Browse curated layouts and save your own as custom presets. Apply replaces your current layout (with confirmation).")))
			.OnClicked(this, &SNCPlusHUDEditor::OnPresetsClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Copy to Clipboard")))
			.ToolTipText(FText::FromString(TEXT("Copy this layout to the system clipboard as JSON. Share it with anyone — they can paste it into their HUD editor.")))
			.OnClicked(this, &SNCPlusHUDEditor::OnCopyToClipboardClicked)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Paste from Clipboard")))
			.ToolTipText(FText::FromString(TEXT("Replace the current layout with JSON from the clipboard. Confirms before overwriting.")))
			.OnClicked(this, &SNCPlusHUDEditor::OnPasteFromClipboardClicked)
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)  // spacer
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Close")))
			.OnClicked(this, &SNCPlusHUDEditor::OnCloseClicked)
		];
}

// =============================================================================
// Element accessors - Find() returns const pointer; we need a mutable handle.
// =============================================================================

FNCPlusHUDElement& SNCPlusHUDEditor::GetOrCreateElement(FName Alias)
{
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	FNCPlusHUDElement* Existing = L.Elements.Find(Alias);
	if (Existing) return *Existing;

	// Seed new entries with the alias's stock anchor + offset so the first edit
	// (e.g. nudging X by 1 px) doesn't jump the widget from its constructor
	// position to the default-constructed Center+(0,0).
	FNCPlusHUDElement Seed;
	Seed.Anchor = NCPlusHUDAliases::GetStockAnchor(Alias);
	Seed.Offset = NCPlusHUDAliases::GetStockOffset(Alias);
	return L.Elements.Add(Alias, Seed);
}

void SNCPlusHUDEditor::MutateElement(FName Alias, TFunctionRef<void(FNCPlusHUDElement&)> Mutator)
{
	Mutator(GetOrCreateElement(Alias));
	FNCPlusHUDLayout::MarkLiveDirty();
}

// =============================================================================
// Callbacks
// =============================================================================

TOptional<float> SNCPlusHUDEditor::GetOffsetX(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->Offset.X : NCPlusHUDAliases::GetStockOffset(Alias).X;
}

TOptional<float> SNCPlusHUDEditor::GetOffsetY(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->Offset.Y : NCPlusHUDAliases::GetStockOffset(Alias).Y;
}

void SNCPlusHUDEditor::OnOffsetXChanged(float NewVal, FName Alias)
{
	MutateElement(Alias, [NewVal](FNCPlusHUDElement& E){ E.Offset.X = NewVal; });
}
void SNCPlusHUDEditor::OnOffsetYChanged(float NewVal, FName Alias)
{
	MutateElement(Alias, [NewVal](FNCPlusHUDElement& E){ E.Offset.Y = NewVal; });
}
void SNCPlusHUDEditor::OnOffsetXCommitted(float NewVal, ETextCommit::Type, FName Alias)
{
	MutateElement(Alias, [NewVal](FNCPlusHUDElement& E){ E.Offset.X = NewVal; });
}
void SNCPlusHUDEditor::OnOffsetYCommitted(float NewVal, ETextCommit::Type, FName Alias)
{
	MutateElement(Alias, [NewVal](FNCPlusHUDElement& E){ E.Offset.Y = NewVal; });
}

ECheckBoxState SNCPlusHUDEditor::GetHiddenState(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	// crosshair_flag_grab, damage_flash, server_info are default-OFF features:
	// with no layout entry the draw is suppressed, so the Hide box must read
	// CHECKED until the user opts in. Unchecking Hide writes a visible entry, at
	// which point the draw fires (the only state where it shows is entry-present
	// + not-hidden).
	if (Alias == TEXT("crosshair_flag_grab")
		|| Alias == TEXT("damage_flash")
		|| Alias == TEXT("server_info"))
	{
		return (!E || E->bHidden) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
	}
	return (E && E->bHidden) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SNCPlusHUDEditor::OnHiddenChanged(ECheckBoxState NewState, FName Alias)
{
	const bool bHide = (NewState == ECheckBoxState::Checked);
	MutateElement(Alias, [bHide](FNCPlusHUDElement& E){ E.bHidden = bHide; });
}

ECheckBoxState SNCPlusHUDEditor::GetTeamColorState(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	const bool bUse = E ? E->GetExtraBool(TEXT("use_team_color"), true) : true;
	return bUse ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SNCPlusHUDEditor::OnTeamColorChanged(ECheckBoxState NewState, FName Alias)
{
	const FString S = (NewState == ECheckBoxState::Checked) ? TEXT("true") : TEXT("false");
	MutateElement(Alias, [&S](FNCPlusHUDElement& E){ E.Extras.Add(TEXT("use_team_color"), S); });
}

TOptional<float> SNCPlusHUDEditor::GetOpacity(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->GetExtraFloat(TEXT("opacity"), 1.f) : 1.f;
}

void SNCPlusHUDEditor::OnOpacityChanged(float NewVal, FName Alias)
{
	const FString S = FString::Printf(TEXT("%.3f"), NewVal);
	MutateElement(Alias, [&S](FNCPlusHUDElement& E){ E.Extras.Add(TEXT("opacity"), S); });
}

void SNCPlusHUDEditor::OnOpacityCommitted(float NewVal, ETextCommit::Type, FName Alias)
{
	OnOpacityChanged(NewVal, Alias);
}

TOptional<float> SNCPlusHUDEditor::GetFontScale(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->GetExtraFloat(TEXT("font_scale"), 1.f) : 1.f;
}

void SNCPlusHUDEditor::OnFontScaleChanged(float NewVal, FName Alias)
{
	// Mirror Opacity's storage: write the float to Extras as a printed string so
	// the unified Extras<string,string> map stays homogeneous (read back via
	// FNCPlusHUDElement::GetExtraFloat). NCPlusHUDFonts::ResolveScale picks it up.
	const FString S = FString::Printf(TEXT("%.3f"), NewVal);
	MutateElement(Alias, [&S](FNCPlusHUDElement& E){ E.Extras.Add(TEXT("font_scale"), S); });
}

void SNCPlusHUDEditor::OnFontScaleCommitted(float NewVal, ETextCommit::Type, FName Alias)
{
	OnFontScaleChanged(NewVal, Alias);
}

TOptional<float> SNCPlusHUDEditor::GetScale(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->Scale : 1.f;
}

void SNCPlusHUDEditor::OnScaleChanged(float NewVal, FName Alias)
{
	const float Clamped = FMath::Clamp(NewVal, 0.25f, 4.f);
	MutateElement(Alias, [Clamped](FNCPlusHUDElement& E){ E.Scale = Clamped; });
}

void SNCPlusHUDEditor::OnScaleCommitted(float NewVal, ETextCommit::Type, FName Alias)
{
	OnScaleChanged(NewVal, Alias);
}

void SNCPlusHUDEditor::OnAnchorSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias)
{
	if (bSuppressComboCallbacks) return;  // skip during programmatic resync (Reset / Reload)
	if (!NewSel.IsValid()) return;
	const ENCPlusHUDAnchor NewAnchor = FNCPlusHUDLayout::ParseAnchor(*NewSel);
	MutateElement(Alias, [NewAnchor](FNCPlusHUDElement& E){ E.Anchor = NewAnchor; });
}

void SNCPlusHUDEditor::OnStyleSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias)
{
	if (bSuppressComboCallbacks) return;
	if (!NewSel.IsValid()) return;
	const FString NewStyle = *NewSel;
	MutateElement(Alias, [&NewStyle](FNCPlusHUDElement& E){
		E.Extras.Add(TEXT("style"), NewStyle);
	});
}

FReply SNCPlusHUDEditor::OnResetRowClicked(FName Alias)
{
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	if (L.Elements.Remove(Alias) > 0)
	{
		FNCPlusHUDLayout::MarkLiveDirty();
		SetStatus(FString::Printf(TEXT("Reset '%s'."), *Alias.ToString()));

		// Suppress callbacks while we resync combos - otherwise SetSelectedItem
		// fires OnAnchorSelected/OnStyleSelected → MutateElement → re-creates the
		// entry we just deleted.
		TGuardValue<bool> Guard(bSuppressComboCallbacks, true);
		for (FNCHUDEditorRow& Row : Rows)
		{
			if (Row.Alias != Alias) continue;
			if (Row.AnchorCombo.IsValid())
			{
				const int32 StockIdx = (int32)NCPlusHUDAliases::GetStockAnchor(Alias);
				Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[StockIdx]);
			}
			if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
			{
				// Index 0 in each enum's GetChoices() is the default style.
				Row.StyleCombo->SetSelectedItem(Row.StyleChoices[0]);
			}
			if (Row.bHasFontPicker && Row.FontCombo.IsValid() && Row.FontChoices.Num() > 0)
			{
				// Index 0 of FontChoices is "Default" (no override).
				Row.FontCombo->SetSelectedItem(Row.FontChoices[0]);
			}
			break;
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnSaveClicked()
{
	const bool bOk = FNCPlusHUDLayout::SaveLive();
	SetStatus(bOk ? TEXT("Saved.") : TEXT("Save failed - check log."));
	return FReply::Handled();
}

ECheckBoxState SNCPlusHUDEditor::GetStockBottomBarState() const
{
	return FNCPlusHUDLayout::WantsStockBottomBar() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SNCPlusHUDEditor::OnStockBottomBarChanged(ECheckBoxState NewState)
{
	const bool bStock = (NewState == ECheckBoxState::Checked);
	// Persist the explicit choice ([NetcodePlus] StockBottomBar). Written only on this toggle, so a
	// fresh install isn't silently locked to stock (see FNCPlusHUDLayout::WantsStockBottomBar).
	FNCPlusHUDLayout::SetStockBottomBar(bStock);
	// Live-swap the bottom-bar widget family on the running HUD so it applies this match.
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		NCPlusHUDDrawCall::RefreshBottomBarWidgets(PlayerOwner->PlayerController->MyHUD, bStock);
	}
	SetStatus(bStock ? TEXT("Bottom bar: Stock (applies now).") : TEXT("Bottom bar: NetcodePlus (applies now)."));
}

FReply SNCPlusHUDEditor::OnReloadClicked()
{
	FNCPlusHUDLayout::ReloadLive();
	SetStatus(TEXT("Reloaded from disk."));
	ResyncCombosToLive();
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnResetAllClicked()
{
	FNCPlusHUDLayout::ResetLive();
	SetStatus(TEXT("All overrides cleared."));
	TGuardValue<bool> Guard(bSuppressComboCallbacks, true);
	for (FNCHUDEditorRow& Row : Rows)
	{
		if (Row.AnchorCombo.IsValid())
		{
			const int32 StockIdx = (int32)NCPlusHUDAliases::GetStockAnchor(Row.Alias);
			Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[StockIdx]);
		}
		if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
		{
			// Index 0 is the default style for any registered enum.
			Row.StyleCombo->SetSelectedItem(Row.StyleChoices[0]);
		}
		if (Row.bHasFontPicker && Row.FontCombo.IsValid() && Row.FontChoices.Num() > 0)
		{
			Row.FontCombo->SetSelectedItem(Row.FontChoices[0]);  // "Default"
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnCopyToClipboardClicked()
{
	const FString Json = FNCPlusHUDLayout::GetLive().ToJsonString();
	FPlatformMisc::ClipboardCopy(*Json);
	const int32 NumElems = FNCPlusHUDLayout::GetLive().Elements.Num();
	SetStatus(FString::Printf(TEXT("Copied %d element(s) to clipboard."), NumElems));
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnPasteFromClipboardClicked()
{
	FString Pasted;
	FPlatformMisc::ClipboardPaste(Pasted);
	if (Pasted.IsEmpty())
	{
		SetStatus(TEXT("Clipboard is empty."));
		return FReply::Handled();
	}
	ApplyJsonReplacingLive(Pasted, TEXT("the clipboard layout"));
	return FReply::Handled();
}

bool SNCPlusHUDEditor::ApplyJsonReplacingLive(const FString& Json, const FString& Source)
{
	bool bOk = false;
	FNCPlusHUDLayout Parsed = FNCPlusHUDLayout::FromJsonString(Json, bOk);
	if (!bOk)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(FString::Printf(TEXT("%s does not contain valid HUD layout JSON."), *Source)));
		SetStatus(FString::Printf(TEXT("Apply failed: invalid JSON in %s."), *Source));
		return false;
	}

	const int32 NumIncoming = Parsed.Elements.Num();
	const int32 NumCurrent  = FNCPlusHUDLayout::GetLive().Elements.Num();
	const FString Prompt = FString::Printf(
		TEXT("Replace your current layout (%d element override(s)) with %s (%d element(s))?\n\n")
		TEXT("This won't write to disk until you click 'Save to Disk'."),
		NumCurrent, *Source, NumIncoming);

	if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Prompt)) != EAppReturnType::Yes)
	{
		SetStatus(FString::Printf(TEXT("Apply cancelled (%s)."), *Source));
		return false;
	}

	FNCPlusHUDLayout::GetLive() = MoveTemp(Parsed);
	FNCPlusHUDLayout::MarkLiveDirty();
	ResyncCombosToLive();
	SetStatus(FString::Printf(TEXT("Applied %d element(s) from %s."), NumIncoming, *Source));
	return true;
}

void SNCPlusHUDEditor::ResyncCombosToLive()
{
	TGuardValue<bool> Guard(bSuppressComboCallbacks, true);
	for (FNCHUDEditorRow& Row : Rows)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Row.Alias);
		if (Row.AnchorCombo.IsValid())
		{
			const int32 Idx = E ? (int32)E->Anchor : (int32)NCPlusHUDAliases::GetStockAnchor(Row.Alias);
			Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[Idx]);
		}
		if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
		{
			const FString CurStr = E ? E->GetExtra(TEXT("style")) : FString();
			const int32 Idx = FMath::Clamp(
				NCHUDEdit::ParseStyleIndex(Row.Alias, CurStr), 0, Row.StyleChoices.Num() - 1);
			Row.StyleCombo->SetSelectedItem(Row.StyleChoices[Idx]);
		}
		if (Row.bHasFontPicker && Row.FontCombo.IsValid() && Row.FontChoices.Num() > 0)
		{
			const FString CurStr = E ? E->GetExtra(TEXT("font")) : FString();
			int32 Idx = 0;  // Default
			for (int32 i = 0; i < Row.FontChoices.Num(); i++)
			{
				if (Row.FontChoices[i].IsValid()
					&& (*Row.FontChoices[i]).Equals(CurStr, ESearchCase::IgnoreCase))
				{
					Idx = i;
					break;
				}
			}
			Row.FontCombo->SetSelectedItem(Row.FontChoices[Idx]);
		}
	}
}

FReply SNCPlusHUDEditor::OnPresetsClicked()
{
	if (PresetGallery.IsValid())
	{
		PresetGallery->RefreshList();
	}
	ShowPresetGallery(true);
	return FReply::Handled();
}

void SNCPlusHUDEditor::ShowPresetGallery(bool bShow)
{
	if (PresetGalleryWrapper.IsValid())
	{
		PresetGalleryWrapper->SetVisibility(bShow ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (bShow && PresetGallery.IsValid())
	{
		// Hand keyboard focus to the gallery so its OnKeyDown sees ESC first.
		FSlateApplication::Get().SetKeyboardFocus(PresetGallery, EFocusCause::SetDirectly);
	}
	else if (!bShow)
	{
		// Restore focus to the editor so subsequent ESC closes the editor
		// (otherwise the now-collapsed gallery would still hold focus and
		// re-handle ESC into a no-op).
		FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::SetDirectly);
	}
}

// =============================================================================
// Color overrides sub-row
// =============================================================================

TSharedRef<SWidget> SNCPlusHUDEditor::BuildColorRow(FNCHUDEditorRow& Row)
{
	const FName Alias = Row.Alias;

	// Wrap colors into chunks of 4 per line so the panel doesn't overflow at 720p.
	const int32 ColorsPerLine = 4;
	TSharedPtr<SVerticalBox> VBox;
	SAssignNew(VBox, SVerticalBox);

	for (int32 LineStart = 0; LineStart < Row.Colors.Num(); LineStart += ColorsPerLine)
	{
		TSharedPtr<SHorizontalBox> LineBox;
		SAssignNew(LineBox, SHorizontalBox);

		// Label only on the first line; spacer on subsequent lines for alignment.
		LineBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(60.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(LineStart == 0 ? TEXT("Colors:") : TEXT("")))
				.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.55f, 1.f))
			]
		];

		const int32 LineEnd = FMath::Min(LineStart + ColorsPerLine, Row.Colors.Num());
		for (int32 i = LineStart; i < LineEnd; i++)
		{
			FNCHUDEditorColor& Color = Row.Colors[i];
			const FName Key      = Color.Key;
			const FLinearColor Default = Color.Default;

			LineBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,4,0)
			[
				SNew(STextBlock)
				.Text(Color.Label)
				.ColorAndOpacity(FLinearColor(0.75f, 0.75f, 0.75f, 1.f))
			];

			// Swatch button - clicking opens SColorPicker. The color block's
			// .Color attribute is bound to GetCurrentColor() so it updates live
			// (both as the picker drags and when other code paths mutate the layout).
			LineBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
			[
				SNew(SBox).WidthOverride(54.f).HeightOverride(20.f)
				[
					SNew(SButton)
					.ContentPadding(FMargin(2.f))
					.OnClicked(this, &SNCPlusHUDEditor::OnSwatchClicked, Alias, Key, Default)
					.ToolTipText(TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
						[this, Alias, Key, Default]() -> FText
						{
							const FLinearColor C = GetCurrentColor(Alias, Key, Default);
							return FText::FromString(NCPlusHUDColor::ToHexString(C, true));
						})))
					[
						SNew(SColorBlock)
						.Color(this, &SNCPlusHUDEditor::GetCurrentColor, Alias, Key, Default)
						.ShowBackgroundForAlpha(true)
						.IgnoreAlpha(false)
						.Size(FVector2D(46.f, 14.f))
					]
				]
			];
		}

		VBox->AddSlot().AutoHeight().Padding(0, 2, 0, 0) [ LineBox.ToSharedRef() ];
	}

	return VBox.ToSharedRef();
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildFontRow(FNCHUDEditorRow& Row)
{
	const FName Alias = Row.Alias;

	// Resolve current selection from layout extras → index in FontChoices.
	const FNCPlusHUDElement* Cur = FNCPlusHUDLayout::GetLive().Find(Alias);
	const FString CurStr = Cur ? Cur->GetExtra(TEXT("font")) : FString();
	int32 InitialIdx = 0;  // "Default"
	if (!CurStr.IsEmpty())
	{
		for (int32 i = 0; i < Row.FontChoices.Num(); i++)
		{
			if (Row.FontChoices[i].IsValid() && (*Row.FontChoices[i]).Equals(CurStr, ESearchCase::IgnoreCase))
			{
				InitialIdx = i;
				break;
			}
		}
	}

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(60.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Font:")))
				.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.55f, 1.f))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(180.f).Visibility(Row.bHasFontPicker ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SAssignNew(Row.FontCombo, STextComboBox)
				.OptionsSource(&Row.FontChoices)
				.InitiallySelectedItem(Row.FontChoices.IsValidIndex(InitialIdx) ? Row.FontChoices[InitialIdx] : nullptr)
				.OnSelectionChanged(this, &SNCPlusHUDEditor::OnFontSelected, Alias)
			]
		]
		// Font display-size multiplier on top of the widget's own Scale (Extras
		// key "font_scale"). 1.0 = whatever the draw-site picks naturally; bigger
		// makes the text bigger without resizing the rest of the widget. Gated on
		// bHasFontScale so it only renders for widgets whose draw site actually
		// calls NCPlusHUDFonts::ResolveScale (otherwise the slider would do
		// nothing and the row would look broken).
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8,0,0,0)
		[
			SNew(SBox).WidthOverride(110.f).Visibility(Row.bHasFontScale ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SNumericEntryBox<float>)
				.Value(this, &SNCPlusHUDEditor::GetFontScale, Alias)
				.OnValueChanged(this, &SNCPlusHUDEditor::OnFontScaleChanged, Alias)
				.OnValueCommitted(this, &SNCPlusHUDEditor::OnFontScaleCommitted, Alias)
				.AllowSpin(true)
				.MinSliderValue(0.5f)
				.MaxSliderValue(2.0f)
				.MinValue(0.25f)
				.MaxValue(4.0f)
				.Delta(0.05f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("FontSz "))) ]
			]
		];
}

void SNCPlusHUDEditor::OnFontSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias)
{
	if (bSuppressComboCallbacks) return;
	if (!NewSel.IsValid()) return;
	const FString Choice = *NewSel;

	// "Default" → remove the override so Resolve() returns the widget's fallback.
	// Anything else → store the display name; Resolve() looks it up in the table.
	if (Choice.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
	{
		MutateElement(Alias, [](FNCPlusHUDElement& E) {
			E.Extras.Remove(TEXT("font"));
		});
	}
	else
	{
		MutateElement(Alias, [&Choice](FNCPlusHUDElement& E) {
			E.Extras.Add(TEXT("font"), Choice);
		});
	}
}

FLinearColor SNCPlusHUDEditor::GetCurrentColor(FName Alias, FName ColorKey, FLinearColor DefaultColor) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->GetExtraColor(ColorKey, DefaultColor) : DefaultColor;
}

FReply SNCPlusHUDEditor::OnSwatchClicked(FName Alias, FName ColorKey, FLinearColor DefaultColor)
{
	const FLinearColor Initial = GetCurrentColor(Alias, ColorKey, DefaultColor);

	// Fullscreen-Exclusive workaround.
	//
	// SColorPicker opens as a new top-level Slate window. Under FSE, Windows
	// owns the swap chain exclusively - the moment the new window steals
	// focus, the OS minimizes the FSE window to release the GPU. The picker
	// then can't render (it expects to share the device), Slate retries,
	// focus bounces back, and the user gets an infinite minimize/restore
	// cycle. Borderless (WindowedFullscreen) is visually identical to FSE
	// on a single monitor, has no exclusive-mode coupling, and Slate
	// dialogs interleave correctly.
	//
	// Strategy: snapshot the current mode, swap to borderless if FSE was
	// active, and restore on picker close. ResolutionSettings apply with
	// bCheckForCommandLineOverrides=false so we don't trigger a respawn /
	// resolution change beyond the window-mode flip.
	EWindowMode::Type SavedMode = EWindowMode::Windowed;
	FIntPoint         SavedRes  = FIntPoint::ZeroValue;
	FVector2D         SavedWindowPos = FVector2D::ZeroVector;
	bool              bHaveWindowPos = false;

	if (UGameUserSettings* Settings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		SavedMode = Settings->GetFullscreenMode();
		SavedRes  = Settings->GetScreenResolution();

		// Capture main game window position BEFORE the borderless flip so we can
		// restore FSE on the same monitor. UE 4.15 has no PreferredMonitorIndex -
		// the engine binds FSE to whichever monitor the main window is currently
		// on. If the window drifts during the picker session, FSE re-attaches to
		// the wrong display.
		if (GEngine && GEngine->GameViewport)
		{
			TSharedPtr<SWindow> GameWindow = GEngine->GameViewport->GetWindow();
			if (GameWindow.IsValid())
			{
				SavedWindowPos = GameWindow->GetPositionInScreen();
				bHaveWindowPos = true;
			}
		}

		if (SavedMode == EWindowMode::Fullscreen)
		{
			Settings->SetFullscreenMode(EWindowMode::WindowedFullscreen);
			Settings->ApplyResolutionSettings(false);
		}
	}

	FColorPickerArgs PickerArgs;
	PickerArgs.bUseAlpha = true;
	PickerArgs.bIsModal = false;
	PickerArgs.bOnlyRefreshOnOk = false;          // live update - value commits as picker drags
	PickerArgs.bExpandAdvancedSection = false;
	PickerArgs.InitialColorOverride = Initial;
	PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateSP(
		this, &SNCPlusHUDEditor::OnColorPickerCommitted, Alias, ColorKey);
	PickerArgs.ParentWidget = SharedThis(this);

	// Restore FSE on picker close. SavedMode captured by value so the
	// lambda is independent of `this` lifetime - the editor panel could
	// outlive the picker, but mode-restore is a pure GEngine operation
	// that doesn't need editor state. Fires for both commit and cancel.
	// Restore lambda mirrors UT4's GUI Settings → Display → Fullscreen
	// dropdown path (SUTSystemSettingsDialog.cpp:1415-1425) — that's the
	// known-working sequence the user confirmed by manually clicking through
	// the GUI to fix the stuck-in-windowed-fullscreen state.
	//
	// Order matters:
	//   1. SetFullscreenMode FIRST — UT4's UUTGameUserSettings override calls
	//      LockSetForegroundWindow(LSFW_LOCK) on FSE, which is what tells
	//      Windows to honor the exclusive swap chain on the next apply.
	//   2. SetScreenResolution second — pure value set, no side effects.
	//   3. SaveConfig — writes the chosen mode + res to the ini AND
	//      flushes LastConfirmed* fields. Without this ApplyResolutionSettings
	//      sees no diff vs the in-memory state and silently no-ops (which is
	//      exactly what was happening before — the silent no-op was the bug).
	//   4. ApplyResolutionSettings(false) — final apply.
	//
	// Earlier attempts: SetScreenResolution + SetFullscreenMode + Apply
	// (wrong order, no SaveConfig) silently no-op'd. setres console exec
	// (with or without 'f') silently no-op'd too — different exec stack.
	// The GUI dropdown is the path that works on the user's machine, so we
	// mirror it exactly.
	PickerArgs.OnColorPickerWindowClosed = FOnWindowClosed::CreateLambda(
		[SavedMode, SavedRes, SavedWindowPos, bHaveWindowPos](const TSharedRef<SWindow>& /*ClosedWindow*/)
		{
			if (SavedMode != EWindowMode::Fullscreen) return;

			// Move the main window back to its original screen position FIRST so
			// the engine picks the correct monitor when it re-establishes FSE.
			if (bHaveWindowPos && GEngine && GEngine->GameViewport)
			{
				TSharedPtr<SWindow> GameWindow = GEngine->GameViewport->GetWindow();
				if (GameWindow.IsValid() && GameWindow->GetPositionInScreen() != SavedWindowPos)
				{
					GameWindow->MoveWindowTo(SavedWindowPos);
				}
			}

			UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
			UE_LOG(LogTemp, Log,
				TEXT("[NCPlusHUDEditor] FSE picker close: restoring mode=Fullscreen res=%dx%d settings=%s"),
				SavedRes.X, SavedRes.Y, S ? *S->GetClass()->GetName() : TEXT("null"));
			if (!S) return;

			// SetFullscreenMode is virtual; the actual instance is
			// UUTGameUserSettings, so the UT4 override (LockSetForegroundWindow)
			// runs via virtual dispatch even though we hold the base pointer.
			S->SetFullscreenMode(EWindowMode::Fullscreen);
			S->SetScreenResolution(SavedRes);
			S->SaveConfig();
			S->ApplyResolutionSettings(false);
		});

	OpenColorPicker(PickerArgs);
	return FReply::Handled();
}

void SNCPlusHUDEditor::OnColorPickerCommitted(FLinearColor NewColor, FName Alias, FName ColorKey)
{
	const FString Hex = NCPlusHUDColor::ToHexString(NewColor, true);
	MutateElement(Alias, [&](FNCPlusHUDElement& E) {
		E.Extras.Add(ColorKey, Hex);
	});
}

// =============================================================================
// Weapon group picker
// =============================================================================

void SNCPlusHUDEditor::GatherWeaponsForPicker()
{
	WeaponPicker.Empty();
	if (!PlayerOwner.IsValid() || !PlayerOwner->PlayerController) return;
	AUTCharacter* UTChar = Cast<AUTCharacter>(PlayerOwner->PlayerController->GetPawn());
	if (!UTChar) return;

	TSet<FName> Seen;
	for (TInventoryIterator<AUTWeapon> It(UTChar); It; ++It)
	{
		AUTWeapon* W = *It;
		if (!W || !W->GetClass()) continue;
		const FName Key(*W->GetClass()->GetName());
		if (Seen.Contains(Key)) continue;
		Seen.Add(Key);

		FNCHUDWeaponPickerEntry E;
		E.ClassKey = Key;
		// Pretty display name: strip common prefixes / suffixes.
		FString Name = W->GetClass()->GetName();
		Name.ReplaceInline(TEXT("BP_"), TEXT(""));
		Name.ReplaceInline(TEXT("_C"), TEXT(""));
		Name.ReplaceInline(TEXT("UTWeap_"), TEXT(""));
		Name.ReplaceInline(TEXT("UTPlus"), TEXT(""));
		Name.ReplaceInline(TEXT("_Plus"), TEXT(""));
		Name.ReplaceInline(TEXT("_"), TEXT(" "));
		E.DisplayName = Name;
		WeaponPicker.Add(E);
	}

	// Sort alphabetically for predictable order.
	WeaponPicker.Sort([](const FNCHUDWeaponPickerEntry& A, const FNCHUDWeaponPickerEntry& B) {
		return A.DisplayName < B.DisplayName;
	});
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildWeaponPicker()
{
	// Header text now lives in the SExpandableArea title; just include the
	// hint blurb here for context.
	TSharedPtr<SVerticalBox> Box;
	SAssignNew(Box, SVerticalBox)
	+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Pick which side of the WeaponBar each weapon shows on. Unassigned = default (hitscan → Left, others → Right).")))
		.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.f))
		.AutoWrapText(true)
	];

	if (WeaponPicker.Num() == 0)
	{
		Box->AddSlot().AutoHeight().Padding(0,4,0,0)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("(Open this panel during a match to discover your inventory's weapons.)")))
			.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f, 1.f))
		];
	}
	else
	{
		for (const FNCHUDWeaponPickerEntry& W : WeaponPicker)
		{
			Box->AddSlot().AutoHeight().Padding(0,2,0,0) [ BuildWeaponPickerRow(W) ];
		}
	}

	return Box.ToSharedRef();
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildWeaponPickerRow(const FNCHUDWeaponPickerEntry& W)
{
	const FName Key = W.ClassKey;
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(220.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(W.DisplayName))
				.ColorAndOpacity(FLinearColor::White)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SCheckBox)
			.Style(FCoreStyle::Get(), "RadioButton")
			.IsChecked(this, &SNCPlusHUDEditor::GetWeaponSideState, Key, FName(TEXT("left")))
			.OnCheckStateChanged(this, &SNCPlusHUDEditor::OnWeaponSideCheckChanged, Key, FName(TEXT("left")))
			.Content() [ SNew(STextBlock).Text(FText::FromString(TEXT("Left"))) ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SCheckBox)
			.Style(FCoreStyle::Get(), "RadioButton")
			.IsChecked(this, &SNCPlusHUDEditor::GetWeaponSideState, Key, FName(TEXT("right")))
			.OnCheckStateChanged(this, &SNCPlusHUDEditor::OnWeaponSideCheckChanged, Key, FName(TEXT("right")))
			.Content() [ SNew(STextBlock).Text(FText::FromString(TEXT("Right"))) ]
		];
}

ECheckBoxState SNCPlusHUDEditor::GetWeaponSideState(FName ClassKey, FName Side) const
{
	const FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	const FName* Assigned = L.WeaponGroupAssignments.Find(ClassKey);
	const FName Effective = Assigned ? *Assigned :
		FNCPlusHUDLayout::GetDefaultWeaponSide(FindObject<UClass>(ANY_PACKAGE, *ClassKey.ToString()));
	return (Effective == Side) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SNCPlusHUDEditor::OnWeaponSideCheckChanged(ECheckBoxState State, FName ClassKey, FName Side)
{
	if (State != ECheckBoxState::Checked) return;  // radio behavior - only react to "checked"
	OnWeaponSideChanged(ClassKey, Side);
}

void SNCPlusHUDEditor::OnWeaponSideChanged(FName ClassKey, FName NewSide)
{
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	L.WeaponGroupAssignments.Add(ClassKey, NewSide);
	FNCPlusHUDLayout::MarkLiveDirty();
}

void SNCPlusHUDEditor::SetStatus(const FString& Msg)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(Msg));
	}
}

void SNCPlusHUDEditor::ClosePanel()
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

	if (UGameViewportClient* VC = (PlayerOwner.IsValid() && PlayerOwner->GetWorld())
		? PlayerOwner->GetWorld()->GetGameViewport() : nullptr)
	{
		VC->RemoveViewportWidgetContent(SharedThis(this));
	}
}

FReply SNCPlusHUDEditor::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		ClosePanel();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}
