// Central audio ownership for UT's retained instant-replay world.
#include "UnrealTournament.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectIterator.h"
#include "UTKillcamPlayback.h"

namespace
{
	// The retained replay world can be paused while hidden, so a world timer or
	// actor tick cannot own this lifecycle. The core ticker remains active and a
	// 20 Hz scan catches newly auto-activated loops. The component walk runs only
	// while UT has a hidden retained killcam world, and is class-filtered to audio.
	constexpr float NCKillcamAudioScanInterval = 0.05f;

	static TAutoConsoleVariable<int32> CVarNCKillcamAudioGuard(
		TEXT("ncp.KillcamAudioGuard"),
		1,
		TEXT("Pause looping audio owned by UT's hidden retained killcam world. 0 restores stock behavior."),
		ECVF_Default);

	struct FNCKillcamAudioWorldState
	{
		TWeakObjectPtr<UWorld> World;
		TArray<TWeakObjectPtr<UAudioComponent>> GuardPausedComponents;
		bool bWasHidden = false;
	};

	static TArray<FNCKillcamAudioWorldState> GNCKillcamAudioWorldStates;
	static FDelegateHandle GNCKillcamAudioTickerHandle;
	static FDelegateHandle GNCKillcamAudioCleanupHandle;

	static bool NCIsTracked(const FNCKillcamAudioWorldState& State,
		const UAudioComponent* Component)
	{
		for (const TWeakObjectPtr<UAudioComponent>& WeakComponent :
			State.GuardPausedComponents)
		{
			if (WeakComponent.Get() == Component)
			{
				return true;
			}
		}
		return false;
	}

	static void NCRestoreGuardPausedComponents(FNCKillcamAudioWorldState& State)
	{
		UWorld* const StateWorld = State.World.Get();
		for (const TWeakObjectPtr<UAudioComponent>& WeakComponent :
			State.GuardPausedComponents)
		{
			UAudioComponent* const Component = WeakComponent.Get();
			if (Component != nullptr && !Component->IsPendingKill()
				&& Component->GetWorld() == StateWorld && Component->bIsPaused)
			{
				// Clearing the manual pause flag never starts an inactive component.
				// If gameplay stopped a loop while hidden, it stays stopped instead
				// of being resurrected by the guard.
				Component->SetPaused(false);
			}
		}
		State.GuardPausedComponents.Reset();
		State.bWasHidden = false;
	}

	static void NCRestoreAllAndClear()
	{
		for (FNCKillcamAudioWorldState& State : GNCKillcamAudioWorldStates)
		{
			NCRestoreGuardPausedComponents(State);
		}
		GNCKillcamAudioWorldStates.Reset();
	}

	static FNCKillcamAudioWorldState& NCFindOrAddWorldState(UWorld* World)
	{
		for (FNCKillcamAudioWorldState& State : GNCKillcamAudioWorldStates)
		{
			if (State.World.Get() == World)
			{
				return State;
			}
		}

		const int32 NewIndex = GNCKillcamAudioWorldStates.AddDefaulted();
		FNCKillcamAudioWorldState& NewState = GNCKillcamAudioWorldStates[NewIndex];
		NewState.World = World;
		return NewState;
	}

	static void NCPauseHiddenKillcamLoops(FNCKillcamAudioWorldState& State)
	{
		UWorld* const World = State.World.Get();
		if (World == nullptr)
		{
			return;
		}

		// Reconcile components already owned by the guard. Once acquired, retain
		// ownership while the component is active even if replay code swaps its
		// sound: UAudioComponent carries bIsPaused into the replacement playback,
		// so releasing here could leak a delayed finite cue from the hidden world.
		for (int32 Index = State.GuardPausedComponents.Num() - 1; Index >= 0; --Index)
		{
			UAudioComponent* const Component =
				State.GuardPausedComponents[Index].Get();
			const bool bStillOwned = Component != nullptr
				&& !Component->IsPendingKill()
				&& Component->GetWorld() == World
				&& Component->IsPlaying();
			if (!bStillOwned)
			{
				if (Component != nullptr && !Component->IsPendingKill()
					&& Component->bIsPaused)
				{
					// This releases only our pause and cannot restart an inactive
					// component. Any later playback is evaluated as a new acquisition.
					Component->SetPaused(false);
				}
				State.GuardPausedComponents.RemoveAtSwap(Index, 1, false);
				continue;
			}

			if (!Component->bIsPaused)
			{
				// Reassert ownership if replay/Blueprint code resumed it while
				// the killcam world was still hidden.
				Component->SetPaused(true);
			}
		}

		for (TObjectIterator<UAudioComponent> It; It; ++It)
		{
			UAudioComponent* const Component = *It;
			if (Component == nullptr || Component->IsPendingKill()
				|| Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
				|| Component->GetWorld() != World || !Component->IsPlaying()
				|| Component->bIsPaused || Component->Sound == nullptr
				|| !Component->Sound->IsLooping() || NCIsTracked(State, Component))
			{
				continue;
			}

			State.GuardPausedComponents.Add(
				TWeakObjectPtr<UAudioComponent>(Component));
			Component->SetPaused(true);
		}
	}

