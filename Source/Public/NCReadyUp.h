// NCReadyUp.h - UTComp-style player ready-up for NCPlus matches.
#pragma once

#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "UTMutator.h"
#include "NCReadyUp.generated.h"

class AUTGameMode;
class AUTPlayerState;

/**
 * Match-scoped replicated ready-up state. Readiness is deliberately separate
 * from AUTPlayerState::bIsWarmingUp: players may keep practicing while marked
 * Ready or Not Ready, matching UTComp's behavior.
 */
UCLASS()
class NETCODEPLUS_API ANCReadyUpState : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** Players who explicitly readied before the final countdown locked. */
	UPROPERTY(Replicated)
	TArray<AUTPlayerState*> ReadyPlayers;

	/** Snapshot displayed by the F5 menu/HUD. Frozen once countdown locks. */
	UPROPERTY(Replicated)
	int32 ReadyCount;

	UPROPERTY(Replicated)
	int32 EligibleCount;

	/** Once true, ready state cannot be changed and roster churn cannot cancel. */
	UPROPERTY(Replicated)
	uint32 bCountdownLocked : 1;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	bool IsPlayerReady(const AUTPlayerState* PlayerState) const;
	bool SetPlayerReady(APlayerController* Sender, bool bReady);
	void RefreshEligibility();
	void LockCountdown(float StartDelay);
	void CancelCountdown();
	float GetRemainingCountdown(float Now) const;

	/** Client/server lookup. There is exactly one while ready-up is enabled. */
	static ANCReadyUpState* Find(const UWorld* World);

private:
	/** Server-only team snapshot used to clear readiness after a pre-lock switch. */
	TMap<TWeakObjectPtr<AUTPlayerState>, uint8> ReadyTeamByPlayer;

	/** Server world time when the irreversible final countdown began. */
	float CountdownStartTime;
	float CountdownDuration;
};

/** Receives nc_ready/nc_notready through UT4's existing ServerMutate RPC. */
UCLASS()
class NETCODEPLUS_API ANCReadyUpMutator : public AUTMutator
{
	GENERATED_UCLASS_BODY()

public:
	virtual void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;
};

namespace NCReadyUp
{
	/** Read and cache [NetcodePlus] bUsePlayerReadyUp for this server world. */
	NETCODEPLUS_API void Initialize(AUTGameMode* GameMode);

	/** Spawn the replicated state once the first player has completed PostLogin. */
	NETCODEPLUS_API void PostLogin(AUTGameMode* GameMode, APlayerController* NewPlayer);

	/** True only for an enabled network match; standalone/PIE use stock behavior. */
	NETCODEPLUS_API bool ShouldHandle(const AUTGameMode* GameMode);

	/** Replacement ReadyToStartMatch path used only when ShouldHandle is true. */
	NETCODEPLUS_API bool ReadyToStartMatch(AUTGameMode* GameMode);
}
