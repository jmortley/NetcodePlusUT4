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
#include "ElimPlusHUD.h"
#include "WipeoutHUD.h"
#include "NCPlusCTFHUD.h"
#include "ShockDomHUD.h"

// -ncpconnect launcher direct-connect support
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreMisc.h"
#include "Engine/GameInstance.h"

// F5 hotkey support
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"

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

	// Harden: skip if the active HUD isn't one of ours. Catches main-menu
	// invocations and stock-mode lobbies — without this guard the overlay
	// would mount as an empty translucent layer that catches mouse input
	// (no crash, just bad UX). The cast chain checks each NetcodePlus mode HUD.
	AHUD* RawHUD = RawPC->MyHUD;
	const bool bIsNCPHUD =
		Cast<AElimPlusHUD>(RawHUD) != nullptr ||
		Cast<AWipeoutHUD>(RawHUD)  != nullptr ||
		Cast<ANCPlusCTFHUD>(RawHUD)!= nullptr ||
		Cast<AShockDomHUD>(RawHUD) != nullptr;
	if (!bIsNCPHUD)
	{
		UE_LOG(LogTemp, Warning, TEXT("nchud_drag: not in a NetcodePlus match (active HUD is %s) — ignoring."),
			RawHUD ? *RawHUD->GetClass()->GetName() : TEXT("<none>"));
		return;
	}

	TSharedRef<SNCPlusHUDDragOverlay> Overlay =
		SNew(SNCPlusHUDDragOverlay)
		.PlayerOwner(LP);

	// ZOrder 1000 — above stock UT slate UI like the pre-match team-preview
	// scoreboard (~100-200) and any in-match overlays. Tested at 95 first; the
	// pre-match scoreboard's slate widget intercepted clicks before reaching us
	// (visible-but-unclickable). 1000 is high enough to cover stock UI without
	// fighting Slate modal dialogs (which mount as windows, not viewport widgets,
	// so ZOrder doesn't affect them anyway).
	ViewportClient->AddViewportWidgetContent(Overlay, 1000);
	FSlateApplication::Get().SetKeyboardFocus(Overlay, EFocusCause::SetDirectly);

	ActiveDragOverlay = Overlay;
}

// ---------------------------------------------------------------------------
// -ncpconnect: UT4 Community Launcher direct-connect to a PUG server.
//
// The retail SHIPPING client compiles out every stock command-line connect path
// (GameInstance.cpp blanks the first-arg map/URL; -EXEC=/-ExecCmds= are stripped),
// so the launcher cannot ask the game to join a server through any normal cmdline
// argument. This plugin-side hook re-enables exactly one narrow case: when the
// launcher passes -ncpconnect=IP:port?Password=pw, we wait until the front-end
// menu world exists and the player is signed in to MCP, then issue a single
// engine-level ClientTravel to that server. Plugin C++ is NOT shipping-gated.
//
// Deliberately PlayerController-free: routing this through a PlayerController
// (even our own ANPPlayerController) crashes the editor, so the travel is driven
// from a core ticker via GEngine->SetClientTravel — the same call that
// APlayerController::ClientTravel makes internally, minus the controller.
// ---------------------------------------------------------------------------

static FDelegateHandle GNcpConnectTickerHandle;
static FString         GNcpConnectURL;
static float           GNcpConnectElapsed = 0.0f;

/** Connect anyway after this long if MCP sign-in never completes (offline/slow login). */
static const float GNcpConnectLoginTimeout = 45.0f;

/** Drop the password option so it never reaches the log. */
static FString RedactConnectURL(const FString& URL)
{
	int32 QueryIdx = INDEX_NONE;
	if (URL.FindChar(TEXT('?'), QueryIdx))
	{
		return URL.Left(QueryIdx) + TEXT("?<options hidden>");
	}
	return URL;
}

