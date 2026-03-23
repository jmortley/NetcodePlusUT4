// SUTWeaponSkinSelector.cpp — Slate weapon skin picker implementation
#include "SUTWeaponSkinSelector.h"
#include "UTLocalPlayer.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTWeaponSkin.h"
#include "UTProfileSettings.h"
#include "AssetRegistryModule.h"

#define LOCTEXT_NAMESPACE "WeaponSkins"

void SUTWeaponSkinSelector::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;
	CurrentWeaponIndex = 0;
	CurrentSkinIndex = 0;

	// Semi-transparent dark background
	BackgroundBrush.TintColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.85f);

	GatherWeaponSkins();

	ChildSlot
	[
		// Full-screen overlay with centered panel
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// Dim background
			SNew(SBorder)
			.BorderImage(&BackgroundBrush)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				// Main panel — fixed size
				SNew(SBox)
				.WidthOverride(700.0f)
				.HeightOverride(500.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
					.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.95f))
					.Padding(FMargin(12.0f))
					[
						SNew(SVerticalBox)

						// Title
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 8)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Title", "Weapon Skins"))
							.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 20))
							.ColorAndOpacity(FLinearColor::White)
						]

						// Content: Weapon list | Skin list
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SHorizontalBox)

							// Left: Weapon list
							+ SHorizontalBox::Slot()
							.FillWidth(0.35f)
							.Padding(0, 0, 6, 0)
							[
								SNew(SBorder)
								.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
								.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.05f, 1.0f))
								.Padding(4.0f)
								[
									SNew(SScrollBox)
									+ SScrollBox::Slot()
									[
										SAssignNew(WeaponListContainer, SVerticalBox)
									]
								]
							]

							// Right: Skin list for selected weapon
							+ SHorizontalBox::Slot()
							.FillWidth(0.65f)
							[
								SNew(SBorder)
								.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
								.BorderBackgroundColor(FLinearColor(0.05f, 0.05f, 0.05f, 1.0f))
								.Padding(4.0f)
								[
									SNew(SScrollBox)
									+ SScrollBox::Slot()
									[
										SAssignNew(SkinListContainer, SVerticalBox)
									]
								]
							]
						]

						// Status text
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 6, 0, 4)
						[
							SAssignNew(PreviewText, STextBlock)
							.Text(LOCTEXT("SelectSkin", "Select a weapon, then choose a skin."))
							.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
							.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
						]

						// Buttons: Apply | Close
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Right)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0, 0, 8, 0)
							[
								SNew(SButton)
								.OnClicked(this, &SUTWeaponSkinSelector::OnApplyClicked)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("Apply", "Apply"))
									.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 14))
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.OnClicked(this, &SUTWeaponSkinSelector::OnCloseClicked)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("Close", "Close"))
									.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 14))
								]
							]
						]
					]
				]
			]
		]
	];

	RebuildWeaponList();
	RebuildSkinList();
}

/** Static cache — persists across open/close so assets only load once per session */
static TMap<FName, TArray<UUTWeaponSkin*>> CachedSkinsByWeapon;
static TArray<FName> CachedWeaponTags;
static TMap<FName, FString> CachedWeaponDisplayNames;
static bool bSkinsCached = false;

