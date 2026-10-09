# 328 firing reconciliation source patch

Status: implemented for review and user compilation; runtime acceptance is **not** established.
Baseline: `9388468` on `328-release-candidate`, clean before this session. No pull, branch switch,
reset, C++ build, commit, push, UT4AC repin, or release identity change was performed.

## Finding and evidence

This is a **source-supported mechanism consistent with the observed failures**.
The existing logs establish client/server firing-time disagreement; they do not identify the
server callback or establish successful projectiles from timestamps.

Stock `AUTWeapon::GotoActiveState` (`UTWeapon.h:836–842` at the supplied UT source) arms the
base resend timer. Stock `ResendNextFireEvent` (`UTWeapon.cpp:491–549`) polls held bits only
when its queue is empty and the weapon is current. Stock state reconciliation (`569–589`)
synthesizes Start/Stop with the accepted byte sentinel 255 (`603–610`). The previous NCP
override delegated that reconciliation unchanged; the previous FireShot gate accepted state
flags as request provenance. This is one conditional mechanism, assessed P1 here, not three
independent proven failures and not an immortal timer leak.

See [log evidence](328-firing-reconciliation-log-evidence.md) for hashes, sessions, counts and
line references. `(21)` and the supplied server are matched. The newly supplied backup is an
earlier, distinct session. The unavailable `(22)` reference was not silently substituted.

## Bounded design and implementation

1. `ServerUpdateFiringStates_Implementation` explicitly scans at most
   `min(8, GetNumFireModes(), FiringState.Num())`, re-reading owner and pending state after
   callbacks. Both synthetic Start and Stop are skipped for actual
   `UUTWeaponStateFiring_Transactional` objects, including subclasses. Other states retain
   stock reconciliation. The base and fixed queues/handles remain separate.
2. The existing virtual `GotoState` slot rejects unauthorized remote transactional entry.
   Stock ActiveState returns after its first attempted pending-mode transition, so the guard
   continues a bounded scan for a permitted pending mode. It does not clear pawn input.
   `FireShot` independently requires exact accepted context before timestamps, ammo or spawn.
   A blocked unowned FireShot schedules an owner/state/mode/generation-checked recovery;
   newer accepted work invalidates that callback.
3. The existing file-local request context now covers synchronous fixed requests, deferred
   equip, and deferred stock-state completion. It retains weapon-key, owner, generation,
   mode, original event, rotation, resolved Z offset, hit claim, expected target/waiting
   states, consumed status and release status. No UObject field was added. Authorization
   consumes the exact context before firing hooks and copies its payload before any callback.
   Restoring aim and removing context require the same surviving generation/owner.
4. Charged completion commits an accepted primary directly before a held-alt restart. It
   verifies no loaded rocket or load/grace/burst/refire timer still owns work. A released
   accepted event still dispatches once and receives guarded release cleanup. Generic held
   primary on a remote human cannot invoke the old unnumbered next-tick StartFire route.
   Empty/watchdog exits through GotoActiveState reach the same retained-context boundary.
5. Review found another real deferred path: Minigun Plus uses stock SpinUp primary but fixed
   shard secondary. A shard accepted while the server is spinning down must survive until
   `UUTWeaponStateFiringSpinUp::CooldownFinished` enters ActiveState. The same retained context
   handles this as `DeferredStateTail`; it does not take over stock spin-up timers. Normal
   Link Plus/NCP code explicitly uses stock Start/FireShot and configured stock states.
6. The queue remains one slot. A new fixed Start cannot supersede an unconsumed accepted
   reservation: it receives the existing watermark ACK before validation can advance that
   watermark. Once the reservation commits, subsequent requests use normal validation.
   This changes old equip latest-wins behavior deliberately to avoid discarding ACKed work.
   Additional predicted requests can be rejected while busy; this is not an unbounded queue.