/** Core-ticker callback: wait for the menu + sign-in, then ClientTravel once. */
static bool TickNcpConnect(float DeltaTime)
{
	GNcpConnectElapsed += DeltaTime;

	if (!GEngine)
	{
		return true; // keep waiting for the engine
	}

	// Find the live game-client world. PIE/editor worlds are EWorldType::PIE/Editor
	// and never carry -ncpconnect, so this also keeps the hook inert in the editor.
	UWorld* GameWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World())
		{
			GameWorld = Context.World();
			break;
		}
	}
	if (!GameWorld)
	{
		return true; // front-end map not up yet
	}

	// Wait for MCP sign-in so trusted PUG servers accept us; fall back on timeout.
	UUTLocalPlayer* UTLP = nullptr;
	if (UGameInstance* GI = GameWorld->GetGameInstance())
	{
		UTLP = Cast<UUTLocalPlayer>(GI->GetFirstGamePlayer());
	}

	const bool bLoggedIn = (UTLP && UTLP->IsLoggedIn());
	const bool bTimedOut = (GNcpConnectElapsed >= GNcpConnectLoginTimeout);
	if (!bLoggedIn && !bTimedOut)
	{
		return true; // keep waiting for sign-in
	}

	// Warning verbosity survives Shipping (Log/Verbose are stripped there).
	UE_LOG(LogLoad, Warning, TEXT("netcodeplus: -ncpconnect -> ClientTravel to %s%s"),
		*RedactConnectURL(GNcpConnectURL),
		bLoggedIn ? TEXT("") : TEXT(" (not signed in; connecting anyway after timeout)"));

	GEngine->SetClientTravel(GameWorld, *GNcpConnectURL, TRAVEL_Absolute);

	GNcpConnectTickerHandle.Reset();
	return false; // single shot — unregister
}

IMPLEMENT_MODULE(FNetcodePlus, NetcodePlus)

// =============================================================================
// F5 -> ncpmenu, via a Slate input pre-processor.
//
// The old DebugExecBindings approach only fires in non-shipping builds, so it
// never worked for shipped players. A pre-processor sees the key globally in
// every build, independent of the user's Input.ini and UT's CustomBinds. F5 is
// consumed and routed straight to HandleNCPMenu (a toggle: opens/closes).
// =============================================================================
class FNCPlusHotkeyInput : public IInputProcessor
{
public:
	virtual void Tick(const float /*DeltaTime*/, FSlateApplication& /*SlateApp*/, TSharedRef<ICursor> /*Cursor*/) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& /*SlateApp*/, const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.GetKey() == EKeys::F5 && !InKeyEvent.IsRepeat())
		{
			HandleNCPMenu(TArray<FString>());
			return true; // consume F5 so it doesn't fall through to game input
		}
		return false;
	}
};

static TSharedPtr<FNCPlusHotkeyInput> GNCPlusHotkeyInput;

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

	// F5 -> ncpmenu (client only), via a Slate input pre-processor. Works in all
	// builds, unlike the old DebugExecBindings approach which only fired in
	// non-shipping. See FNCPlusHotkeyInput above.
	if (!IsRunningDedicatedServer() && FSlateApplication::IsInitialized())
	{
		GNCPlusHotkeyInput = MakeShareable(new FNCPlusHotkeyInput());
		FSlateApplication::Get().RegisterInputPreProcessor(GNCPlusHotkeyInput);
	}

	// -ncpconnect=IP:port?Password=pw — launcher direct-connect (real clients only).
	// Register the ticker ONLY when the arg is present, so there is zero overhead
	// on normal launches. bShouldStopOnComma=false keeps a comma in the password.
	if (!IsRunningDedicatedServer() && !GIsEditor)
	{
		FString ConnectURL;
		if (FParse::Value(FCommandLine::Get(), TEXT("ncpconnect="), ConnectURL, /*bShouldStopOnComma=*/ false)
			&& !ConnectURL.IsEmpty())
		{
			GNcpConnectURL = ConnectURL;
			GNcpConnectElapsed = 0.0f;
			GNcpConnectTickerHandle = FTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&TickNcpConnect), 0.0f);
		}
	}

	UE_LOG(LogLoad, Log, TEXT("netcodeplus loaded"));
}

void FNetcodePlus::ShutdownModule()
{
	// Unregister the F5 hotkey pre-processor.
	if (GNCPlusHotkeyInput.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(GNCPlusHotkeyInput);
		GNCPlusHotkeyInput.Reset();
	}

	// Stop the -ncpconnect ticker if it never fired.
	if (GNcpConnectTickerHandle.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(GNcpConnectTickerHandle);
		GNcpConnectTickerHandle.Reset();
	}

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
