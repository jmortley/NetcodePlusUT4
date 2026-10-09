# Paired weapon-fire diagnostics (4.15 and 4.27.2), schema 2

Observation only, disabled by default (`ncp.FireProvenance 0`). No new RPC, wire
field, reflected property, gameplay timer, fire-rate change or switch-policy change.
The stock validator is called exactly once; its actual return value is observed.
Acceptance, entry into Begin/EndFiringSequence, its result, and a firing outcome are
separate observations. A pending bit or shared byte changing does not prove a shot.

Both client and server need instrumented builds of their **own, matching engine
version**. These source changes have not established binary compatibility with older
builds. Full client/server builds, a mixed-version connection test and live paired
captures remain required. Syntax checks and synthetic tests are not runtime proof.

## Capture a private test

1. Record both source revisions and binary hashes, engine version, map, mode/mutators,
   ping, netspeed, FPS cap and launch arguments. Join the match and finish loading.
   Use one local player; the report can select `--player PLAYERSTATE_NETGUID` if a
   client log has multiple local players.
2. Generate a fresh session nonce, for example in PowerShell:

   ```powershell
   [Guid]::NewGuid().ToString('N')
   ```

   Choose a whitespace-free run label. Use the **same label and nonce on both
   processes**, and never reuse the nonce after a reconnect, map travel or restart.
   In the client's game console:

   ```text
   ncp.FireTraceLimit 200000
   ncp.FireTraceBegin shock_rocket_a REPLACE_WITH_FRESH_32_HEX_NONCE
   ```

   In a Shipping match server, use its authenticated admin console path. From the
   client connected to that match server:

   ```text
   rconauth YOUR_PRIVATE_ADMIN_PASSWORD
   rconexec ncp.FireTraceLimit 200000
   rconexec ncp.FireTraceBegin shock_rocket_a REPLACE_WITH_SAME_NONCE
   ```

   Keep the password out of shared capture artifacts. Verify BEGIN appears in the
   **match server's** log as well as the client log. Hub children have separate
   processes/logs; starting a trace in a hub is not starting its match's trace.
   The rcon-to-console route is present in source; verify it on the deployed hub.
   `-ExecCmds` is compiled out in Shipping and is **not** a fallback. INI cvars can
   enable logging but cannot supply the explicit begin/end commands; the reporter
   treats that as incomplete evidence. Do not change the shared player config.
3. Reproduce for about a minute. First do Shock/Rocket taps, holds through switching,
   rapid reversals, and releases during equip. Then stop firing and wait a few
   seconds for queued requests/volleys to settle.
4. End both captures from the game console:

   ```text
   rconexec ncp.FireTraceEnd
   ncp.FireTraceEnd
   ```

   END completes even an empty capture and disables tracing. An empty capture is
   never a successful shot test. Directly disabling the toggle emits ABORT, and
   re-enabling starts a different capture ID. A missing END, ABORT, LIMIT, mixed
   sessions or multiple world/connection lifetimes requires review. Do not trim
   trace lines out of the logs. Collect both original log files.

Each process generates its own capture ID; the shared nonce is supplied by the
operator, not authenticated or transmitted by a new protocol. Fixed-shot matching
also verifies the exact existing client timestamp's IEEE-754 float bits at SEND
and RECEIVE, with decimal/bits consistency. Actor/owner/player identity must agree.
A nonce is not a substitute for correct capture provenance; never deliberately
reuse one. World, driver, connection and local weapon-object IDs detect observed
lifetime changes. Unknown identity is reported as unknown, not guessed.

## What the evidence means

| Path | Evidence |
| --- | --- |
| Sniper shot, Shock both modes, Flak both modes, Minigun secondary, Rocket primary | Numbered SEND/RECEIVE, ACCEPT, rejection/cancellation, dispatch and outcome. Fixed requests on stock firing states keep their identity for the first synchronous dispatch. |
| Minigun primary; Link plasma/beam (NCP and Plus) | Actual stock byte sent, validator result, whether Begin/EndFiringSequence ran, sync decisions, per-hold client/server pulse counts. |
| Charged Rockets | Stock charge hold, fixed Stop ID and accepted/skipped reasons, actual charge commit, per-volley projectile counts and state-end markers. |
| Sniper zoom | Input/state context, not a damaging shot. |

`PREDICT` means the client's dispatch was entered, not proof of a visible effect.
`fired` means a server damaging hitscan path executed (including a miss), or a
projectile spawn returned an actor. It does not mean an enemy took damage. A
trade-grace direct spawn is reported separately. ACKs are watermarks and never
proof of firing. Cancellations after an accepted reservation remain visible.

