// SUTNCPlusMenu.cpp — NetcodePlus client settings implementation
#include "SUTNCPlusMenu.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"

// Mod.ini section
static const TCHAR* NCPSection = TEXT("NetcodePlus");

void SUTNCPlusMenu::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;

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
			.Padding(0, 20, 0, 20)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("NETCODEPLUS SETTINGS")))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 28))
				.ColorAndOpacity(FLinearColor(1.f, 0.6f, 0.f, 1.f))
			]

			// ── Gore Settings ──
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 10, 0, 5)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Gore Settings")))
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 18))
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
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 14))
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
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 14))
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
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 14))
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
				.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Bold.ttf"), 18))
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
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 14))
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
					.Font(FSlateFontInfo(FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf"), 14))
					.ColorAndOpacity(FLinearColor::White)
				]
			]

			// ── Buttons ──
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

void SUTNCPlusMenu::LoadSettings()
{
	FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	FString Val;
	if (GConfig->GetString(NCPSection, TEXT("AllowGib"), Val, ConfigPath))
		bAllowGib = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	else
		bAllowGib = false;

	if (GConfig->GetString(NCPSection, TEXT("ShowRagdoll"), Val, ConfigPath))
		bShowRagdoll = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
	else
		bShowRagdoll = true;

	if (GConfig->GetString(NCPSection, TEXT("RagdollTime"), Val, ConfigPath))
		RagdollTime = FCString::Atof(*Val);
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
}

void SUTNCPlusMenu::SaveSettings()
{
	FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	GConfig->SetString(NCPSection, TEXT("AllowGib"), bAllowGib ? TEXT("True") : TEXT("False"), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("ShowRagdoll"), bShowRagdoll ? TEXT("True") : TEXT("False"), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("RagdollTime"), *FString::SanitizeFloat(RagdollTime), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("OwnFootstepVolume"), *FString::SanitizeFloat(OwnFootstepVolume), ConfigPath);
	GConfig->SetString(NCPSection, TEXT("HighResScreenshotPostMatch"), bHighResScreenshotPostMatch ? TEXT("True") : TEXT("False"), ConfigPath);

	GConfig->Flush(false, ConfigPath);

	UE_LOG(LogTemp, Log, TEXT("NCPlus settings saved to Mod.ini"));
}

void SUTNCPlusMenu::ClosePanel()
{
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
