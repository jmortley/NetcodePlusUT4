// NCPlusVersionGate.h — early plugin-version check (post-PostLogin, pre-match).
//
// The original BP version check fires after warmup starts, so an outdated client
// joins, learns the map / spawns / picks up the flag, then gets kicked when the
// match begins — wrecking the PUG. This C++ gate spawns a per-player owner-only
// AInfo right at PostLogin; the client's PostNetInit calls ServerReportVersion
// with NETCODE_PLUGIN_VERSION. Server compares: mismatch → kick immediately.
// 100-second timeout with no reply → broadcast a "missing plugin" notice, then kick 5s
// later (non-banning). Gated by a movement-corroboration guard: a MoveWatch latch
// confirms + destroys the gate the first time the owner ever moves (moving ⇒ loaded the
// NCPlus pawn ⇒ has the plugin), and pawnless-by-design owners (spectators / eliminated
// round players) are deferred rather than kicked — so a functional client whose handshake
// was merely lost is never kicked. See BeginPlay.
//
// Bots + the listen-host local PC are exempt (no remote client to handshake with).
//
// ADVISOR MODE (hubs): the same handshake, spawned at lobby PostLogin via the
// engine-level FGameModeEvents::GameModePostLoginEvent (no hub config change).
// On a hub the stock lobby HUD/chat work even for plugin-less clients — the one
// place they can actually be told anything — so no report → NEVER kick, just a
// private ClientSay whisper pointing at the launcher, repeated at a slow cadence
// while unconfirmed. A version MISMATCH gets a single "update via the launcher"
// whisper. ⚠️ Advisor state is server-side ONLY (plain members, no new RPCs or
// replicated properties): the class's net-field layout must stay identical to
// already-shipped client builds or the ServerReportVersion handshake breaks.
#pragma once

#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "NCPlusVersionGate.generated.h"

UCLASS()
class NETCODEPLUS_API ANCVersionGate : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void PostNetInit() override;

	/** Client → server: report our locally-compiled NETCODE_PLUGIN_VERSION. */
	UFUNCTION(Reliable, Server, WithValidation)
	void ServerReportVersion(int32 ClientVersion);

	/** Hub advisor mode: whisper instead of kick. Server-side only, plain member
	 *  (NOT a UPROPERTY / NOT replicated — see the net-field warning above). Must
	 *  be set before FinishSpawning so BeginPlay arms the right timers. */
	bool bAdvisorMode;

protected:
	/** Server-side: fired after the report deadline (default 100s). No matching
	 *  report → broadcast a "missing plugin" notice, then schedule OnKickDeadline. */
	void OnTimeout();

	/** Server-side: fired kKickGraceSec after OnTimeout's broadcast — kicks the
	 *  owner unless a matching late report cancelled it in the meantime. */
	void OnKickDeadline();

	/** Server-side: repeating "has the owner ever moved?" latch. The first time the
	 *  owner's pawn has sent a move (⇒ loaded the NCPlus pawn ⇒ has the plugin) it
	 *  confirms + destroys the gate — so a legit client that later goes pawnless
	 *  (respawn, eliminated for a round) can't be re-exposed to the kick. */
	void OnMoveWatch();

	/** Server-side: kick the owning PC with a clear message. Safe if already gone. */
	void KickOwner(const FString& Reason);

	/** Advisor mode: no report by the check time → whisper the install pointer to
	 *  the owner (private ClientSay), then re-arm at the nag cadence. Never kicks. */
	void OnAdvisorCheck();

	/** Private system-chat line to the owning player only. No-op if the owner or
	 *  its PlayerState is gone (retried on the next advisor nag). */
	void WhisperOwner(const FString& Msg);

	UPROPERTY(Transient)
	bool bConfirmed;

	/** Advisor whispers sent so far (first one logs at Warning, repeats are silent). */
	int32 AdvisorNagCount;

	/** Resolved timeout for THIS spawn (from Mod.ini [NetcodePlus]
	 *  VersionReportTimeoutSec, default 100s, clamped 1-120s). Captured in
	 *  BeginPlay so the timeout-log line can report the actual value used. */
	float TimeoutSec;

	FTimerHandle TimeoutHandle;
	FTimerHandle KickHandle;
	FTimerHandle MoveWatchHandle;
};

namespace NCPlusVersionGate
{
	/** Spawn the gate for a freshly-joined PC. Server-only; no-op for bots and
	 *  the listen host's local PC (they can't / don't need to handshake). Safe
	 *  to call from every NCPlus gamemode's PostLogin after Super:: returns. */
	NETCODEPLUS_API void SpawnFor(class APlayerController* PC);

	/** Spawn the gate in ADVISOR mode (whisper, never kick) — hub lobbies. */
	NETCODEPLUS_API void SpawnAdvisorFor(class APlayerController* PC);

	/** Bind/unbind the engine-level PostLogin event that auto-spawns advisor
	 *  gates on lobby servers (IsLobbyServer() == true). Call from the module's
	 *  Startup/ShutdownModule on dedicated servers. */
	NETCODEPLUS_API void RegisterHubAdvisor();
	NETCODEPLUS_API void UnregisterHubAdvisor();
}
