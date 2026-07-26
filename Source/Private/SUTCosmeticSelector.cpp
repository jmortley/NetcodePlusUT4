// SUTCosmeticSelector.cpp — Cosmetic selector for NetcodePlus
// Enumerates ALL cosmetics including entitlement-gated items.
#include "SUTCosmeticSelector.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTHat.h"
#include "UTHatLeader.h"
#include "UTEyewear.h"
#include "UTTaunt.h"
#include "UTGroupTaunt.h"
#include "UTCharacterContent.h"

FString SUTCosmeticSelector::GetSlotName(ECosmeticSlot Slot)
{
	switch (Slot)
	{
		case ECosmeticSlot::Hat:        return TEXT("Hat");
		case ECosmeticSlot::LeaderHat:  return TEXT("Leader Hat");
		case ECosmeticSlot::Eyewear:    return TEXT("Eyewear");
		case ECosmeticSlot::Taunt1:     return TEXT("Taunt 1");
		case ECosmeticSlot::Taunt2:     return TEXT("Taunt 2");
		case ECosmeticSlot::GroupTaunt: return TEXT("Group Taunt");
		case ECosmeticSlot::Character:  return TEXT("Character");
		default: return TEXT("Unknown");
	}
}

void SUTCosmeticSelector::GatherAllCosmetics()
{
	ItemsBySlot.Empty();

	// Map each slot to its base class
	struct FSlotClassPair
	{
		ECosmeticSlot Slot;
		UClass* BaseClass;
	};

	TArray<FSlotClassPair> Pairs;
	Pairs.Add({ ECosmeticSlot::Hat, AUTHat::StaticClass() });
	Pairs.Add({ ECosmeticSlot::LeaderHat, AUTHatLeader::StaticClass() });
	Pairs.Add({ ECosmeticSlot::Eyewear, AUTEyewear::StaticClass() });
	Pairs.Add({ ECosmeticSlot::Taunt1, AUTTaunt::StaticClass() });
	Pairs.Add({ ECosmeticSlot::Taunt2, AUTTaunt::StaticClass() });
	Pairs.Add({ ECosmeticSlot::GroupTaunt, AUTGroupTaunt::StaticClass() });
	Pairs.Add({ ECosmeticSlot::Character, AUTCharacterContent::StaticClass() });

	for (const FSlotClassPair& Pair : Pairs)
	{
		TArray<FAssetData> AssetList;
		GetAllBlueprintAssetData(Pair.BaseClass, AssetList, false); // false = no entitlement filtering

		TArray<FCosmeticItemInfo>& Items = ItemsBySlot.FindOrAdd(Pair.Slot);

		for (const FAssetData& Asset : AssetList)
		{
			// Skip abstract classes
			const FString* bIsAbstract = Asset.TagsAndValues.Find(FName(TEXT("BlueprintType")));
			if (bIsAbstract && *bIsAbstract == TEXT("true"))
			{
				// Check if it's actually abstract via the GeneratedClass tag
			}

			FCosmeticItemInfo Info;
			Info.AssetData = Asset;
			Info.ClassPath = Asset.ObjectPath.ToString() + TEXT("_C");

			// Extract display name from asset name (strip BP_ prefix, underscores to spaces)
			FString Name = Asset.AssetName.ToString();
			Name.RemoveFromStart(TEXT("BP_"));
			Name.ReplaceInline(TEXT("_"), TEXT(" "));
			Info.DisplayName = Name;

			Items.Add(Info);
		}

		// Sort alphabetically
		Items.Sort([](const FCosmeticItemInfo& A, const FCosmeticItemInfo& B)
		{
			return A.DisplayName < B.DisplayName;
		});
	}
}

void SUTCosmeticSelector::LoadSettings()
{
	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	for (uint8 SlotIdx = 0; SlotIdx < (uint8)ECosmeticSlot::MAX; SlotIdx++)
	{
		ECosmeticSlot Slot = (ECosmeticSlot)SlotIdx;
		FString Key = FString::Printf(TEXT("Cosmetic.%s"), *GetSlotName(Slot));
		FString SavedPath;
		if (GConfig->GetString(TEXT("NetcodePlus.Cosmetics"), *Key, SavedPath, ModIniPath) && !SavedPath.IsEmpty())
		{
			// Find matching item index
			TArray<FCosmeticItemInfo>* Items = ItemsBySlot.Find(Slot);
			if (Items)
			{
				for (int32 i = 0; i < Items->Num(); i++)
				{
					if ((*Items)[i].ClassPath == SavedPath)
					{
						SelectedIndex.Add(Slot, i);
						break;
					}
				}
			}
		}
	}
}

