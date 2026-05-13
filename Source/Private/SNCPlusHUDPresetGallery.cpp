// SNCPlusHUDPresetGallery.cpp - implementation.
#include "SNCPlusHUDPresetGallery.h"
#include "NCPlusHUDLayout.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Misc/MessageDialog.h"

namespace NCPlusHUDPresetGalleryImpl
{
	static const float ThumbW = 320.f;
	static const float ThumbH = 180.f;
	// 1080p reference height - drives the design-pixel -> thumbnail-pixel scale.
	static const float DesignH = 1080.f;
	static const float DesignW = 1920.f;
	// Use the smaller axis ratio so footprints don't overflow at extreme aspect.
	static const float UniformScale = ThumbH / DesignH;

	// Element footprint in 1080p design pixels + default tint when no
	// override is present in the preset's JSON.
	struct FFootprint { float W; float H; FLinearColor Tint; };

	static FFootprint GetFootprint(FName Alias)
	{
		// HP/Armor accent green is the editor's default for color_health.
		static const FLinearColor HPGreen     (0.37f, 0.86f, 0.48f, 1.f);
		static const FLinearColor ArmorYellow (0.95f, 0.83f, 0.34f, 1.f);
		static const FLinearColor RedTeam     (0.88f, 0.25f, 0.25f, 1.f);
		static const FLinearColor BlueTeam    (0.19f, 0.44f, 0.88f, 1.f);
		static const FLinearColor White       (1.f, 1.f, 1.f, 1.f);
		static const FLinearColor Gray        (0.55f, 0.55f, 0.55f, 1.f);
		static const FLinearColor Orange      (0.88f, 0.50f, 0.25f, 1.f);
		static const FLinearColor Purple      (0.78f, 0.50f, 0.92f, 1.f);

		if (Alias == TEXT("hp_armor"))         return { 240, 80,  HPGreen };
		if (Alias == TEXT("ammo"))             return { 130, 60,  White };
		if (Alias == TEXT("weapon_bar_left"))  return {  80, 360, ArmorYellow };
		if (Alias == TEXT("weapon_bar_right")) return {  80, 360, ArmorYellow };
		if (Alias == TEXT("portrait_red"))     return {  60, 60,  RedTeam };
		if (Alias == TEXT("portrait_blue"))    return {  60, 60,  BlueTeam };
		if (Alias == TEXT("scorebar"))         return { 320, 40,  Gray };
		if (Alias == TEXT("score_kda"))        return { 130, 40,  White };
		if (Alias == TEXT("killfeed"))         return { 300, 120, Orange };
		if (Alias == TEXT("announcements"))    return { 480, 40,  ArmorYellow };
		if (Alias == TEXT("accuracy"))         return {  80, 40,  HPGreen };
		if (Alias == TEXT("voice_status"))     return {  80, 24,  Gray };
		if (Alias == TEXT("powerups"))         return { 200, 40,  Purple };
		if (Alias == TEXT("weapon_crosshair")) return {  16, 16,  White };
		// Aliases without a footprint don't render in the thumbnail (e.g.
		// candy_marker, heal_ability - context-dependent draws). That's fine
		// for v1; presets vary mostly by the visualized aliases above.
		return { 0, 0, FLinearColor::Black };
	}

	// Pivot within the element's WxH box (0..1) for each anchor. Mirrors
	// HUD's actual ScreenPosition + Origin convention so the thumbnail
	// matches in-game placement direction.
	static FVector2D AnchorPivot(ENCPlusHUDAnchor A)
	{
		switch (A)
		{
			case ENCPlusHUDAnchor::TopLeft:      return FVector2D(0.f, 0.f);
			case ENCPlusHUDAnchor::TopCenter:    return FVector2D(0.5f, 0.f);
			case ENCPlusHUDAnchor::TopRight:     return FVector2D(1.f, 0.f);
			case ENCPlusHUDAnchor::CenterLeft:   return FVector2D(0.f, 0.5f);
			case ENCPlusHUDAnchor::Center:       return FVector2D(0.5f, 0.5f);
			case ENCPlusHUDAnchor::CenterRight:  return FVector2D(1.f, 0.5f);
			case ENCPlusHUDAnchor::BottomLeft:   return FVector2D(0.f, 1.f);
			case ENCPlusHUDAnchor::BottomCenter: return FVector2D(0.5f, 1.f);
			case ENCPlusHUDAnchor::BottomRight:  return FVector2D(1.f, 1.f);
		}
		return FVector2D(0.5f, 0.5f);
	}

