// NetcodePlus.cpp
#include "NetcodePlus.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "UObject/UObjectGlobals.h"   // FCoreUObjectDelegates::PreLoadMap
#include "UTPlayerController.h"
#include "UTPlayerInput.h"
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
#include "NCPlusForceModels.h"

// -ncpconnect launcher direct-connect support
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreMisc.h"
#include "Misc/ConfigCacheIni.h"
#include "Engine/GameInstance.h"

/** Weak reference to active skin selector — only one can be open at a time */
static TWeakPtr<SUTWeaponSkinSelector> ActiveSkinSelector;

/** Weak reference to active NCP menu — only one can be open at a time */
static TWeakPtr<SUTNCPlusMenu> ActiveNCPMenu;

/** Weak reference to active cosmetic selector */
static TWeakPtr<SUTCosmeticSelector> ActiveCosmeticSelector;

/** Weak reference to active HUD layout editor */
static TWeakPtr<SNCPlusHUDEditor>      ActiveHUDEditor;
static TWeakPtr<SNCPlusHUDDragOverlay> ActiveDragOverlay;

/** PreLoadMap delegate handle — self-heals the menu input state across level loads. */
static FDelegateHandle GNCPPreLoadMapHandle;

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

	// Optional first arg picks the opening tab: `ncpmenu forcemodels` (or the
	// teamskins/models synonyms) → Force Models, `general` → General; bare
	// `ncpmenu` / F5 → About.
	ENCPMenuTab InitialTab = ENCPMenuTab::About;
	if (Args.Num() > 0)
	{
		const FString Tab = Args[0].ToLower();
		if (Tab == TEXT("forcemodels") || Tab == TEXT("teamskins") || Tab == TEXT("models"))
		{
			InitialTab = ENCPMenuTab::ForceModels;
		}
		else if (Tab == TEXT("general"))
		{
			InitialTab = ENCPMenuTab::General;
		}
	}

	TSharedRef<SUTNCPlusMenu> Menu =
		SNew(SUTNCPlusMenu)
		.PlayerOwner(LP)
		.InitialTab(InitialTab);

	ViewportClient->AddViewportWidgetContent(Menu, 100);
	FSlateApplication::Get().SetKeyboardFocus(Menu, EFocusCause::SetDirectly);

	ActiveNCPMenu = Menu;
}

// On a level transition the GameViewportClient drops our menu widgets WITHOUT
// calling ClosePanel(). Each panel's destructor now releases its own
// NCPlusHUDDragMode count (RAII), but close the F5 menu cleanly here first while
// the outgoing PC is still valid (so its input mode is properly restored), then
// hard-clear the refcount as a final backstop against any leaked count.
static void OnNCPPreLoadMap(const FString& /*MapName*/)
{
	if (ActiveNCPMenu.IsValid())
	{
		ActiveNCPMenu.Pin()->ClosePanel();
		ActiveNCPMenu.Reset();
	}
	NCPlusHUDDragMode::Reset();
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

// ---------------------------------------------------------------------------
// HUD team-colour recolour (Force Models "HUD" flag). Persistent client core ticker,
// self-throttled to ~4 Hz (matching the BP's "Update team colour" 0.25s timer), that
// re-asserts each team's TeamColor to the configured skin colour. Re-assertion is needed
// because TeamColor is replicated and the server periodically reverts it. No-op unless a
// game world exists and the feature + HUD flag are on (see NCPlusForceModels::SyncHudTeamColours).
// ---------------------------------------------------------------------------
static FDelegateHandle GHudColourTickerHandle;
static float           GHudColourAccum = 0.0f;

static bool TickHudTeamColours(float DeltaTime)
{
	// Flag-cloth wind needs a per-frame update (smooth gusting/direction); the colour/outline work is
	// throttled to ~4 Hz (matching the BP's 0.25s "Update team colour" timer).
	GHudColourAccum += DeltaTime;
	const bool bSlowTick = (GHudColourAccum >= 0.25f);
	if (bSlowTick) { GHudColourAccum = 0.0f; }

	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World())
			{
				UWorld* const W = Context.World();
				NCPlusForceModels::TickFlagWind(W, DeltaTime);   // every frame
				if (bSlowTick)
				{
					NCPlusForceModels::SyncHudTeamColours(W);
					NCPlusForceModels::SyncFlagColours(W);
					NCPlusForceModels::SuppressFlagCarrierOutlines(W);
				}
				break;
			}
		}
	}
	return true; // persistent
}

IMPLEMENT_MODULE(FNetcodePlus, NetcodePlus)

// Debug: dump the current ForceModels config + every installed AUTCharacterContent path,
// so you can copy a valid path into [ForceModels.Model.<side>] Class= before the picker UI exists.
static void HandleForceModelsList(const TArray<FString>& Args)
{
	const FNCPlusForceModelsConfig& C = NCPlusForceModels::Get();
	UE_LOG(LogTemp, Warning, TEXT("[ForceModels] Enabled=%d Models=%d Style=%d"),
		C.bEnabled ? 1 : 0, C.bModels ? 1 : 0, (int32)C.Style);
	UE_LOG(LogTemp, Warning, TEXT("[ForceModels]   Enemy='%s' Team='%s' Red='%s' Blue='%s'"),
		*C.Enemy.ContentPath, *C.Team.ContentPath, *C.Red.ContentPath, *C.Blue.ContentPath);

	// Audit path: include the curated-out (HiddenModels) entries too, marked [HIDDEN], so you can see the
	// full set and decide what to add to [ForceModels] HiddenModels=.
	TArray<NCPlusForceModels::FContentEntry> Entries;
	NCPlusForceModels::EnumerateContent(Entries, /*bIncludeHidden=*/true);
	int32 NumHidden = 0;
	for (const NCPlusForceModels::FContentEntry& E : Entries) { if (E.bHidden) { ++NumHidden; } }
	UE_LOG(LogTemp, Warning, TEXT("[ForceModels] %d installed character(s) (%d hidden via drop-list / HiddenModels / bHideInUI). Name -> Class path:"), Entries.Num(), NumHidden);
	for (const NCPlusForceModels::FContentEntry& E : Entries)
	{
		UE_LOG(LogTemp, Warning, TEXT("  %s%s  ->  %s"), E.bHidden ? TEXT("[HIDDEN] ") : TEXT(""), *E.DisplayName, *E.ClassPath);
	}
}

