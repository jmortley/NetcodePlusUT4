// SUTWeaponSkinSelector.cpp — NetcodePlus weapon settings implementation
#include "SUTWeaponSkinSelector.h"
#include "NCPlusHUDLayout.h"
#include "UTLocalPlayer.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTWeaponFix.h"
#include "UTWeaponSkin.h"
#include "TeamArenaCharacter.h"
#include "UTProfileSettings.h"
#include "UTGameplayStatics.h"
#include "UTGameMode.h"
#include "UTGameState.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Input/SNumericEntryBox.h"  // SNumericEntryBox<float> (was leaking transitively via the unity build)
#include "GameFramework/GameUserSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"

#define LOCTEXT_NAMESPACE "WeaponSkins"

/** Main-menu fallback populated from the last in-game inventory. */
static TArray<FNetcodePlusWeaponInfo> CachedWeapons;

void SUTWeaponSkinSelector::Construct(const FArguments& InArgs)
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
	CurrentWeaponIndex = 0;
	// Defaults match AUTWeaponFix statics so the spinner shows real values even
	// before LoadSettings runs. LoadSettings overwrites with the values that
	// Mod.ini already populated into the statics.
	HiddenBeamBack = AUTWeaponFix::HiddenBeamBackOffset;
	HiddenBeamDown = AUTWeaponFix::HiddenBeamDownOffset;
	// Sensible defaults — LoadSettings overwrites from Mod.ini if present.
	// Hitscan: warm orange (matches stock sniper feel). Shock: cyan.
	HitscanBeamColor = FLinearColor(1.f, 0.55f, 0.f, 1.f);
	ShockBeamColor   = FLinearColor(0.2f, 0.85f, 1.f, 1.f);

	BackgroundBrush.TintColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.85f);

	GatherWeapons();
	GatherSkins();
	LoadSettings();

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SBorder)
			.BorderImage(&BackgroundBrush)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(750.0f)
				.HeightOverride(520.0f)
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
							.Text(LOCTEXT("Title", "NetcodePlus Weapon Settings"))
							.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 18))
							.ColorAndOpacity(FLinearColor::White)
						]

						// Hitscan choice toggle
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 8)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0, 0, 8, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HitscanLabel", "Hitscan Weapon:"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
								.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.OnClicked(this, &SUTWeaponSkinSelector::OnHitscanToggleClicked)
								[
									SAssignNew(HitscanValueText, STextBlock)
									.Text(FText::FromString(CurrentHitscanChoice == TEXT("LG") ? TEXT("Lightning Gun") : TEXT("Sniper Rifle")))
									.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 13))
									.ColorAndOpacity(FLinearColor(0.3f, 0.9f, 0.3f, 1.0f))
								]
							]
						]

						// Hidden-weapon style toggle — unchecked (default) = BP-parity
						// visibility-only hide, beam from the live muzzle socket;
						// checked = classic camera-relative beam (pre-2026-07-19),
						// which is what the Back/Down spinners below feed.
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 8)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
							[
								SNew(SCheckBox)
								.IsChecked(this, &SUTWeaponSkinSelector::GetClassicHideState)
								.OnCheckStateChanged(this, &SUTWeaponSkinSelector::OnClassicHideChanged)
								.ToolTipText(LOCTEXT("ClassicHideTooltip", "Hidden weapons: fire the beam from the camera (classic style) instead of the gun's muzzle. Uses the Back/Down offsets below."))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ClassicHideLabel", "Classic hidden-weapon beam (from camera)"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
								.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
							]
						]

						// Camera-beam origin offsets — read by AUTWeaponFix::
						// GetImpactSpawnPosition ONLY in the classic style above;
						// inert (but still persisted) in the default BP-parity hide.
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 8)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HiddenBeamLabel", "Hidden-weapon beam:"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
								.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("HiddenBeamBackLbl", "Back"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
								.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
							[
								SNew(SBox).WidthOverride(80.f)
								[
									SNew(SNumericEntryBox<float>)
									.Value(this, &SUTWeaponSkinSelector::GetHiddenBeamBack)
									.OnValueChanged(this, &SUTWeaponSkinSelector::OnHiddenBeamBackChanged)
									.AllowSpin(true)
									.MinSliderValue(0.f).MaxSliderValue(60.f)
									.MinValue(0.f).MaxValue(100.f)
									.Delta(1.f)
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("HiddenBeamDownLbl", "Down"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
								.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SBox).WidthOverride(80.f)
								[
									SNew(SNumericEntryBox<float>)
									.Value(this, &SUTWeaponSkinSelector::GetHiddenBeamDown)
									.OnValueChanged(this, &SUTWeaponSkinSelector::OnHiddenBeamDownChanged)
									.AllowSpin(true)
									.MinSliderValue(0.f).MaxSliderValue(80.f)
									.MinValue(0.f).MaxValue(100.f)
									.Delta(1.f)
								]
							]
						]

						// Beam (tracer) colors — drive the UTNPLightningGun /
						// UTNPSniper (shared LGColor) and UTNPShockRifle (ShockBeam)
						// BP defaults which read [WeaponSkinsPlus] in Mod.ini at spawn.
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0, 0, 0, 8)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BeamColorsLabel", "Beam color:"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
								.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 0.8f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("HitscanColorLbl", "Hitscan (LG/Sniper)"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
								.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 16, 0)
							[
								SNew(SBox).WidthOverride(56.f).HeightOverride(18.f)
								[
									SNew(SButton)
									.OnClicked(this, &SUTWeaponSkinSelector::OnHitscanColorClicked)
									[
										SNew(SColorBlock)
										.Color(TAttribute<FLinearColor>::Create(
											TAttribute<FLinearColor>::FGetter::CreateSP(
												this, &SUTWeaponSkinSelector::GetHitscanBeamColor)))
									]
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("ShockColorLbl", "Shock beam"))
								.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
								.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f, 1.0f))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SBox).WidthOverride(56.f).HeightOverride(18.f)
								[
									SNew(SButton)
									.OnClicked(this, &SUTWeaponSkinSelector::OnShockColorClicked)
									[
										SNew(SColorBlock)
										.Color(TAttribute<FLinearColor>::Create(
											TAttribute<FLinearColor>::FGetter::CreateSP(
												this, &SUTWeaponSkinSelector::GetShockBeamColor)))
									]
								]
							]
						]

						// Content: Weapon list | Skin list
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SHorizontalBox)

							// Left: Weapon list with hide checkboxes
							+ SHorizontalBox::Slot()
							.FillWidth(0.40f)
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
							.FillWidth(0.60f)
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
							SAssignNew(StatusText, STextBlock)
							.Text(LOCTEXT("SelectWeapon", "Select a weapon to configure skins and visibility."))
							.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
							.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f))
						]

						// Buttons
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
									.Text(LOCTEXT("Apply", "Apply & Save"))
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