void SUTCosmeticSelector::ApplySlot(ECosmeticSlot Slot, const FString& ClassPath)
{
	if (!PlayerOwner.IsValid()) return;

	// Route through the UUTLocalPlayer setters instead of the raw ServerReceive*
	// RPCs. Each setter writes CurrentProfileSettings (so the choice shows in the
	// stock Player Settings menu and persists to the profile) AND sends the live
	// ServerReceive* RPC. The old path only fired the RPC, so selections never
	// reached the profile and never appeared in Player Settings.
	switch (Slot)
	{
		case ECosmeticSlot::Hat:        PlayerOwner->SetHatPath(ClassPath); break;
		case ECosmeticSlot::LeaderHat:  PlayerOwner->SetLeaderHatPath(ClassPath); break;
		case ECosmeticSlot::Eyewear:    PlayerOwner->SetEyewearPath(ClassPath); break;
		case ECosmeticSlot::Taunt1:     PlayerOwner->SetTauntPath(ClassPath); break;
		case ECosmeticSlot::Taunt2:     PlayerOwner->SetTaunt2Path(ClassPath); break;
		case ECosmeticSlot::GroupTaunt: PlayerOwner->SetGroupTauntPath(ClassPath); break;
		case ECosmeticSlot::Character:  PlayerOwner->SetCharacterPath(ClassPath); break;
		default: break;
	}
}

void SUTCosmeticSelector::SaveAndApply()
{
	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	for (uint8 SlotIdx = 0; SlotIdx < (uint8)ECosmeticSlot::MAX; SlotIdx++)
	{
		ECosmeticSlot Slot = (ECosmeticSlot)SlotIdx;
		FString Key = FString::Printf(TEXT("Cosmetic.%s"), *GetSlotName(Slot));

		int32* IdxPtr = SelectedIndex.Find(Slot);
		int32 Idx = IdxPtr ? *IdxPtr : -1;
		TArray<FCosmeticItemInfo>* Items = ItemsBySlot.Find(Slot);

		if (Items && Idx >= 0 && Idx < Items->Num())
		{
			const FString& ClassPath = (*Items)[Idx].ClassPath;
			GConfig->SetString(TEXT("NetcodePlus.Cosmetics"), *Key, *ClassPath, ModIniPath);
			ApplySlot(Slot, ClassPath);
		}
		else
		{
			GConfig->SetString(TEXT("NetcodePlus.Cosmetics"), *Key, TEXT(""), ModIniPath);
			ApplySlot(Slot, FString());
		}
	}

	GConfig->Flush(false, ModIniPath);

	// Persist the cosmetic choices to the UT profile (cloud + stock Player
	// Settings). ApplySlot wrote them into CurrentProfileSettings via the
	// UUTLocalPlayer setters; this flushes that profile to disk.
	if (PlayerOwner.IsValid())
	{
		PlayerOwner->SaveProfileSettings();
	}
}

void SUTCosmeticSelector::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;

	// Take mouse input: show the cursor and switch to GameAndUI so Slate gets
	// mouse events. NCPlusHUDDragMode holds the HUD's per-tick GetInputMode poll
	// open (it returns GameOnly during a match and would otherwise re-capture the
	// cursor). Released in ClosePanel. Mirrors SNCPlusHUDDragOverlay.
	NCPlusHUDDragMode::SetActive(true);
	bHeldDragMode = true;
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
	CurrentSlot = ECosmeticSlot::Hat;

	// Initialize selections to -1 (none)
	for (uint8 i = 0; i < (uint8)ECosmeticSlot::MAX; i++)
	{
		SelectedIndex.Add((ECosmeticSlot)i, -1);
	}

	GatherAllCosmetics();
	LoadSettings();

	// Dark background
	BackgroundBrush.TintColor = FLinearColor(0.02f, 0.02f, 0.02f, 0.92f);

	// Build UI
	TSharedPtr<SHorizontalBox> SlotTabs;

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(&BackgroundBrush)
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(800)
			.HeightOverride(550)
			[
				SNew(SVerticalBox)

				// Title
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(10, 8)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("NetcodePlus Cosmetic Selector")))
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 18))
					.ColorAndOpacity(FLinearColor::White)
				]

				// Slot tabs
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(10, 2)
				[
					SAssignNew(SlotTabs, SHorizontalBox)
				]

				// Item list
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(10, 4)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(ItemListContainer, SVerticalBox)
					]
				]

				// Status + buttons
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(10, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SAssignNew(StatusText, STextBlock)
						.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
						.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Apply & Save")))
						.OnClicked(this, &SUTCosmeticSelector::OnApplyClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0)
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Close")))
						.OnClicked(this, &SUTCosmeticSelector::OnCloseClicked)
					]
				]
			]
		]
	];

	// Build slot tabs. Dark button tint + explicit light text: the default light
	// SButton brush under pale grey/green text was the "hard to read" report —
	// same fix as the item rows below. Active tab reads amber (F5 menu accent),
	// via attribute lambdas so clicking re-colors without rebuilding the tabs.
	for (uint8 i = 0; i < (uint8)ECosmeticSlot::MAX; i++)
	{
		ECosmeticSlot Slot = (ECosmeticSlot)i;
		TArray<FCosmeticItemInfo>* Items = ItemsBySlot.Find(Slot);
		int32 Count = Items ? Items->Num() : 0;

		SlotTabs->AddSlot()
		.AutoWidth()
		.Padding(2, 0)
		[
			SNew(SButton)
			.OnClicked(FOnClicked::CreateSP(this, &SUTCosmeticSelector::OnSlotTabClicked, Slot))
			.ContentPadding(FMargin(12, 5))
			.ButtonColorAndOpacity_Lambda([this, Slot]() -> FSlateColor
			{
				return (CurrentSlot == Slot)
					? FLinearColor(0.85f, 0.55f, 0.10f, 1.f)    // active: amber
					: FLinearColor(0.10f, 0.10f, 0.10f, 1.f);   // inactive: dark plate
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), *GetSlotName(Slot), Count)))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 13))
				.ColorAndOpacity_Lambda([this, Slot]() -> FSlateColor
				{
					return (CurrentSlot == Slot)
						? FLinearColor::White
						: FLinearColor(0.85f, 0.85f, 0.85f, 1.f);
				})
			]
		];
	}

	RebuildItemList();
}

