# Instagib click investigation — 2026-09-11

The paired capture supports taps released before equip completes, and one tap
released during cooldown. It also exposes a false-positive bug in
`NCShockInputTrace` press correlation. It does not establish a mouse-switch,
debounce, or server rate-limit cause for this player's reported missing beams.

## Inputs and method

- Client: `UnrealTournament (23).log`, 3,832 lines,
  SHA-256 `13ab7e4a235f5c7190ec92ff74f7ad39ab0cf6093967cb987dbb2abaf3a6f6ba`.
- Server: `Instance_Pug_3141.log`, 4,748 lines,
  SHA-256 `cacc6b717fa7c962d71fe884657aaf5d97743d85317f660b9f075f5357f37e40`.
- Player: `magik` (with a trailing backtick in server rows).
- Source inspected at `cd6f7a4` on `328-release-candidate`.
- Client enables `ncp.ShockInputTrace 2` at line 432 and `ncp.FireDebug 1`
  at line 434. All 75 attached trace sessions report `native=1`.
- Server line 627 identifies transactional instagib firing states,
  `firingBuild=328-fire-auth-r2`, compiled Sep 11 2026 11:45:33.
  The client log does not establish its exact DLL hash/commit.

Count original `stage` rows only; `chain` rows repeat retained events. Press IDs
restart on each weapon attachment. Correlate queue and weapon stages by event
order and frame before trusting their assigned press IDs.

Server/client shot streams have a stable observed timestamp difference:
server row minus client row is -632 to -595 ms, median -613 ms, for 189 unique
matches. This includes clock differences and transport/processing time; it is
not negative network latency or an independently measured clock offset.
Matching uses a 50 ms tolerance around that median and has no duplicate server
assignments. Neither log supplies shared request IDs for this correlation.

## Counts (left-button / primary trace)

`ShockInputTrace` observes the left mouse button and weapon mode 0. These are
not totals for every physical button or both fire modes. `FireDebug` separately
records four mode-1 starts in this capture; see the overlap section below.

| Observation | Count | Interpretation |
|---|---:|---|
| Native left-button downs | 230 | Windows messages observed; not a physical-switch measurement |
| Player-input downs / action starts / queue starts | 230 each | No observed loss through these input stages |
| Distinct non-retry weapon starts | 205 | All occur in the corresponding queue entry's frame |
| Retry weapon starts | 5 | Each leads to a local shot while the button remains held |
| Local `FireShot` entries | 197 | Function entry, not proof of a visible rendered beam |
| Local shots correlated with server `HitAttrib` | 189 | Accepted/processed shots, including misses |
| Reported `CHAIN_GAP` rows | 21 | All misclassify the queue-to-weapon boundary |
| Dropped native trace events | 0 | Sum of all session summaries |

The 25 starts that do not reach the weapon all occur in `StateEquipping`, with
`eligible=0`, at the beginning of a trace session. They still reach the input
action and queue. The trace does not log which eligibility condition failed:
possession/current weapon, controller state, input suppression, etc. Do not
label these 25 as proven hardware loss or identify one particular guard from
the existing data.

## Actual starts with no local shot

Seven gameplay-eligible taps reach `StartFire` while equipping. Their releases
also reach `StopFire` while still in `StateEquipping`; no local `FireShot` occurs
for those taps.

| Client queue line | Client timestamp | Hold duration |
|---:|---|---:|
| 1678 | 18:43:22.503 | 86 ms |
| 1745 | 18:43:29.686 | 132 ms |
| 2036 | 18:44:51.458 | 86 ms |
| 2766 | 18:47:46.463 | 106 ms |
| 3083 | 18:49:11.144 | 122 ms |
| 3118 | 18:49:20.600 | 89 ms |
| 3329 | 18:49:58.581 | 110 ms |

The source explains this behavior: client `StartFire` latches `PendingFire`
and calls `BeginFiringSequence(..., false)`. Stock
`UUTWeaponStateEquipping::BeginFiringSequence` does not fire that client call.
`BringUpFinished` enters Active, whose `BeginState` checks held pending fire.
`AUTWeaponFix::StopFire` clears that bit on release, including during equip.
Thus a tap completed before readiness has no held input left to fire.

Four other equip presses do produce a local shot after 10–31 ms, before their
release, and correlate with server shots. The equip path is not universally
stuck. Preserving an already-released equip tap would be a deliberate buffering
change, not a correction to the diagnostic or a server reconciliation fix.

