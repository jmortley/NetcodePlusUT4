// NCPlusHostPause.cpp — see header.
#include "NCPlusHostPause.h"
#include "UnrealTournament.h"
#include "UTBaseGameMode.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"   // ClientSay
#include "UTATypes.h"                 // ChatDestinations
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"  // AWorldSettings::Pauser
#include "Containers/Ticker.h"
#include "Misc/ConfigCacheIni.h"          // GConfig
#include "Misc/Paths.h"                    // FPaths

DEFINE_LOG_CATEGORY_STATIC(LogNCHostPause, Log, All);

// Read once per process (server restart applies changes) — same convention as
// the other [NetcodePlus] Mod.ini keys.
static bool GHostPauseLoaded  = false;
static bool GHostPauseEnabled = false;   // [NetcodePlus] bAllowHostPause

static void LoadHostPauseConfig()
{
	if (GHostPauseLoaded)
	{
		return;
	}
	GHostPauseLoaded = true;

	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	GConfig->GetBool(TEXT("NetcodePlus"), TEXT("bAllowHostPause"), GHostPauseEnabled, ModIni);
}

// ── Unpause countdown ────────────────────────────────────────────────────────
// [NetcodePlus] UnpauseCountdownSec (default 3; 0 = disabled). Cached per process.
static bool  GUnpauseCfgLoaded = false;
static int32 GUnpauseSec       = 3;

static void LoadUnpauseConfig()
{
	if (GUnpauseCfgLoaded) return;
	GUnpauseCfgLoaded = true;
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	GConfig->GetInt(TEXT("NetcodePlus"), TEXT("UnpauseCountdownSec"), GUnpauseSec, ModIni);
	if (GUnpauseSec < 0) GUnpauseSec = 0;
}

// Live countdown state (one match per server, so file-static is fine; CreateStatic
// ticker can't bind a member). GFiring marks the re-entrant ClearPause our own ticker
// fires so the override passes through to Super instead of re-deferring.
static TWeakObjectPtr<AUTBaseGameMode> GUnpauseGM;
static int32         GUnpauseRemain = 0;
static bool          GUnpauseActive = false;
static bool          GUnpauseFiring = false;
static FDelegateHandle GUnpauseTicker;

static void BroadcastResuming(AUTBaseGameMode* GM, int32 N)
{
	UWorld* W = GM ? GM->GetWorld() : nullptr;
	if (!W) return;
	const FString Msg = FString::Printf(TEXT("Resuming in %d..."), N);
	for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* UTPC = Cast<AUTPlayerController>(It->Get()))
		{
			// nullptr speaker = system line (mirrors WipeoutGame's system messages).
			UTPC->ClientSay(nullptr, Msg, ChatDestinations::System);
		}
	}
}

// Pause-immune: fires while WorldSettings->Pauser freezes the world. Returns true to
// keep ticking (re-fire after the 1s period), false to stop (auto-removes the ticker).
static bool UnpauseTick(float /*DeltaTime*/)
{
	AUTBaseGameMode* GM = GUnpauseGM.Get();
	if (GM == nullptr)   // gamemode gone (level travel / GC) — abort cleanly, no wedge.
	{
		GUnpauseActive = false;
		return false;
	}

	GUnpauseRemain--;
	if (GUnpauseRemain > 0)
	{
		BroadcastResuming(GM, GUnpauseRemain);
		return true;     // keep counting
	}

	// Reached zero — perform the real unpause. Re-enter ClearPause with GFiring set so
	// the mode override falls through to Super::ClearPause() (which removes the stock
	// pauser delegate and nulls WorldSettings->Pauser).
	GUnpauseActive = false;
	GUnpauseFiring = true;
	GM->ClearPause();
	return false;        // stop ticker
}

namespace NCPlusHostPause
{
	bool HostMayPause(APlayerController* PC, AUTBaseGameMode* GM)
	{
		LoadHostPauseConfig();
		if (!GHostPauseEnabled || PC == nullptr || GM == nullptr)
		{
			return false;
		}
		AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
		if (PS == nullptr)
		{
			return false;
		}
		// Engine flag (set while the host loop recognized them in warmup), OR the
		// direct server-side id compare — identical match to the engine host loop
		// (UTGameMode::ReadyToStartMatch). Both are server-local reads here, so no
		// replication caveats apply.
		if (PS->bIsMatchHost)
		{
			return true;
		}
		const FString HostId = GM->GetHostId();
		return !HostId.IsEmpty() && PS->UniqueId.IsValid()
			&& HostId.Equals(PS->UniqueId.ToString(), ESearchCase::IgnoreCase);
	}

	bool DeferUnpauseForCountdown(AUTBaseGameMode* GM)
	{
		LoadUnpauseConfig();
		if (GUnpauseSec <= 0 || GM == nullptr)
		{
			return false;   // feature disabled -> immediate (stock) unpause
		}

		// Re-entrant call from our own ticker completion -> let Super::ClearPause run.
		if (GUnpauseFiring)
		{
			GUnpauseFiring = false;
			return false;
		}

		// Abandon a countdown left over from a PRIOR match/level: the file-static state
		// and the core ticker survive seamless travel, so a stale GUnpauseActive could
		// otherwise intercept this match's first unpause. If the still-"active" countdown
		// belongs to a different (or dead) gamemode, drop it and fall through to start fresh.
		if (GUnpauseActive && GUnpauseGM.Get() != GM)
		{
			FTicker::GetCoreTicker().RemoveTicker(GUnpauseTicker);
			GUnpauseActive = false;
		}

		// Already counting down for THIS match -> keep deferring; don't restart the timer.
		if (GUnpauseActive)
		{
			return true;
		}

		// Only defer a genuine unpause. If the world isn't actually paused, this is one
		// of the many other ClearPause callers (match transitions, logout cleanup with
		// an empty list, teardown) -> pass straight through.
		AWorldSettings* WS = GM->GetWorldSettings();
		if (WS == nullptr || WS->Pauser == nullptr)
		{
			return false;
		}

		// Start the countdown on a pause-immune core ticker (FTimerManager/Tick are
		// frozen while paused). Broadcast the first tick now so N..1 reads naturally.
		GUnpauseActive = true;
		GUnpauseFiring = false;
		GUnpauseRemain = GUnpauseSec;
		GUnpauseGM     = GM;
		BroadcastResuming(GM, GUnpauseRemain);
		GUnpauseTicker = FTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&UnpauseTick), 1.0f);
		UE_LOG(LogNCHostPause, Log, TEXT("[HostPause] unpause requested — holding %ds countdown"), GUnpauseSec);
		return true;   // defer: stay paused until the countdown fires the real clear
	}
}