	// Resolve a per-element override tint with sensible fallbacks. Most
	// presets distinguish themselves by hp_armor's accent color; honoring
	// it makes Streamer (bright green) vs QL (Q3 green) immediately readable
	// in the thumbnail.
	static FLinearColor ResolveTint(FName Alias, const FNCPlusHUDElement& E, const FLinearColor& Default)
	{
		if (Alias == TEXT("hp_armor"))
		{
			return E.GetExtraColor(TEXT("color_health"), Default);
		}
		if (Alias == TEXT("weapon_bar_left") || Alias == TEXT("weapon_bar_right"))
		{
			return E.GetExtraColor(TEXT("color_outline"), Default);
		}
		if (Alias == TEXT("ammo"))
		{
			return E.GetExtraColor(TEXT("color_number"), Default);
		}
		return Default;
	}
}

void SNCPlusHUDPresetGallery::Construct(const FArguments& InArgs)
{
	OnCloseRequested = InArgs._OnCloseRequested;
	OnApplyPreset    = InArgs._OnApplyPreset;

	BackgroundBrush.TintColor = FLinearColor(0.04f, 0.04f, 0.04f, 0.96f);
	CardBrush.TintColor       = FLinearColor(0.10f, 0.10f, 0.10f, 1.f);
	DialogBrush.TintColor     = FLinearColor(0.06f, 0.06f, 0.06f, 0.98f);
	ScreenBrush.TintColor     = FLinearColor(0.10f, 0.10f, 0.10f, 1.f);

	SAssignNew(CardListBox, SVerticalBox);

	ChildSlot
	[
		SNew(SOverlay)

		// --- Layer 1: dim the editor body underneath. ---
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
		[
			SNew(SColorBlock).Color(FLinearColor(0.f, 0.f, 0.f, 0.65f))
		]

		// --- Layer 2: gallery panel. ---
		+ SOverlay::Slot()
		.HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(900.f)
			[
				SNew(SBorder)
				.BorderImage(&BackgroundBrush)
				.Padding(FMargin(18.f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10) [ BuildHeader() ]
					+ SVerticalBox::Slot().FillHeight(1.f).MaxHeight(560.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot() [ CardListBox.ToSharedRef() ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0) [ BuildSaveCurrentRow() ]
				]
			]
		]

		// --- Layer 3: Save-Current sub-dialog (collapsed unless invoked). ---
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill).VAlign(VAlign_Fill)
		[
			SAssignNew(SaveDialogWrapper, SBox)
			.Visibility(EVisibility::Collapsed)
			[
				BuildSaveDialog()
			]
		]
	];

	RefreshList();
}

TSharedRef<SWidget> SNCPlusHUDPresetGallery::BuildHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("HUD Layout Presets")))
				.ColorAndOpacity(FLinearColor(0.95f, 0.95f, 0.95f, 1.f))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Pick a curated layout or your own saved preset. Apply replaces your current overrides (with confirmation).")))
				.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f, 1.f))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
			[
				SAssignNew(StatusText, STextBlock)
				.Text(FText::GetEmpty())
				.ColorAndOpacity(FLinearColor(0.4f, 0.95f, 0.48f, 1.f))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Close")))
			.OnClicked(this, &SNCPlusHUDPresetGallery::OnCloseClicked)
		];
}

void SNCPlusHUDPresetGallery::RefreshList()
{
	if (!CardListBox.IsValid()) return;
	CardListBox->ClearChildren();
	Presets = NCPlusHUDPresets::GetAll();

	// Curated section.
	bool bAnyCurated = false;
	for (const FNCPlusHUDPreset& P : Presets)
	{
		if (!P.bIsCustom) { bAnyCurated = true; break; }
	}
	if (bAnyCurated)
	{
		CardListBox->AddSlot().AutoHeight().Padding(0,4,0,4)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Curated")))
			.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.f))
		];
		for (const FNCPlusHUDPreset& P : Presets)
		{
			if (P.bIsCustom) continue;
			CardListBox->AddSlot().AutoHeight().Padding(0,4) [ BuildCard(P) ];
		}
	}

	// Custom section (only if any exist).
	bool bAnyCustom = false;
	for (const FNCPlusHUDPreset& P : Presets)
	{
		if (P.bIsCustom) { bAnyCustom = true; break; }
	}
	if (bAnyCustom)
	{
		CardListBox->AddSlot().AutoHeight().Padding(0,12,0,4)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Your Custom Presets")))
			.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.f))
		];
		for (const FNCPlusHUDPreset& P : Presets)
		{
			if (!P.bIsCustom) continue;
			CardListBox->AddSlot().AutoHeight().Padding(0,4) [ BuildCard(P) ];
		}
	}
}

