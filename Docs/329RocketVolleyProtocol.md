# 329 charged rocket identities

This is a coordinated client/server protocol change. Do not mix these RPCs or
projectile replication layouts with 328. No Blueprint reparenting, UBT build,
cook, deployment or multiplayer validation was performed for this source pass.

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

The first charge is fail-closed until the owning client receives a nonzero
ownership epoch. An immediate pickup/equip click before that replication is not
latched and must be pressed again; no predicted volley starts in that window.
This readiness boundary is covered by the adapter but still needs an actual
pickup/equip canary. The change does not promise to fix that separate input-latch
edge case, and it should be checked before widening the rollout.

## Diagnostics and validation

`ncp.RocketVolleyDebug 1` on the actual client and match-server processes logs
weapon, ownership epoch, volley, mode/count, load completion, per-ordinal server spawn result/NetGUID,
terminal volley mask, client receipt and reconciliation expiry. It defaults off.

`python -m unittest tools.tests.test_rocket_volley -v` compiles the production
identity core and 21 actual ownership/RPC/reconciliation/spawn-wrapper methods with a native
Unreal adapter. Tests cover wrap, replay, mismatched pawn, handshake rejection,
empty ammo, cooldown wait, release before state entry, count/refund bounds,
immutable release, partial cancellation, stale result, receipt before prediction,
temporarily unavailable actor, independent three-rocket spawn, no shared delay,
and spawned/resolved/rejected ordinal results. It also exercises same-pawn
drop/repick with a recreated counter, old-epoch Begin/Release/Mode/outcome rejection,
zero-epoch readiness, unnumbered charged-spawn rejection, and an initially unmapped
generic grenade resolving later by NetGUID without accepting a foreign instigator.

The adapter does not replace UHT, engine timers/weak references, actor channel
replication, Blueprint asset validation or multiplayer tests. Before release,
test spread/grenade/spiral at 0/50/100/200ms RTT with loss/reordering, all counts,
grace release, mode changes near grace, rapid primary/load transitions, equip and
release while queued, death mid-burst, swaps, repick, pawn respawn, listen host,
bots, and server impact before its actor reaches the client. Confirm one server
spawn/outcome per ordinal and no cross-volley pairing/visual deletion.