// On-demand: dump every visible character's body materials + parameter names. No respawn needed,
// works in online/Shipping clients (Warning verbosity survives Shipping; not #if'd out).
static void HandleForceModelsDumpMats(const TArray<FString>& /*Args*/, UWorld* World)
{
	NCPlusForceModels::DumpAllCharacterMaterials(World);
}

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

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("forcemodels_list"),
		TEXT("Log the current ForceModels config + every installed AUTCharacterContent class path"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleForceModelsList),
		ECVF_Default
	);

	IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("forcemodels_dumpmats"),
		TEXT("Dump every visible character's body materials + parameter names (recolour diagnostics; on-demand, Shipping-safe)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleForceModelsDumpMats),
		ECVF_Default
	);

	// Bind F5 -> ncpmenu by SEEDING the UUTPlayerInput CDO's CustomBinds at startup (pure run-once).
	// The CDO exists by now (GetMutableDefault constructs + config-loads it if needed), so this adds on top
	// of whatever Input.ini had; new UUTPlayerInput instances inherit the CDO's CustomBinds, and
	// UTForceRebuildingKeyMaps(true) (the settings "restore defaults" path) copies the CDO too. add-if-missing.
	// Client-only. F5 is otherwise free (old CTFPlus-UI BP retired). NB if F5 still doesn't bind, instances
	// are re-LoadConfig'ing CustomBinds from the file over the CDO inheritance — fall back to binding the
	// live UUTPlayerInput via a per-map ticker.
	if (!IsRunningDedicatedServer())
	{
		UUTPlayerInput* InputCDO = GetMutableDefault<UUTPlayerInput>();
		bool bHasNcpMenuBind = false;
		for (const FCustomKeyBinding& B : InputCDO->CustomBinds)
		{
			if (B.Command.Contains(TEXT("ncpmenu"))) { bHasNcpMenuBind = true; break; }
		}
		if (!bHasNcpMenuBind)
		{
			InputCDO->CustomBinds.Add(FCustomKeyBinding(FName(TEXT("F5")), IE_Pressed, TEXT("ncpmenu")));
			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: F5 -> ncpmenu seeded on UUTPlayerInput CDO"));
		}

		// Spectators take a SEPARATE bind path: UUTPlayerInput::ExecuteCustomBind checks
		// SpectatorBinds first while bOnlySpectator/bOutOfLives, so the CustomBinds seed
		// above never fires F5 for a spectator. Seed it into SpectatorBinds too.
		bool bHasNcpMenuSpecBind = false;
		for (const FCustomKeyBinding& B : InputCDO->SpectatorBinds)
		{
			if (B.Command.Contains(TEXT("ncpmenu"))) { bHasNcpMenuSpecBind = true; break; }
		}
		if (!bHasNcpMenuSpecBind)
		{
			InputCDO->SpectatorBinds.Add(FCustomKeyBinding(FName(TEXT("F5")), IE_Pressed, TEXT("ncpmenu")));
			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: F5 -> ncpmenu seeded on UUTPlayerInput CDO SpectatorBinds"));
		}
	}

	// Self-heal the menu input state across level transitions: LoadMap drops our
	// viewport widgets without ClosePanel, which would otherwise leak a
	// NCPlusHUDDragMode count and strand the cursor on the next map. Client only.
	if (!IsRunningDedicatedServer())
	{
		GNCPPreLoadMapHandle = FCoreUObjectDelegates::PreLoadMap.AddStatic(&OnNCPPreLoadMap);
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

	// HUD team-colour recolour ticker (Force Models "HUD" flag). Client-only; cheap no-op until a
	// game world exists and the feature + HUD flag are enabled. Self-throttled to ~4 Hz.
	if (!IsRunningDedicatedServer())
	{
		GHudColourAccum = 0.0f;
		GHudColourTickerHandle = FTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&TickHudTeamColours), 0.0f);
	}

	UE_LOG(LogLoad, Log, TEXT("netcodeplus loaded"));
}

void FNetcodePlus::ShutdownModule()
{
	// Stop the -ncpconnect ticker if it never fired.
	if (GNcpConnectTickerHandle.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(GNcpConnectTickerHandle);
		GNcpConnectTickerHandle.Reset();
	}

	// Stop the HUD team-colour ticker.
	if (GHudColourTickerHandle.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(GHudColourTickerHandle);
		GHudColourTickerHandle.Reset();
	}

	// Close skin selector if open and free cached assets
	if (ActiveSkinSelector.IsValid())
	{
		ActiveSkinSelector.Pin()->ClosePanel();
		ActiveSkinSelector.Reset();
	}
	SUTWeaponSkinSelector_CleanupCache();

	if (GNCPPreLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PreLoadMap.Remove(GNCPPreLoadMapHandle);
		GNCPPreLoadMapHandle.Reset();
	}

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
