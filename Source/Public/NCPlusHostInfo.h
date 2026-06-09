// NCPlusHostInfo.h — plugin-replicated match-host identity.
//
// The scoreboard HOST badge originally read engine-replicated fields
// (AUTPlayerState::bIsMatchHost, AUTGameState::HostIdString). Those ride the
// ENGINE classes' replication lists, and our stack pairs `patched`-built server
// binaries with stock shipping clients whose lists provably differ
// (AUTPlayerState: shipping has IntroClass that patched lacks, ~40 entries
// before bIsMatchHost; AUTGameState: LineUpHelper/ActiveLineUpHelper skew
// around HostIdString). Fields past a divergence point can silently fail to
// arrive depending on which engine-binary vintage a server box runs — which is
// exactly the per-server "badge shows on UK, not on NYC" signature.
//
// The plugin DLL, by contrast, is version-gated (ANCVersionGate) — both ends
// are guaranteed the same build, so a plugin class is the only replication
// channel in this stack with guaranteed layout symmetry. This AInfo is that
// channel: one per match, always relevant, server resolves the host PlayerState
// from the gamemode's HostId (pure server-side data) and replicates the
// pointer. NCPlusScoreboardHost::IsHost reads it first; the engine fields are
// demoted to a fallback for old servers that don't spawn this actor.
#pragma once

#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "NCPlusHostInfo.generated.h"

UCLASS()
class NETCODEPLUS_API ANCHostInfo : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The match host's PlayerState (the ?HostId= player when present), resolved
	 *  server-side and replicated by net GUID — layout-skew-proof. Null when no
	 *  host is configured or the host isn't connected. */
	UPROPERTY(Replicated)
	class AUTPlayerState* HostPS;

protected:
	/** Server-side 1s poll: re-resolve HostPS from the gamemode's HostId vs the
	 *  current PlayerArray (host can join late / leave / rejoin with a fresh PS).
	 *  Self-destructs once the match starts — the badge is pre-match only. */
	void ResolveHost();

	FTimerHandle ResolveHandle;
};

namespace NCPlusHostInfo
{
	/** Spawn the per-match singleton if it doesn't exist yet. Server-only no-op
	 *  otherwise. Safe to call from every PostLogin (first caller spawns it). */
	NETCODEPLUS_API void EnsureSpawned(class UWorld* World);
}