void SUTWeaponSkinSelector::GatherWeaponSkins()
{
	// Use cached data if available — no loading freeze on subsequent opens
	if (bSkinsCached)
	{
		SkinsByWeapon = CachedSkinsByWeapon;
		WeaponTags = CachedWeaponTags;
		WeaponDisplayNames = CachedWeaponDisplayNames;
		return;
	}

	SkinsByWeapon.Empty();
	WeaponTags.Empty();
	WeaponDisplayNames.Empty();

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssetsByClass(UUTWeaponSkin::StaticClass()->GetFName(), AssetList, true);

	for (const FAssetData& Asset : AssetList)
	{
		UUTWeaponSkin* Skin = Cast<UUTWeaponSkin>(Asset.GetAsset());
		if (!Skin) continue;

		FName Tag = Skin->WeaponSkinCustomizationTag;
		if (Tag == NAME_None) continue;

		TArray<UUTWeaponSkin*>& Skins = SkinsByWeapon.FindOrAdd(Tag);
		Skins.Add(Skin);

		if (!WeaponTags.Contains(Tag))
		{
			WeaponTags.Add(Tag);
			FString TagStr = Tag.ToString();
			TagStr.ReplaceInline(TEXT("_Skins"), TEXT(""));
			TagStr.ReplaceInline(TEXT("_"), TEXT(" "));
			WeaponDisplayNames.Add(Tag, TagStr);
		}
	}

	// Sort tags alphabetically
	WeaponTags.Sort([this](const FName& A, const FName& B)
	{
		return WeaponDisplayNames[A] < WeaponDisplayNames[B];
	});

	// Cache for future opens
	CachedSkinsByWeapon = SkinsByWeapon;
	CachedWeaponTags = WeaponTags;
	CachedWeaponDisplayNames = WeaponDisplayNames;
	bSkinsCached = true;
}

FString SUTWeaponSkinSelector::GetWeaponDisplayName(const FString& WeaponClassPath)
{
	// Extract class name from path like "/Script/UnrealTournament.UTWeap_RocketLauncher"
	FString Name = WeaponClassPath;
	int32 DotIndex;
	if (Name.FindLastChar('.', DotIndex))
	{
		Name = Name.Mid(DotIndex + 1);
	}
	Name.ReplaceInline(TEXT("UTWeap_"), TEXT(""));
	Name.ReplaceInline(TEXT("_"), TEXT(" "));
	return Name;
}

void SUTWeaponSkinSelector::RebuildWeaponList()
{
	if (!WeaponListContainer.IsValid()) return;
	WeaponListContainer->ClearChildren();

	for (int32 i = 0; i < WeaponTags.Num(); i++)
	{
		const bool bSelected = (i == CurrentWeaponIndex);
		FLinearColor BgColor = bSelected ? FLinearColor(0.15f, 0.4f, 0.7f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);
		FLinearColor TextColor = bSelected ? FLinearColor::White : FLinearColor(0.7f, 0.7f, 0.7f, 1.0f);

		WeaponListContainer->AddSlot()
		.AutoHeight()
		.Padding(1)
		[
			SNew(SButton)
			.ButtonColorAndOpacity(BgColor)
			.OnClicked(this, &SUTWeaponSkinSelector::OnWeaponClicked, i)
			[
				SNew(STextBlock)
				.Text(FText::FromString(WeaponDisplayNames[WeaponTags[i]]))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
				.ColorAndOpacity(TextColor)
				.Margin(FMargin(6.0f, 4.0f))
			]
		];
	}
}

void SUTWeaponSkinSelector::RebuildSkinList()
{
	if (!SkinListContainer.IsValid()) return;
	SkinListContainer->ClearChildren();

	if (!WeaponTags.IsValidIndex(CurrentWeaponIndex)) return;

	FName CurrentTag = WeaponTags[CurrentWeaponIndex];
	TArray<UUTWeaponSkin*>* Skins = SkinsByWeapon.Find(CurrentTag);
	if (!Skins) return;

	// Add "Default (No Skin)" option
	{
		const bool bSelected = (CurrentSkinIndex == 0);
		FLinearColor BgColor = bSelected ? FLinearColor(0.15f, 0.6f, 0.15f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);

		SkinListContainer->AddSlot()
		.AutoHeight()
		.Padding(1)
		[
			SNew(SButton)
			.ButtonColorAndOpacity(BgColor)
			.OnClicked(this, &SUTWeaponSkinSelector::OnSkinClicked, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DefaultSkin", "Default (No Skin)"))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
				.ColorAndOpacity(FLinearColor::White)
				.Margin(FMargin(6.0f, 4.0f))
			]
		];
	}

	// Add each skin
	for (int32 i = 0; i < Skins->Num(); i++)
	{
		UUTWeaponSkin* Skin = (*Skins)[i];
		if (!Skin) continue;

		const int32 SkinIdx = i + 1; // +1 because index 0 is "Default"
		const bool bSelected = (CurrentSkinIndex == SkinIdx);
		FLinearColor BgColor = bSelected ? FLinearColor(0.15f, 0.6f, 0.15f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);

		FString DisplayStr = Skin->DisplayName.ToString();
		if (DisplayStr.IsEmpty())
		{
			DisplayStr = Skin->GetName();
		}
		if (!Skin->ItemAuthor.IsEmpty())
		{
			DisplayStr += FString::Printf(TEXT("  (by %s)"), *Skin->ItemAuthor);
		}

		SkinListContainer->AddSlot()
		.AutoHeight()
		.Padding(1)
		[
			SNew(SButton)
			.ButtonColorAndOpacity(BgColor)
			.OnClicked(this, &SUTWeaponSkinSelector::OnSkinClicked, SkinIdx)
			[
				SNew(STextBlock)
				.Text(FText::FromString(DisplayStr))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
				.ColorAndOpacity(FLinearColor::White)
				.Margin(FMargin(6.0f, 4.0f))
			]
		];
	}
}

