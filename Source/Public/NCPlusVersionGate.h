// NCPlusVersionGate.h — early plugin-version check (post-PostLogin, pre-match).
//
// The original BP version check fires after warmup starts, so an outdated client
// joins, learns the map / spawns / picks up the flag, then gets kicked when the
// match begins — wrecking the PUG. This C++ gate spawns a per-player owner-only
// AInfo right at PostLogin; the client's PostNetInit calls ServerReportVersion
// with NETCODE_PLUGIN_VERSION. Server compares: mismatch → kick immediately.
// 10-second timeout → kick if no reply (covers old clients that don't have this
// class at all and never replicate the actor cleanly).
//
// Bots + the listen-host local PC are exempt (no remote client to handshake with).
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

protected:
	/** Server-side: fired 10s after spawn. Kick if we never got a matching report. */
	void OnTimeout();

	/** Server-side: kick the owning PC with a clear message. Safe if already gone. */
	void KickOwner(const FString& Reason);

	UPROPERTY(Transient)
	bool bConfirmed;

	/** Resolved timeout for THIS spawn (from Mod.ini [NetcodePlus]
	 *  VersionReportTimeoutSec, default 10s, clamped 1-60s). Captured in
	 *  BeginPlay so the timeout-log line can report the actual value used. */
	float TimeoutSec;

	FTimerHandle TimeoutHandle;
};

namespace NCPlusVersionGate
{
	/** Spawn the gate for a freshly-joined PC. Server-only; no-op for bots and
	 *  the listen host's local PC (they can't / don't need to handshake). Safe
	 *  to call from every NCPlus gamemode's PostLogin after Super:: returns. */
	NETCODEPLUS_API void SpawnFor(class APlayerController* PC);
}
