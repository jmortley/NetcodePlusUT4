# 328 Stop identity and ownership: deeper trace

Date: 2026-09-04. Branch `328-release-candidate`, HEAD `9388468`, with the
uncommitted firing-reconciliation patch. This follow-up distinguishes source
findings, modeled ordering, the additional code correction, and runtime work.
The updated source marker is `328-fire-auth-r2`; it is not a runtime DLL hash.
It supersedes the earlier Stop audit's unresolved general concerns where the
more specific findings below apply. The supplied historical logs do not identify
the exact Stop/input/spawn chain; these are not newly proven incident events.

## What the deeper trace changed

1. **A reliable original Stop has usable transport identity.** Unreal suppresses
   retransmission duplicates before the RPC executes. Separate original calls
   with the same fixed shot watermark are distinct logical Stop commands.
2. **Incrementing releases has a concrete mixed-rollout regression.** Accepted
   fixed shot IDs overwrite stock UT's separate byte counter. Changing the
   fixed counter's stride can make a later charged Start look like a duplicate.
3. **The shared pending bit cannot establish physical ownership across weapon
   channels.** Stock Start/Stop paths also mutate it; guarding only fixed Stops
   does not isolate ownership. Blanket equip clearing harms held input across
   successive switches into stock-managed modes.
4. **An accepted shot can own dispatch without owning that shared bit.** The
   trace found a real gap in the working patch's equip path and corrected it:
   the exact accepted transactional equip request now commits before stock's
   pending-mode scan, retaining its event, aim, generation, and release state.

Normal Stop numbering, acceptance, retry timing, and pawn-wide held-input
semantics remain unchanged. The new gameplay edit is confined to the accepted
transactional equip completion path. No C++ build, commit, push, branch change,
RPC/layout/protocol change, or asset change was performed.

## Input to release: the actual call chain

Paths under `NCP` below mean this plugin's `Source/`; `UT` means
`D:/UnrealTournament/UnrealTournament/Source/UnrealTournament/`; `Engine` means
`D:/UnrealTournament/Engine/Source/Runtime/Engine/Private/`. Function names remain
the stable anchors as the uncommitted patch shifts line numbers.

| Boundary | Source and consequence |
| --- | --- |
| Raw/controller input | UT `Private/UTPlayerController.cpp`, `OnFire`/`OnStopFire` and alternate variants, approximately 1600-1671: update controller flags and append deferred fire inputs. |
| Deferred dispatch | Same file, `ApplyDeferredFireInputs`, 3764-3790: drains input in order after movement and resolves the character's current weapon at dispatch. The final controller flag cannot reconstruct every edge in a multi-edge frame. |
| Character dispatch | UT `Private/UTCharacter.cpp`, `StartFire`/`StopFire`, 2647-2688: locally controlled input forwards to the current weapon; Stop with no weapon clears pending. `StopFiring` also supplies game-driven Stops. |
| Logical/internal Stop | NCP `Private/UTWeaponFix.cpp`, `StopFireInternal`/`StopOwnerFireInternal`/`StopFire`: internal stops exist for buffered shots, cross-mode cancellation, release reconciliation and game logic. `bHandlingRetry` is not a universal physical-input identity. |
| Normal fixed Stop | NCP `StopFire`: sends current `ClientFireEventIndex[mode]`; no increment. Its application copies repeat the same payload. |
| Charged route | NCP `StopFire`: stock `Super::StopFire` runs first, then the fixed counter increments and a fixed Stop is sent. Current charged state selects this branch even for a primary Stop. The stock call can synchronously discharge a charged volley; do not hoist the counter change above it. |
| Server fixed Stop | NCP `ServerStopFireFixed_Implementation`: deduplicates `<= last Stop`, rejects `< authoritative` and `> authoritative + 10`, advances both watermarks, marks the same weapon/mode's retained release, and clears pawn pending before its old-weapon state-lifetime guard. |
| Application retry | NCP `ResendServerStopFireFixed_Implementation`: calls the same implementation, so its first arrival can consume the ID and advance the authoritative watermark. |

Zoom and Link use stock release paths. Minigun Plus has stock primary spin-up
and a transactional alternate mode. Charged rockets inherit stock firing-state
behavior for their charge mode. Classification must use the configured mode's
actual state, not a weapon-wide label or a class name containing "Transactional".
Listen hosts, standalone, bots and replay retain their separate local behavior.

Do not suppress every internal Stop: a buffered Shock shot can fire after the
physical release, obtain a new shot ID, and need an internal Stop to finish the
server session. Cross-mode cleanup also needs to end the preceding mode.

## Same-weapon identity: what reliable ordering actually buys

NCP's initial fixed Start and Stop RPC declarations are Reliable; their resend
RPCs are Unreliable (`NCP Public/UTWeaponFix.h`). Engine source confirms:

