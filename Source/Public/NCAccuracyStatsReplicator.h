// NCAccuracyStatsReplicator - shared per-weapon hits/shots replicator.
//
// AUTPlayerState::StatsData (TMap that holds NAME_LinkHits / NAME_ShockRifleHits
// / NAME_SniperHits etc.) is UPROPERTY() with no Replicated specifier, so on a
// dedicated-server client PS->GetStatsValue(...) returns 0. The accuracy HUD
// widget (NCPlusHUDWidget_Accuracy) needs raw hits + shots for the held weapon
// to render the percentage and Hits/Shots sub-text.
//
// Per-mode replicators (NCShaftArenaStatsReplicator, NCLeagueDuelStatsReplicator,
// etc.) only expose a single combined accuracy float - not enough for the
// "current held weapon" widget path which needs to look up by FName. This shared
// replicator broadcasts the standard weapon hits/shots per player at 2Hz so the
// widget can render the same number on listen and dedicated.
//
// Spawned by every NCPlus gamemode's InitGame on authority. AInfo subclass with
// bAlwaysRelevant. Wire cost is small (~88 bytes/player/tick).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCAccuracyStatsReplicator.generated.h"

/** Per-player snapshot of every weapon's hits + shots that the accuracy widget
 *  cares about. Mirrors the stat NAMEs in NCPlusHUDWidget_Accuracy's
 *  ResolveWeaponInfo table - if you add a weapon there, add a field here. */
USTRUCT()
struct FNCAccuracyStatsEntry
{
	GENERATED_BODY()

	UPROPERTY() FString PlayerId;

	UPROPERTY() int32 LinkHits          = 0;
	/** Stored as NAME_LinkBeamShots (per-refire-tick), not stock NAME_LinkShots
	 *  (per-trigger-pull). Quake-style; matches what NCPlusHUDWidget_Accuracy
	 *  swaps to for any link display. */
	UPROPERTY() int32 LinkShots         = 0;

	UPROPERTY() int32 ShockHits         = 0;
	UPROPERTY() int32 ShockShots        = 0;
	UPROPERTY() int32 RocketHits        = 0;
	UPROPERTY() int32 RocketShots       = 0;
	UPROPERTY() int32 FlakHits          = 0;
	UPROPERTY() int32 FlakShots         = 0;
	UPROPERTY() int32 MinigunHits       = 0;
	UPROPERTY() int32 MinigunShots      = 0;
	UPROPERTY() int32 SniperHits        = 0;
	UPROPERTY() int32 SniperShots       = 0;
	UPROPERTY() int32 LightningHits     = 0;
	UPROPERTY() int32 LightningShots    = 0;
	UPROPERTY() int32 EnforcerHits      = 0;
	UPROPERTY() int32 EnforcerShots     = 0;
	UPROPERTY() int32 BioHits           = 0;
	UPROPERTY() int32 BioShots          = 0;
	UPROPERTY() int32 RedeemerHits      = 0;
	UPROPERTY() int32 RedeemerShots     = 0;
	UPROPERTY() int32 InstagibHits      = 0;
	UPROPERTY() int32 InstagibShots     = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCAccuracyStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(Replicated, Transient)
	TArray<FNCAccuracyStatsEntry> StatsEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** Server-only: snapshot per-weapon stats from PlayerArray. */
	void UpdateFromPlayerStates();

	/** Client-safe: raw hits for a stat NAME (NAME_LinkHits / NAME_ShockRifleHits
	 *  etc.). Returns 0 if no entry yet or the stat isn't tracked. */
	int32 GetHitsForPlayer (const FString& PlayerId, FName StatName) const;
	int32 GetShotsForPlayer(const FString& PlayerId, FName StatName) const;

	/** Idempotent server-only spawn helper. Each NCPlus gamemode calls this from
	 *  InitGame or HandleMatchHasStarted; subsequent calls are no-ops thanks to
	 *  the TActorIterator guard. Returns the live instance (existing or freshly
	 *  spawned), or nullptr off-authority / on a missing world. */
	static ANCAccuracyStatsReplicator* EnsureSpawned(AActor* Owner);

private:
	float UpdateInterval     = 0.5f;
	float TimeSinceLastUpdate = 0.0f;
};