LAYOUT records list every firing-state slot when an observed weapon first enters
the trace, including its Blueprint-selected runtime class. Current state, current
and pending weapon GUIDs, held bits, frame/delta, local clocks, refire/earliest-fire,
equip remaining/time-since-completion and nearby switch events accompany evidence.
INPUT_PRESS/RELEASE are weapon-method calls, not raw hardware mouse messages.

Stream/beam/charged pulses have **no unique per-pulse wire identity**. They remain
individually unexplained rather than being paired by order or clock proximity.
The report additionally pairs unique stock Start IDs per hold and shows its Stop
validation results and pulse counts. Reused/wrapped byte IDs are ambiguous. Sync
calls have `origin=sync` and event 255; they are never presented as received input
packets. Sync evidence within a server-local request window is labeled as such,
not assigned to a client pulse or asserted as the cause of a ghost shot.

A charged fixed Stop is a release watermark. A stock Stop or automatic release
may commit the first rocket before it arrives. The volley summary requires a
unique release and uniquely paired charge hold/commit; it does not claim the fixed
Stop caused that first rocket. Missing or multiple releases/commits remain
unexplained. End markers show whether the observed volley finished. Automatic
volleys without a unique fixed Stop remain in charge/hold evidence, not an invented
release pairing.

Delayed fake records distinguish queued Flak reservations, duplicate suppression,
callbacks/results, cleanup and ACK cancellation. The legacy shared timer does not
retain a reliable original event ID: its callback explicitly has unknown identity.
A missing local projectile at synchronous dispatch end is not automatically a
missing server shot. Local fake evidence is attached to its original event when
that existing reservation retains one.

Owner/player identity is retained per weak weapon only after it was observed;
owner-loss rows are marked `identity=retained`. A newly observed owner replaces it.
Unknown ownership is not silently matched. The report filters other players and
bots from shot, hold and server-only summaries. Packet samples remain driver-wide:
`inPacketsRaw` and `inLostRaw` reset with the engine's stats interval on servers and
Development clients; on Shipping clients they accumulate. `outOfOrderTotal` is
cumulative. Do not subtract resettable counters, compare them as identical windows,
or attribute a particular shot's result to a packet-counter increment.

## Report

```powershell
python -B tools/check-fire-provenance.py --client client.log --server server.log --run shock_rocket_a
python -B tools/check-fire-provenance.py --client client.log --server server.log --run shock_rocket_a --json > paired-fire-report.json
```

The standalone `check-fire-trace.py` accepts the same arguments. Use `--context 0..20`
(default 4) for neighboring events. Text lists every rejected/cancelled/unexplained
prediction, unpaired server dispatches, hold/volley counts and Stop/sync results.
JSON includes all evidence, successful predictions, layouts, local fake lifecycle
and network samples. Server-only evidence is not by itself proof of a ghost shot.

Paired exit 0 requires complete evidence and every observed numbered prediction
verified fired, with no unpaired server events. Exit 1 means review is needed,
including stream pulses lacking exact identity; exit 2 is a CLI/file error.
The legacy server-only invocation (`check-fire-provenance.py server.log`) retains
its exit-code rules. The newer cancellation labels are recognized and explicitly
listed as CLIENT_VISIBLE_CANCEL, rather than disappearing or becoming unclassified.
Schema-1 paired captures are unsupported for certification because they lack these
identity/validation safeguards.

## Validation and remaining runtime work

Run `python -B -m unittest discover -s tools/tests -p test_fire*.py`.
The suite includes audit regressions and stored synthetic lines formatted from the
C++ printf templates. Those fixtures are **not** captured engine output.

For each engine separately, obtain real instrumented client/server captures:

- Shock/Rocket primary taps and holds; all six weapons' runtime layouts.
- Switch/equip/release boundaries (10/20/40 ms), Sniper zoom, Flak pellets/shell,
  Shock core duplicate suppression and owner death/trade grace.
- Minigun primary before/after 20+ shards; Link NCP and Plus plasma/beam holds.
- Charged Rockets with 1/2/3 loaded, early/automatic releases and switches.
- 0/50/120 ms latency, 1-3% loss and reordering, with controls at identical FPS caps.
- Negative controls: wrong session, reconnect/travel, LIMIT, missing END, disabling
  mid-capture, repeated stock byte IDs and mixed old/new instrumented endpoints.

The detailed cap is process-wide, not a cap on pre-existing provenance messages.
Logging affects frame timing; use short captures for diagnosis, not FPS benchmarks.
Do not open a live `-log` window for performance comparisons. No causal claim about
mouse hardware, engine packet handling or the stock shared byte is established by
these source changes alone.