void SUTWeaponSkinSelector::GatherWeapons()
{
	Weapons.Empty();

	bool bHasInventory = false;

	// PRIMARY: Discover weapons from player's current inventory (in-game)
	// One entry per weapon INSTANCE — no deduplication by tag.
	// This means Lightning Gun and Sniper show separately even if they share a tag.
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		AUTCharacter* UTChar = Cast<AUTCharacter>(PlayerOwner->PlayerController->GetPawn());
		if (UTChar)
		{
			for (TInventoryIterator<AUTWeapon> It(UTChar); It; ++It)
			{
				AUTWeapon* Weapon = *It;
				if (!Weapon) continue;
				FName Tag = Weapon->WeaponSkinCustomizationTag;
				if (Tag == NAME_None) continue;

				FNetcodePlusWeaponInfo Info;
				Info.Tag = Tag;
				Info.HideKey = FName(*Weapon->GetClass()->GetName());
				Info.WeaponClass = Weapon->GetClass();
				Info.bHasSkins = false;

				// Use the weapon's actual display name or clean class name
				FString ClassName = Weapon->GetClass()->GetName();
				// Strip common prefixes for cleaner display
				ClassName.ReplaceInline(TEXT("BP_"), TEXT(""));
				ClassName.ReplaceInline(TEXT("_C"), TEXT(""));
				ClassName.ReplaceInline(TEXT("UTWeap_"), TEXT(""));
				ClassName.ReplaceInline(TEXT("UTPlusShockRifle"), TEXT("Shock Rifle"));
				ClassName.ReplaceInline(TEXT("UTPlusSniper"), TEXT("Sniper Rifle"));
				ClassName.ReplaceInline(TEXT("UTPlusFlakCannon"), TEXT("Flak Cannon"));
				ClassName.ReplaceInline(TEXT("UTWeap_LinkGun_Plus"), TEXT("Link Gun"));
				ClassName.ReplaceInline(TEXT("LinkGun_Plus"), TEXT("Link Gun"));
				ClassName.ReplaceInline(TEXT("_"), TEXT(" "));
				Info.DisplayName = ClassName;

				Weapons.Add(Info);
				bHasInventory = true;
			}
		}
	}

	// In instagib, filter to only show weapons from the game mode's DefaultInventory.
	// Prevents showing NC+ weapons that the mutator chain added but aren't part of the loadout.
	if (bHasInventory && PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		AUTGameState* GS = PlayerOwner->PlayerController->GetWorld()->GetGameState<AUTGameState>();
		if (GS)
		{
			TSubclassOf<AGameMode> GMClass = GS->GetGameModeClass();
			if (GMClass)
			{
				AUTGameMode* GMCDO = GMClass->GetDefaultObject<AUTGameMode>();
				if (GMCDO && GMCDO->bIsInstagib && GMCDO->DefaultInventory.Num() > 0)
				{
					Weapons = Weapons.FilterByPredicate([&](const FNetcodePlusWeaponInfo& W)
					{
						if (!W.WeaponClass) return false;
						for (const TSubclassOf<AUTInventory>& InvClass : GMCDO->DefaultInventory)
						{
							if (InvClass && W.WeaponClass->IsChildOf(InvClass))
							{
								return true;
							}
						}
						return false;
					});
				}
			}
		}
	}

	// FALLBACK: If not in-game (main menu), use cached weapon tags from Mod.ini
	if (!bHasInventory)
	{
		// Use cached weapons from last session if available
		if (CachedWeapons.Num() > 0)
		{
			Weapons = CachedWeapons;
		}
		else
		{
			// Last resort: read weapon tags from Mod.ini keys
			FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
			TSet<FName> SeenTags;

			// Scan for Hide.* and Skin.* keys to discover known weapons
			// This is imperfect but gives us something to show
			for (const auto& Prefix : { TEXT("Hide."), TEXT("Skin.") })
			{
				// We can't enumerate keys easily, but the tags we know about
				// from a previous session are in the static HiddenWeaponsByTag
				for (auto& Pair : AUTWeaponFix::HiddenWeaponsByTag)
				{
					if (SeenTags.Contains(Pair.Key)) continue;
					SeenTags.Add(Pair.Key);

					FNetcodePlusWeaponInfo Info;
					Info.Tag = Pair.Key;
					Info.HideKey = Pair.Key; // Fallback: use tag as hide key
					Info.WeaponClass = nullptr;
					Info.bHasSkins = false;

					FString TagStr = Pair.Key.ToString();
					TagStr.ReplaceInline(TEXT("_Skins"), TEXT(""));
					TagStr.ReplaceInline(TEXT("_"), TEXT(" "));
					Info.DisplayName = TagStr;

					Weapons.Add(Info);
				}
			}
		}
	}

	// Sort alphabetically
	Weapons.Sort([](const FNetcodePlusWeaponInfo& A, const FNetcodePlusWeaponInfo& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	// Cache for main menu fallback next time
	if (bHasInventory)
	{
		CachedWeapons = Weapons;
	}
}

void SUTWeaponSkinSelector::GatherSkins()
{
	SkinsByTag.Empty();

	// Lifecycle preload owns all asset loading. Opening F5 only groups pointers
	// already resident in the shared catalog, so the menu cannot reintroduce a
	// synchronous first-open hitch.
	TArray<UUTWeaponSkin*> CatalogSkins;
	AUTWeaponFix::GetPreloadedWeaponSkins(CatalogSkins);
	if (CatalogSkins.Num() == 0)
	{
		for (auto& W : Weapons)
		{
			W.bHasSkins = false;
		}
		return;
	}

	TSet<FString> SeenSkinPaths; // Deduplicate
	for (UUTWeaponSkin* Skin : CatalogSkins)
	{
		if (!Skin) continue;
		if (Skin->WeaponSkinCustomizationTag == NAME_None) continue;

		// Check if this skin's WeaponType matches any weapon in our inventory
		FString SkinWeaponType = Skin->WeaponType.ToString();
		if (SkinWeaponType.IsEmpty()) continue;

		// Try to match: the skin's WeaponType might be a parent class of our weapon
		// or it might be the exact class. Check if any of our weapon classes
		// match or are children of the skin's WeaponType.
		bool bMatchesInventory = false;
		FName MatchedTag = NAME_None;

		for (const auto& W : Weapons)
		{
			if (!W.WeaponClass) continue;
			if (Skin->WeaponSkinCustomizationTag != W.Tag) continue;

			if (AUTWeaponFix::IsWeaponSkinCompatible(Skin, W.WeaponClass))
			{
				bMatchesInventory = true;
				MatchedTag = W.Tag;
				break;
			}
		}

		if (!bMatchesInventory || MatchedTag == NAME_None) continue;

		// Deduplicate
		FString SkinPath = Skin->GetPathName();
		if (SeenSkinPaths.Contains(SkinPath)) continue;
		SeenSkinPaths.Add(SkinPath);

		SkinsByTag.FindOrAdd(MatchedTag).Add(Skin);
	}

	// Update bHasSkins
	for (auto& W : Weapons)
	{
		W.bHasSkins = SkinsByTag.Contains(W.Tag) && SkinsByTag[W.Tag].Num() > 0;
	}

	// Sort skins alphabetically within each weapon tag for consistent UI ordering
	// (AssetRegistry does not guarantee deterministic return order)
	for (auto& Pair : SkinsByTag)
	{
		Pair.Value.Sort([](UUTWeaponSkin& A, UUTWeaponSkin& B)
		{
			FString NameA = A.DisplayName.IsEmpty() ? A.GetName() : A.DisplayName.ToString();
			FString NameB = B.DisplayName.IsEmpty() ? B.GetName() : B.DisplayName.ToString();
			return NameA < NameB;
		});
	}

}

void SUTWeaponSkinSelector::LoadSettings()
{
	// Copy hide state from the static map (keyed by class name)
	HideState = AUTWeaponFix::HiddenWeaponsByTag;

	// Pick up hidden-weapon beam offsets + style from the now-populated statics so
	// the spinners/checkbox reflect whatever Mod.ini already had on disk.
	HiddenBeamBack = AUTWeaponFix::HiddenBeamBackOffset;
	HiddenBeamDown = AUTWeaponFix::HiddenBeamDownOffset;
	bClassicWeaponHide = AUTWeaponFix::bClassicWeaponHide;

	// Beam colors from Mod.ini [WeaponSkinsPlus]. Format = FLinearColor::ToString
	// output "(R=...,G=...,B=...,A=...)" so the BP parser (Convert String to
	// LinearColor) reads it directly. Bad / empty value = keep ctor defaults.
	{
		FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		FString S;
		if (GConfig->GetString(TEXT("WeaponSkinsPlus"), TEXT("LGColor"), S, ModIniPath) && !S.IsEmpty())
		{
			FLinearColor Parsed;
			if (Parsed.InitFromString(S))
			{
				HitscanBeamColor = Parsed;
			}
		}
		if (GConfig->GetString(TEXT("WeaponSkinsPlus"), TEXT("ShockBeam"), S, ModIniPath) && !S.IsEmpty())
		{
			FLinearColor Parsed;
			if (Parsed.InitFromString(S))
			{
				ShockBeamColor = Parsed;
			}
		}
		// Seed the BP-side apply gate (see header) so an existing True survives a save.
		GConfig->GetBool(TEXT("WeaponSkinsPlus"), TEXT("CustomShockBeam"), bShockBeamCustomized, ModIniPath);
		// Swatches re-poll via TAttribute on next paint — nothing to push.
	}

	// Load hitscan choice from Mod.ini
	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	FString HitscanStr;
	GConfig->GetString(TEXT("WeaponSkinsPlus"), TEXT("HitscanChoice"), HitscanStr, ModIniPath);
	CurrentHitscanChoice = HitscanStr.Equals(TEXT("LG"), ESearchCase::IgnoreCase) ? TEXT("LG") : TEXT("Sniper");
	if (HitscanValueText.IsValid())
	{
		HitscanValueText->SetText(FText::FromString(CurrentHitscanChoice == TEXT("LG") ? TEXT("Lightning Gun") : TEXT("Sniper Rifle")));
	}

	// Load skin selections from Mod.ini
	for (const auto& W : Weapons)
	{
		FString Key = FString::Printf(TEXT("Skin.%s"), *W.Tag.ToString());
		FString Value;
		if (GConfig->GetString(TEXT("NetcodePlus.WeaponSettings"), *Key, Value, ModIniPath) && !Value.IsEmpty())
		{
			// Find matching skin index
			TArray<UUTWeaponSkin*>* Skins = SkinsByTag.Find(W.Tag);
			if (Skins)
			{
				for (int32 i = 0; i < Skins->Num(); i++)
				{
					if ((*Skins)[i] && (*Skins)[i]->GetPathName() == Value)
					{
						SelectedSkinIndex.Add(W.Tag, i + 1); // +1 because 0 = default
						break;
					}
				}
			}
		}
	}
}

void SUTWeaponSkinSelector::SaveAndApply()
{
	if (!PlayerOwner.IsValid()) return;

	AUTPlayerController* PC = Cast<AUTPlayerController>(PlayerOwner->PlayerController);
	if (!PC) return;

	FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	// Hidden-weapon style + camera-beam offsets — pushed to the AUTWeaponFix
	// statics so the next fire reads them (GetImpactSpawnPosition reads the
	// statics directly in classic style), and persisted to Mod.ini so they
	// survive a restart. Clamp here too — the spinner Min/Max already enforces,
	// but a manual edit could poke in something weird.
	AUTWeaponFix::bClassicWeaponHide = bClassicWeaponHide;
	GConfig->SetBool(TEXT("NetcodePlus.WeaponSettings"), TEXT("ClassicWeaponHide"), bClassicWeaponHide, ModIniPath);
	AUTWeaponFix::HiddenBeamBackOffset = FMath::Clamp(HiddenBeamBack, 0.f, 100.f);
	AUTWeaponFix::HiddenBeamDownOffset = FMath::Clamp(HiddenBeamDown, 0.f, 100.f);
	GConfig->SetString(TEXT("NetcodePlus.WeaponSettings"), TEXT("HiddenBeamBack"),
		*FString::Printf(TEXT("%.3f"), AUTWeaponFix::HiddenBeamBackOffset), ModIniPath);
	GConfig->SetString(TEXT("NetcodePlus.WeaponSettings"), TEXT("HiddenBeamDown"),
		*FString::Printf(TEXT("%.3f"), AUTWeaponFix::HiddenBeamDownOffset), ModIniPath);

	// Beam colors → [WeaponSkinsPlus] LGColor / ShockBeam. FLinearColor::ToString
	// writes "(R=...,G=...,B=...,A=...)" which BP's "Convert String to LinearColor"
	// reads back unchanged. Applies on next weapon spawn (BPs read at BeginPlay).
	GConfig->SetString(TEXT("WeaponSkinsPlus"), TEXT("LGColor"),  *HitscanBeamColor.ToString(), ModIniPath);
	GConfig->SetString(TEXT("WeaponSkinsPlus"), TEXT("ShockBeam"), *ShockBeamColor.ToString(),  ModIniPath);
	// The shock BP only applies ShockBeam when this gate is True (see header) —
	// written from the latched flag so saving unrelated settings never force-enables
	// a custom beam for users who never picked a shock color.
	GConfig->SetString(TEXT("WeaponSkinsPlus"), TEXT("CustomShockBeam"),
		bShockBeamCustomized ? TEXT("True") : TEXT("False"), ModIniPath);

	// Save hide states and skin selections to config only — no server RPCs.
	// Skins apply on next spawn; hide states are client-local (checked in BringUp).
	for (const auto& W : Weapons)
	{
		bool bHidden = HideState.Contains(W.HideKey) ? HideState[W.HideKey] : false;
		AUTWeaponFix::HiddenWeaponsByTag.Add(W.HideKey, bHidden);

		FString HideKeyStr = FString::Printf(TEXT("Hide.%s"), *W.HideKey.ToString());
		GConfig->SetString(TEXT("NetcodePlus.WeaponSettings"), *HideKeyStr, bHidden ? TEXT("1") : TEXT("0"), ModIniPath);

		if (W.bHasSkins)
		{
			int32 SkinIdx = SelectedSkinIndex.Contains(W.Tag) ? SelectedSkinIndex[W.Tag] : 0;
			FString SkinKey = FString::Printf(TEXT("Skin.%s"), *W.Tag.ToString());

			if (SkinIdx == 0)
			{
				GConfig->SetString(TEXT("NetcodePlus.WeaponSettings"), *SkinKey, TEXT(""), ModIniPath);
				AUTWeaponFix::SavedSkinPaths.Remove(W.Tag);
				AUTWeaponFix::CachedSkinAssets.Remove(W.Tag);
			}
			else
			{
				TArray<UUTWeaponSkin*>* Skins = SkinsByTag.Find(W.Tag);
				if (Skins && Skins->IsValidIndex(SkinIdx - 1))
				{
					UUTWeaponSkin* SelectedSkin = (*Skins)[SkinIdx - 1];
					FString SkinPath = SelectedSkin->GetPathName();
					GConfig->SetString(TEXT("NetcodePlus.WeaponSettings"), *SkinKey, *SkinPath, ModIniPath);
					AUTWeaponFix::SavedSkinPaths.Add(W.Tag, SkinPath);
					if (SelectedSkin)
					{
						AUTWeaponFix::CachedSkinAssets.Add(W.Tag, SelectedSkin);
					}
				}
			}
		}
	}

	// Save hitscan choice
	GConfig->SetString(TEXT("WeaponSkinsPlus"), TEXT("HitscanChoice"), *CurrentHitscanChoice, ModIniPath);

	GConfig->Flush(false, ModIniPath);

	// Re-apply to the CURRENT weapon so hide/style changes take effect now — the
	// BringUp/swap-detector paths only fire on a weapon CHANGE, so without this a
	// style flip while a hidden weapon is equipped strands the old style's state
	// until the next switch.
	AUTCharacter* UTChar = Cast<AUTCharacter>(PC->GetPawn());
	if (UTChar && UTChar->GetWeapon())
	{
		AUTWeapon* CurWeap = UTChar->GetWeapon();
		bool* bHidden = AUTWeaponFix::HiddenWeaponsByTag.Find(FName(*CurWeap->GetClass()->GetName()));
		AUTWeaponFix::ApplyWeaponHideState(CurWeap, UTChar, bHidden && *bHidden);
		if (ATeamArenaCharacter* TeamChar = Cast<ATeamArenaCharacter>(UTChar))
		{
			if (AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(CurWeap))
			{
				TeamChar->SubmitConfiguredWeaponSkin(FixWeapon, true);
			}
			else
			{
				UTChar->UpdateWeaponSkinPrefFromProfile(CurWeap);
			}
		}
		else
		{
			UTChar->UpdateWeaponSkinPrefFromProfile(CurWeap);
		}
	}

	// Send hitscan choice to server via console command — defers through the command pipeline
	// instead of calling ServerMutate directly (which executes inline on listen servers).
	FString MutateCmd = CurrentHitscanChoice == TEXT("LG") ? TEXT("mutate hsc_LG") : TEXT("mutate hsc_SR");
	PC->ConsoleCommand(MutateCmd);
}

void SUTWeaponSkinSelector::RebuildWeaponList()
{
	if (!WeaponListContainer.IsValid()) return;
	WeaponListContainer->ClearChildren();

	for (int32 i = 0; i < Weapons.Num(); i++)
	{
		const auto& W = Weapons[i];
		const bool bSelected = (i == CurrentWeaponIndex);
		FLinearColor BgColor = bSelected ? FLinearColor(0.15f, 0.4f, 0.7f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);
		FLinearColor TextColor = bSelected ? FLinearColor::White : FLinearColor(0.7f, 0.7f, 0.7f, 1.0f);

		bool bHidden = HideState.Contains(W.HideKey) ? HideState[W.HideKey] : false;

		WeaponListContainer->AddSlot()
		.AutoHeight()
		.Padding(1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SButton)
				.ButtonColorAndOpacity(BgColor)
				.OnClicked(this, &SUTWeaponSkinSelector::OnWeaponClicked, i)
				[
					SNew(STextBlock)
					.Text(FText::FromString(W.DisplayName))
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 13))
					.ColorAndOpacity(TextColor)
					.Margin(FMargin(6.0f, 4.0f))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0, 2, 0)
			[
				SNew(SCheckBox)
				.IsChecked(bHidden ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged(this, &SUTWeaponSkinSelector::OnHideCheckChanged, W.HideKey)
				.ToolTipText(LOCTEXT("HideTooltip", "Hide weapon in first person"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Hide", "Hide"))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 10))
				.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f))
			]
		];
	}
}

