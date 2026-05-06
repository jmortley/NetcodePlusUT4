// NCShaftArenaStatsReplicator - replicates per-player Link accuracy + damage
// from server to clients for the NCShaftArena scoreboard.
//
// Two stats live server-only on AUTPlayerState and need replication for
// dedicated-server play:
//   - StatsData[NAME_LinkHits / NAME_LinkShots]: UPROPERTY() with no
//     Replicated specifier. PS->GetStatsValue(...) returns 0 on clients.
//   - DamageDone: UPROPERTY(BlueprintReadWrite) with no Replicated specifier.
//     Reads as 0 on remote clients.
//
// Pattern mirrors ANCLeagueDuelStatsReplicator: AInfo subclass,
// bAlwaysRelevant, 1Hz Tick on the authority, replicated TArray of
// per-player entries.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCShaftArenaStatsReplicator.generated.h"

USTRUCT()
struct FNCShaftArenaStatsEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	/** Link accuracy * 10000 packed as int32 (3500 = 35.00%). */
	UPROPERTY()
	int32 LinkAccuracyTimes100 = 0;

	/** Total damage dealt this match. AUTPlayerState::DamageDone is
	 *  server-side only - this is the wire copy. */
	UPROPERTY()
	int32 DamageDone = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCShaftArenaStatsReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(Replicated, Transient)
	TArray<FNCShaftArenaStatsEntry> StatsEntries;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** Server-only: snapshot StatsData + DamageDone from PlayerArray. */
	void UpdateFromPlayerStates();

	/** Client-safe accessors. Return 0 if no entry yet. */
	float GetAccuracyForPlayer(const FString& UniqueIdStr) const;
	int32 GetDamageForPlayer  (const FString& UniqueIdStr) const;

private:
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
