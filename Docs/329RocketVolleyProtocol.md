# 329 charged rocket identities

This is a coordinated client/server protocol change. Do not mix these RPCs or
projectile replication layouts with 328. The exact hit-claim addition also needs
matching updated 329 client/server builds. Native regressions and the local UE4.15
module build are separate from the cooked multiplayer release checks below.

## Identity and authority

The key is weapon actor, server-owned ownership epoch, owning pawn, monotonically
increasing nonzero volley ID, then projectile ordinal 0..2. Sequence comparisons support uint32 wrap. Begin,
Release and absolute Mode messages are reliable; release duplicates never restart
loading, increase count or change the committed mode. Every RPC checks the 329
handshake and current owning pawn/epoch. The server advances the owner-only
replicated epoch on both removal and GivenTo, even when the same pawn drops and
re-picks the same inventory actor. A recreated client actor can safely restart
its volley counter at one. Old Begin, Release, Mode, AbortLoad, outcomes and
projectile observations cannot target that new lifetime.

The client predicts loading. The server still owns load timers and ammunition.
Begin waits for the existing refire/equip boundary, using the cadence helper and
EarliestFireTime, rather than setting a generic pending-fire bit that could bypass
the cooldown. Queued begins expire after five seconds or owner/state/match failure.
A release during that wait is retained and releases after the first legal load.

Release contains selected mode and requested count. The server
never promotes an incomplete load. It caps the volley to completed server loads
and the request, refunds only ammunition actually consumed for excess server-only
loads, and freezes the selected mode for the burst. Existing per-projectile aim
sampling remains unchanged, including sweep aiming during timed bursts. A tap during first load
retains the existing guaranteed-first-rocket behavior. Invalid configuration or
ownership cancels explicitly. Server grace release remains available.

## Reconciliation

- Accepted, completed, rejected and cancelled volley receipts are exact IDs,
  not watermarks. Completed includes a bitmask of ordinals that really spawned.
- Each ordinal separately reports spawned (with authoritative actor and its
  server-assigned NetGUID), already
  resolved during catch-up, spawn rejected or cancelled.
- Charged fakes never enter the legacy shared delayed-spawn slot. Each ordinal
  spawns independently, even above the projectile prediction ping cap.
- Charged fakes are removed from both generic watermark retirement and the
  class/direction-based fake matching pool. The exact authoritative actor performs
  the fake handoff; acceptance alone does not destroy a visible fake.
- `AUTPlusProj_Rocket` carries initial-only weapon/epoch/volley/ordinal identity and
  refuses heuristic matching for a loaded projectile. Its replicated identity
  can also resolve a previously unavailable actor reference on the owning client.
- Every projectile class, including stock grenade and spiral Blueprints, retains
  the server NetGUID when the RPC's Actor* is initially unmapped. Reconciliation
  looks it up in the same world's NetDriver cache until actor replication maps
  it, then checks instigator/class and pairs only that ordinal. This avoids
  relying on 4.15 to replay an RPC after an unresolved object reference maps.
- Late rejections cannot clear a newer load. Unnumbered FlashExtra, FlashCount,
  mode replication and ClientAbortLoad cannot overwrite the owning player's
  current ledger; remote spectator presentation remains separate.

Cancellation after partial dispatch preserves already-fired ordinals. Death
discharges completed loaded rockets through the identified path, then clears the
visual barrel count so the inherited removal path cannot create an unnumbered
duplicate. Dropped/destroyed weapon cleanup does not destroy already-paired fakes.

If an actor reference never resolves, its fake is not silently treated as
rejected. Its ordinary collision/lifespan still applies and reconciliation emits
an expiry diagnostic after 12 seconds. This is not a promise that visuals survive
arbitrary network loss, actor destruction or invalid custom projectile content.

The first charge still requires a nonzero server ownership epoch. A locally
controlled client now retains one initial alt-fire press, plus its release, for
up to 250 ms of real time while that epoch is zero. It creates no volley ID, RPC,
pending-fire permission, predicted charge or ammo consumption during the wait.
Repeated presses do not refresh the active buffer or queue more volleys. An
expired request is discarded; a later fresh press can start a new request.

After initial epoch cleanup, the buffer is consumed once through the normal
StartFire path. A retained release applies only to the exact resulting volley
on the same pawn/controller/world, and the existing first-load boundary still
applies. Buffered waiting time grants no load progress, cooldown credit or extra
rockets. An old primary retry does not erase the buffer; a fresh primary press
does. Death, disabled firing, missing ammo, a pending weapon switch, put-down,
detach, removal, destruction/end-play, ownership/possession changes or timeout
cancel it. Only the initial zero-to-nonzero epoch transition preserves the buffer.
This adds no RPC or replicated fields and does not alter the 329 wire protocol.
Test immediate pickup/equip taps on matching built clients and servers before
rollout, including releases while ownership replication is delayed past 250 ms.

