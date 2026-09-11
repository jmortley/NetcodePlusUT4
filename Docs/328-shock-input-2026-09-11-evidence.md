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
