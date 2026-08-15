#include "NCAutoPauseState.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// A missing actor is normal briefly during client join. Bound the negative
	// retry with real time so lookup recovery continues while gameplay is paused.
	static TWeakObjectPtr<UWorld> GCachedAutoPauseWorld;
	static TWeakObjectPtr<ANCAutoPauseState> GCachedAutoPauseState;
	static float GNextAutoPauseFindRealTime = 0.0f;
	static const float GAutoPauseFindRetrySeconds = 0.5f;

	bool SnapshotsHaveSameState(const FNCAutoPauseSnapshot& A,
		const FNCAutoPauseSnapshot& B)
	{
		return A.Phase == B.Phase
			&& A.PauseReason == B.PauseReason
			&& A.AwaitedPlayerIds == B.AwaitedPlayerIds
			&& A.CountdownDurationSeconds == B.CountdownDurationSeconds
			&& A.CountdownSecondsRemaining == B.CountdownSecondsRemaining
			&& A.CountdownStartServerRealTime == B.CountdownStartServerRealTime;
	}
}

FNCAutoPauseSnapshot::FNCAutoPauseSnapshot()
	: Phase(ENCAutoPausePhase::Inactive)
	, CountdownDurationSeconds(0)
	, CountdownSecondsRemaining(0)
	, CountdownStartServerRealTime(0.0f)
	, StateRevision(0)
{
}

ANCAutoPauseState::ANCAutoPauseState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = false;
	bReplicateMovement = false;
	NetUpdateFrequency = 10.0f;
	PrimaryActorTick.bCanEverTick = false;
}

void ANCAutoPauseState::BeginPlay()
{
	Super::BeginPlay();
	GCachedAutoPauseWorld = GetWorld();
	GCachedAutoPauseState = this;
	GNextAutoPauseFindRealTime = 0.0f;
}

void ANCAutoPauseState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GCachedAutoPauseState.Get() == this)
	{
		GCachedAutoPauseState = nullptr;
		GNextAutoPauseFindRealTime = 0.0f;
	}
	Super::EndPlay(EndPlayReason);
}

void ANCAutoPauseState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCAutoPauseState, Snapshot);
}

ANCAutoPauseState* ANCAutoPauseState::Find(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	if (GCachedAutoPauseWorld.Get() != World)
	{
		GCachedAutoPauseWorld = World;
		GCachedAutoPauseState = nullptr;
		GNextAutoPauseFindRealTime = 0.0f;
	}

	if (GCachedAutoPauseState.IsValid())
	{
		return GCachedAutoPauseState.Get();
	}

	const float Now = World->GetRealTimeSeconds();
	if (Now < GNextAutoPauseFindRealTime)
	{
		return nullptr;
	}

	for (TActorIterator<ANCAutoPauseState> It(World); It; ++It)
	{
		GCachedAutoPauseState = *It;
		GNextAutoPauseFindRealTime = 0.0f;
		return *It;
	}

	GNextAutoPauseFindRealTime = Now + GAutoPauseFindRetrySeconds;
	return nullptr;
}

void ANCAutoPauseState::SetPaused(const FString& Reason,
	const TArray<FString>& AwaitedIds)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	FNCAutoPauseSnapshot NewSnapshot = Snapshot;
	NewSnapshot.Phase = ENCAutoPausePhase::Paused;
	NewSnapshot.PauseReason = Reason;
	NewSnapshot.AwaitedPlayerIds = AwaitedIds;
	NewSnapshot.CountdownDurationSeconds = 0;
	NewSnapshot.CountdownSecondsRemaining = 0;
	NewSnapshot.CountdownStartServerRealTime = 0.0f;
	ApplySnapshot(NewSnapshot);
}

void ANCAutoPauseState::BeginResumeCountdown(const FString& Reason,
	int32 DurationSeconds, const TArray<FString>& AwaitedIds)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	const int32 SafeDuration = FMath::Max(0, DurationSeconds);
	FNCAutoPauseSnapshot NewSnapshot = Snapshot;
	NewSnapshot.Phase = ENCAutoPausePhase::Resuming;
	NewSnapshot.PauseReason = Reason;
	NewSnapshot.AwaitedPlayerIds = AwaitedIds;
	NewSnapshot.CountdownDurationSeconds = SafeDuration;
	NewSnapshot.CountdownSecondsRemaining = SafeDuration;
	NewSnapshot.CountdownStartServerRealTime = GetWorld() != nullptr
		? GetWorld()->GetRealTimeSeconds()
		: 0.0f;
	ApplySnapshot(NewSnapshot);
}

void ANCAutoPauseState::UpdateResumeCountdown(int32 SecondsRemaining,
	const TArray<FString>& AwaitedIds)
{
	if (Role != ROLE_Authority || Snapshot.Phase != ENCAutoPausePhase::Resuming)
	{
		return;
	}

	FNCAutoPauseSnapshot NewSnapshot = Snapshot;
	NewSnapshot.AwaitedPlayerIds = AwaitedIds;
	NewSnapshot.CountdownSecondsRemaining = FMath::Clamp(
		SecondsRemaining, 0, Snapshot.CountdownDurationSeconds);
	ApplySnapshot(NewSnapshot);
}

void ANCAutoPauseState::SetInactive(const FString& Reason)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	FNCAutoPauseSnapshot NewSnapshot = Snapshot;
	NewSnapshot.Phase = ENCAutoPausePhase::Inactive;
	NewSnapshot.PauseReason = Reason;
	NewSnapshot.AwaitedPlayerIds.Reset();
	NewSnapshot.CountdownDurationSeconds = 0;
	NewSnapshot.CountdownSecondsRemaining = 0;
	NewSnapshot.CountdownStartServerRealTime = 0.0f;
	ApplySnapshot(NewSnapshot);
}

void ANCAutoPauseState::OnRep_Snapshot()
{
	// Intentionally empty. HUD/audio consumers poll the atomic snapshot and use
	// StateRevision to dedupe presentation when replication catches up in bursts.
}

void ANCAutoPauseState::ApplySnapshot(const FNCAutoPauseSnapshot& NewSnapshot)
{
	if (Role != ROLE_Authority || SnapshotsHaveSameState(Snapshot, NewSnapshot))
	{
		return;
	}

	const int32 NextRevision = Snapshot.StateRevision == MAX_int32
		? 1
		: Snapshot.StateRevision + 1;
	Snapshot = NewSnapshot;
	Snapshot.StateRevision = NextRevision;
	ForceNetUpdate();
}
