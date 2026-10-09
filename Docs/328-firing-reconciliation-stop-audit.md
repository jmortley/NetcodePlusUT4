# 328 normal Stop identity audit

Follow-up: [deep trace and executable ordering models](328-stop-identity-deep-trace.md)
establish reliable-original identity, a concrete stock-byte counter collision,
and cross-weapon ownership counterexamples. That trace also adds accepted equip
dispatch priority to the firing patch. Normal Stop numbering/acceptance remain
unchanged. Read the follow-up for the more precise ACK and retry conclusions.

Audited against `9388468` on `328-release-candidate`. Source line numbers below
refer to that baseline, before the firing-reconciliation changes in this working
tree. Paths beginning `Source/` are relative to this plugin; stock UT paths are
relative to `../../Source/UnrealTournament/`.

## Decision and scope

Keep normal Stop numbering and acceptance behavior unchanged in this patch. Add
default-off diagnostics at the existing rejection branches, and keep a semantic
Stop correction separate until its ordering and ownership rules are established.
This is an explicit unresolved defect, not a claim that filtering stock held-state
synchronization fixes release identity.

The 328 constraints prohibit changes to RPC/UFUNCTION signatures, replicated
fields, serialization, network protocol, state layout, Blueprint assets, and
companion pak requirements. A local implementation change must also work during
the normal client/server rollout. No C++ build or runtime verification was
performed for this audit; the user compiles the plugin.

## Confirmed source behavior

- Normal `StopFire()` sends the current `ClientFireEventIndex[mode]` without
  incrementing it (`Source/Private/UTWeaponFix.cpp:3353-3368`). The counter normally
  advances when client `FireShot()` generates a numbered request (`2685-2687`).
- `ServerStopFireFixed_Implementation()` returns when the received index is less
  than or equal to `LastProcessedStopEventIndex` (`4305-4308`). This precedes
  deferred-release marking (`4339-4349`) and the pawn pending clear (`4443`).
- A Stop older than the authoritative index is rejected (`4317-4319`), and a Stop
  more than ten indexes ahead is rejected (`4321-4326`). An accepted Stop advances
  the authoritative watermark and the Stop watermark (`4328-4332`).
- Initial fixed Starts and Stops are Reliable
  (`Source/Public/UTWeaponFix.h:752,762`). Application-level retries are Unreliable
  (`893,897`) and repeat the original event payload. The retry Stop funnels into
  the same implementation (`Source/Private/UTWeaponFix.cpp:8634-8639`).

Consequently, distinct no-shot press/release cycles can reuse one Stop index.
The later release can be discarded as if it were a retry. Source inspection
establishes this identity defect; it does not establish which individual release
in the reported session encountered it.

## Why pending state cannot supply the missing identity

Consider the same weapon, owner, and mode with a previously processed Stop `K`.
No newer numbered shot has been accepted. At a later Stop boundary, pending is
true and the received payload is again `(mode, K)`:

1. The player made another press/release without a shot. An upstream held-state
   path latched server pending between releases. The new release must clear it.
2. The player is holding a newer press without a shot. A delayed copy of the old
   release arrives after an upstream held-state path latched pending. That old
   release must not clear the newer hold.

The Stop payload supplies neither a press generation nor a release generation.
The pending bit and existing Stop/shot watermarks at this boundary do not
distinguish these histories. Initial-versus-retry transport is useful diagnostic
information, but treating every reliable original or every pending duplicate as
a fresh release has not established ownership against intervening unnumbered
intent. A server-local generation for an accepted shot is not evidence of the
physical generation of a later no-shot input.

This is a limitation of the present handler's information, not a proof that every
possible wire-compatible redesign is impossible. A defensible redesign must
define the missing ordering and ownership explicitly.

## Why incrementing normal Stops is not the complete correction

Incrementing the shared counter makes subsequent releases numerically distinct
and is mechanically accepted by the present RPC signatures. It does not alone
meet the requested acceptance criteria:

**Pawn-global ownership across weapons.** Stop computes whether this weapon owns
the current state lifetime at `Source/Private/UTWeaponFix.cpp:4406-4409`, clears
pawn-global pending at `4443`, then returns for an old lifetime at `4446-4453`.
Independent per-weapon counters cannot order an old weapon's release against a
newer hold on the incoming weapon. Simply suppressing every old-weapon release
would also require proving that genuine release-during-switch intent still
reaches the current owner.

**The authoritative index is also an ACK watermark.** Stops advance it at `4328`.
Rejected Starts ACK the current watermark at `3693`. Client ACK handling cancels
delayed Flak reservations at `8527`, processes tracked fakes at `8540`, applies
special exact-event retention for unpaired Flak at `8564`, and removes retries at
`8580`, using comparisons with that same index. More Stop-only indexes change
the meaning of these comparisons. This needs explicit tests with skipped,
rejected, queued, and accepted shots; a Stop ACK must not be treated as proof of
a projectile. Existing accepted deferred requests are already ACKed before their
eventual firing (`3959-3975`).

**Lookahead and transport ordering.** Start sequence validation requires
`last < incoming <= last + 10` (`3588`); Stop has the same upper window (`4321`).
Reliable originals can catch up in actor-channel order after packet loss, so
loss alone does not prove permanent lookahead exhaustion. Unreliable retries
overtaking originals, locally consumed indexes for rejected shots, and additional
Stop increments still need to be modeled together. Do not widen the acceptance
window without an independently justified bound.