TSharedRef<SWidget> SNCPlusHUDPresetGallery::BuildCard(const FNCPlusHUDPreset& P)
{
	const FString Json        = P.JsonString;     // capture by value for delegate
	const FString DisplayName = P.DisplayName;
	const FString Id          = P.Id;
	const bool    bCustom     = P.bIsCustom;

	TSharedRef<SHorizontalBox> ButtonRow = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Apply")))
			.OnClicked(this, &SNCPlusHUDPresetGallery::OnApplyClicked, Json, DisplayName)
		];
	if (bCustom)
	{
		ButtonRow->AddSlot().AutoWidth()
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Delete")))
			.OnClicked(this, &SNCPlusHUDPresetGallery::OnDeleteClicked, Id, DisplayName)
		];
	}

	return SNew(SBorder)
		.BorderImage(&CardBrush)
		.Padding(FMargin(10.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,14,0)
			[
				BuildThumbnail(P)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Top)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(P.DisplayName))
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(P.Description))
					.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f, 1.f))
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)
				[
					ButtonRow
				]
			]
		];
}

TSharedRef<SWidget> SNCPlusHUDPresetGallery::BuildThumbnail(const FNCPlusHUDPreset& P)
{
	using namespace NCPlusHUDPresetGalleryImpl;

	bool bOk = false;
	FNCPlusHUDLayout Parsed = FNCPlusHUDLayout::FromJsonString(P.JsonString, bOk);

	// SOverlay-based composition: each visible element is an HAlign_Left +
	// VAlign_Top slot whose Padding offsets it from the overlay's top-left.
	// SBox inside fixes the size. SOverlay is heavily used in UT4 Slate
	// code; SCanvas is not, so we avoid it for portability.
	TSharedPtr<SOverlay> Stack;
	SAssignNew(Stack, SOverlay);

	if (bOk)
	{
		for (const auto& Pair : Parsed.Elements)
		{
			const FName Alias = Pair.Key;
			const FNCPlusHUDElement& E = Pair.Value;
			if (E.bHidden) continue;

			const FFootprint Foot = GetFootprint(Alias);
			if (Foot.W <= 0.f || Foot.H <= 0.f) continue;

			const float Scale = FMath::Max(E.Scale, 0.01f);
			const float W = Foot.W * Scale * UniformScale;
			const float H = Foot.H * Scale * UniformScale;

			// Anchor -> thumbnail pivot pixel.
			const FVector2D Anchor = FNCPlusHUDLayout::AnchorToScreenCoords(E.Anchor);
			const float PivotPxX = Anchor.X * ThumbW + E.Offset.X * UniformScale;
			const float PivotPxY = Anchor.Y * ThumbH + E.Offset.Y * UniformScale;

			// Box top-left from pivot + element-internal pivot offset (matches
			// HUD's ScreenPosition+Origin convention so the thumbnail's
			// growth direction matches in-game).
			const FVector2D PivotInBox = AnchorPivot(E.Anchor);
			float TopLeftX = PivotPxX - W * PivotInBox.X;
			float TopLeftY = PivotPxY - H * PivotInBox.Y;

			// Translate-into-bounds (don't shrink - preserve apparent size).
			TopLeftX = FMath::Clamp(TopLeftX, 0.f, FMath::Max(0.f, ThumbW - W));
			TopLeftY = FMath::Clamp(TopLeftY, 0.f, FMath::Max(0.f, ThumbH - H));

			const FLinearColor Tint = ResolveTint(Alias, E, Foot.Tint);

			Stack->AddSlot()
				.HAlign(HAlign_Left).VAlign(VAlign_Top)
				.Padding(FMargin(TopLeftX, TopLeftY, 0.f, 0.f))
				[
					SNew(SBox)
					.WidthOverride(W).HeightOverride(H)
					[
						SNew(SColorBlock).Color(Tint)
					]
				];
		}
	}

	return SNew(SBox)
		.WidthOverride(ThumbW).HeightOverride(ThumbH)
		[
			SNew(SBorder)
			.BorderImage(&ScreenBrush)
			.Padding(FMargin(0))
			[
				Stack.ToSharedRef()
			]
		];
}

TSharedRef<SWidget> SNCPlusHUDPresetGallery::BuildSaveCurrentRow()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Save your current layout as a custom preset for later. Custom presets appear above with [Delete] buttons.")))
			.ColorAndOpacity(FLinearColor(0.55f, 0.55f, 0.55f, 1.f))
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Save Current as Preset...")))
			.OnClicked(this, &SNCPlusHUDPresetGallery::OnSaveCurrentClicked)
		];
}

