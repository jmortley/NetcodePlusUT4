// SNCPlusHUDEditor.cpp — implementation of the live HUD layout editor.
#include "SNCPlusHUDEditor.h"
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
		if (Alias == TEXT("weapon_bar_left") || Alias == TEXT("weapon_bar_right"))
		{
			return TEXT("Weapon Bars");
		}
		if (Alias == TEXT("portrait_red") || Alias == TEXT("portrait_blue") || Alias == TEXT("scorebar"))
		{
			return TEXT("Top Bar (Portraits + Scorebar)");
		}
		return TEXT("Stock Widgets");
	}

	static const TArray<FString>& GetSectionOrder()
	{
		static const TArray<FString> Order = {
			TEXT("HP / Armor"),
			TEXT("Weapon Bars"),
			TEXT("Top Bar (Portraits + Scorebar)"),
			TEXT("Stock Widgets"),
		};
		return Order;
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

	BackgroundBrush.TintColor = FLinearColor(0.05f, 0.05f, 0.05f, 0.92f);

	// Build per-row state
	for (FName Alias : NCPlusHUDAliases::GetAllAliases())
	{
		FNCHUDEditorRow Row;
		Row.Alias         = Alias;
		Row.DisplayName   = NCPlusHUDAliases::GetDisplayName(Alias);
		Row.AnchorChoices = NCHUDEdit::BuildAnchorChoices();

		// hp_armor: style picker + color overrides.
		if (Alias == TEXT("hp_armor"))
		{
			Row.bHasStylePicker = true;
			Row.StyleChoices    = NCPlusHPArmorStyle::GetChoices();
			Row.Colors.Add({ TEXT("color_health_number"), FText::FromString(TEXT("HP #")),       FLinearColor::White,                    nullptr });
			Row.Colors.Add({ TEXT("color_armor_number"),  FText::FromString(TEXT("AR #")),       FLinearColor::White,                    nullptr });
			Row.Colors.Add({ TEXT("color_health"),        FText::FromString(TEXT("HP Accent")),  FLinearColor(0.37f, 0.96f, 0.48f, 1.f), nullptr });
			Row.Colors.Add({ TEXT("color_armor"),         FText::FromString(TEXT("AR Accent")),  FLinearColor(0.95f, 0.83f, 0.34f, 1.f), nullptr });
			Row.Colors.Add({ TEXT("color_low_hp"),        FText::FromString(TEXT("Low HP")),     FLinearColor(1.f,   0.32f, 0.28f, 1.f), nullptr });
			Row.Colors.Add({ TEXT("color_damage_flash"),  FText::FromString(TEXT("Damage")),     FLinearColor(1.f,   0.45f, 0.30f, 1.f), nullptr });
		}
		// Portraits + scorebar opt-in to the use_team_color checkbox.
		if (Alias == TEXT("portrait_red") || Alias == TEXT("portrait_blue") || Alias == TEXT("scorebar"))
		{
			Row.bHasTeamColorToggle = true;
		}
		// WeaponBar (both sides): bg/outline/ammo color overrides.
		if (Alias == TEXT("weapon_bar_left") || Alias == TEXT("weapon_bar_right"))
		{
			Row.Colors.Add({ TEXT("color_slot_bg_inactive"), FText::FromString(TEXT("Slot BG")),     FLinearColor(0.04f, 0.04f, 0.04f, 0.30f), nullptr });
			Row.Colors.Add({ TEXT("color_slot_bg_active"),   FText::FromString(TEXT("Slot Active")), FLinearColor(0.10f, 0.10f, 0.10f, 0.55f), nullptr });
			Row.Colors.Add({ TEXT("color_outline"),          FText::FromString(TEXT("Outline")),     FLinearColor(0.95f, 0.83f, 0.34f, 1.f),   nullptr });
			Row.Colors.Add({ TEXT("color_ammo_full"),        FText::FromString(TEXT("Ammo Full")),   FLinearColor(0.4f,  0.95f, 0.48f, 1.f),   nullptr });
			Row.Colors.Add({ TEXT("color_ammo_warn"),        FText::FromString(TEXT("Ammo Warn")),   FLinearColor(1.0f,  0.85f, 0.30f, 1.f),   nullptr });
			Row.Colors.Add({ TEXT("color_ammo_danger"),      FText::FromString(TEXT("Ammo Low")),    FLinearColor(1.0f,  0.32f, 0.28f, 1.f),   nullptr });
		}
		Rows.Add(Row);
	}

	// Group rows by section (Phase 3.6 — SExpandableArea-based UI).
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
			if (Row.Colors.Num() > 0)
			{
				RowBox->AddSlot().AutoHeight().Padding(8.f, 1.f, 0.f, 0.f) [ BuildColorRow(Row) ];
			}
			SectionBody->AddSlot().AutoHeight().Padding(2.f) [ RowBox.ToSharedRef() ];
		}

		// Default-open: HP/Armor (the headline element). Others collapsed
		// so the panel stays compact at lower resolutions.
		const bool bCollapsed = (Section != TEXT("HP / Armor"));
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
	];
}

