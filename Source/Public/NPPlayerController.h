// NPPlayerController.h
// Neutral C++ PlayerController shell — behaviorally identical to stock
// AUTPlayerController. Kept as the proven-safe home for future PC-level work
// (e.g. an ncp.FlushNetOnFire experiment) that must not live in a Blueprint.
//
// HISTORY (2026-08-06): this class previously carried a deferred-fire-queue
// rewrite that tried to rescue sub-frame clicks by delaying a same-frame
// StopFire one tick. Two independent audits retired it:
//  - Inert on every healthy frame: the scan ran before Super::PlayerTick, but
//    fire input enqueues INSIDE Super and the queue drains the same frame at
//    the end of UUTCharacterMovement::TickComponent — the scan always saw an
//    empty queue.
//  - Harmful on the rare frames where it DID run (skipped drains at ragdoll/
//    pause/hitch boundaries): re-injecting the deferred Stop ahead of a
//    retained Start reordered [Start,Stop] into [Stop,Start], leaving a
//    released click logically HELD with no future release event (stuck fire).
// Click reliability lives at the weapon layer instead, which owns the ROF
// state: AUTWeaponFix debounce-retry + ncp.ClickBufferMs.
//
// NotBlueprintable is load-bearing: creating a BP child of a custom PC hangs
// the editor in a pump-less O(N^2) reinstance/recompile. Assign this class
// directly from C++ (game mode InitGame), never via a BP subclass.

#pragma once

#include "NetcodePlus.h"
#include "UTPlayerController.h"
#include "NPPlayerController.generated.h"

UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANPPlayerController : public AUTPlayerController
{
	GENERATED_BODY()

public:
	ANPPlayerController(const FObjectInitializer& ObjectInitializer);
};