**The fixed counter has no implemented wrap policy.** Client allocation uses
signed `int32 + 1` (`3576`), and Start lookahead uses signed `last + 10` (`3588`).
Start payload validation rejects nonpositive IDs (`4285`); Stop validation rejects
negative IDs (`4582,8653`). The retry comment calling `<= last` plus a difference
check "wrap-around safe" (`8615-8616`) does not establish wrap correctness. Stock
UT's separate `uint8` sequence explicitly skips sentinel `255`
(`Private/UTWeapon.cpp:469-473,759-763`); this is not the fixed `int32` sequence.
Do not silently add a new wrap convention to the 328 wire semantics.

## Retained accepted requests and release ownership

An accepted request's event and generation must remain immutable even if a later
Stop advances the authoritative watermark. In existing code, `StopFire(0)` while
the current state is charged takes the charged Stop branch regardless of the
requested mode (`Source/Private/UTWeaponFix.cpp:3253-3258`) and increments that
mode's client counter (`3275-3278`). Thus accepted primary `K` followed by Stop
`K+1` during the charged tail is already a legitimate sequence.

Do not require a retained request's event to equal the current authoritative
watermark after release. Existing deferred release marking allows
`Stop >= retained event` (`4347`), while its later cleanup also checks generation,
owner, current weapon, and expected state (`2514-2553`). A new accepted request
must invalidate older cleanup ownership. A retained request generation should be
logged as such; it must not be labeled a known physical release generation.

## Diagnostic requirements

At each existing rejection branch, log behind a default-off firing diagnostic:

- Reason: repeated/older Stop, older than authoritative event, or excessive jump.
- Initial/retry transport, received ID, previous Stop ID, authoritative ID, mode,
  pending bits, and LastFireTime.
- Weapon and owner identity, current weapon, pending weapon, current state, and
  whether this weapon owns the current state lifetime.
- Any retained request's event, generation, owner match, release flag, consumed
  flag, and expected-state match. Report absence explicitly.

Client send diagnostics should make repeated same-index sends visible. These
observations can demonstrate a discarded release with pending latched, but do
not alone identify physical input, prove that pending was stale, or prove a
projectile spawned. Correlate them with input evidence and the new firing-source
and spawn-result diagnostics. No diagnostic should accept a duplicate Stop or
alter a pending bit merely to improve a trace.

## Runtime Stop tests after the user compiles

Run on a dedicated server with a remote human client, then repeat the applicable
cases on listen host and standalone. Capture both client sends and server Stop,
authorization, ACK, retained-request, and spawn-result rows. Record the exact
plugin fingerprint and diagnostic settings on both endpoints.

| Case | Exercise | Required observation |
| --- | --- | --- |
| Repeated no-shot taps | Several press/releases within cooldown and during equip, including around stock sync's 0.2-second phase | Identify repeated Stop IDs and each server rejection; do not claim the unresolved identity defect is fixed. |
| Retry idempotence | Deliver initial Stop and its two retries in varied arrival order | One logical release has no repeated firing/state cleanup; later copies do not cancel newer numbered intent. |
| New same-weapon hold | Delay old Stop/retry until after a newer numbered Start | Old Stop cannot clear the newer accepted hold or its retained context. |
| New unnumbered hold | Delay old Stop/retry across a no-shot press with no accepted numbered Start | Capture the missing ownership distinction; pending alone is insufficient evidence to accept the old Stop. |
| Switch ordering | Delay outgoing weapon Stop across one and multiple switches; repeat with genuine release during switch | Detect whether old release clears incoming pending; verify genuine release still reaches the appropriate owner. This remains an unresolved semantic requirement. |
| Accepted equip request | Accept event K while equipping, then release before dispatch | Exactly one retained shot uses K's captured aim; release cleanup cannot erase a newer generation. |
| Charged tail | Accept primary K during the final charged-tail overlap, then send Stop K or K+1 as the actual client path dictates | Original accepted request remains owned, fires once with its captured payload, and exits without a second manufactured shot. |
| ACK/fake pairing | Mix accepted/rejected events, delayed Flak fakes, release, and ACK delays | ACK watermark changes do not cancel a valid later fake/request or falsely establish a spawn. |
| Loss and lookahead | Lag/loss/duplicate retries with sequence gaps 9, 10, and 11; allow reliable originals to recover | Observe exact acceptance/rejection and eventual reliable ordering; distinguish retry rejection from lost accepted work. |
| Boundary values | Use a controlled source-level harness for fixed indexes near INT32_MAX | Expose the existing unsupported wrap boundary; do not run an overflow experiment against a live match or infer wrap correctness from ordinary low IDs. |
| Lifecycle | Death, drop, reconnect, travel, and re-equip while release/context cleanup is pending | No cleanup crosses owner or accepted-request generation; no retained work persists into an unrelated lifetime. |

Repeat timing-sensitive cases at 60, 240, 480, and 700 FPS, including 4 kHz input.
Charged release/volley, zoom, minigun continuous fire, and Link beam must continue
to use their intended stock-managed release behavior. Exercise bot and
replay/spectator paths separately; a remote human is not a bot merely because
the server pawn is not locally controlled.

The accepted conclusion remains: **source-supported mechanism consistent with
the observed failures**. The complete live incident chain and the normal Stop
identity correction remain unproven until correlated runtime evidence and a
separately audited ownership design resolve them.
