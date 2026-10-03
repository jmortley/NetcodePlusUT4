# NetcodePlus 329 protocol candidate

Branch: `codex/329`, based on `bb1292d` from `328-release-candidate`.
This is the 4.15 plugin source candidate. No 4.27 port, engine rebuild, cook,
package, deployment or push is part of this change. The user performs UBT builds.

## Matching packages are required

329 changes the fixed Start/Retry RPC payload and introduces identified loaded
rocket RPCs. UE4 uses class field indices for RPC decoding; even adding an RPC
can break old peers. Use matching 329 client and server packages. Neither the
version check nor a disabled CVar makes 329 wire-compatible with 328.

The exact-version gate is tied to the current controller, world and connection.
Remote firing is blocked until that session confirms 329. Hub advisor reports
are informational, not authorization to fire in a match. Mixed versions may
disconnect at engine decoding before an explanatory gate message can run.
Initial clicks before a fresh gate report, including after travel, need testing.
See `VERSION329-GATE-VALIDATION.md` for the gate-specific checks and limitations.

## Precision hitscan: bounded movement/fire association

The original fixed-fire request and its retry copies now carry the same movement
stamp and quantized eye origin. The server matches only its latest observed
shot-marked movement sample for that pawn/weapon/session, once. It does not
search arbitrary history using a client wall clock. The existing explicit eye-Z
and precision movement flush fixes remain in place; view bob is not reworked.

Admission requires a matching stamp, no intervening teleport/reset/equip change,
at most 80 ms of server-observed movement-to-fire processing delay, origin within
20 units horizontally and 24 vertically, aim within 2 degrees, a clear short
origin trace and recent ACK traffic. The ACK check establishes liveness only;
it does not expose the age of the engine's last valid RTT sample.

Only transactional precision hitscan on Shock/Insta/Sniper is eligible. Other
weapons/modes retain their existing timing. Missing or invalid evidence falls
back to that existing path. A valid enforced snapshot expiring before dispatch
is cancelled explicitly instead of being reinterpreted as a newer shot.

Accepted snapshots freeze origin, measured baseline RTT/rewind and session
identity. Dispatch adds server queue residence once. Primary rewind, rescue
lead checks and unclaimed-hit render checks use the same added age. Relevant
history must be continuous. Base rewind plus added age remains at or below the
existing 125 ms ceiling (and a lower weapon cap); rescue rungs cannot extend an
anchored shot beyond that ceiling. This can deliberately reject high-latency
or stale evidence rather than grant additional rewind.

A movement marker is bounded evidence, not proof of a physical click. This does
not measure one-way routing asymmetry or recover delay shared by both movement
and fire packets, and it is not evidence that ExitLag caused the reported issue.
An already-sent unmarked movement sample also yields a safe fallback; measure
admission frequency during flicks, buffered clicks, hitches and low FPS.

Server controls:

```text
ncp.FireAnchorMode 1
ncp.FireAnchorDebug 1
```

Mode 0 disables association; mode 1 is the default and evaluates candidates
without changing shot geometry/timing; mode 2 enables bounded correction. Debug
defaults to 0. Begin with mode 1, inspect admissions and dispatch expiry, then
compare mode 0 against mode 2 on a private server. Record the same scenario and
network conditions; a better score after reconnect alone is not validation.

## Loaded rockets: identity and explicit outcomes

Charged fire uses a reliable, ordered Begin/Release/absolute-mode protocol on the
launcher actor. Each weapon's volley has a nonzero 32-bit ID and each projectile
has an ordinal 0 through 2. A server-issued ownership generation accompanies
every request, receipt and projectile marker. Dropping and re-picking the same
launcher resets its sequence under a new generation, including when the same
pawn re-picks it. Old responses cannot cancel the new load. The server controls
load timing, ammunition and actual projectile count; client counts cannot
manufacture loads. Firing before the initial generation has replicated is a
readiness boundary that must be exercised during pickup/equip testing.

Owner receipts report accepted/completed/rejected/cancelled volleys and individual
spawned/resolved/rejected/cancelled rockets. Predictions reconcile with the exact
authoritative actor for their identity. Charged fakes are removed from generic
class/direction matching and byte/watermark acknowledgement cleanup. Old stock
Start/Stop/sync traffic cannot control the 329 charged lane. Replicated visual
load counters cannot overwrite the owning client's current volley.

Per-rocket receipts retain a server-issued network actor ID as well as the actor
reference. If the reference is initially unavailable, reconciliation retries
against that connection's actor registry. This covers stock grenade and spiral
classes as well as the identified native rocket class. Burst aim continues to
follow the existing per-projectile aiming path; IDs do not freeze sweep aim.

Charged predictions also bypass the legacy single delayed-projectile slot, which
cannot represent multiple loaded siblings independently. Cooldown/equip waiting,
release before the first load, mode changes and death cleanup remain explicit
state-machine work; identities alone would not fix those paths.

Enable on both peers for the private test:

```text
ncp.RocketVolleyDebug 1
ncp.FireProvenance 1
```

IDs prevent old/mismatched acknowledgements from retiring another load. They do
not guarantee a visible rocket through a legitimate server rejection, immediate
impact/explosion, death/cancellation or a failed connection. An unexplained loss
is a failure to investigate, not something to hide by keeping a fake alive.

## Validation and remaining gates

Native tests compile the actual anchor helper, timing/scope methods, gate logic
and rocket protocol core with engine adapters. Existing clock, eye-Z, movement
flush, bob, reservation and firing ownership regressions are also exercised.
Source contract checks verify original payload retention and lifecycle wiring.
Adapters do not validate UHT generation, Blueprint dispatch, actor reference
replication, effects, real net drivers or physics.

The focused suite below passed on 2026-10-02: 27 unittest cases, including
the helper's 62 native policy/lifecycle checks, exact-version gate checks and
21 extracted rocket ownership/RPC/reconciliation methods. The broader firing
trace, provenance, accepted-equip and stop-ownership regressions also passed.

Example focused command (PowerShell from the plugin root):

```powershell
$env:PYTHONPATH = (Resolve-Path tools/tests).Path
python -B -m unittest test_fire_anchor329 test_fire_anchor_integration test_rocket_volley test_version329_gate test_server_rate_reservation test_client_fire_timing test_fire_z_offset test_precision_move_flush test_bob_settings -v
```

Before release, use matching built 329 packages and validate:

1. UBT/UHT for supported client/server targets, then join, first-spawn fire,
   respawn, spectator transitions, reconnect, hub travel and mixed-version refusal.
2. One, two and three rockets; spread/grenades/spiral; early release, auto-release,
   held repeat, primary-to-load and load-to-primary, weapon swaps, ammo exhaustion,
   death during load and mid-burst, dropped/re-picked launchers and immediate wall
   impacts. Check ammo, damage, count and visuals on owner, server and another client.
3. Emulated latency, jitter, loss/reordering, low/high FPS and pause/time dilation.
   Correlate every predicted rocket ordinal with its outcome. Confirm no duplicate
   authoritative projectile or stale-result cancellation of the next volley.
4. Shock/Insta/Sniper mode 0/1/2 comparisons: normal shots, flicks, buffered shots,
   crouch/slide/teleport, rate reservations, movement timestamp resets, missing
   markers and insufficient history. Verify origin and all timing checks agree.

No UBT/UHT, PIE, cooked client or multiplayer validation has been performed for
this source candidate. Do not deploy it to the live 328 population as-is.