	static bool NCTickKillcamAudioGuard(float /*DeltaTime*/)
	{
		if (CVarNCKillcamAudioGuard.GetValueOnGameThread() == 0)
		{
			NCRestoreAllAndClear();
			return true;
		}
		if (GEngine == nullptr)
		{
			return true;
		}

		TArray<UWorld*, TInlineAllocator<4>> SeenKillcamWorlds;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const World = Context.World();
			if (Context.WorldType != EWorldType::Game || World == nullptr)
			{
				continue;
			}

			UGameInstance* const GameInstance = World->GetGameInstance();
			if (GameInstance == nullptr
				|| !UUTKillcamPlayback::IsKillcamWorld(GameInstance, World))
			{
				// Ordinary demo playback deliberately stays outside this guard.
				continue;
			}

			SeenKillcamWorlds.AddUnique(World);
			FNCKillcamAudioWorldState& State = NCFindOrAddWorldState(World);
			UGameViewportClient* const ViewportClient =
				GameInstance->GetGameViewportClient();
			const bool bHidden = ViewportClient == nullptr
				|| ViewportClient->GetWorld() != World;

			if (bHidden)
			{
				NCPauseHiddenKillcamLoops(State);
			}
			else if (State.bWasHidden || State.GuardPausedComponents.Num() > 0)
			{
				NCRestoreGuardPausedComponents(State);
			}
			State.bWasHidden = bHidden;
		}

		// A still-valid world that no longer classifies as UT's killcam must not
		// retain a pause owned by this module (hot reload and teardown included).
		for (int32 Index = GNCKillcamAudioWorldStates.Num() - 1; Index >= 0; --Index)
		{
			FNCKillcamAudioWorldState& State = GNCKillcamAudioWorldStates[Index];
			UWorld* const World = State.World.Get();
			if (World == nullptr || !SeenKillcamWorlds.Contains(World))
			{
				if (World != nullptr)
				{
					NCRestoreGuardPausedComponents(State);
				}
				GNCKillcamAudioWorldStates.RemoveAtSwap(Index, 1, false);
			}
		}

		return true;
	}

	static void NCOnKillcamAudioWorldCleanup(UWorld* World,
		bool /*bSessionEnded*/, bool /*bCleanupResources*/)
	{
		for (int32 Index = GNCKillcamAudioWorldStates.Num() - 1; Index >= 0; --Index)
		{
			if (GNCKillcamAudioWorldStates[Index].World.Get() == World)
			{
				// The world is being destroyed; drop weak ownership instead of
				// issuing new audio-thread work from the cleanup callback.
				GNCKillcamAudioWorldStates.RemoveAtSwap(Index, 1, false);
			}
		}
	}
}

// Called from FNetcodePlus::StartupModule / ShutdownModule (extern-declared there).
void RegisterNCKillcamAudioGuard()
{
	if (IsRunningDedicatedServer() || IsRunningCommandlet())
	{
		return;
	}
	if (!GNCKillcamAudioCleanupHandle.IsValid())
	{
		GNCKillcamAudioCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(
			&NCOnKillcamAudioWorldCleanup);
	}
	if (!GNCKillcamAudioTickerHandle.IsValid())
	{
		GNCKillcamAudioTickerHandle = FTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&NCTickKillcamAudioGuard),
			NCKillcamAudioScanInterval);
	}
}

void UnregisterNCKillcamAudioGuard()
{
	if (GNCKillcamAudioTickerHandle.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(GNCKillcamAudioTickerHandle);
		GNCKillcamAudioTickerHandle.Reset();
	}
	if (GNCKillcamAudioCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(GNCKillcamAudioCleanupHandle);
		GNCKillcamAudioCleanupHandle.Reset();
	}
	NCRestoreAllAndClear();
}