## Diagnostics and validation

### Exact loaded-rocket hit claims

The shooter's replicated real `AUTPlusProj_Rocket` reports contact on the original
launcher using ownership epoch, volley ID and ordinal. Loaded rockets are always
fire channel 1, including ordinary spread rockets. The old hard-coded primary
channel 0 could not match their server tracking entries.

Authority snapshots the firing pawn and identity before catchup. Selection uses
that exact entry, live or recently resolved, without falling back to another
rocket or another launcher. The current ownership epoch and firing pawn must
match; a normal weapon switch or a newer volley does not invalidate a rocket in
flight. Dropping/reassigning the launcher does. Legacy fire-mode-only claims
cannot select identified loaded rockets. Missing or malformed identities fail
closed. Client fakes still do not send claims; this does not add pre-spawn claims
or recover a rocket that resolved before its real actor ever reached the client.

The existing target-history, capsule contact, projectile-path, LOS, ping/window
and direct-damage checks still decide the hit. An accepted tracking entry is
consumed before callbacks so duplicate or reentrant claims cannot damage again
or select another sibling. New spawns retain recently resolved records for the
existing grace period, within the existing ten-entry tracking bound.

For loaded rockets, resolution is recorded at actual explosion rather than an
overlap that stock might ignore. Before stock applies splash, a candidate overlap
query uses its adjusted blast radius, origin and collision channel. An exact
grace claim is rejected if its target could already have received splash, or if
the explosion snapshot is unavailable. This is deliberately conservative: a
candidate blocked from splash by geometry is also rejected. Normal authoritative
direct/splash damage is unchanged. The guard prevents a late full direct-hit
award on top of possible prior splash; it does not increase hitboxes or damage.

`python -m unittest tools.tests.test_projectile_hit_claims -v` compiles the actual
RPC gates, validator, resolution and pruning methods against a native adapter.
It exercises sibling isolation, duplicate/reordered claims, ownership changes,
close-range grace, preserved validation failures and damage callback re-entry.
It also covers 0/10/20/40 ms live/grace claims, the unchanged rewind bounds,
conservative splash rejection and mutations that deliberately break ordinal or
splash checks. The 18 claim tests pass, alongside the volley, fire-anchor and
client-cadence native suites. UE4.15 Editor, Windows Shipping client and Linux
Shipping server module builds pass. These checks do not simulate actor replication
or player input.

Before Friday's release, test matching cooked builds on a dedicated server at
0/10/20/40 ms RTT: release immediately before/after the third load; hit with each
of three spread rockets in varying orders; explode near a wall or another pawn;
switch weapons while rockets travel; start another volley before the previous
one resolves. Record server/client logs together. A possible-splash grace denial
must not add damage; a valid exact rescue must consume only its own ordinal.

### Volley lifecycle

`ncp.RocketVolleyDebug 1` on the actual client and match-server processes logs
weapon, ownership epoch, volley, mode/count, load completion, per-ordinal server spawn result/NetGUID,
terminal volley mask, client receipt and reconciliation expiry. It defaults off.

`python -m unittest tools.tests.test_rocket_volley -v` compiles the production
identity core and actual input/ownership/RPC/reconciliation/spawn-wrapper methods with a native
Unreal adapter. Tests cover wrap, replay, mismatched pawn, handshake rejection,
empty ammo, cooldown wait, release before state entry, count/refund bounds,
immutable release, partial cancellation, stale result, receipt before prediction,
temporarily unavailable actor, independent three-rocket spawn, no shared delay,
and spawned/resolved/rejected ordinal results. It also exercises same-pawn
drop/repick with a recreated counter, old-epoch Begin/Release/Mode/outcome rejection,
zero-epoch press/release buffering, fixed real-time expiry, duplicate callbacks,
local-only admission, lifetime cancellation, primary retry versus fresh input,
exact release identity after reentrant state changes, unnumbered charged-spawn rejection, and an initially unmapped
generic grenade resolving later by NetGUID without accepting a foreign instigator.

The adapter does not replace UHT, engine timers/weak references, actor channel
replication, Blueprint asset validation or multiplayer tests. Before release,
test spread/grenade/spiral at 0/50/100/200ms RTT with loss/reordering, all counts,
grace release, mode changes near grace, rapid primary/load transitions, equip and
release while queued, death mid-burst, swaps, repick, pawn respawn, listen host,
bots, and server impact before its actor reaches the client. Confirm one server
spawn/outcome per ordinal and no cross-volley pairing/visual deletion.