7. Commit-time ammo, disabled-fire and match/mode policy are checked for deferred state tails.
   Lifecycle cleanup invalidates requests on switch/removal/destruction/new equip; invalid
   waiting-state contexts cannot permanently hold the reservation lock. Explicit policy,
   failed-dispatch or lost-state cancellation remains an investigation item in diagnostics.
8. Local prediction, standalone, listen hosts, explicit `AUTBot` controllers and demo playback
   retain their local execution paths. `!IsLocallyControlled()` is never used as a bot test.
9. The Stop deep trace found an accepted equip-primary loss when a release cleared primary
   pending while charged alt remained held. Exact transactional equip reservations now commit
   before ActiveState scans other held modes, retaining their saved payload and clearing the
   stock queue to prevent replay. False-pending dedicated-server dispatch keeps stock's delayed
   origin timing. Other mode bits and normal Stop/retry semantics remain unchanged.

Stop semantics are deliberately unchanged. The normal Stop watermark cannot identify two
separate no-shot releases, and old-weapon Stops can still clear pawn-global input. See the
[separate Stop audit](328-firing-reconciliation-stop-audit.md). Filtering stock sync does not
resolve these defects. No new counter, duplicate-PendingFire heuristic, ACK meaning, wrap
rule, or release protocol was introduced.
The [follow-up deep trace](328-stop-identity-deep-trace.md) establishes a reliable-original
identity candidate, its loss-latency tradeoff, stock-byte counter coupling, and cross-weapon
ownership counterexamples with executable source-semantics models.

## Diagnostics and checking

Enable on the compiled server before reproducing:

```text
ncp.FireProvenance 1
ncp.RocketPrimaryDiag 2
```

Enable `ncp.RocketPrimaryDiag 2` and optionally `ncp.FireDebug 1` on the owning client. All new
per-event diagnostics are default-off. The existing one-per-class StateLayout row now includes
`firingBuild=328-fire-auth-r2` and compile date/time. This is a source/build marker, **not** a
verified DLL hash or final commit identity. Record deployed DLL/SO hashes separately.

`[NCFireAuth]` records cover SYNC, BLOCK_ENTRY/BLOCK_SHOT, ACCEPT, REJECT, ACK, SHOT, SHOT_END,
SPAWN, STOP, RESERVATION_BUSY and CANCEL. Numbered sources are FixedInitial, FixedRetry,
DeferredEquip, DeferredChargedTail and DeferredStateTail. Other allowed paths identify
StockManaged, ChargedDirect, Bot, ListenHost and Standalone. StockSync identifies the
reconciliation boundary. STOP records contain received/prior/auth IDs, pending bits, owner,
current weapon and retained context generation; no physical release generation is invented.

ACCEPT includes captured aim and claim identity. SPAWN reports actual returned projectile,
class and actor rotation, or a null/suppressed result. SHOT_END reports observed spawn attempts
and successes separately from LastFireTime. A shot may produce multiple Flak projectiles.
Zero observed attempts does **not** prove an override: hitscan, Blueprint overrides, missing
owner/class and custom spawn paths are also possible. The patch does not evaluate
FireShotOverride twice or replace stock FireShot to guess its return value.

The existing owner-lost trade-grace SpawnActor branch bypasses request validation/FireShot.
Its behavior is unchanged and its result is explicitly labeled DIRECT_SPAWN/TradeGraceDirect.
Thus this patch does not establish a global theorem that every authoritative projectile in
all NCP paths has numbered provenance. Replays and custom/Blueprint spawn paths also require
separate interpretation. Charged-direct outcomes are counted independently of primary events.

From the plugin directory, after a runtime capture:

```powershell
python -B tools/check-fire-provenance.py server.log --rocket-primary
python -B tools/check-fire-provenance.py server.log --all --json
```

Exit 0 means observed records reconcile, 1 means a detected contradiction, and 2 means
incomplete evidence. The checker compares per-request dispatches and per-dispatch spawn
records, never converts timestamps into shots, and does not count multiple Flak projectiles
as multiple requests. Policy/state-loss cancellations remain unresolved. Use one capture per
session; actor-name/event reuse with different generations is reported as ambiguous.

