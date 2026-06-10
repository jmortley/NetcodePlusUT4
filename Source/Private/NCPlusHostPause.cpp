// NCPlusHostPause.cpp — see header.
#include "NCPlusHostPause.h"
#include "UnrealTournament.h"
#include "UTBaseGameMode.h"
#include "UTPlayerState.h"

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
}
