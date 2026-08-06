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

	/** The pause-authority check each NCPlus mode's AllowPausing override calls:
	 *  true if PC is the host (HostMayPause) OR a bot-designated team captain
	 *  (CaptainMayPause). */
	NETCODEPLUS_API bool MayPause(APlayerController* PC, AUTBaseGameMode* GM);

	/** True when [NetcodePlus] bAllowCaptainPause is set AND PC's UniqueId is one of
	 *  the IDs in the match's ?Captains=<id>,<id> launch option. The bot passes the
	 *  top-ELO player on each team (only for PUGs), so each team has a pause-capable
	 *  player — a lone host on the opposite team can no longer strand a team that
	 *  needs to pause. Captains pause/unpause through the same stock path as the host
	 *  (so the unpause countdown + logout-cleanup safety apply identically). A shared
	 *  anti-spam cooldown gates repeated pauses: [NetcodePlus] CaptainPauseCooldownSec
	 *  (default 8; 0 = off). Server-only; ?Captains is only honored for PUGs (the bot
	 *  only sends it there). */
	NETCODEPLUS_API bool CaptainMayPause(APlayerController* PC, AUTBaseGameMode* GM);

	/** Call at the TOP of a gamemode's ClearPause() override. Returns true if the
	 *  unpause was DEFERRED behind a short server-only "Resuming in N..." countdown —
	 *  the override must then return false (stay paused). Returns false if the caller
	 *  should proceed with Super::ClearPause() immediately: feature disabled, the world
	 *  isn't actually paused (so every non-unpause ClearPause passes straight through),
	 *  or the countdown just completed and is firing the real clear.
	 *
	 *  Gated by [NetcodePlus] UnpauseCountdownSec (default 7; 0 = disabled = instant
	 *  unpause as before). Driven by a pause-immune core FTicker (the world is frozen
	 *  while paused) that is SELF-COMPLETING — worst case it always resumes after the
	 *  countdown, so it can never wedge a match even if the pauser disconnects. Only
	 *  affects the stock pause path (host via [NetcodePlus] bAllowHostPause, or rcon);
	 *  auto-pause-on-drop clears WorldSettings->Pauser directly and is unaffected. */
	NETCODEPLUS_API bool DeferUnpauseForCountdown(AUTBaseGameMode* GM);

	/** Call right after ANY successful unpause (ClearPause override post-Super, or a
	 *  direct WorldSettings->Pauser clear). Forces an immediate ReplicatedWorldTimeSeconds
	 *  refresh + net update so clients' GetServerWorldTimeSeconds estimates re-sync NOW
	 *  instead of at the engine's 5s cadence — a pause freezes the server clock while
	 *  unpaused clients keep counting, and until the re-sync every fire RPC used to fail
	 *  ValidateFireRequest's 1s timestamp gate (fire-dead players, the 13.3s desync
	 *  log bursts of 2026-08-06). Server-only no-op elsewhere. */
	NETCODEPLUS_API void ResyncServerWorldTime(class AGameModeBase* GM);
}
