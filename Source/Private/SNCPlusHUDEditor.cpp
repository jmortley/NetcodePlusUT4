// SNCPlusHUDEditor.cpp — implementation of the live HUD layout editor.
#include "SNCPlusHUDEditor.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace NCHUDEdit
{
	static const float RowHeightPx        = 28.f;
	static const float NumericInputWidth  = 80.f;
	static const float ComboWidth         = 130.f;
	static const float DisplayNameWidth   = 180.f;

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
		Rows.Add(Row);
	}

	TSharedPtr<SVerticalBox> RowList;
	SAssignNew(RowList, SVerticalBox);

	for (FNCHUDEditorRow& Row : Rows)
	{
		RowList->AddSlot()
			.AutoHeight()
			.Padding(2.f)
			[ BuildRow(Row) ];
	}

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(720.f)
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
		// Hidden checkbox
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)
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

void SNCPlusHUDEditor::OnAnchorSelected(TSharedPtr<FString> NewSel, ESelectInfo::Type, FName Alias)
{
	if (!NewSel.IsValid()) return;
	const ENCPlusHUDAnchor NewAnchor = FNCPlusHUDLayout::ParseAnchor(*NewSel);
	MutateElement(Alias, [NewAnchor](FNCPlusHUDElement& E){ E.Anchor = NewAnchor; });
}

FReply SNCPlusHUDEditor::OnResetRowClicked(FName Alias)
{
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	if (L.Elements.Remove(Alias) > 0)
	{
		FNCPlusHUDLayout::MarkLiveDirty();
		SetStatus(FString::Printf(TEXT("Reset '%s'."), *Alias.ToString()));

		// Re-sync the anchor combo to "Center" since element no longer exists.
		for (FNCHUDEditorRow& Row : Rows)
		{
			if (Row.Alias == Alias && Row.AnchorCombo.IsValid())
			{
				Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[(int32)ENCPlusHUDAnchor::Center]);
				break;
			}
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

	// Sync anchor combos to reloaded values.
	for (FNCHUDEditorRow& Row : Rows)
	{
		if (!Row.AnchorCombo.IsValid()) continue;
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Row.Alias);
		const int32 Idx = E ? (int32)E->Anchor : (int32)ENCPlusHUDAnchor::Center;
		Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[Idx]);
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnResetAllClicked()
{
	FNCPlusHUDLayout::ResetLive();
	SetStatus(TEXT("All overrides cleared."));
	for (FNCHUDEditorRow& Row : Rows)
	{
		if (Row.AnchorCombo.IsValid())
		{
			Row.AnchorCombo->SetSelectedItem(Row.AnchorChoices[(int32)ENCPlusHUDAnchor::Center]);
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDEditor::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
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
