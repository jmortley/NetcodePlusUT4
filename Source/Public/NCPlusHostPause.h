// NCPlusHostPause.h — Mod.ini-gated match-host pause.
//
// [NetcodePlus] bAllowHostPause=true (default OFF) lets the match host — the
// ?HostId= player, the same identity that gates Enter-to-start — pause and
// unpause with the stock `pause` console command. The whole round trip is
// stock machinery (APlayerController::Pause exec → ServerPause RPC → the
// gamemode's AllowPausing gate, which each NCPlus mode overrides to add this
// check), so it deploys server-only: no client roll, no version bump. Clients
// already render the state (UTHUD "GAME IS PAUSED") and freeze movement off
// the replicated WorldSettings->Pauser.
//
// Rcon admins can already pause via AUTGameMode::AllowPausing — this only ADDS
// the host. If the pauser disconnects, UTBaseGameMode's logout cleanup clears
// the pause, so a vanished host can't wedge the match.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class APlayerController;
class AUTBaseGameMode;

namespace NCPlusHostPause
{
	/** True when [NetcodePlus] bAllowHostPause is set in the server's Mod.ini
	 *  and PC is the match host (bIsMatchHost, or direct GetHostId() vs
	 *  UniqueId compare — the same match the engine host loop uses). */
	NETCODEPLUS_API bool HostMayPause(APlayerController* PC, AUTBaseGameMode* GM);

	/** Call at the TOP of a gamemode's ClearPause() override. Returns true if the
	 *  unpause was DEFERRED behind a short server-only "Resuming in N..." countdown —
	 *  the override must then return false (stay paused). Returns false if the caller
	 *  should proceed with Super::ClearPause() immediately: feature disabled, the world
	 *  isn't actually paused (so every non-unpause ClearPause passes straight through),
	 *  or the countdown just completed and is firing the real clear.
	 *
	 *  Gated by [NetcodePlus] UnpauseCountdownSec (default 3; 0 = disabled = instant
	 *  unpause as before). Driven by a pause-immune core FTicker (the world is frozen
	 *  while paused) that is SELF-COMPLETING — worst case it always resumes after the
	 *  countdown, so it can never wedge a match even if the pauser disconnects. Only
	 *  affects the stock pause path (host via [NetcodePlus] bAllowHostPause, or rcon);
	 *  auto-pause-on-drop clears WorldSettings->Pauser directly and is unaffected. */
	NETCODEPLUS_API bool DeferUnpauseForCountdown(AUTBaseGameMode* GM);
}