One additional tap at client line 1550, 18:42:54.286, reaches the weapon with
756.93 ms of cooldown remaining. The player releases after 184 ms, leaving
about 573 ms. `StopFire` cancels the pending retry; no shot follows that tap.
The five successful cooldown retries elsewhere fire after 14–64 ms while held.

## Primary / alternate overlap

The user confirms that both instagib modes produce the same beam. They still
arrive at the weapon as distinct mode numbers. Three mode-1 starts occur while
primary is physically held and has just fired:

| Client mode-1 Start line | Primary shot | Mode-1 Start | Mode-1 Stop | Queued retry |
|---:|---|---|---|---:|
| 811 | 18:39:14.476 | 18:39:14.502 | 18:39:14.566 | 984 ms |
| 2175 | 18:45:25.481 | 18:45:25.509 | 18:45:25.562 | 982 ms |
| 3419 | 18:50:16.992 | 18:50:17.068 | 18:50:17.126 | 933 ms |

In all three, the log explicitly shows the cross-mode branch internally
stopping mode 0 (`internalStop=1`), then arming mode 1's retry. The mode-1
release arrives after 53–64 ms and cancels that retry. The prior primary shot
is corroborated by the server in every case. These are not second shots lost
after passing their legal firing time.

There is nevertheless an ownership concern worth a targeted reproduction:
`StartFire` calls `StopFireInternal(CurrentlyFiringMode)` on a cross-mode press.
With no weapon switch pending, `StopFire` clears primary `PendingFire`, clears
the held flag when GhostFix is enabled, and cancels the current refire timer.
Releasing alternate does not explicitly restore the still-held primary intent.
For identical instagib modes, tapping the other button should not silently
discard a held trigger. The capture establishes the internal stop, but all
three physical primary releases occur before the next shot would be due, so
it does not prove a missed *continued-held* shot or a persistent stall.

A fourth mode-1 start, line 3497 at 18:50:36.776, occurs after primary release
but during its firing-state tail; it is released 80 ms later. There are no
mode-1 `HitAttrib` rows for this player. The left-only native/action trace is
insufficient to audit every right-button press in the input-report screenshot
or establish that screenshot's binding/capture correspondence.

Targeted runtime check: hold primary for more than two refire periods, tap and
release alternate during the first cooldown, and keep primary held. Repeat
with modes reversed. Confirm the originally held trigger resumes at the legal
time, preserves one instagib rate of fire, and stops only when held intent ends.
The initial investigation changed diagnostics only. The follow-up below adds a
narrow held-input fix; it does not turn these three early released pairs into
extra shots or establish them as the cause of every reported missing beam.

## Follow-up: preserve overlapping Instagib holds

The user asked for a cautious fix that preserves normal Shock core -> primary
combo input. `ncp.InstagibSharedHold` defaults to `1`; set it to `0` to restore
the existing cross-mode behavior for comparison.

`AUTPlusShockRifle::HasSharedInstagibFireModes()` requires Instagib identity and
two distinct plain transactional firing-state objects, equal positive fire
intervals, equal ammo costs, no projectile class in either mode, and matching
positive instant-hit damage/type, momentum, range, and trace size. Both cone
assist values must be disabled. A missing/mismatched configuration, core mode,
zoom, charged state, or custom firing-state subclass falls through unchanged.

Offline inspection of the local `N+InstagibRifle` Blueprint CDO confirms native
parent `UTPlusShockRifle`, two equal zero ammo costs, and two equal instant-hit
entries: 100 damage, the Instagib damage type, 250000 momentum, 25000 range,
zero trace half-size, and zero cone assist. The two 1-second intervals and null
projectiles are inherited. The supplied server's StateLayout independently
shows both modes using plain `UTWeaponStateFiring_Transactional`. The editor
connector was unavailable; no Blueprint was edited for this fix.

For a locally controlled human currently holding one eligible mode, pressing
the other mode now preserves the current mode and latches the new pending bit.
It cancels only the incoming mode's retry timer and clears that retry's cross-
mode marker. The current refire timer continues to own cadence. There is no
mode remapping, added per-frame work, replicated property, or RPC change.

Releasing the second button clears only its own input. Releasing the original
button leaves the other held input for the existing deferred Active transition
to pick up at cooldown completion. Tests caught one necessary boundary change:
the existing Stop path transitions immediately when <=10 ms remains. For a
local identical-Instagib handoff with the other mode pending, it now waits out
any positive remainder. Otherwise two releases in one frame near cooldown end
could cause an early handoff shot between those releases. Ordinary Shock keeps
its original timing and cross-mode paths.