TSharedRef<SWidget> SNCPlusHUDPresetGallery::BuildSaveDialog()
{
	return SNew(SOverlay)
		// Dim backdrop.
		+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
		[
			SNew(SColorBlock).Color(FLinearColor(0.f, 0.f, 0.f, 0.55f))
		]
		// Centered prompt.
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(440.f)
			[
				SNew(SBorder)
				.BorderImage(&DialogBrush)
				.Padding(FMargin(16.f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Save Current Layout as Preset")))
						.ColorAndOpacity(FLinearColor::White)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Name (used for the gallery title and the filename slug):")))
						.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
					[
						SAssignNew(SaveDialogNameBox, SEditableTextBox)
						.HintText(FText::FromString(TEXT("MySetup")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Description (1 line shown under the title):")))
						.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.f))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)
					[
						SAssignNew(SaveDialogDescBox, SEditableTextBox)
						.HintText(FText::FromString(TEXT("My competitive layout")))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.f)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Cancel")))
							.OnClicked(this, &SNCPlusHUDPresetGallery::OnSaveDialogCancelClicked)
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("Save")))
							.OnClicked(this, &SNCPlusHUDPresetGallery::OnSaveDialogConfirmClicked)
						]
					]
				]
			]
		];
}

// =============================================================================
// Click handlers
// =============================================================================

FReply SNCPlusHUDPresetGallery::OnApplyClicked(FString Json, FString DisplayName)
{
	const FString Source = FString::Printf(TEXT("the '%s' preset"), *DisplayName);
	if (OnApplyPreset.IsBound())
	{
		const bool bApplied = OnApplyPreset.Execute(Json, Source);
		if (bApplied && OnCloseRequested.IsBound())
		{
			OnCloseRequested.Execute();
		}
	}
	return FReply::Handled();
}

FReply SNCPlusHUDPresetGallery::OnDeleteClicked(FString Id, FString DisplayName)
{
	const FString Prompt = FString::Printf(
		TEXT("Delete the custom preset '%s'?\n\nThis removes the file from disk and cannot be undone."),
		*DisplayName);
	if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Prompt)) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	if (NCPlusHUDPresets::DeleteCustom(Id))
	{
		SetStatus(FString::Printf(TEXT("Deleted '%s'."), *DisplayName));
		RefreshList();
	}
	else
	{
		SetStatus(FString::Printf(TEXT("Failed to delete '%s'."), *DisplayName));
	}
	return FReply::Handled();
}

FReply SNCPlusHUDPresetGallery::OnSaveCurrentClicked()
{
	if (SaveDialogNameBox.IsValid()) SaveDialogNameBox->SetText(FText::GetEmpty());
	if (SaveDialogDescBox.IsValid()) SaveDialogDescBox->SetText(FText::GetEmpty());
	ShowSaveDialog(true);
	return FReply::Handled();
}

FReply SNCPlusHUDPresetGallery::OnSaveDialogConfirmClicked()
{
	const FString Name = SaveDialogNameBox.IsValid()
		? SaveDialogNameBox->GetText().ToString() : FString();
	const FString Desc = SaveDialogDescBox.IsValid()
		? SaveDialogDescBox->GetText().ToString() : FString();

	// UE 4.15 has no FString::TrimStartAndEnd. Copy + use the in-place
	// Trim() / TrimTrailing() pair instead. Both mutate the string in place.
	FString TrimmedName = Name;
	TrimmedName.Trim();
	TrimmedName.TrimTrailing();
	if (TrimmedName.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("Preset name can't be empty.")));
		return FReply::Handled();
	}

	FString OutId;
	if (NCPlusHUDPresets::SaveCustom(TrimmedName, Desc, OutId))
	{
		SetStatus(FString::Printf(TEXT("Saved '%s' as custom preset."), *TrimmedName));
		ShowSaveDialog(false);
		RefreshList();
	}
	else
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("Failed to save preset. Check the name (must contain at least one alphanumeric character).")));
	}
	return FReply::Handled();
}

FReply SNCPlusHUDPresetGallery::OnSaveDialogCancelClicked()
{
	ShowSaveDialog(false);
	return FReply::Handled();
}

FReply SNCPlusHUDPresetGallery::OnCloseClicked()
{
	if (OnCloseRequested.IsBound())
	{
		OnCloseRequested.Execute();
	}
	return FReply::Handled();
}

// =============================================================================
// Misc helpers
// =============================================================================

void SNCPlusHUDPresetGallery::SetStatus(const FString& Msg)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(Msg));
	}
}

void SNCPlusHUDPresetGallery::ShowSaveDialog(bool bShow)
{
	if (SaveDialogWrapper.IsValid())
	{
		SaveDialogWrapper->SetVisibility(bShow ? EVisibility::Visible : EVisibility::Collapsed);
	}
}

FReply SNCPlusHUDPresetGallery::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// If save dialog is open, ESC dismisses it first; second ESC closes
		// the gallery. Otherwise ESC closes the gallery directly.
		const bool bDialogOpen = SaveDialogWrapper.IsValid()
			&& SaveDialogWrapper->GetVisibility() == EVisibility::Visible;
		if (bDialogOpen)
		{
			ShowSaveDialog(false);
			return FReply::Handled();
		}
		if (OnCloseRequested.IsBound())
		{
			OnCloseRequested.Execute();
		}
		return FReply::Handled();
	}
	return FReply::Unhandled();
}