void SUTCosmeticSelector::RebuildItemList()
{
	if (!ItemListContainer.IsValid()) return;
	ItemListContainer->ClearChildren();

	TArray<FCosmeticItemInfo>* Items = ItemsBySlot.Find(CurrentSlot);
	if (!Items) return;

	int32* SelPtr = SelectedIndex.Find(CurrentSlot);
	int32 SelIdx = SelPtr ? *SelPtr : -1;

	// Row styling: the default light SButton brush under pale text was unreadable
	// (light-on-light). Dark plates + near-white text; the selected row gets a
	// dark-green plate + bright green bold text so it pops without relying on a
	// subtle hue shift. The list rebuilds on every click, so static colors are fine.
	const FLinearColor RowPlate     (0.10f, 0.10f, 0.10f, 1.f);
	const FLinearColor SelectedPlate(0.06f, 0.28f, 0.10f, 1.f);
	const FLinearColor RowText      (0.90f, 0.90f, 0.90f, 1.f);
	const FLinearColor SelectedText (0.45f, 1.00f, 0.45f, 1.f);
	const FString RegularFontPath = FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf");
	const FString BoldFontPath    = FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf");

	auto AddItemRow = [&](const FString& Label, int32 ItemIndex, bool bSelected)
	{
		ItemListContainer->AddSlot()
		.AutoHeight()
		.Padding(2, 2)
		[
			SNew(SButton)
			.OnClicked(FOnClicked::CreateSP(this, &SUTCosmeticSelector::OnItemClicked, ItemIndex))
			.ButtonColorAndOpacity(bSelected ? SelectedPlate : RowPlate)
			.ContentPadding(FMargin(8, 3))
			[
				SNew(STextBlock)
				.Text(FText::FromString(bSelected ? FString::Printf(TEXT("\x25B6 %s"), *Label) : Label))
				.Font(FSlateFontInfo(bSelected ? BoldFontPath : RegularFontPath, 14))
				.ColorAndOpacity(bSelected ? SelectedText : RowText)
			]
		];
	};

	// "None" option
	AddItemRow(TEXT("Default (None)"), -1, SelIdx < 0);

	for (int32 i = 0; i < Items->Num(); i++)
	{
		AddItemRow((*Items)[i].DisplayName, i, i == SelIdx);
	}
}

FReply SUTCosmeticSelector::OnSlotTabClicked(ECosmeticSlot Slot)
{
	CurrentSlot = Slot;
	RebuildItemList();
	return FReply::Handled();
}

FReply SUTCosmeticSelector::OnItemClicked(int32 Index)
{
	SelectedIndex.Add(CurrentSlot, Index);
	RebuildItemList();
	return FReply::Handled();
}

FReply SUTCosmeticSelector::OnApplyClicked()
{
	SaveAndApply();
	RebuildItemList();

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(TEXT("Cosmetics saved and applied.")));
	}
	return FReply::Handled();
}

FReply SUTCosmeticSelector::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
}

SUTCosmeticSelector::~SUTCosmeticSelector()
{
	// Map load drops the viewport widget without ClosePanel — release the refcount.
	if (bHeldDragMode) { NCPlusHUDDragMode::SetActive(false); bHeldDragMode = false; }
}

void SUTCosmeticSelector::ClosePanel()
{
	// Release the mouse capture taken in Construct (see NCPlusHUDDragMode).
	if (bHeldDragMode) { NCPlusHUDDragMode::SetActive(false); bHeldDragMode = false; }
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		APlayerController* MenuPC = PlayerOwner->PlayerController;
		MenuPC->bShowMouseCursor = false;
		FInputModeGameOnly InputMode;
		MenuPC->SetInputMode(InputMode);
	}

	if (PlayerOwner.IsValid() && PlayerOwner->ViewportClient)
	{
		PlayerOwner->ViewportClient->RemoveViewportWidgetContent(AsShared());
	}
}

FReply SUTCosmeticSelector::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		ClosePanel();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}
