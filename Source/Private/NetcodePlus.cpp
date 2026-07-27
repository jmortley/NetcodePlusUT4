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
#include "UTGameState.h"
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
#include "NCPlusVersionGate.h"        // hub advisor registration (whisper-mode version gate)

// -ncpconnect launcher direct-connect support
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Char.h"                 // FChar::IsAlnum (launcher credential handoff)
#include "HAL/PlatformMisc.h"          // Get/SetEnvironmentVar (launcher credential handoff)
#include "Misc/CoreMisc.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"                // FPaths::GeneratedConfigDir (NoAlias Mod.ini scrub)
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
			// Per-weapon hide — keyed by CLASS NAME (2026-07-19 fix: this used to
			// key by WeaponSkinCustomizationTag, which BringUp / TeamArenaCharacter /
			// LoadWeaponSettings never read — the entry only worked until respawn).
			// Works for any weapon class now, stock included.
			AUTCharacter* UTChar = Cast<AUTCharacter>(PC->GetPawn());
			if (UTChar && UTChar->GetWeapon())
			{
				AUTWeapon* CurWeap = UTChar->GetWeapon();
				const FName HideKey = FName(*CurWeap->GetClass()->GetName());
				AUTWeaponFix::HiddenWeaponsByTag.Add(HideKey, true);
				AUTWeaponFix::SaveWeaponSettings();
				AUTWeaponFix::ApplyWeaponHideState(CurWeap, UTChar, true);
				PC->ClientMessage(FString::Printf(TEXT("Hidden: %s (saved to Mod.ini)"), *HideKey.ToString()));
			}
			return;
		}
		else if (Hand == TEXT("show") || Hand == TEXT("s"))
		{
			// Per-weapon show — unhides the CURRENT weapon by its class name
			AUTCharacter* UTChar = Cast<AUTCharacter>(PC->GetPawn());
			if (UTChar && UTChar->GetWeapon())
			{
				AUTWeapon* CurWeap = UTChar->GetWeapon();
				const FName HideKey = FName(*CurWeap->GetClass()->GetName());
				AUTWeaponFix::HiddenWeaponsByTag.Add(HideKey, false);
				AUTWeaponFix::SaveWeaponSettings();
				AUTWeaponFix::ApplyWeaponHideState(CurWeap, UTChar, false);
				PC->ClientMessage(FString::Printf(TEXT("Shown: %s (saved to Mod.ini)"), *HideKey.ToString()));
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
// Launcher credential handoff (NCP_AUTH_PASSWORD -> -AUTH_PASSWORD).
//
// The engine writes the whole command line to the log ("LogInit: Command line:",
// LaunchEngineLoop AppInit) and into the CommandLine property of every crash
// report. Players routinely post those files publicly when asking for help, so
// the one-shot Epic exchange code the UT4 Community Launcher passes as
// -AUTH_PASSWORD leaks with them — a live login credential until it is redeemed.
//
// Launcher builds that detect this handoff in the installed plugin pass the code
// in the NCP_AUTH_PASSWORD environment variable instead of on the command line,
// and we append it back here. FCommandLine::Append writes ONLY the live command
// line: the two copies the engine keeps for logging are snapshotted in
// FCommandLine::Set and never rebuilt, so an appended value can never reach the
// log line or the crash context. Environment variables are not logged.
//
// Timing: StartupModule runs after the engine has already logged the command
// line, and well before anything reads AUTH_PASSWORD (the MCP autologin, the
// login hook DLL, and the EULA check in UUTGameEngine all run later in startup).
// ---------------------------------------------------------------------------
static void ApplyLauncherAuthHandoff()
{
	// 4.15 only offers the fixed-buffer form of GetEnvironmentVariable. Exchange
	// codes are 32 hex characters; 128 is headroom with no heap allocation.
	TCHAR CodeBuf[128] = { 0 };
	FPlatformMisc::GetEnvironmentVariable(TEXT("NCP_AUTH_PASSWORD"), CodeBuf, ARRAY_COUNT(CodeBuf));
	CodeBuf[ARRAY_COUNT(CodeBuf) - 1] = 0;

	FString Code(CodeBuf);

	// Clear it either way: nothing else needs it, and it keeps the code out of the
	// environment block this process would hand to anything it later spawns.
	FPlatformMisc::SetEnvironmentVar(TEXT("NCP_AUTH_PASSWORD"), TEXT(""));

	Code.Trim();
	Code.TrimTrailing();
	if (Code.IsEmpty())
	{
		return;
	}

	// The append is parser input, so accept only a bare token — whitespace, a
	// quote or a dash in this value could otherwise inject further switches.
	for (int32 Index = 0; Index < Code.Len(); ++Index)
	{
		if (!FChar::IsAlnum(Code[Index]) && Code[Index] != TEXT('_'))
		{
			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: ignoring malformed NCP_AUTH_PASSWORD"));
			return;
		}
	}

	// An explicit -AUTH_PASSWORD wins (a hand-made shortcut, or a launcher that
	// sent both): never append a second one for FParse to pick between.
	FString Existing;
	if (FParse::Value(FCommandLine::Get(), TEXT("AUTH_PASSWORD="), Existing))
	{
		return;
	}

	FCommandLine::Append(*FString::Printf(TEXT(" -AUTH_PASSWORD=%s"), *Code));
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

/** Connect anyway after this long if the MCP profile/progression never finish
 *  loading (offline / slow login) — the deadlock backstop for the readiness gate. */
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

	// Wait for MCP sign-in AND the cloud profile (keybinds) + progression (XP) to
	// finish downloading before we travel. IsLoggedIn() alone only means OSS auth is
	// done; the profile + progression cloud reads land asynchronously AFTER that, so
	// travelling on login alone races them — a fast client joins with DEFAULT BINDS
	// and 0 XP (and a later profile save-back can overwrite the cloud binds with those
	// defaults: the "binds lost" report). We mirror stock UT4's own data-ready gate,
	// IsPlayerCardDataLoaded() == IsLoggedIn() && !IsPendingMCPLoad() (SUTMenuBase.cpp),
	// which stock defines but never applies to its own join/travel path. Falls back on
	// the timeout below if MCP is slow/offline (same degraded outcome as before).
	UUTLocalPlayer* UTLP = nullptr;
	if (UGameInstance* GI = GameWorld->GetGameInstance())
	{
		UTLP = Cast<UUTLocalPlayer>(GI->GetFirstGamePlayer());
	}

	// All four accessors are public + tick-safe; the two getters are only null-COMPARED
	// (never dereferenced), so any not-yet-ready / early-null signal keeps bReady false
	// and we keep waiting. IsPendingMCPLoad() is the OR of the profile + progression
	// pending flags, so it is false only once BOTH cloud reads complete — exactly
	// "keybinds loaded AND XP loaded" in one read (UTLocalPlayer.cpp).
	const bool bReady = (UTLP
		&& UTLP->IsLoggedIn()
		&& !UTLP->IsPendingMCPLoad()
		&& UTLP->GetProfileSettings() != nullptr
		&& UTLP->GetProgressionStorage() != nullptr);
	const bool bTimedOut = (GNcpConnectElapsed >= GNcpConnectLoginTimeout);
	if (!bReady && !bTimedOut)
	{
		return true; // keep waiting for sign-in + profile/progression download
	}

	// Warning verbosity survives Shipping (Log/Verbose are stripped there).
	UE_LOG(LogLoad, Warning, TEXT("netcodeplus: -ncpconnect -> ClientTravel to %s%s"),
		*RedactConnectURL(GNcpConnectURL),
		bReady ? TEXT("") : TEXT(" (profile/progression not ready; connecting anyway after timeout)"));

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
// Post-match-join killcam crash guard (see TickHudTeamColours). GIRGuardWorld = the game world whose
// initial match state we've already evaluated (raw ptr, compared only, never dereferenced);
// GSavedInstantReplay = the user's UT.EnableInstantReplay value while we suppress it (-1 = not suppressing).
static UWorld*         GIRGuardWorld = nullptr;
static int32           GSavedInstantReplay = -1;

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

				// Post-match-join killcam crash guard. The stock killcam recorder
				// (AUTGameState::StartRecordingReplay, armed on a 0.5s timer in ReceivedGameModeClass) has NO
				// match-state gate, so a client that JOINS a server already in post-match bootstraps a _DeathCam
				// replay with no data -> a 2nd, local LoadMap -> the UPlayer::Exec world-mismatch assert
				// (Player.cpp:98). Its sole arming condition is the cvar UT.EnableInstantReplay==1. We force it
				// to 0 ONLY for a client that joined STRAIGHT INTO post-match (first time we see this world it is
				// already ended), and restore the user's value once a live match is in progress -- so a player
				// present the whole match (recorder already running) is never touched and still gets the
				// end-of-match replay. Client-only, no GameState subclass (replicated-class-identity crash, the
				// CTF ABI doctrine), no stock-source edit. HasMatchEnded()/IsMatchInProgress() are call-only
				// engine accessors; SetByConsole so it wins over however the user enabled it.
				if (AUTGameState* const GS = W->GetGameState<AUTGameState>())
				{
					if (IConsoleVariable* const CVarIR = IConsoleManager::Get().FindConsoleVariable(TEXT("UT.EnableInstantReplay")))
					{
						const bool bNewWorld = (W != GIRGuardWorld);
						GIRGuardWorld = W;
						if (bNewWorld && GSavedInstantReplay < 0 && GS->HasMatchEnded())
						{
							GSavedInstantReplay = CVarIR->GetInt();
							CVarIR->Set(TEXT("0"), ECVF_SetByConsole);
						}
						else if (GSavedInstantReplay >= 0 && !GS->HasMatchEnded())
						{
							// No longer post-match (same world restarted, or we travelled to the next match's
							// world while it's still in warmup) -> restore BEFORE that world's 0.5s recorder timer
							// fires, so the next match's killcam records normally.
							CVarIR->Set(*FString::FromInt(GSavedInstantReplay), ECVF_SetByConsole);
							GSavedInstantReplay = -1;
						}
					}
				}

				if (bSlowTick)
				{
					NCPlusForceModels::SyncHudTeamColours(W);
					NCPlusForceModels::SyncFlagColours(W);
					NCPlusForceModels::SuppressFlagCarrierOutlines(W);
				}
				// Per-frame: LOS traces gate the ForceModels outline (visible -> outlined, occluded ->
				// none — the stock stencil has no visible-only mode, see OutlinePlayers). bSlowTick
				// re-asserts + refreshes the MPC colours. After Suppress so carriers stay suppressed.
				NCPlusForceModels::OutlinePlayers(W, bSlowTick);
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

// =============================================================================
// NoAlias Mod.ini scrub — repair the [OldIdentifiers]/[Identifiers] damage left
// by the legacy NoAlias BP (cooked into hub paks; recooking needs every hub
// owner to act, so fix the DATA instead). The BP appends "<epicid>_<name>" on
// every launch with no dedupe, and the BP gamemodes' testing-era default player
// name ("Christian") injects a bogus rename event each session — the array
// ping-pongs real-name/Christian forever (observed ~110 entries) and the BP's
// join-time chat spam reads it all out. The BP reads these sections from Mod.ini
// via GConfig at map join, so sanitizing BEFORE world load (module startup +
// every PreLoadMap, catching mid-session re-pollution) starves the spam with no
// pak change. CLIENT-side; each client repairs only its own file. Conservative
// gate: act ONLY when the append-loop fingerprint (exact-duplicate entries) is
// present — a legit one-time rename history is never rewritten.
// =============================================================================
static FDelegateHandle GNCPNoAliasScrubMapHandle;

static void ScrubNoAliasIdentifiers()
{
	if (!GConfig) return;
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	TArray<FString> OldIds;
	GConfig->GetArray(TEXT("OldIdentifiers"), TEXT("IDArray"), OldIds, ModIni);
	if (OldIds.Num() == 0) return;

	TArray<FString> CurIds;
	GConfig->GetArray(TEXT("Identifiers"), TEXT("IDArray"), CurIds, ModIni);

	// ALWAYS: drop "old" entries equal to a CURRENT identifier — the BP re-appends
	// the current identity every launch, so without this every join announces
	// "I am also known as <my current name>", which is spam by definition, never
	// information. Exact-identity match only (same account id, same name); real
	// aliases are untouched. Runs regardless of the duplicate fingerprint below.
	TArray<FString> Cleaned = OldIds;
	for (const FString& Cur : CurIds) { Cleaned.Remove(Cur); }

	// Fingerprint for the DEEP clean: the BP's append loop produces exact
	// duplicates. No dupes = healthy rename history — only the self-identity
	// redundancy above is removed, nothing else is rewritten.
	TSet<FString> Unique;
	for (const FString& Id : OldIds) { Unique.Add(Id); }
	const bool bHasDupes = (Unique.Num() != OldIds.Num());
	if (!bHasDupes && Cleaned.Num() == OldIds.Num()) return;   // healthy and no self-entries: no write

	// "<32-hex-epicid>_<name>" -> name; empty when the entry doesn't match that shape.
	auto NamePart = [](const FString& Id) -> FString
	{
		if (Id.Len() <= 33 || Id[32] != TEXT('_')) return FString();
		for (int32 i = 0; i < 32; ++i) { if (!FChar::IsHexDigit(Id[i])) return FString(); }
		return Id.Mid(33);
	};
	// The BP gamemodes' testing-era DefaultPlayerName — the manufactured "alias".
	auto IsBogus = [&NamePart](const FString& Id)
	{
		return NamePart(Id).Equals(TEXT("Christian"), ESearchCase::CaseSensitive);
	};

	bool bCurChanged = false;
	if (bHasDupes)
	{
		// 1) Dedupe, preserving first-appearance order.
		TArray<FString> Deduped;
		for (const FString& Id : Cleaned) { Deduped.AddUnique(Id); }
		Cleaned = MoveTemp(Deduped);

		// 2) Drop the manufactured default-name entries.
		Cleaned.RemoveAll(IsBogus);

		// 3) Heal a stale [Identifiers] stuck on the bogus default: promote the most
		//    recent REAL alias (last well-formed, non-bogus occurrence in original order)
		//    belonging to the SAME 32-hex account — never another local account's alias
		//    (shared-machine files can interleave accounts). Replace only the offending
		//    last entry; any other current identifiers are left alone.
		if (CurIds.Num() > 0 && IsBogus(CurIds.Last()))
		{
			const FString AccountPrefix = CurIds.Last().Left(32);
			for (int32 i = OldIds.Num() - 1; i >= 0; --i)
			{
				if (!NamePart(OldIds[i]).IsEmpty() && !IsBogus(OldIds[i])
					&& OldIds[i].Left(32) == AccountPrefix)
				{
					CurIds.Last() = OldIds[i];
					bCurChanged = true;
					break;
				}
			}
		}

		// 4) The (possibly just-healed) current identity is not an "old" one.
		for (const FString& Cur : CurIds) { Cleaned.Remove(Cur); }

		// 5) Cap the tail (most recent 8) so the section can't grow without bound.
		if (Cleaned.Num() > 8) { Cleaned.RemoveAt(0, Cleaned.Num() - 8); }
	}

	GConfig->SetArray(TEXT("OldIdentifiers"), TEXT("IDArray"), Cleaned, ModIni);
	if (bCurChanged)
	{
		GConfig->SetArray(TEXT("Identifiers"), TEXT("IDArray"), CurIds, ModIni);
	}
	GConfig->Flush(false, ModIni);
	UE_LOG(LogLoad, Warning, TEXT("netcodeplus: NoAlias identifier scrub — %d -> %d old aliases%s"),
		OldIds.Num(), Cleaned.Num(), bCurChanged ? TEXT(" (stale current identity healed)") : TEXT(""));
}

static void ScrubNoAliasIdentifiersOnLoad(const FString& /*MapName*/)
{
	ScrubNoAliasIdentifiers();
}

// NCAmpRespawnFix.cpp — server-side amp respawn-interval correction (headerless: pure static hook,
// no UObject/UHT surface, keeps this a server-DLL-only change).
extern void RegisterNCAmpRespawnFix();
extern void UnregisterNCAmpRespawnFix();

void FNetcodePlus::StartupModule()
{
	// First: hand the launcher's login credential from the environment back onto
	// the command line, before any consumer reads it (see ApplyLauncherAuthHandoff).
	ApplyLauncherAuthHandoff();

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

	// NoAlias Mod.ini scrub (see ScrubNoAliasIdentifiers above): once at startup so
	// the first hub join reads clean data, and again before every map load so a
	// mid-session re-pollution never reaches the next join's chat spam. Client only;
	// the early-outs make the per-map cost two GetArray calls when healthy.
	if (!IsRunningDedicatedServer())
	{
		ScrubNoAliasIdentifiers();
		GNCPNoAliasScrubMapHandle = FCoreUObjectDelegates::PreLoadMap.AddStatic(&ScrubNoAliasIdentifiersOnLoad);
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

	// Hub advisor: on dedicated servers, auto-spawn the version gate in ADVISOR
	// mode (private whisper, never kick) for every lobby joiner — the hub is the
	// one place a plugin-less client still has working chat/UI to be told
	// anything. Instances keep the kick-mode gate spawned from each NCPlus
	// gamemode's PostLogin; the event handler filters on IsLobbyServer().
	if (IsRunningDedicatedServer())
	{
		NCPlusVersionGate::RegisterHubAdvisor();
	}

	// Amp respawn-interval fix: world-init hook, acts only on authority game worlds (the callback
	// itself no-ops for NM_Client), Mod.ini-gated ([NetcodePlus] AmpRespawnFix). See NCAmpRespawnFix.cpp.
	RegisterNCAmpRespawnFix();

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

	// Unbind the hub-advisor PostLogin hook (no-op if never registered).
	NCPlusVersionGate::UnregisterHubAdvisor();

	// Unbind the amp respawn-fix world-init hook.
	UnregisterNCAmpRespawnFix();

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

	if (GNCPNoAliasScrubMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PreLoadMap.Remove(GNCPNoAliasScrubMapHandle);
		GNCPNoAliasScrubMapHandle.Reset();
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
