// NCPlusHostPause.cpp — see header.
#include "NCPlusHostPause.h"
#include "UnrealTournament.h"
#include "UTBaseGameMode.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"   // ClientSay
#include "UTCountDownMessage.h"        // big center-screen countdown (stock localized message)
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

// ── Captain pause ([NetcodePlus] bAllowCaptainPause + CaptainPauseCooldownSec) ──
// The bot passes ?Captains=<id>,<id> (top-ELO per team) so each side has a
// pause-capable player. Read once per process (server restart applies changes).
static bool  GCaptainPauseLoaded      = false;
static bool  GCaptainPauseEnabled     = false;
static int32 GCaptainPauseCooldownSec = 8;
static float GLastCaptainPauseTime    = -1.f;   // world seconds of the last captain pause (one match per server)

static void LoadCaptainPauseConfig()
{
	if (GCaptainPauseLoaded)
	{
		return;
	}
	GCaptainPauseLoaded = true;

	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	GConfig->GetBool(TEXT("NetcodePlus"), TEXT("bAllowCaptainPause"), GCaptainPauseEnabled, ModIni);
	GConfig->GetInt(TEXT("NetcodePlus"), TEXT("CaptainPauseCooldownSec"), GCaptainPauseCooldownSec, ModIni);
	if (GCaptainPauseCooldownSec < 0)
	{
		GCaptainPauseCooldownSec = 0;
	}
}

// ── Unpause countdown ────────────────────────────────────────────────────────
// [NetcodePlus] UnpauseCountdownSec (default 7; 0 = disabled). Cached per process.
static bool  GUnpauseCfgLoaded = false;
static int32 GUnpauseSec       = 7;

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
			// Big center-screen countdown: the stock round-start counter (UUTCountDownMessage via the
			// stock ClientReceiveLocalizedMessage RPC — exactly like ElimPlus/Wipeout's round countdown).
			// Both the message class AND the RPC are stock, so it draws on EVERY client (incl. un-updated /
			// no-launcher) with no version bump — server-only. Shows N big in the Announcements area.
			UTPC->ClientReceiveLocalizedMessage(UUTCountDownMessage::StaticClass(), N);

			// Keep the system chat line as a paused-proof fallback: chat renders while paused, so if the
			// localized message's lifetime/scale animation misbehaves on the frozen game clock, the
			// countdown is still visible. nullptr speaker = system line (mirrors WipeoutGame's).
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

	bool CaptainMayPause(APlayerController* PC, AUTBaseGameMode* GM)
	{
		LoadCaptainPauseConfig();
		if (!GCaptainPauseEnabled || PC == nullptr || GM == nullptr)
		{
			return false;
		}
		AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
		if (PS == nullptr || !PS->UniqueId.IsValid())
		{
			return false;
		}
		UWorld* World = GM->GetWorld();
		if (World == nullptr)
		{
			return false;
		}

		// The bot passes ?Captains=<id>,<id> (top-ELO player per team) ONLY for PUGs,
		// so its presence doubles as the PUG gate. Parsed per call — pause attempts are
		// rare, so no cache (avoids stale-match state).
		const FString CaptainsOpt = World->URL.GetOption(TEXT("Captains="), TEXT(""));
		if (CaptainsOpt.IsEmpty())
		{
			return false;
		}

		const FString MyId = PS->UniqueId.ToString();
		TArray<FString> Ids;
		CaptainsOpt.ParseIntoArray(Ids, TEXT(","), /*CullEmpty=*/true);
		bool bIsCaptain = false;
		for (const FString& Id : Ids)
		{
			// UE 4.15 has no FString::TrimStartAndEnd; .Trim()/.TrimTrailing() mutate in place.
			FString Trimmed = Id;
			Trimmed.Trim();
			Trimmed.TrimTrailing();
			if (Trimmed.Equals(MyId, ESearchCase::IgnoreCase))
			{
				bIsCaptain = true;
				break;
			}
		}
		if (!bIsCaptain)
		{
			return false;
		}

		// Anti-spam: throttle a NEW captain pause (world currently unpaused) by a shared
		// cooldown. World time is frozen while paused and resets per map, so a stored time
		// in the future = a new match → reset (otherwise the first pause of the next match
		// would be wrongly blocked). The cooldown is measured in UNPAUSED seconds, which is
		// exactly "don't re-pause too soon after resuming". Unpause is never throttled.
		AWorldSettings* WS = GM->GetWorldSettings();
		const bool bWouldBeNewPause = (WS != nullptr && WS->Pauser == nullptr);
		if (bWouldBeNewPause)
		{
			const float Now = World->GetTimeSeconds();
			if (Now < GLastCaptainPauseTime)
			{
				GLastCaptainPauseTime = -1.f;   // new map / match
			}
			if (GCaptainPauseCooldownSec > 0 && GLastCaptainPauseTime >= 0.f
				&& (Now - GLastCaptainPauseTime) < static_cast<float>(GCaptainPauseCooldownSec))
			{
				return false;   // too soon after the last captain pause
			}
			GLastCaptainPauseTime = Now;
			UE_LOG(LogNCHostPause, Warning, TEXT("[CaptainPause] %s paused the match"), *PS->PlayerName);
		}
		return true;
	}

	bool MayPause(APlayerController* PC, AUTBaseGameMode* GM)
	{
		return HostMayPause(PC, GM) || CaptainMayPause(PC, GM);
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