## Verification performed

- Independently read stock reconciliation, ActiveState, equip, charged completion, fixed
  validation/Stop/ACK, minigun mode split and spin-down code.
- Three independent review passes found and addressed pending-mode starvation, stale
  watchdog reservations, policy bypass at direct tail commit, nested-generation cleanup,
  different-mode spawn attribution, and minigun deferred shard loss.
- Standalone Python checker tests pass, including duplicate dispatch, lost/truncated rows,
  retained event with a newer Stop watermark, suppression, multiple Flak results, explicit
  charged-direct outcomes, per-shot counter mismatches, and deferred minigun mode1.
- The supplied historical server log correctly reports incomplete evidence because it has
  no NCFireAuth rows. It cannot validate this uncompiled patch.
- Diff whitespace, source delimiter checks and reflection/declaration comparison are local
  inspection checks only. They are not an Unreal compiler or runtime test.

## Runtime acceptance matrix — all pending user compilation/testing

| Case | Required evidence |
|---|---|
| Same-gun rocket press before ROF, release and retry | No rejection creates a projectile; each accepted request dispatches once |
| Repeated taps around 0.2-second stock-sync phase | Filtered transactional mismatches produce no manufactured shot or Stop |
| Press/release during equip | Original accepted aim/event survives; release cleanup does not cancel the reserved shot |
| Accepted equip primary released while charged alt is held | Exact primary commits before held-mode scan, once with saved aim and stock delayed-origin timing; alt pending survives |
| Held through one and multiple switches | Incoming weapon gets client-owned requests; no old callback commits or clears newer state |
| Rapid Shock/Rocket/Flak alternation | Event/ACK/provenance pairing remains coherent across modes/weapons |
| Deferred equip followed by aim/stance change | One shot uses captured view/Z/claim, not current server aim |
| Charged load/release/three-rocket volley | Stock load/burst count/timing preserved; primary authorization never owns volley outcomes |
| Primary during burst and last ~40 ms tail | Accepted request survives its Stop, commits once after full completion, uses original event/aim |
| Held alt plus accepted tail primary | Exact accepted primary wins completion once; no repeated server primary from held bits |
| Sniper/Shock zoom | Stock zoom reconciliation and direct primary transition remain usable |
| Minigun continuous/spin-down then shard | Stock stream preserved; accepted shard survives server spin-down as DeferredStateTail |
| Link beam and normal Link primary | Stock mode families continue using their existing paths |
| Bot/listen/standalone/dedicated/replay/spectator | Explicit permitted local paths preserved; remote transactional shots require context |
| Duplication/loss/lag/reordering | Retry idempotence, matching accepted ACKs, retained payload and busy-slot behavior |
| Death/drop/reconnect/travel/rapid switch | Old generations cannot commit or undo a newer state; cancellations are visible |
| Repeated no-shot releases/old Stop/new hold/wrap | Diagnose the separately unresolved Stop and existing fixed-counter wrap defects |
| 60/240/480/700 FPS, 4 kHz input | Repeat timing/phase cases; judge event, ACK, provenance and spawn counts |

For active, legal, uncanceled accepted numbered requests, require one dispatch and the expected
outcome count, captured aim for delays, no unauthorized remote transactional entry/shot, and
no retained-context loss. The overall “no stale pending after every genuine release” criterion
cannot be declared passed until the separate Stop-identity/ownership defect is solved.

## Compatibility and release boundary

The only header changes are native helper declarations and an override of an existing virtual
slot. RPC/UFUNCTION declarations, replicated fields, serialization and UObject/state member
layouts remain unchanged; no Blueprint or companion pak changes are included. Mixed 328
client/server rollout still needs runtime testing, particularly the one-reservation policy.
User compilation and runtime results are required before calling this release-ready.

UT4AC must be pinned only after an approved final NCP commit and an explicit release request.
Do not rewrite shipped dogfood17/plugin117 identity; any later importer/build additions remain
separate follow-up work.
