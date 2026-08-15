// NCPClockSync — anchored match-clock sync beacon (ncp.ClockSync).
//
// WHY: the displayed match clock free-runs on a self-rescheduling 1s timer on
// BOTH sides — engine AGameState::DefaultTimer re-SetTimers itself inside its
// own callback, so every firing bakes that frame's overshoot into the next
// boundary and the phase only ever slips later (~4ms/s at 120 tick, plus full
// hitch lengths on clients). The server corrects clients once per 10s with a
// raw, transit-stale value snap (UTGameState RepTimeInterval) that never fixes
// the flip phase. Pickup respawns meanwhile run on exact world-time timers, so
// a 60s belt reads "spawned at 3:01/3:02" against the lagging clock.
//
// DESIGN: server-side this actor is an OBSERVER — while the clock runs it never
// writes it (round flow, match end, bot/mode logic all keep reading the pure
// stock int). It replicates an anchor {AnchorRemaining, AnchorServerTime}
// captured the frame the mode starts/sets the clock — the same schedule origin
// the Wipeout pickup timers are anchored to. Clients re-derive RemainingTime
// from the anchor + GetServerWorldTimeSeconds() every frame (in
// TG_PostUpdateWork, so the stock client-side decrement can never be the last
// writer a frame sees) and push it through the public SetRemainingTime(), so
// every stock reader — HUD clock widgets, scoreboard, local timer
// announcements — inherits the corrected value with no UI changes.
//
// The ONE server write: the stock server cadence runs ~0.4% slow, so its int
// reaches 0 up to ~1s of true time after the anchored schedule does. When the
// schedule hits 0 while the stock int is still >0, the beacon calls
// SetRemainingTime(0) once and releases — the mode's own end-of-time check
// fires on its next 1Hz sample, so rounds end with the displayed clock sitting
// at 0:00 instead of ~1s past it.
//
// Releases (falls back to bone-stock everywhere) on: intermission, overtime,
// bStopGameClock, cvar off, terminal write, or any externally-set clock value
// it didn't predict — and re-anchors the next time the gate opens.
// NCPlusHostPause::ResyncServerWorldTime already covers the pause hole in the
// client's server-world-time estimate; pauses freeze world time on both sides,
// so the anchor itself needs no pause handling.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCPClockSync.generated.h"

class AUTGameState;

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCPClockSync : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** True while the server is enforcing an anchored schedule. Clients only
	 *  drive the clock while this is set; false = bone-stock behavior. */
	UPROPERTY(Replicated, Transient)
	bool bClockOwned = false;

	/** Clock value (whole seconds) at the anchor moment. */
	UPROPERTY(Replicated, Transient)
	int32 AnchorRemaining = 0;

	/** GetServerWorldTimeSeconds() at the anchor moment. The client-side
	 *  estimate of that clock runs ~half-RTT behind truth, which shows up as
	 *  the whole display lagging the server by tens of ms — uniform, harmless. */
	UPROPERTY(Replicated, Transient)
	float AnchorServerTime = 0.f;

	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Find-or-spawn the world's beacon. Server only; cheap to call repeatedly.
	 *  Called lazily from UTWeaponFix::BeginPlay so every hub mode gets one —
	 *  including stock TDM, where no NetcodePlus game-mode code runs. */
	static ANCPClockSync* Ensure(UWorld* World);

private:
	void ServerTick();
	void ClientTick();

	/** Anchored schedule value for "now" on whichever side is asking. */
	int32 ScheduledRemaining(const AUTGameState* GS) const;

	/** Server: last GameState RemainingTime seen, for external-set detection. */
	int32 LastObservedRemaining = -1;

	/** Server: one-shot guard for the terminal SetRemainingTime(0). */
	bool bDidTerminalWrite = false;

	/** Client: last value the applier wrote, so a normal -1 flip (which should
	 *  re-run the timer-message check) is distinguishable from a re-anchor jump. */
	int32 LastAppliedRemaining = -1;
};
