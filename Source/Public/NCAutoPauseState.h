#pragma once

// NetcodePlus.h must be the first include in headers that expose UT types.
// UE4.15 unity-build ordering otherwise produces fragile cascade failures.
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCAutoPauseState.generated.h"

/** Authoritative phase of NetcodePlus automatic pause handling. */
UENUM(BlueprintType)
enum class ENCAutoPausePhase : uint8
{
	Inactive,
	Paused,
	Resuming
};

/**
 * One atomic, late-join-safe view of automatic pause state.
 *
 * Keeping the phase, reason, awaited identities, and countdown timing in one
 * replicated property prevents clients from rendering combinations that never
 * existed on the server while several independent properties arrive.
 */
USTRUCT(BlueprintType)
struct FNCAutoPauseSnapshot
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	ENCAutoPausePhase Phase;

	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	FString PauseReason;

	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	TArray<FString> AwaitedPlayerIds;

	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	int32 CountdownDurationSeconds;

	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	int32 CountdownSecondsRemaining;

	/** Server UWorld::GetRealTimeSeconds() when the current countdown began. */
	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	float CountdownStartServerRealTime;

	/** Monotonic (within a match) revision for reliable client-side deduping. */
	UPROPERTY(BlueprintReadOnly, Category = "NetcodePlus|AutoPause")
	int32 StateRevision;

	FNCAutoPauseSnapshot();
};

/** Always-relevant replicated carrier for automatic pause state. */
UCLASS(NotPlaceable, BlueprintType)
class NETCODEPLUS_API ANCAutoPauseState : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(ReplicatedUsing = OnRep_Snapshot, Transient, BlueprintReadOnly,
		Category = "NetcodePlus|AutoPause")
	FNCAutoPauseSnapshot Snapshot;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Returns the state actor for World without repeatedly scanning on a miss. */
	static ANCAutoPauseState* Find(UWorld* World);

	/** Authority-only state transitions. Calls on clients are ignored. */
	void SetPaused(const FString& Reason, const TArray<FString>& AwaitedIds);
	void BeginResumeCountdown(const FString& Reason, int32 DurationSeconds,
		const TArray<FString>& AwaitedIds);
	void UpdateResumeCountdown(int32 SecondsRemaining,
		const TArray<FString>& AwaitedIds);
	void SetInactive(const FString& Reason);

	UFUNCTION()
	void OnRep_Snapshot();

private:
	/** Applies a semantic change, bumps the revision, and wakes replication. */
	void ApplySnapshot(const FNCAutoPauseSnapshot& NewSnapshot);
};