This does not add a released-click buffer. Taps released before equip or
cooldown readiness retain the existing cancellation behavior. Bots, remote
server input, replay playback, pending weapon swaps, and buffered synthetic
clicks are excluded from the new hold guard.

### Build and playtest gate

The source changes can be dogfooded on the client against the existing server:
the client sends the same numbered Start/Stop protocol when it actually fires
or releases. Mixed-version behavior still requires a real client/server test;
the adapter does not validate transport, prediction, or rendered beams.

After the normal build, test with `ncp.FireDebug 1` and compare
`ncp.InstagibSharedHold 1` with `0`:

1. Hold M1 for several refires; tap/release M2 during cooldown. Repeat reversed.
   The original hold must keep firing on cadence after the other button is up.
2. Hold both, then release the original button. The other should take over on
   the next legal refire boundary without a duplicate or faster beam.
3. Release both before readiness, including in the final 10 ms. There should be
   no extra shot after those releases. Repeat rapid release/repress and respawn
   equip taps; a held press may fire when ready, an early released tap may not.
4. In normal Shock, fire a core and immediately press M1, both with the core
   button held and released. Confirm the established beam/combo behavior and
   effects in PIE and on a dedicated server.
5. Repeat across death/respawn, weapon swaps, listen host, and nonzero ping. On
   a matched server capture, enable `ncp.FireProvenance 1` to distinguish local
   shot prediction from accepted/committed server shots.

## Diagnostic defect and correction

Example, client lines 940–982:

1. A respawn/equip action receives synthetic trace ID `2147483649`. Its Start
   does not reach the weapon, but its physical Stop does (line 948).
2. At 18:39:55.271, press 2 reaches the queue (line 958), then the weapon and
   `FireShot` in the same frame (lines 959 and 961).
3. The tracer's oldest-unstarted-action search assigns these calls to the
   already-closed synthetic action. It reports press 2 as missing at line 963.
4. The next click inherits the previous click's ID, continuing the false alarm.

All 21 reported queue gaps have a same-frame weapon start incorrectly assigned
to an older action whose weapon Stop already occurred. Eighteen also have a
local shot and a corresponding server shot. The remaining three are equip taps
in the table above; their missing-shot location is after weapon dispatch.

`NCShockInputTrace::RecordWeaponStart` now excludes actions with an already
dispatched physical weapon Stop from its non-retry FIFO selection. It does not
exclude an action merely because its release was queued: same-frame taps still
need Start and Stop dispatched in FIFO order.

This only corrects diagnostic ownership. It changes no gameplay input,
cooldowns, replication, or release behavior, and adds no work when the trace
is disabled.

## Server-side limits of the evidence

No rapid-fire rejection names this player. The four such server rows name
another player. Neither capture enables `ncp.FireProvenance`, so silent request
dispositions cannot be reconstructed by event ID.

Eight of the 197 local `FireShot` entries have no corresponding server shot.
All eight coincide with death: the client weapon trace ends 2–110 ms later,
and a server instagib hit on this player precedes the expected shot processing
time by approximately 15–121 ms after stream alignment. This is consistent
with shots crossing a death/weapon-destruction boundary. The exact server
disposition is not proven without request/ACK provenance.

These eight predicted-shot cases are separate from the eight input taps that
never enter `FireShot`. They should not be combined into one missing-click count.

No mouse debounce branch is logged. The capture contains no gameplay-eligible
queue entry without a same-frame weapon start once trace ownership is corrected.
This does not prove every perceived missing click is explained: there is no
timestamped video/reproduction identifying the particular complaint, and beam
rendering is not measured by these logs.

## Validation

- Offline replay of the original FIFO algorithm reproduces all 205 observed
  non-retry weapon-stage press assignments in the supplied capture.
- Replaying with the closed-action exclusion assigns all 205 to their actual
  same-frame queue entries, including all 21 previously misclassified cases.
- The underlying event stream, 197 local shot calls, and 189 unique server
  correlations remain unchanged.
