# Paired weapon-fire diagnostics (4.15 and 4.27.2)

This is observation only. It does not change fire rates, put-down/equip timing,
input buffering, authorization, retransmission, or weapon-switch behavior. No new
RPCs, reflected properties, replicated classes, or wire fields are added. The
existing RPC implementations are wrapped only to log and forward their arguments.

The trace is **disabled by default**, using the existing `ncp.FireProvenance 0`.
Both the client and the server need the new NetcodePlus code compiled into their
respective builds to produce a paired report. Stock clients/servers do not produce
the missing half of a capture. This patch alone does not prove runtime compatibility
with an older binary; that still needs a mixed-version connection test.

## Coverage and evidence limits

| Weapon/mode | Evidence |
| --- | --- |
| Sniper shot | Existing numbered request, acceptance/rejection/cancellation, actual hitscan execution |
| Shock primary/secondary | Existing numbered request and hitscan/projectile outcome |
| Flak primary/secondary | Existing numbered request, all synchronous projectile attempts, one outcome per trigger shot rather than one per pellet |
| Minigun secondary | Existing numbered request and projectile outcome |
| Minigun primary | Stock start/stop receipt, spin-up/state changes, locally counted firing pulses and hitscans |
| Link plasma/beam, both NCP and Plus implementations | Stock start/stop, plasma spawn/pulse observations, beam effects pulses and traces sampled at 100 ms |
| Rocket primary | Existing numbered request and projectile outcome |
| Charged Rockets | Charge begin/load/release/recovery events, local dispatch scopes and projectile outcomes |

The stock-managed continuous/charged paths **do not carry a unique network ID for
each predicted pulse**. Their client/server counts are observations, not an exact
one-to-one pairing. The report includes each such prediction as `unexplained` with
an explicit `no_per_shot_wire_identity` reason. Do not match them by local ordinal
or nearest timestamp. Adding exact per-pulse correlation would require a separate
diagnostic protocol; this patch deliberately leaves the game's protocol unchanged.
Sniper zoom is input/state telemetry, not a projectile or damaging shot.

`PREDICT` means entry into the client's firing dispatch. It is not proof that a
particular effect reached the screen. Delayed projectile visuals may occur after
that scope and be unscoped. `fired` means the server executed a damaging hitscan
path (including a miss) or returned a projectile from its spawn path; it does not
mean damage was applied to an enemy. ACKs are reported separately as watermarks,
never as proof of a shot. A cancelled, already-ACKed reservation remains visible
as cancelled.

The record includes runtime firing-state class, current and pending weapon GUIDs,
held-fire bits, frame number, frame delta, local clocks, last-fire/refire/earliest-fire
times, equip timer remaining, and time since observed equip completion. Additional
records cover stale equip lifetime, mode policy, rate/sequence rejection, busy
reservation, release receipt, retries, retry queue cleanup, put-down callbacks and
their timing, actual state transitions, and lifecycle cancellation reasons.
`INPUT_PRESS/RELEASE` are weapon-method calls, not raw hardware mouse events.

Replicated weapon/pawn/PlayerState GUIDs are looked up without allocating GUIDs.
Missing GUIDs, respawns, reused event IDs, reconnects and multiple world/driver
lifetimes are not guessed away. Driver-wide packet counters are sampled every ten
seconds; they are not per-shot counters, proof of packet loss, or proof that a
particular out-of-order packet caused a rejection. All mutable diagnostic maps are
used on the normal gameplay thread and hold weak actor references.

## One-minute capture

1. Use a private test client/server pair built from the same engine version and
   diagnostic source revision. Record both commit IDs, executable/plugin hashes,
   map, mode/mutators, ping, netspeed, FPS cap and launch arguments alongside the logs.
   Do not mix a 4.15 client capture with a 4.27 server capture.
2. Join the match and finish loading before enabling the trace. Choose a fresh,
   whitespace-free label for each attempt. On **each process**, execute:

   ```text
   ncp.FireTraceRun shock_rocket_20260927_a
   ncp.FireTraceLimit 200000
   ncp.FireProvenance 1
   ```

   Server commands must execute in the server's engine console/admin command path,
   not just in the player's local console. They can also be supplied to the server
   at launch with `-ExecCmds="ncp.FireTraceRun LABEL,ncp.FireProvenance 1"` when a
   runtime console is unavailable. Collect the server process's actual log.
3. For the first run, rapidly switch Shock/Rocket for about a minute: tap, hold
   through switching, alternate fire, release during equip, and reverse a switch.
   Pause firing for a few seconds so queued work can settle. Then execute on both:

   ```text
   ncp.FireTraceEnd
   ```

   This writes an `END` marker and sets `ncp.FireProvenance` back to zero. Merely
   exiting/crashing, disconnecting, or setting the cvar to zero without the end
   command leaves an explicitly incomplete capture; available evidence remains
   reportable. Start a fresh label after any map travel/reconnect. Each server child
   process in a hub needs its own capture; a hub log is not the match server log.
4. Save both logs. Repeat separate labelled runs for Sniper/Shock, Flak/Rocket,
   Minigun/Shock, and Link/Rocket. Include both fire modes and charged-rocket release.
   Use the same reproducer in 4.15 and 4.27.2. Trace logging affects frame timing, so
   use it for causality investigation, not FPS benchmarking.

The limit bounds the new detailed trace records per process/capture, not the size
of all pre-existing provenance logging. Beam samples are throttled. Keep sessions
short. A `LIMIT` record means the report must not certify capture completeness.
Do not persist these cvars into the shared player config.

## Report

From the NetcodePlus repository:

```powershell
python -B tools/check-fire-provenance.py --client client.log --server server.log --run shock_rocket_20260927_a
python -B tools/check-fire-provenance.py --client client.log --server server.log --run shock_rocket_20260927_a --json > paired-fire-report.json
```

`check-fire-trace.py` is the paired implementation; either entry point accepts those
arguments. Existing server-only `check-fire-provenance.py server.log` behavior and
exit codes remain unchanged.

The text report lists every rejected, cancelled or unexplained client prediction,
with nearby switch/equip/input events from each side's own sequence. JSON also
includes successful predictions and server evidence, unmatched server events,
continuous-mode counts and network samples. It never sorts the two computers by
wall clock or silently promotes a later cumulative ACK into a shot outcome.
Use `--context 0..20` to adjust surrounding events (default 4).

Paired exit code 0 means a complete capture with every observed numbered prediction
verified fired. Code 1 means review is needed (including cancellations, continuous
pulses with no exact identity, limits, missing data, or no predictions). Code 2 is a
command-line/file error. This is an evidence report, not a pass/fail gameplay fix.

## Validation

Run `python -B -m unittest discover -s tools/tests -p test_fire*.py`.
Synthetic fixtures cover actual hitscan/projectile outcomes, ACK-only paths,
accepted/cancelled requests, retries, multi-projectile Flak, sequence/identity
ambiguity, missing/capped/truncated logs and continuous-fire limitations. These
tests do not substitute for an instrumented client/server capture.