- `NetConnection.cpp:1118-1123`: already-processed reliable bunches are discarded.
- `DataChannel.cpp:316-359`: reliable gaps are buffered, including duplicate
  suppression in that queue.
- `DataChannel.cpp:420-425`: the reliable sequence advances per channel.

This identity is an ordered logical command, not proof of a physical mouse-up.
Internal Stop calls may create additional originals. Initial and retry paths
would need explicit separation; accepting every duplicate merely because
pending is true remains incorrect.

A concrete server-only candidate for **actual transactional modes** is:

- Let each engine-delivered reliable original pass the equal-Stop-ID case,
  retaining the authoritative lower bound and existing +10 upper bound.
- Make application-level unreliable Stop copies non-mutating for those modes.
- Keep stock-managed modes, client counters, fixed Starts, ACKs and fake pairing
  unchanged. Engine reliable retransmission remains responsible for delivery.

This resolves the same-ID original/copy ambiguity in the modeled domain and
does not let an old Stop clear a newer **accepted numbered** same-mode request:
that request already advanced the authoritative watermark. It also removes this
additional loss mechanism:

```
Unreliable Stop K arrives while reliable Start K is missing
Current:   Stop advances Auth to K -> eventual Start K is rejected as old
Candidate: Stop copy does nothing -> reliable Start K -> original Stop K
```

It is not enabled. Waiting for reliable recovery can delay a release compared
with the current application-copy path. That changes packet-loss behavior under
the original compatibility requirements. Leaving both paths mutating while
accepting equal originals can execute native or subclass stop hooks twice.
Newer Start retries can still overtake older Starts; this proposal does not fix
all request ordering or establish global physical input ownership.

The stdlib [identity model](../tools/experiments/stop_identity_model.py) enumerates
these bounded delivery orders, with eventual reliable originals and optional
unreliable copies. Counts are not measured failure rates:

| Domain | Orders | Current Stop retires an unprocessed Start | Candidate does so |
| --- | ---: | ---: | ---: |
| One shot, up to two copies per call | 685 | 212 | 0 |
| Two shots, up to one copy per call | 2,721 | 1,216 | 0 |

The candidate still misses an older Start in 410 two-shot orders due to Start
retry overtaking. These models omit real timers, ROF/policy rejections, actor
destruction, packet encoding, and projectile dispatch. They do not prove runtime
behavior or signed `int32` wrap correctness.

## Why "increment every release" is unsafe for mixed rollout

`NCP ServerStartFireFixed_Implementation` permanently assigns
`FireEventIndex = (uint8)InFireEventIndex` after validating a fixed Start. This
is stock UT's inherited byte sequence, shared across modes, not the per-mode
fixed counter. Stock `ValidateFireEventIndex` (`UT Private/UTWeapon.cpp:603-619`)
uses it to reject old/duplicate charged/stock messages. Temporary save/restore
around a shot does not undo the permanent assignment before that scope.

The executable counterexample is:

| Step | Existing client | Client that increments ordinary releases |
| --- | --- | --- |
| Two charged Start/Stop cycles | Client stock byte reaches 4 | Same |
| Three primary shots | Fixed IDs 1, 2, 3; server stock byte ends at 3 | Fixed IDs 1, 3, 5; old server stock byte ends at 5 |
| Next charged Start | Client stock byte 5 is accepted | The same byte 5 is rejected as a duplicate |

An updated server could separate permanent stock sequencing from scoped fixed
projectile/claim IDs, but that alone does not protect a changed client connecting
to an older server. Resetting the client stock byte to a per-mode fixed counter
would introduce a different backward-sequence/retry hazard.

The earlier audit's generic ACK concern is weaker than this concrete finding.
For an accepted shot K, its reliable confirmation is queued before a later
rejection can confirm a Stop watermark K+1 on the same weapon channel. The
counter increment alone therefore does not demonstrate valid-fake destruction.
Deferred ACK already means reservation, not spawn. Those facts do not remove
the byte-counter collision or cross-weapon ownership problem.

## Ownership across switches

Character switch/verification RPCs use the pawn actor; fire RPCs use weapon
actors. Reliable order within each does not impose a total order across them.
`UT Private/UTCharacter.cpp`, `WeaponChanged:3293-3321`, assigns the incoming
weapon and calls BringUp while retaining pawn pending bits.

The [ownership model](../tools/experiments/stop_ownership_model.py) exhibits a
shared server receive prefix `switch B, Start B, Stop A` for two client histories:

- A was released before switching to B and pressing B; B is still held.
- B fired while held, then A became current locally but remained equipping;
  A was released before another A shot. The switch-back RPC is still in flight.

At that prefix, physical pending should differ, but available per-weapon
watermarks and received traffic cannot tell which history occurred. This is a
transient information limit, not a proof that no compatible redesign exists;
the delayed switch can later resolve the difference.

