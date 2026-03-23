// NetcodePlus.cpp
#include "NetcodePlus.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "UTPlayerController.h"
#include "UTProfileSettings.h"
#include "UTLocalPlayer.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTWeaponFix.h"
#include "UTATypes.h"
#include "SUTWeaponSkinSelector.h"

/** Weak reference to active skin selector — only one can be open at a time */
static TWeakPtr<SUTWeaponSkinSelector> ActiveSkinSelector;

static void HandleWeaponHand(const TArray<FString>& Args)
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
	if (!World) return;

	APlayerController* RawPC = World->GetFirstPlayerController();
	AUTPlayerController* PC = Cast<AUTPlayerController>(RawPC);
	if (!PC) return;

	if (Args.Num() > 0)
	{
		EWeaponHand NewHand = EWeaponHand::HAND_Right;
		const TCHAR* HandName = TEXT("Right");
		FString Hand = Args[0].ToLower();

		if (Hand == TEXT("right") || Hand == TEXT("r"))
		{
			NewHand = EWeaponHand::HAND_Right;
			HandName = TEXT("Right");
		}
		else if (Hand == TEXT("left") || Hand == TEXT("l"))
		{
			NewHand = EWeaponHand::HAND_Left;
			HandName = TEXT("Left");
		}
		else if (Hand == TEXT("center") || Hand == TEXT("c"))
		{
			NewHand = EWeaponHand::HAND_Center;
			HandName = TEXT("Center");
		}
		else if (Hand == TEXT("hidden") || Hand == TEXT("h") || Hand == TEXT("hide"))
		{
			// Per-weapon hide — hides the CURRENT weapon by its tag
			AUTCharacter* UTChar = Cast<AUTCharacter>(PC->GetPawn());
			if (UTChar && UTChar->GetWeapon())
			{
				AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(UTChar->GetWeapon());
				if (FixWeapon && FixWeapon->WeaponSkinCustomizationTag != NAME_None)
				{
					AUTWeaponFix::HiddenWeaponsByTag.Add(FixWeapon->WeaponSkinCustomizationTag, true);
					AUTWeaponFix::SaveWeaponSettings();

					if (FixWeapon->GetMesh())
						FixWeapon->GetMesh()->SetVisibility(false, true);
					if (UTChar->FirstPersonMesh)
						UTChar->FirstPersonMesh->SetVisibility(false, true);

					PC->ClientMessage(FString::Printf(TEXT("Hidden: %s (saved to Mod.ini)"), *FixWeapon->WeaponSkinCustomizationTag.ToString()));
				}
				else
				{
					PC->ClientMessage(TEXT("Current weapon is not a NetcodePlus weapon."));
				}
			}
			return;
		}
		else if (Hand == TEXT("show") || Hand == TEXT("s"))
		{
			// Per-weapon show — unhides the CURRENT weapon by its tag
			AUTCharacter* UTChar = Cast<AUTCharacter>(PC->GetPawn());
			if (UTChar && UTChar->GetWeapon())
			{
				AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(UTChar->GetWeapon());
				if (FixWeapon && FixWeapon->WeaponSkinCustomizationTag != NAME_None)
				{
					AUTWeaponFix::HiddenWeaponsByTag.Add(FixWeapon->WeaponSkinCustomizationTag, false);
					AUTWeaponFix::SaveWeaponSettings();

					if (FixWeapon->GetMesh())
						FixWeapon->GetMesh()->SetVisibility(true, true);
					if (UTChar->FirstPersonMesh)
						UTChar->FirstPersonMesh->SetVisibility(true, true);

					PC->ClientMessage(FString::Printf(TEXT("Shown: %s (saved to Mod.ini)"), *FixWeapon->WeaponSkinCustomizationTag.ToString()));
				}
			}
			// Re-apply current hand position
			NewHand = PC->GetWeaponHand();
			HandName = TEXT("Shown");
		}
		else
		{
			PC->ClientMessage(TEXT("Usage: weaponhand [right|left|center|hidden|show]"));
			return;
		}

		// Epic's SetWeaponHand() is bugged — it takes NewHand but never stores it.
		// ProfileSettings->WeaponHand is what GetWeaponHand() actually reads.
		UUTProfileSettings* ProfileSettings = PC->GetProfileSettings();
		if (ProfileSettings)
		{
			ProfileSettings->WeaponHand = NewHand;
		}
		PC->SetWeaponHand(NewHand);
		PC->ClientMessage(FString::Printf(TEXT("Weapon hand: %s"), HandName));
	}
	else
	{
		EWeaponHand Current = PC->GetWeaponHand();
		const TCHAR* Name = TEXT("Unknown");
		switch (Current)
		{
			case EWeaponHand::HAND_Right:  Name = TEXT("Right"); break;
			case EWeaponHand::HAND_Left:   Name = TEXT("Left"); break;
			case EWeaponHand::HAND_Center: Name = TEXT("Center"); break;
			case EWeaponHand::HAND_Hidden: Name = TEXT("Hidden"); break;
		}
		PC->ClientMessage(FString::Printf(TEXT("Weapon hand: %s. Usage: weaponhand [right|left|center|hidden]"), Name));
	}
}


static void HandleWeaponSkins(const TArray<FString>& Args)
{
	// Toggle: if already open, close it
	if (ActiveSkinSelector.IsValid())
	{
		ActiveSkinSelector.Pin()->ClosePanel();
		ActiveSkinSelector.Reset();
		return;
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
	if (!World) return;

	APlayerController* RawPC = World->GetFirstPlayerController();
	if (!RawPC) return;

	UUTLocalPlayer* LP = Cast<UUTLocalPlayer>(RawPC->GetLocalPlayer());
	if (!LP) return;

	UGameViewportClient* ViewportClient = World->GetGameViewport();
	if (!ViewportClient) return;

	TSharedRef<SUTWeaponSkinSelector> SkinSelector =
		SNew(SUTWeaponSkinSelector)
		.PlayerOwner(LP);

	ViewportClient->AddViewportWidgetContent(SkinSelector, 100);

	// Give it keyboard focus
	FSlateApplication::Get().SetKeyboardFocus(SkinSelector, EFocusCause::SetDirectly);

	ActiveSkinSelector = SkinSelector;
}

IMPLEMENT_MODULE(FNetcodePlus, NetcodePlus)

void FNetcodePlus::StartupModule()
{
	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("weaponhand"),
		TEXT("Set weapon position. Usage: weaponhand [right|left|center|hidden]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleWeaponHand),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("weaponskins"),
		TEXT("Open weapon skin selector UI"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleWeaponSkins),
		ECVF_Default
	);

	UE_LOG(LogLoad, Log, TEXT("netcodeplus loaded"));
}

void FNetcodePlus::ShutdownModule()
{
	// Close skin selector if open and free cached assets
	if (ActiveSkinSelector.IsValid())
	{
		ActiveSkinSelector.Pin()->ClosePanel();
		ActiveSkinSelector.Reset();
	}
	SUTWeaponSkinSelector_CleanupCache();

	IConsoleObject* Cmd = IConsoleManager::Get().FindConsoleObject(TEXT("weaponhand"));
	if (Cmd) { IConsoleManager::Get().UnregisterConsoleObject(Cmd, false); }

	IConsoleObject* Cmd2 = IConsoleManager::Get().FindConsoleObject(TEXT("weaponskins"));
	if (Cmd2) { IConsoleManager::Get().UnregisterConsoleObject(Cmd2, false); }

	UE_LOG(LogLoad, Log, TEXT("netcodeplus unloaded"));
}
