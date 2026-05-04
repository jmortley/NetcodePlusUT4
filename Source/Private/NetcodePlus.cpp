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
#include "SUTNCPlusMenu.h"
#include "SUTCosmeticSelector.h"
#include "SNCPlusHUDEditor.h"
#include "SNCPlusHUDDragOverlay.h"

/** Weak reference to active skin selector — only one can be open at a time */
static TWeakPtr<SUTWeaponSkinSelector> ActiveSkinSelector;

/** Weak reference to active NCP menu — only one can be open at a time */
static TWeakPtr<SUTNCPlusMenu> ActiveNCPMenu;

/** Weak reference to active cosmetic selector */
static TWeakPtr<SUTCosmeticSelector> ActiveCosmeticSelector;

/** Weak reference to active HUD layout editor */
static TWeakPtr<SNCPlusHUDEditor>      ActiveHUDEditor;
static TWeakPtr<SNCPlusHUDDragOverlay> ActiveDragOverlay;

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

static void HandleNCPMenu(const TArray<FString>& Args)
{
	// Toggle: if already open, close it
	if (ActiveNCPMenu.IsValid())
	{
		ActiveNCPMenu.Pin()->ClosePanel();
		ActiveNCPMenu.Reset();
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

	TSharedRef<SUTNCPlusMenu> Menu =
		SNew(SUTNCPlusMenu)
		.PlayerOwner(LP);

	ViewportClient->AddViewportWidgetContent(Menu, 100);
	FSlateApplication::Get().SetKeyboardFocus(Menu, EFocusCause::SetDirectly);

	ActiveNCPMenu = Menu;
}

static void HandleCosmetics(const TArray<FString>& Args)
{
	if (ActiveCosmeticSelector.IsValid())
	{
		ActiveCosmeticSelector.Pin()->ClosePanel();
		ActiveCosmeticSelector.Reset();
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

	TSharedRef<SUTCosmeticSelector> Selector =
		SNew(SUTCosmeticSelector)
		.PlayerOwner(LP);

	ViewportClient->AddViewportWidgetContent(Selector, 100);
	FSlateApplication::Get().SetKeyboardFocus(Selector, EFocusCause::SetDirectly);

	ActiveCosmeticSelector = Selector;
}

static void HandleHUDEditor(const TArray<FString>& Args)
{
	// Toggle: close if open.
	if (ActiveHUDEditor.IsValid())
	{
		ActiveHUDEditor.Pin()->ClosePanel();
		ActiveHUDEditor.Reset();
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

	TSharedRef<SNCPlusHUDEditor> Editor =
		SNew(SNCPlusHUDEditor)
		.PlayerOwner(LP);

	ViewportClient->AddViewportWidgetContent(Editor, 100);
	FSlateApplication::Get().SetKeyboardFocus(Editor, EFocusCause::SetDirectly);

	ActiveHUDEditor = Editor;
}

static void HandleHUDDragOverlay(const TArray<FString>& Args)
{
	// Toggle: close if open.
	if (ActiveDragOverlay.IsValid())
	{
		ActiveDragOverlay.Pin()->ClosePanel();
		ActiveDragOverlay.Reset();
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

	TSharedRef<SNCPlusHUDDragOverlay> Overlay =
		SNew(SNCPlusHUDDragOverlay)
		.PlayerOwner(LP);

	// ZOrder 95 — below the editor panel (100) so if both are up the editor sits
	// on top; above all in-game HUD widgets so frames + labels are visible.
	ViewportClient->AddViewportWidgetContent(Overlay, 95);
	FSlateApplication::Get().SetKeyboardFocus(Overlay, EFocusCause::SetDirectly);

	ActiveDragOverlay = Overlay;
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

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ncpmenu"),
		TEXT("Open NetcodePlus settings menu (gore, footsteps, screenshots)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleNCPMenu),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("cosmetics"),
		TEXT("Open cosmetic selector (hats, eyewear, characters, taunts)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleCosmetics),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("nchud"),
		TEXT("Open the NetcodePlus HUD layout editor (live preview, save to disk)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleHUDEditor),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("nchud_drag"),
		TEXT("Toggle the visual drag-drop overlay — click and drag any HUD element frame"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleHUDDragOverlay),
		ECVF_Default
	);

	// Bind F5 to ncpmenu by default
	if (GEngine)
	{
		UPlayerInput* DefInput = GetMutableDefault<UPlayerInput>();
		if (DefInput)
		{
			FKeyBind Bind;
			Bind.Key = EKeys::F5;
			Bind.Command = TEXT("ncpmenu");
			DefInput->DebugExecBindings.Add(Bind);
		}
	}

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

	// Close NCP menu if open
	if (ActiveNCPMenu.IsValid())
	{
		ActiveNCPMenu.Pin()->ClosePanel();
		ActiveNCPMenu.Reset();
	}

	IConsoleObject* Cmd3 = IConsoleManager::Get().FindConsoleObject(TEXT("ncpmenu"));
	if (Cmd3) { IConsoleManager::Get().UnregisterConsoleObject(Cmd3, false); }

	// Close cosmetic selector if open
	if (ActiveCosmeticSelector.IsValid())
	{
		ActiveCosmeticSelector.Pin()->ClosePanel();
		ActiveCosmeticSelector.Reset();
	}

	IConsoleObject* Cmd4 = IConsoleManager::Get().FindConsoleObject(TEXT("cosmetics"));
	if (Cmd4) { IConsoleManager::Get().UnregisterConsoleObject(Cmd4, false); }

	// Close HUD layout editor if open
	if (ActiveHUDEditor.IsValid())
	{
		ActiveHUDEditor.Pin()->ClosePanel();
		ActiveHUDEditor.Reset();
	}

	IConsoleObject* Cmd5 = IConsoleManager::Get().FindConsoleObject(TEXT("nchud"));
	if (Cmd5) { IConsoleManager::Get().UnregisterConsoleObject(Cmd5, false); }

	// Close drag overlay if open
	if (ActiveDragOverlay.IsValid())
	{
		ActiveDragOverlay.Pin()->ClosePanel();
		ActiveDragOverlay.Reset();
	}

	IConsoleObject* Cmd6 = IConsoleManager::Get().FindConsoleObject(TEXT("nchud_drag"));
	if (Cmd6) { IConsoleManager::Get().UnregisterConsoleObject(Cmd6, false); }

	UE_LOG(LogLoad, Log, TEXT("netcodeplus unloaded"));
}