Two tempting ownership changes have additional concrete problems:

1. Guarding only fixed Stops misses stock `ServerStopFire` and
   `ServerStopFireRecent`, which call `EndFiringSequence` and clear the shared
   bit unconditionally (`UT Private/UTWeapon.cpp:777-824`). Stock Start paths also
   reach `BeginFiringSequence` and set it even from an inactive outgoing weapon
   (`625-741`); requesting client verification does not return early.
2. Clearing incoming transactional pending at BringUp loses legitimate held
   intent through A -> transactional B -> stock C if B is switched away before
   a numbered B request re-latches the server. C's local held auto-start need not
   send a Start RPC; the server waits for stock's subsequent 0.2-second poll,
   plus transport delay/loss. BringUp can also mean cancelling a switch back to
   the current weapon, not necessarily a fresh incoming lifetime.

Existing native Begin/EndFiringSequence slots could guard more mutations without
new RPC declarations, but do not themselves solve the held-carry tradeoff. The
ownership model includes both the stock bypass and the multiple-switch regression.

## Implemented correction: accepted equip dispatch wins before the held scan

The first firing patch already keys accepted work to weapon, owner, mode, event,
generation, expected target and waiting state. Most native transactional paths
can dispatch that work despite an old weapon clearing the pawn bit. The deep
trace found an exception:

1. Rocket B accepts primary K during equip. Its stock equip queue and saved
   context contain K; charged alternate pending is also true.
2. B's own Stop K, or outgoing A's Stop, clears primary pending. Only B's Stop
   marks B's retained request released; A's Stop has no such ownership.
3. Stock `BringUpFinished` enters ActiveState before replaying the queued mode.
   With primary false, the held charged mode wins that scan.
4. The later primary replay enters charged's inherited BeginFiringSequence,
   which queues instead of firing. The earlier wrapper then cleared the equip
   context after that one attempt, losing accepted K.

The additional edit extends `AUTWeaponFix::CompleteAcceptedDeferredFire` to an
exact transactional equip reservation when the completing state is Equipping
and the stock queue still matches its mode. The existing GotoState pre-scan hook
then enters the accepted target directly. It uses the existing policy, owner,
generation and target checks and existing captured payload/once-only consumption.
Clearing the consumed context also clears the stock queue, preventing replay.
An owned release retains its guarded post-commit cleanup. Neither mode's pending
bit is synthesized to choose the shot.

False-pending equip replay previously set `bNetDelayedShot` on dedicated servers,
which affects stock projectile start position. The direct commit preserves that
timing input for the same case and guards restoration by the surviving request,
owner and target state. The flag still supplies no shot authorization.

The extension is restricted to actual `UUTWeaponStateFiring_Transactional`
targets: stock spin-up may not fire on entry and must not consume a reservation
under this assumption. Zoom's stock replay already redirects primary immediately;
the proven dropped-shot example is charged fire, not zoom. The charged/zoom
pending bit remains intact for its normal later handling.

This resolves a dispatch-ownership consequence without claiming the shared bit
is a precise physical-input ledger. It does not fix reused normal Stop IDs.
Remote timing changes can still arm inherited refire callbacks; the provenance
gate, not a claim that those timers never run, prevents unaccepted shots.

## Validation and runtime acceptance

Run the Python models and tests with:

```powershell
python -B -m unittest discover -s tools/tests -p 'test_*.py'
python -B tools/experiments/stop_identity_model.py
python -B tools/experiments/stop_ownership_model.py
python -B tools/experiments/accepted_equip_model.py
```

These are semantic model and log-checker tests, not compiled C++ tests. After the
user compiles, prioritize accepted rocket primary during equip with charged alt
held, then release primary before completion; repeat with an outgoing weapon's
delayed Stop. Require one DeferredEquip dispatch with the accepted event/aim,
no second stock replay, and owned-release cleanup that cannot touch a newer
generation. Also repeat with zoom, stock spin-up, held multiple switches, latency
and loss, dedicated/remote-listen clients, local hosts, bots, standalone and replay.
Use the firing provenance checker to distinguish reservation, dispatch and spawn.

Local verification passed **72 Python tests**. The accepted-equip model explores
66 bounded orders: the preceding Active-first path loses 24 accepted requests;
the corrected priority path dispatches all 66 once with their saved event/aim.
Those counts describe the chosen model cases, not a live failure rate. Source
delimiter checks, unchanged reflected declaration blocks, unchanged fixed Stop
behavior/retry bodies, and `git diff --check` also passed. The r2 completion and
delayed-origin scope received a separate source review; none substitutes for
compilation and the runtime acceptance cases above.

The release-identity candidate needs a separately measured loss-latency decision
before implementation; no test result here authorizes calling it rollout-safe.
