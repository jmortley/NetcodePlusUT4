// NPPlayerController.cpp
#include "NPPlayerController.h"
#include "UnrealTournament.h"
#include "UTPlayerCameraManager.h"

ANPPlayerController::ANPPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Explicitly set the CameraManager to prevent "SpawnActor failed" crashes
	// when the editor creates the CDO and the parent's reference doesn't resolve.
	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
}

void ANPPlayerController::PlayerTick(float DeltaTime)
{
	// ---------------------------------------------------------------
	// Step 1: Inject deferred StopFires from the previous tick.
	// These go at the FRONT of the queue so they execute before any
	// new inputs from this frame (previous click's release completes
	// before a new click's press).
	// ---------------------------------------------------------------
	if (DeferredStopFires.Num() > 0)
	{
		// Build a temporary array: deferred stops first, then current queue
		TArray<FDeferredFireInput> Merged;
		Merged.Reserve(DeferredStopFires.Num() + DeferredFireInputs.Num());

		for (uint8 Mode : DeferredStopFires)
		{
			Merged.Add(FDeferredFireInput(Mode, false));
		}
		for (const FDeferredFireInput& Existing : DeferredFireInputs)
		{
			Merged.Add(Existing);
		}

		DeferredFireInputs.Empty();
		for (const FDeferredFireInput& M : Merged)
		{
			new(DeferredFireInputs) FDeferredFireInput(M.FireMode, M.bStartFire);
		}

		DeferredStopFires.Empty();
	}

	// ---------------------------------------------------------------
	// Step 2: Scan for swallowed clicks — StartFire(N) followed by
	// StopFire(N) in the same frame with no intervening StartFire(N).
	// Remove the StopFire and defer it to next tick.
	// ---------------------------------------------------------------
	for (uint8 Mode = 0; Mode < 2; Mode++)
	{
		int32 StartIdx = INDEX_NONE;
		int32 StopIdx = INDEX_NONE;

		for (int32 i = 0; i < DeferredFireInputs.Num(); i++)
		{
			const FDeferredFireInput& Input = DeferredFireInputs[i];
			if (Input.FireMode != Mode)
			{
				continue;
			}

			if (Input.bStartFire)
			{
				// Found a StartFire — track it, reset any previous StopFire match
				StartIdx = i;
				StopIdx = INDEX_NONE;
			}
			else if (StartIdx != INDEX_NONE)
			{
				// Found StopFire after a StartFire — this is the swallowed pair
				StopIdx = i;
				break;
			}
		}

		if (StartIdx != INDEX_NONE && StopIdx != INDEX_NONE)
		{
			// Remove the StopFire from this frame's queue
			DeferredFireInputs.RemoveAt(StopIdx);
			// Inject it next tick instead
			DeferredStopFires.AddUnique(Mode);
		}
	}

	// ---------------------------------------------------------------
	// Step 3: Normal tick — movement component will call
	// ApplyDeferredFireInputs() on us during PerformMovement.
	// ---------------------------------------------------------------
	Super::PlayerTick(DeltaTime);
}