void SUTWeaponSkinSelector::RebuildSkinList()
{
	if (!SkinListContainer.IsValid()) return;
	SkinListContainer->ClearChildren();

	if (!Weapons.IsValidIndex(CurrentWeaponIndex)) return;

	const auto& W = Weapons[CurrentWeaponIndex];

	if (!W.bHasSkins)
	{
		// No skins for this weapon — show message
		SkinListContainer->AddSlot()
		.AutoHeight()
		.Padding(8)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("No skins available for %s.\nUse the Hide checkbox to hide this weapon."), *W.DisplayName)))
			.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 12))
			.ColorAndOpacity(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f))
			.AutoWrapText(true)
		];
		return;
	}

	int32 CurrentSkin = SelectedSkinIndex.Contains(W.Tag) ? SelectedSkinIndex[W.Tag] : 0;

	// Default option
	{
		FLinearColor BgColor = (CurrentSkin == 0) ? FLinearColor(0.15f, 0.6f, 0.15f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);
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

	TArray<UUTWeaponSkin*>* Skins = SkinsByTag.Find(W.Tag);
	if (!Skins) return;

	for (int32 i = 0; i < Skins->Num(); i++)
	{
		UUTWeaponSkin* Skin = (*Skins)[i];
		if (!Skin) continue;

		const int32 SkinIdx = i + 1;
		FLinearColor BgColor = (CurrentSkin == SkinIdx) ? FLinearColor(0.15f, 0.6f, 0.15f, 1.0f) : FLinearColor(0.08f, 0.08f, 0.08f, 1.0f);

		FString DisplayStr = Skin->DisplayName.ToString();
		if (DisplayStr.IsEmpty()) DisplayStr = Skin->GetName();
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

FReply SUTWeaponSkinSelector::OnWeaponClicked(int32 Index)
{
	CurrentWeaponIndex = Index;
	RebuildWeaponList();
	RebuildSkinList();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnSkinClicked(int32 Index)
{
	if (Weapons.IsValidIndex(CurrentWeaponIndex))
	{
		SelectedSkinIndex.Add(Weapons[CurrentWeaponIndex].Tag, Index);
	}
	RebuildSkinList();
	return FReply::Handled();
}

void SUTWeaponSkinSelector::OnHideCheckChanged(ECheckBoxState NewState, FName HideKey)
{
	HideState.Add(HideKey, NewState == ECheckBoxState::Checked);
}

FReply SUTWeaponSkinSelector::OnApplyClicked()
{
	SaveAndApply();
	RebuildWeaponList(); // Refresh checkbox visuals

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(TEXT("Settings saved and applied.")));
	}

	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnCloseClicked()
{
	ClosePanel();
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnHitscanColorClicked()
{
	OpenBeamColorPicker(HitscanBeamColor, [this](FLinearColor NewC) { OnHitscanColorCommitted(NewC); });
	return FReply::Handled();
}

FReply SUTWeaponSkinSelector::OnShockColorClicked()
{
	OpenBeamColorPicker(ShockBeamColor, [this](FLinearColor NewC) { OnShockColorCommitted(NewC); });
	return FReply::Handled();
}

void SUTWeaponSkinSelector::OnHitscanColorCommitted(FLinearColor NewColor)
{
	// TAttribute binding re-polls the getter on next paint — no swatch push needed.
	HitscanBeamColor = NewColor;
}

void SUTWeaponSkinSelector::OnShockColorCommitted(FLinearColor NewColor)
{
	ShockBeamColor = NewColor;
	// Picking a shock color IS the customization intent — latch the BP apply gate
	// so the save writes CustomShockBeam=True (see header; the gate was previously
	// never written by this selector, so the picked color silently did nothing for
	// anyone the retired BP tool hadn't already flagged).
	bShockBeamCustomized = true;
}

void SUTWeaponSkinSelector::OpenBeamColorPicker(FLinearColor Initial, TFunction<void(FLinearColor)> OnCommit)
{
	// Fullscreen-Exclusive workaround. SColorPicker opens as a new top-level
	// Slate window; under FSE the OS minimizes the game to release the swap
	// chain, the picker can't render, focus bounces, and the user gets an
	// infinite minimize/restore cycle. Snapshot the current mode, swap to
	// borderless (WindowedFullscreen) if FSE was active, restore on close.
	// Direct port of the nchud OnSwatchClicked pattern - keep them in sync.
	EWindowMode::Type SavedMode = EWindowMode::Windowed;
	FIntPoint         SavedRes  = FIntPoint::ZeroValue;
	FVector2D         SavedWindowPos = FVector2D::ZeroVector;
	bool              bHaveWindowPos = false;

	if (UGameUserSettings* Settings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		SavedMode = Settings->GetFullscreenMode();
		SavedRes  = Settings->GetScreenResolution();
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
	PickerArgs.bOnlyRefreshOnOk = false;
	PickerArgs.bExpandAdvancedSection = false;
	PickerArgs.InitialColorOverride = Initial;
	PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda(
		[OnCommit](FLinearColor C) { if (OnCommit) OnCommit(C); });
	PickerArgs.ParentWidget = SharedThis(this);

	// Restore on close - same order as nchud (SetFullscreenMode → SetScreenResolution
	// → SaveConfig → ApplyResolutionSettings) which mirrors UT4's GUI Settings
	// Display dropdown. See SNCPlusHUDEditor::OnSwatchClicked for full rationale.
	PickerArgs.OnColorPickerWindowClosed = FOnWindowClosed::CreateLambda(
		[SavedMode, SavedRes, SavedWindowPos, bHaveWindowPos](const TSharedRef<SWindow>& /*W*/)
		{
			if (SavedMode != EWindowMode::Fullscreen) return;
			if (bHaveWindowPos && GEngine && GEngine->GameViewport)
			{
				TSharedPtr<SWindow> GameWindow = GEngine->GameViewport->GetWindow();
				if (GameWindow.IsValid() && GameWindow->GetPositionInScreen() != SavedWindowPos)
				{
					GameWindow->MoveWindowTo(SavedWindowPos);
				}
			}
			UGameUserSettings* S = GEngine ? GEngine->GetGameUserSettings() : nullptr;
			if (!S) return;
			S->SetFullscreenMode(EWindowMode::Fullscreen);
			S->SetScreenResolution(SavedRes);
			S->SaveConfig();
			S->ApplyResolutionSettings(false);
		});

	OpenColorPicker(PickerArgs);
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

/** Static cleanup — call when module shuts down or cache should be invalidated */
void SUTWeaponSkinSelector_CleanupCache()
{
	CachedWeapons.Empty();
}

SUTWeaponSkinSelector::~SUTWeaponSkinSelector()
{
	// Map load drops the viewport widget without ClosePanel — release the refcount.
	if (bHeldDragMode) { NCPlusHUDDragMode::SetActive(false); bHeldDragMode = false; }
}

void SUTWeaponSkinSelector::ClosePanel()
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

FReply SUTWeaponSkinSelector::OnHitscanToggleClicked()
{
	// Toggle between Sniper and LG
	if (CurrentHitscanChoice == TEXT("LG"))
	{
		CurrentHitscanChoice = TEXT("Sniper");
	}
	else
	{
		CurrentHitscanChoice = TEXT("LG");
	}

	if (HitscanValueText.IsValid())
	{
		HitscanValueText->SetText(FText::FromString(CurrentHitscanChoice == TEXT("LG") ? TEXT("Lightning Gun") : TEXT("Sniper Rifle")));
	}

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(FString::Printf(TEXT("Hitscan set to %s — click Apply to save."),
			CurrentHitscanChoice == TEXT("LG") ? TEXT("Lightning Gun") : TEXT("Sniper Rifle"))));
	}

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
