// NCPClockSync.cpp — see NCPClockSync.h for the why and the architecture.
#include "NetcodePlus.h"
#include "NCPClockSync.h"
#include "UTGameState.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCPClockSync, Log, All);

// Server value gates anchoring + the terminal zero write; each client's own
// value gates the display applier. Runtime-flippable, no restart needed.
static TAutoConsoleVariable<int32> CVarNCPClockSync(
	TEXT("ncp.ClockSync"), 1,
	TEXT("Anchored match-clock sync. 1 (default): server replicates a {value, server-world-time} anchor at clock start and clients re-derive the displayed clock from it every frame. 0: stock behavior (free-running local 1s timer + 10s value snaps)."),
	ECVF_Default);

ANCPClockSync::ANCPClockSync(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	// Post-update so on clients this runs after the stock DefaultTimer's blind
	// RemainingTime-- fires from the timer queue — the applier must be the last
	// writer a frame sees or the HUD could render the one-frame-stale decrement.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 10.f;
}

void ANCPClockSync::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCPClockSync, bClockOwned);
	DOREPLIFETIME(ANCPClockSync, AnchorRemaining);
	DOREPLIFETIME(ANCPClockSync, AnchorServerTime);
}

ANCPClockSync* ANCPClockSync::Ensure(UWorld* World)
{
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	for (TActorIterator<ANCPClockSync> It(World); It; ++It)
	{
		return *It;
	}
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<ANCPClockSync>(SpawnParams);
}

int32 ANCPClockSync::ScheduledRemaining(const AUTGameState* GS) const
{
	const float Elapsed = GS->GetServerWorldTimeSeconds() - AnchorServerTime;
	return AnchorRemaining - FMath::FloorToInt(FMath::Max(0.f, Elapsed));
}

void ANCPClockSync::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Role == ROLE_Authority)
	{
		ServerTick();
	}
	else
	{
		ClientTick();
	}
}

void ANCPClockSync::ServerTick()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS == nullptr)
	{
		return;
	}

	const int32 Current = GS->GetRemainingTime();
	const bool bWantOwn = CVarNCPClockSync.GetValueOnGameThread() != 0
		&& GetNetMode() != NM_Standalone
		&& GS->IsMatchInProgress()
		&& !GS->bStopGameClock
		&& GS->TimeLimit > 0
		&& Current > 0;

	if (!bWantOwn)
	{
		if (bClockOwned)
		{
			bClockOwned = false;
			ForceNetUpdate();
		}
		bDidTerminalWrite = false;
		LastObservedRemaining = Current;
		return;
	}

	// Gate just opened (round/match clock started this frame — the same frame
	// the mode anchors its pickup timers to), or the mode/admin set a value we
	// didn't predict: (re)anchor here. A stock 1s step is Current == Last - 1;
	// anything else while owned is an external SetRemainingTime.
	const bool bExternalSet = bClockOwned
		&& Current != LastObservedRemaining
		&& Current != LastObservedRemaining - 1;
	if (!bClockOwned || bExternalSet)
	{
		AnchorRemaining = Current;
		AnchorServerTime = GS->GetServerWorldTimeSeconds();
		bClockOwned = true;
		bDidTerminalWrite = false;
		ForceNetUpdate();
		UE_LOG(LogNCPClockSync, Log, TEXT("[ClockSync] Anchored %d @ %.3f%s"),
			AnchorRemaining, AnchorServerTime, bExternalSet ? TEXT(" (external set)") : TEXT(""));
	}
	LastObservedRemaining = Current;

	// The stock server cadence runs ~0.4% slow (self-rescheduling timer), so
	// its int reaches 0 up to ~1s of true time after the anchored schedule.
	// Close that gap once so the round ends with the displayed clock at 0:00.
	// Everything before this moment is read-only.
	if (!bDidTerminalWrite && Current > 0 && ScheduledRemaining(GS) <= 0)
	{
		bDidTerminalWrite = true;
		bClockOwned = false;
		ForceNetUpdate();
		UE_LOG(LogNCPClockSync, Log, TEXT("[ClockSync] Schedule reached zero with stock clock at %d — terminal SetRemainingTime(0)"), Current);
		GS->SetRemainingTime(0);
	}
}

void ANCPClockSync::ClientTick()
{
	if (CVarNCPClockSync.GetValueOnGameThread() == 0)
	{
		LastAppliedRemaining = -1;
		return;
	}
	UWorld* World = GetWorld();
	if (World == nullptr || World->DemoNetDriver != nullptr || World->GetNetMode() != NM_Client)
	{
		// Demo playback / replay scrubbing: leave the recorded clock alone.
		return;
	}
	AUTGameState* GS = World->GetGameState<AUTGameState>();
	if (GS == nullptr || !bClockOwned)
	{
		// Released → bone-stock local ticking + 10s snaps resume seamlessly.
		LastAppliedRemaining = -1;
		return;
	}
	// Belt & braces against replication races around intermission/overtime
	// transitions: if the replicated stock state says the clock shouldn't run,
	// don't drive it.
	if (GS->bStopGameClock || !GS->IsMatchInProgress() || GS->TimeLimit <= 0)
	{
		LastAppliedRemaining = -1;
		return;
	}

	const int32 S = FMath::Clamp(ScheduledRemaining(GS), 0, AnchorRemaining);
	if (S != GS->GetRemainingTime())
	{
		GS->SetRemainingTime(S);
		// Stock announcements sample the value at the client DefaultTimer's own
		// sloppy 1Hz phase, which can straddle a displayed second entirely and
		// skip a threshold ("1 minute remains", the 10..1 countdown). Re-check
		// on every applied -1 flip so each displayed value gets evaluated
		// exactly once — CheckTimerMessage's index+time dedup swallows the
		// duplicate when the stock call lands on the same value. Re-anchor
		// jumps skip it, matching stock (which never announces the set value).
		if (S == LastAppliedRemaining - 1)
		{
			GS->CheckTimerMessage();
		}
		LastAppliedRemaining = S;
	}
}