TSharedRef<SWidget> SNCPlusHUDEditor::BuildHeader()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("NetcodePlus HUD Editor — ElimPlus")))
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

	// Helper for the optional style picker — only shown if Row.bHasStylePicker.
	TSharedRef<SWidget> StyleSlot = SNullWidget::NullWidget;
	if (Row.bHasStylePicker && Row.StyleChoices.Num() > 0)
	{
		// Resolve currently-selected style from live layout's extras.
		const FNCPlusHUDElement* Cur = FNCPlusHUDLayout::GetLive().Find(Alias);
		const FString CurStr = Cur ? Cur->GetExtra(TEXT("style")) : FString();
		const ENCPlusHPArmorStyle CurStyle = NCPlusHPArmorStyle::Parse(CurStr);
		const int32 InitialIdx = FMath::Clamp((int32)CurStyle, 0, Row.StyleChoices.Num() - 1);

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
		// Anchor combo
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
		[
			SNew(SBox).WidthOverride(ComboWidth)
			[
				SAssignNew(Row.AnchorCombo, STextComboBox)
				.OptionsSource(&Row.AnchorChoices)
				.InitiallySelectedItem(Row.AnchorChoices[(int32)
					(GetOrCreateElement(Alias).Anchor)])
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
				.MinSliderValue(-1920.f)
				.MaxSliderValue( 1920.f)
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
				.MinSliderValue(-1080.f)
				.MaxSliderValue( 1080.f)
				.Delta(1.f)
				.LabelPadding(FMargin(0))
				.Label() [ SNew(STextBlock).Text(FText::FromString(TEXT("Y "))) ]
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
		// Hidden checkbox — explicit width so the label never truncates to "Hid".
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
		// "Use Team Color" checkbox — only populated for portrait/scorebar rows.
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
		+ SHorizontalBox::Slot().FillWidth(1.f)  // spacer
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Close")))
			.OnClicked(this, &SNCPlusHUDEditor::OnCloseClicked)
		];
}

// =============================================================================
// Element accessors — Find() returns const pointer; we need a mutable handle.
// =============================================================================

FNCPlusHUDElement& SNCPlusHUDEditor::GetOrCreateElement(FName Alias)
{
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	FNCPlusHUDElement* Existing = L.Elements.Find(Alias);
	if (Existing) return *Existing;
	return L.Elements.Add(Alias, FNCPlusHUDElement());
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
	return E ? E->Offset.X : 0.f;
}

TOptional<float> SNCPlusHUDEditor::GetOffsetY(FName Alias) const
{
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
	return E ? E->Offset.Y : 0.f;
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

		// Suppress callbacks while we resync combos — otherwise SetSelectedItem
		// fires OnAnchorSelected/OnStyleSelected → MutateElement → re-creates the
		// entry we just deleted.
		TGuardValue<bool> Guard(bSuppressComboCallbacks, true);
		for (FNCHUDEditorRow& Row : Rows)
		{
			if (Row.Alias != Alias) continue;
			if (Row.AnchorCombo.IsValid())
			{
				Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[(int32)ENCPlusHUDAnchor::Center]);
			}
			if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
			{
				Row.StyleCombo->SetSelectedItem(Row.StyleChoices[(int32)ENCPlusHPArmorStyle::MinimalTypography]);
			}
			break;
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnSaveClicked()
{
	const bool bOk = FNCPlusHUDLayout::SaveLive();
	SetStatus(bOk ? TEXT("Saved.") : TEXT("Save failed — check log."));
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnReloadClicked()
{
	FNCPlusHUDLayout::ReloadLive();
	SetStatus(TEXT("Reloaded from disk."));

	// Sync anchor + style combos to reloaded values (suppress callbacks).
	TGuardValue<bool> Guard(bSuppressComboCallbacks, true);
	for (FNCHUDEditorRow& Row : Rows)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Row.Alias);
		if (Row.AnchorCombo.IsValid())
		{
			const int32 Idx = E ? (int32)E->Anchor : (int32)ENCPlusHUDAnchor::Center;
			Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[Idx]);
		}
		if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
		{
			const FString CurStr = E ? E->GetExtra(TEXT("style")) : FString();
			const int32 Idx = FMath::Clamp((int32)NCPlusHPArmorStyle::Parse(CurStr), 0, Row.StyleChoices.Num() - 1);
			Row.StyleCombo->SetSelectedItem(Row.StyleChoices[Idx]);
		}
	}
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
			Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[(int32)ENCPlusHUDAnchor::Center]);
		}
		if (Row.bHasStylePicker && Row.StyleCombo.IsValid() && Row.StyleChoices.Num() > 0)
		{
			Row.StyleCombo->SetSelectedItem(Row.StyleChoices[(int32)ENCPlusHPArmorStyle::MinimalTypography]);
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
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
			const FName Key = Color.Key;

			FString InitialHex;
			const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
			if (E)
			{
				const FString Existing = E->GetExtra(Key);
				if (!Existing.IsEmpty()) InitialHex = Existing;
			}
			if (InitialHex.IsEmpty())
			{
				InitialHex = NCPlusHUDColor::ToHexString(Color.Default, true);
			}

			LineBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,4,0)
			[
				SNew(STextBlock)
				.Text(Color.Label)
				.ColorAndOpacity(FLinearColor(0.75f, 0.75f, 0.75f, 1.f))
			];

			LineBox->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
			[
				SNew(SBox).WidthOverride(80.f)
				[
					SAssignNew(Color.Input, SEditableTextBox)
					.Text(FText::FromString(InitialHex))
					.OnTextCommitted(this, &SNCPlusHUDEditor::OnColorTextCommitted, Alias, Key)
				]
			];
		}

		VBox->AddSlot().AutoHeight().Padding(0, 2, 0, 0) [ LineBox.ToSharedRef() ];
	}

	return VBox.ToSharedRef();
}

void SNCPlusHUDEditor::OnColorTextCommitted(const FText& NewText, ETextCommit::Type, FName Alias, FName ColorKey)
{
	const FString Hex = NewText.ToString();

	// Validate before storing — if it doesn't parse, don't write a bogus value.
	FLinearColor Parsed;
	if (!NCPlusHUDColor::TryParse(Hex, Parsed))
	{
		SetStatus(FString::Printf(TEXT("Invalid color '%s' (use #RRGGBB or #RRGGBBAA)."), *Hex));
		return;
	}

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
	if (State != ECheckBoxState::Checked) return;  // radio behavior — only react to "checked"
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