- Eleven native adapter tests pass. They compile the actual new guard and
  classifier, complete `StartFire`/`StopFire`, retry/deferred methods, stock
  Active/continued-fire methods, and transactional Begin/refire methods with
  MSVC. The adapter supplies deterministic timers and a shot recorder; it is
  not a plugin build. Coverage includes both button directions/release orders,
  the three recorded overlap timings, both same-time callback orders, debounce,
  a deferred-stop repress, early equip/cooldown cancellation, a listen host,
  a fire-rate multiplier, classifier exclusions, and the final-10-ms boundary.
- The overlap regression first reproduces the lost continued hold with the
  CVar off, then confirms cadence and releases with it on. Normal core -> M1
  dispatch matches the CVar-off shot sequence and multi-press count.
- All 72 existing Stop identity/ownership, accepted-equip, and fire-provenance
  tests pass. These tests do not establish live gameplay correctness.
- `git diff --check` passes. No Unreal build or live runtime test was run.

Run the focused suites from the plugin root:

```text
python -B -m unittest tools.tests.test_instagib_shared_hold tools.tests.test_stop_identity_model tools.tests.test_stop_ownership_model tools.tests.test_accepted_equip_model tools.tests.test_fire_provenance -v
```

For another unexplained instance, a video timestamp plus the existing client
trace and server `ncp.FireProvenance 1` would separate a pre-shot equip/cooldown
case from a predicted shot rejected or lost at the death boundary.

## 329 follow-up: identical Instagib beam feedback (2026-10-06)

The primary-only cosmetic guards left the standard rifle's identical alternate
beam on a different path. When the stock projectile sleep time was positive,
alternate fire could delay its local beam; it also ignored Show Own Beam and
did not receive the existing cosmetic thickness layer.

`IsInstagibBeamFireMode` now shares those three decisions. Primary keeps its
existing behavior; alternate qualifies only when `HasSharedInstagibFireModes`
verifies both modes are identical plain transactional hitscan beams. Normal
Shock and custom alternate modes retain their existing effects. The thickness
check uses the effect's fire mode rather than the weapon's mutable current mode.

This changes local cosmetics, not shot admission, cadence, damage, hitboxes,
rewind or RPCs. The native beam regression exercises the production decision
and effect-dispatch methods against engine adapters. Rendered particle lifetime,
short-click appearance, hidden-weapon origins and actual owner replication still
need a matching-build multiplayer check using both fire buttons. In particular,
an effect callback or an allocated particle component alone is not evidence that
the beam reached the screen.

## 329 follow-up: one Instagib tap during weapon raise (2026-10-06)

`ncp.InstagibEquipTap` defaults to `0` as of the 2026-10-09 follow-up below
(initially `1`). When enabled, a real primary or alternate press received
while the current, living player's identical-mode Instagib rifle is equipping
retains one shot even if released before the raise completes. Several taps
coalesce into one intent. In ordinary one-weapon Instagib this applies to spawn
and respawn weapon raise, not the one-second refire cycle.

Non-consuming observers of the existing controller's fire actions provide
same-frame owner-bound input tokens. A held-input verification, timer retry,
or dead/unpossessed respawn click cannot create the intent by itself. The
2026-10-08 follow-up below separately covers a real click on the already
replicated living pawn before possession acknowledgment. The normal release
still clears physical held fire.
Existing diagnostic observers and the equip observers remove only their exact
delegate handles, so toggling the trace cannot remove gameplay observers.

On equip completion, ordinary held fire takes precedence. A released intent
dispatches once through the normal local StartFire/transactional path when both
equip and cadence gates permit, followed by a guarded internal stop. A residual
cadence-clock boundary waits for a weapon tick rather than firing early. The shot
uses current aim, origin and time at execution; no click-time rewind is added.

The intent is discarded on a new equip, switch/state interruption, internal stop,
removal/detach/destruction, changed owner/controller, or blocked gameplay. Normal
Shock/core, custom alternate modes, cooldown taps and the server's accepted-shot
policy are unchanged. `ncp.InstagibEquipTap 0` disables retention. No new RPC or
custom PlayerController is required; the new native build must still be rebuilt.

Validation includes native extracted-method tests for eligible taps in both
modes, held-fire cadence, coalescing, input provenance, lifecycle invalidation,
local client/standalone/listen roles, current aim/time and readiness boundaries.
These adapters do not establish rendered beam visibility or live network timing.

Verification: UnrealTournamentEditor Win64 Development module build passed with
UE4.15; 57 tests passed across `test_instagib_shared_hold`, `test_instagib_beam`,
`test_stop_ownership_model` and `test_stop_identity_model`. This includes input
actions collected before FIFO dispatch and cancellation before token consumption.
The first build required `-gather` to include the newly added source file in UBT's
cached source list. Shipping/cooked and live multiplayer checks remain outstanding.

