// NCPCandyLiftGuard — server-side keeper for the candy orbs that ElimPlus and
// Wipeout drop on PreventDeath (BP CandyPlaceholder, a physics-simulated
// AUTPickupHealth).
//
// THE BUG (2026-08-06): someone dies mid-lift, the candy spawns in the shaft,
// and the lift JAMS — AUTLift::MoveLiftTo sweeps the platform, the sweep
// blocks on the candy's physics body (bMoveWasBlocked), and the Generic_Lift
// "Lift Move" timeline finishes with the platform physically wedged, leaving
// the BP's logical position desynced from the mesh until the candy is eaten.
//
// The candy BP's own recovery (ReceiveHit -> cast the lift -> MoveLiftTo back
// to start) can never be reliable:
//   1. Generic_Lift's timeline update calls MoveLiftTo(VLerp(...)) every tick,
//      stomping any external move a frame later.
//   2. It only "works on some lifts": the BP casts AsGeneric_Lift and falls
//      back to AsUTLift — but the reset target (Generic_Lift's LiftStart BP
//      variable) doesn't exist on other lift classes, so custom/map lift BPs
//      get no valid target. (AUTLift::LiftStartLocation is protected C++,
//      invisible to BP.)
//   3. Physics-contact ReceiveHit needs Simulation Generates Hit Events and is
//      unreliable against a fast-sleeping body (candy is ESleepFamily::Sensitive).
//
// THE FIX — never command the lift; make the jam impossible and keep the orb
// grabbable. Three layers:
//   1. HARDEN: every simulating candy body ignores ECC_WorldDynamic — a lift
//      sweep can no longer block on it, so a stuck lift is structurally
//      impossible (worst case the platform passes through the orb). The same
//      pass makes the orb non-shootable per Jeremy 2026-08-06: ignore
//      COLLISION_PROJECTILE + both weapon trace channels, and ignore radial
//      impulse/force so splash can't shove it either. Applied at TWO levels:
//      the CandyPlaceholder class archetypes are fixed once at match start
//      (every later orb is born hardened — no first-sweep window in which a
//      moving lift could still wedge on a fresh corpse-drop), and each live
//      instance is re-hardened on first sight as the catch-all.
//   2. EVICT (4 Hz sweep): a candy inside a lift's swept path (platform bounds
//      replayed across GetStops(), plus margins) — including the died-mid-lift
//      spawn case on first sight — teleports to its last known-good resting
//      spot, or to a floor spot just OUTSIDE the lift path (nearest-face exit,
//      floor-traced ignoring lifts) so it lands beside the lift, not in it.
//   3. RESTORE: below-KillZ candies come back instead of being lost.
// Relocations move the actor and every simulating component, zero velocities,
// and FlushNetDormancy so clients see the move.
//
// Spawned by WipeoutGame + ElimPlusGame in HandleMatchHasStarted via
// EnsureSpawned (same pattern as ANCAccuracyStatsReplicator). Not replicated;
// pure server logic. No-ops on maps/modes without candies or lifts.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCPCandyLiftGuard.generated.h"

class AUTLift;
class AUTPickupHealth;

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCPCandyLiftGuard : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** Find-or-spawn the world's guard. Server only; cheap to call repeatedly. */
	static ANCPCandyLiftGuard* EnsureSpawned(UWorld* World);

private:
	struct FLiftInfo
	{
		AUTLift* Lift;
		FBox Box;      // current platform bounds
		FBox Detect;   // Box + interaction margins
		FBox Hull;     // full swept path (Box replayed across GetStops()) + margins
		bool bMoving;
	};

	void Sweep();

	/** True for the PreventDeath candy orbs — same identification the modes'
	 *  CheckRelevance whitelists use (AUTPickupHealth subclass named *Candy*). */
	static bool IsCandy(const AActor* Actor);

	/** Collision hardening for one simulating body: lift sweeps can't block on
	 *  it (no jam), weapons can't hit it, splash can't shove it. Safe on both
	 *  live components and class-default templates. */
	static void HardenPrim(class UPrimitiveComponent* Prim);

	/** Instance-level hardening: every simulating component of a live candy. */
	static void HardenCandy(AActor* Candy);

	/** Archetype-level hardening: fix the candy class's SCS + AddComponent
	 *  templates so every future spawn is born hardened. */
	static void HardenClassTemplates(UClass* CandyClass);

	/** Teleport the candy (actor + every simulating component) to Target with
	 *  velocities zeroed, and flush dormancy so the move replicates. */
	static void RelocateCandy(AActor* Candy, const FVector& Target);

	/** Where an evicted candy should go: Home if it's clear of every lift
	 *  path, else a floor spot pushed out of the containing hull's nearest
	 *  face (traced ignoring lifts), else straight down, else Seed itself. */
	FVector PickEvictionTarget(UWorld* World, const FVector& Seed,
		const FVector* Home, const TArray<FLiftInfo>& Lifts) const;

	static bool InsideAnyHull(const FVector& Loc, const TArray<FLiftInfo>& Lifts);

	/** Last known-good resting spot per candy. */
	TMap<TWeakObjectPtr<AActor>, FVector> CandyHome;

	/** Candies already run through HardenCandy. */
	TSet<TWeakObjectPtr<AActor>> HardenedCandies;

	/** Last sweep's platform-component location per lift, for movement detection. */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, FVector> LiftCompLastPos;

	float SweepAccumulator = 0.f;
};