void SUTWeaponSkinSelector::ApplySelectedSkin()
{
	if (!PlayerOwner.IsValid()) return;

	AUTPlayerController* PC = Cast<AUTPlayerController>(PlayerOwner->PlayerController);
	if (!PC) return;

	AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
	if (!PS) return;

	if (!WeaponTags.IsValidIndex(CurrentWeaponIndex)) return;

	FName CurrentTag = WeaponTags[CurrentWeaponIndex];

	if (CurrentSkinIndex == 0)
	{
		// Default — clear skin for this weapon
		PS->ServerReceiveWeaponSkin(TEXT(""));

		// Also update profile
		UUTProfileSettings* ProfileSettings = PC->GetProfileSettings();
		if (ProfileSettings)
		{
			ProfileSettings->WeaponSkins.Remove(CurrentTag);
		}

		if (PreviewText.IsValid())
		{
			PreviewText->SetText(FText::FromString(FString::Printf(TEXT("Cleared skin for %s"), *WeaponDisplayNames[CurrentTag])));
		}
	}
	else
	{
		TArray<UUTWeaponSkin*>* Skins = SkinsByWeapon.Find(CurrentTag);
		if (!Skins || !Skins->IsValidIndex(CurrentSkinIndex - 1)) return;

		UUTWeaponSkin* SelectedSkin = (*Skins)[CurrentSkinIndex - 1];
		if (!SelectedSkin) return;

		FString SkinPath = SelectedSkin->GetPathName();
		PS->ServerReceiveWeaponSkin(SkinPath);

		// Also update profile so it persists
		UUTProfileSettings* ProfileSettings = PC->GetProfileSettings();
		if (ProfileSettings)
		{
			ProfileSettings->WeaponSkins.Add(CurrentTag, SkinPath);
		}

		FString SkinName = SelectedSkin->DisplayName.ToString();
		if (SkinName.IsEmpty()) SkinName = SelectedSkin->GetName();

		if (PreviewText.IsValid())
		{
			PreviewText->SetText(FText::FromString(FString::Printf(TEXT("Applied '%s' to %s"), *SkinName, *WeaponDisplayNames[CurrentTag])));
		}
	}
}

FReply SUTWeaponSkinSelector::OnWeaponClicked(int32 Index)
{
	CurrentWeaponIndex = Index;
	CurrentSkinIndex = 0;
	RebuildWeaponList();
	RebuildSkinList();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnSkinClicked(int32 Index)
{
	CurrentSkinIndex = Index;
	RebuildSkinList();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnApplyClicked()
{
	ApplySelectedSkin();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		ClosePanel();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SUTWeaponSkinSelector::ClosePanel()
{
	UWorld* World = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
			{
				World = Context.World();
				break;
			}
		}
	}

	if (World)
	{
		UGameViewportClient* ViewportClient = World->GetGameViewport();
		if (ViewportClient)
		{
			ViewportClient->RemoveViewportWidgetContent(AsShared());
		}
	}
}

#undef LOCTEXT_NAMESPACE
