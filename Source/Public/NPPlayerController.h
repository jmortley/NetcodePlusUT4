// NPPlayerController.h
// Custom PlayerController that fixes input swallowing on low/zero-debounce mice.
//
// UE 4.15 uses polling-based input: mouse press+release events are queued into
// DeferredFireInputs and replayed in the same frame. If both arrive between polls,
// StartFire→StopFire executes back-to-back and the weapon never gets a tick to fire.
//
// Fix: Pre-process the deferred queue each tick. When a StartFire+StopFire pair
// for the same fire mode appears in the same frame, remove the StopFire and defer
// it to the next tick — giving the weapon state machine time to actually fire.

#pragma once

#include "NetcodePlus.h"
#include "UTPlayerController.h"
#include "NPPlayerController.generated.h"

UCLASS()
class NETCODEPLUS_API ANPPlayerController : public AUTPlayerController
{
	GENERATED_BODY()

public:
	ANPPlayerController(const FObjectInitializer& ObjectInitializer);

	virtual void PlayerTick(float DeltaTime) override;

protected:
	/** Fire modes whose StopFire was deferred from the previous tick. */
	TArray<uint8> DeferredStopFires;
};