## 329 follow-up: recover input during respawn possession (2026-10-08)

The earlier equip fix needed stock to deliver `StartFire` first. Stock
`ApplyDeferredFireInputs` instead empties queued starts while the controller is
Inactive, even when pawn/controller replication already exposes the new living
pawn and its equipping rifle. Releases still reach `StopFire`. UE4.15 explicitly
allows `ClientRestart` to arrive before the pawn parameter is mapped; the retry
can leave these two readiness states temporarily out of step. The original
capture has 25 pre-weapon dropped starts, distinct from its seven eligible equip
taps. It does not record enough possession state to prove the cause of every drop.

`ncp.InstagibEquipTap` now also retains one real action on that current living
pawn's equipping, identical-mode Instagib rifle while a network client is Inactive.
It expires 500 ms after the physical press
using real time. That is a maximum age, not an added firing delay. Dispatch waits
for Playing on that same current pawn, equip completion and legal cadence.
`ClientRestart` acknowledges possession before entering Playing; the plugin uses
the public controller state and `GetPawn()` without reading `AcknowledgedPawn`.
The shot uses current aim and the normal firing protocol. No RPC, rewind, shot
identity or server admission policy changes.

Both fire-action presses and releases are observed without consuming input.
Release is tracked at action time so an old deferred release cannot turn a later
same-frame repress-and-hold into a released tap. A genuine hold is left to normal
input or the restart recovery below. Multiple released taps coalesce; a fresh
action supersedes the old one. Menu/chat/console, lost focus, paused/blocked play,
death, changed pawn/controller/input component, weapon switching and the existing
equip lifetime invalidations cancel the intent. A normal same-weapon restart does
not re-equip the rifle and therefore does not erase the valid pending click.

`ATeamArenaCharacter::PawnClientRestart` also schedules one bounded local held-input
recovery for all weapons. It starts next tick, after ClientRestart can enter
Playing, and waits at most 500 ms of monotonic time for that state and the
weapon to arrive and initialize. It uses stock `ClientVerifyFiringInputs` to read
the controller's current held buttons. Released buttons do not create starts.
If either mode is already pending, any start is queued, or the weapon is firing,
normal input keeps control and recovery ends. Repeated restarts cannot extend the
deadline or rearm completed recovery. A changed weapon, owner or blocked gameplay
cancels it; there is no permanent polling or global held-button replay.

Player-facing patch note: Holding fire through respawn now fires when your weapon
is ready, without needing to release and click again. With `ncp.InstagibEquipTap 1`,
Instagib also remembers one tap during weapon raise, including the brief possession handoff. Normal fire rate
and server shot validation still apply.

The fire guards and trainer input-focus check use the qualified exported
`UUTLocalPlayer::AreMenusOpen` and non-inline `GetQuickChatWidget().IsValid()`.
This avoids relying on retail UTLocalPlayer virtual-slot or member-offset parity
with the public headers, as required by the earlier trainer launcher fix.

Native regression adapters execute the actual stock deferred dispatcher and held
verifier, the production tap/restart methods and existing firing state machine.
They cover both fire modes, equip/Playing transition order, released taps, continuous
holds, same-frame release/repress, duplicate starts, timeout and lifecycle guards.
Separate binding tests exercise the actual installation/removal methods, including
preserving other observers. These are not a UE build or a multiplayer playtest.

After rebuilding, test repeated respawns with both taps and holds before the rifle
finishes raising, including simulated latency. Check menu/focus cancellation,
release/repress, and ordinary Shock, Link and rocket held fire. In particular,
verify one legal shot for a released Instagib tap and sustained normal cadence for
a held button. No content recook is required for these native-only changes.

## 329 follow-up: equip-tap retention is opt-in (2026-10-09)

The default for `ncp.InstagibEquipTap` is now `0` after reports of unwanted shots
when a released click during respawn weapon raise was retained. Players can opt
in with `ncp.InstagibEquipTap 1`; explicit client configuration still takes
precedence. This is a local client setting, so a hub cannot change it for players.
The new default requires the updated client build. Existing clients can disable
it immediately with `ncp.InstagibEquipTap 0`.

Only the default changed. Normal firing and held-button recovery remain enabled;
the released-tap retention path and its guards are still available when opted in.
