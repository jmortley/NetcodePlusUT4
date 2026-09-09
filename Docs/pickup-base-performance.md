# Pickup base and spectator performance audit — 2026-09-09

## Current status

The optimized bases are **saved copies**, not edits to stock content. All three
original class-default dumps match their captured baselines. All three copies
compile without messages and newly created instances inherit a 0.05-second
actor tick interval.

The owner-built native class is loaded. `NCWepMut`, `NCStockWeapons` and
`WipeoutMutator` now inherit `NCPickupBaseMutator`, reference all three copies,
and are compiled and saved in the active editor content tree. Existing CDO
settings and all 18 exported graphs' connections were preserved.

**Runtime acceptance has not passed.** The owner's 16:11:31 UTC Wipeout PIE
rerun loaded the diagnostic DLL and reached the hook for 10 weapon bases and
2 powerup bases. All 12 were kept because `OnDestroyed` was bound; no replacement
was observed. Stock UT installs its replay bookkeeping callback there before
BeginPlay, so the original delegate guard was too broad.

The source now excludes that exact stock binding from the preservation check,
while leaving the actual callback attached. It also handles two confirmed
stock/default component differences described below. This guard correction
needs an owner build/deploy and runtime verification. Editor activation and
ordinary Wipeout play do not establish successful replacement or FPS savings.

## Saved content

| Original package, restored to tick interval 0 | Copied package, tick interval 0.05 |
| --- | --- |
| `/Game/RestrictedAssets/Weapons/WeaponBase` | `/Game/Blueprints/Netcode/Performance/NCWeaponBase` |
| `/Game/RestrictedAssets/Pickups/Powerups/PowerupBase` | `/Game/Blueprints/Netcode/Performance/NCPowerupBase` |
| `/Game/Blueprints/Netcode/NCPowerupBase_test` | `/Game/Blueprints/Netcode/Performance/NCPowerupBaseTimer` |

After normalizing each copy's package/class name, its complete actor CDO dump
matches the original except for the tick interval. The Event Graph node counts
remain 3 / 0 / 0; construction graph counts remain 3 / 1 / 2. WeaponBase's
GiveTo/parent/delegate flow and timer-template construction remain intact.

In this fork, `AUTPickup::Tick()` updates the timer particle's Progress parameter.
Inventory and weapon pickups do not override Tick, and these three Blueprints
have no Event Tick logic. Respawning uses TimerManager, collection uses capsule
overlaps, and rotation has a separate RotatingMovement component tick. Their
intervals and callbacks were not changed. Actor tick remains enabled on dedicated
servers as well.

At a 144 Hz actor tick rate, eligible instances have at most 20 progress-update
opportunities per second instead of 144. This is **not an FPS measurement**.

## Relevance integration activated in editor

The shared native class is the parent of all three existing mutator BPs.
Their current CheckRelevance graphs already call the parent after their inventory
substitutions; the new parent forwards to the next mutator before considering
replacement. No new graph nodes or native-function rebinding is needed.

The hook:

- Runs during authority-side BeginPlay, matches the three exact source class
  paths, and requires the corresponding configured copy class. Specialized
  descendants, including `BP_BaseSiphon`, retain their existing behavior.
- Keeps originals when content is missing or another mutator's AlwaysKeep rule
  requires retaining them. Downstream rejection remains a rejection.
- Retains actors with an owner, attachment, bound actor delegates, a direct
  reference from their level script, or detected presentation/component
  overrides. This includes WeaponBase's bound PickedUpWeapon events. This is a
  conservative gate, not a general rewrite of every possible external reference.
  The stock current-world `LevelActorDestroyed` callback on a startup actor's
  native `OnDestroyed` delegate is excluded from classification using a local
  delegate copy. The real callback and any additional map listeners are retained.
  The component comparison also accounts for stock weapon timer preview
  visibility and an inactive legacy MaxAngularVelocity value.
- Copies native editable pickup configuration, including InventoryType,
  WeaponType, respawn/spawn settings and pickup metadata, before construction
  and BeginPlay. Engine inventory initialization still applies its usual
  inventory/game-mode respawn rules. Runtime timers, customers, component pointers
  and actor identity are not copied.
- Spawns in the source level at the same transform with AlwaysSpawn. Spawn
  failure keeps the original. Successful creation returns false for the original;
  the replacement goes through the engine's normal relevance scheduling.
- Logs swap/keep decisions at LogGameMode Verbose with the `[PickupBase]` prefix.
  The diagnostic follow-up distinguishes AlwaysKeep, owner, attachment, actor
  delegate, level-script reference, pickup settings, and the exact component
  property that differs. After reaching the presentation check, VeryVerbose
  prints all mismatching component properties and their instance/archetype
  values in the same run. Normal logging still stops at the first mismatch.

The three class references belong on the mutator BPs so their cooked dependencies
include the copied assets. See [the setup manifest](pickup-base-editor-setup.json)
for exact packages, property names and remaining verification steps.

## Source FPS cleanup

The expanded spectator slideout now caches player-name and spectator-number text
and their fitted font scales. Previously both labels were rebuilt and measured
for every visible player on every frame. Name, clan, slot and font changes
invalidate the cache before the existing 5 Hz stat gate, so labels still update
immediately. Existing weak player/world cleanup owns the new cache values.
Vitals and mouse/camera behavior keep their existing paths.

## Verified and still required

- Original asset files were backed up with SHA-256 hashes before edits.
  The external audit folder contains
  `pickup-base-backups-20260909T142653899Z/manifest.json`, original/restored/copy
  default dumps and before/copy graph exports.
- All originals and copies compiled successfully. Original defaults were restored;
  copies have matching defaults apart from tick interval and their own names.
- One temporary instance of each copy read back a 0.05-second interval.
  All temporary test actors were deleted and their absence verified.
- The original map file was not saved. Blueprint compilation dirtied the open
  Temple map through reinstancing; the saved copies do not require saving it.
- The assets are in the active LAEditorUT4 Content tree, outside this Git repo.
  They need a content pak cook after activation and acceptance.
- Native APIs/signatures were checked against the local UT4/4.15 source.
  The owner supplied the native build containing the new parent and detailed
  diagnostics. No native build or cook was run by this audit; the subsequent
  guard correction is uncompiled.
- Test pickup identity/count, collection, weapon stay, delayed spawn, respawn
  indicators, rotation, spectator pickup buttons, Wipeout Siphon/AMP selection
  and other mutator rejection paths on a server and client. Check bases under
  baked map lighting: a spawned actor does not automatically retain a placed
  actor's baked-lighting association. Compare frame times on the same route.
- Deploy the native DLL containing the new parent together with the recooked
  mutator/content paks. Those reparented Blueprints require the new native class;
  they must not be paired with an older client DLL. A dogfood client DLL alone
  supplies only the spectator label-cache change.
- The loaded `BP_BaseSiphon` child still has its original zero interval.
  It was not reparented or substituted.

## Activation and runtime evidence

The external `pickup-base-activation-20260909T151830069Z` folder contains the
three original mutator packages and backup hashes, full before/after Blueprint
and CDO dumps, all 18 before/after graph exports, compiler results, graph
comparison output, six saved-package hashes, and isolated runtime logs.

- Parent and class references read back correctly for all three mutators.
  Complete CDO comparisons add only the three inherited copy references.
- All three copies compile without diagnostics. NCStockWeapons compiles without
  diagnostics. NCWepMut and WipeoutMutator have five informational messages
  each for pre-existing unconnected paths; neither has compiler errors/warnings.
- Graph comparison accounts for refreshed localized labels, unconnected pin
  IDs, compiler-message display flags, and inherited callbacks now naming
  UTMutator as their declaring class. Connected pin IDs, links, node counts,
  graph defaults and logic are unchanged. The generated native CheckRelevance
  thunk calls the virtual implementation on the new parent.
- Example_Map had 518 actors and was clean at activation. Following owner PIE
  tests it still has 518 actors and is marked dirty; this audit did not save it.
- NCStockWeapons standalone, using the existing editor executable with `-game
  -nullrhi`: 11 WeaponBase and 5 PowerupBase actors reached the preservation
  guard, all were kept, and their actor tick intervals remained zero. The
  process exited normally. This verifies dispatch, not successful replacement.
- Editor object dumps show differences from base templates in capsule physics
  values and timer visibility. Those observations are possible contributors,
  not proof of the first runtime rejection condition. The original coarse log
  cannot distinguish the individual checks. Do not remove a preservation rule
  until the new diagnostics identify the rejection and its cause.
- NCWepMut headless startup raised an UnrealTournament DLL access violation
  before any pickup-hook rows. Its cause and relationship to reparenting are
  unverified. A subsequent NCStockWeapons fresh-instance attempt stalled before
  map startup and only that owned process was terminated. Neither supplies a
  valid fresh-instance result. The live editor remained responsive.

Next: rebuild/deploy the guard correction, use a normal rendered play test with
`-LogCmds="LogGameMode VeryVerbose"` (or console `log LogGameMode VeryVerbose`),
and capture swap/keep decisions for an unmodified newly placed base as well as
an existing map base. Reapply console logging after an editor restart. Establish
actual swaps before the server/client and lighting checks above. Recook the six
configured/copied assets only after runtime acceptance.

### Owner PIE follow-up

The owner reports that Wipeout worked in PIE without visible errors. The live
editor log confirms Example_Map/WipeoutPlus PIE sessions at 15:49 and 15:51 UTC
on 2026-09-09, round start, and normal PIE shutdown. The headless failure has
not reproduced in this reported Wipeout play test. The log does contain
WipeoutPlus `GS`-reference Blueprint runtime errors during startup; their
relationship to pickup replacement is unverified.

Those early sessions have no `[PickupBase]` rows. A 15:57:42 UTC run with logging
enabled reported 10 weapon and 2 powerup bases kept with the old coarse reason.
After the owner rebuilt and restarted, the 16:06:22 UTC session again had no
pickup rows because console verbosity had reset. The loaded firing build and
the editor DLL's 16:04:53 UTC write time confirm the new native build was present.

Logging was re-enabled through the connector at 16:08:39 UTC. The owner's next
run, 16:11:31 UTC, reported all 12 bases kept with `bound actor delegate
OnDestroyed`, zero swaps, and normal PIE shutdown. The existing startup
`GS`-reference Blueprint errors were also present. The external evidence folder
contains `wipeout-pie-161131.log`, its count/hash receipt, and read-only editor
component dumps in `guard-editor-snapshots.json`.

### Guard correction and source trace

- `UTWorldSettings.cpp`, `NotifyBeginPlay`: stock adds
  `OnDestroyed -> LevelActorDestroyed` to every net-startup actor immediately
  before calling its BeginPlay. The callback records destroyed map actors.
  `UTGameInstance.cpp` consumes that list when initializing replay recording.
  The correction removes only this target/function pair from a **local copy**
  while deciding whether bindings require retaining the actor. Stock-only
  binding passes this check; an additional listener or a different target still
  keeps the actor. No actual delegate is removed or transferred.
- `UTPickupWeapon.cpp`, `OnConstruction`/`PostEditChangeProperty` hide ordinary
  weapon timers in the editor. `BeginPlay` unconditionally sets their visibility
  true after the relevance hook. The guard therefore ignores only weapon
  TimerEffect `bVisible`; it still checks `bHiddenInGame`, custom templates and
  the timer's other properties.
- The editor capsule dump differs from its archetype at MaxAngularVelocity
  (399.999939 versus 3600) with `bOverrideMaxAngularVelocity=false` on both.
  `FBodyInstance::GetMaxAngularVelocity` uses global PhysicsSettings in that
  case. The correction compares every other reflected body field, including
  fixed-array entries, and skips that inactive value only when neither body
  overrides it. Active angular limits and collision settings still block a
  mismatched actor. No live body is copied or modified.
- The editor capsule also has `bShouldUpdatePhysicsVolume=true` versus false
  on its archetype. MovementComponent can set this field, but the inspected
  editor rotating component has no UpdatedComponent and auto-registration is
  disabled. Its origin and the exact PIE value are not established, so this
  guard remains. VeryVerbose now collects all component differences once the
  presentation check is reached, avoiding a separate rebuild per property.

Validation for this correction is source/API review against the local fork,
read-only editor comparison, log receipts, JSON parsing and `git diff --check`.
Native compilation, actual replacement and replay/server-client behavior remain
unverified. The timer/default comparisons are separate findings; the PIE log
proves only that the earlier delegate guard was the first blocker.

### Init-time Blueprint scan alternative

A one-time `Event Init -> GetAllActorsOfClass -> spawn copy -> destroy original`
flow can handle actors already loaded with the map without per-frame scan cost.
However, `AUTGameMode::AddMutatorClass` calls the new mutator's Init **before**
adding it to the mutator chain. Startup Init also precedes WorldSettings'
per-actor replay listener installation. Destroying map actors there can bypass
that destruction bookkeeping and changes the classes later mutators encounter.

Such an implementation needs authority and exact-class checks (the scan includes
subclasses), configuration/transform transfer, map-reference preservation,
spawn-failure handling, and a separate path for later or streamed actors. Merely
calling the current helper from Init would not work: it requires BeginPlay.
Keep the current CheckRelevance path and correct the proven false positives
rather than changing lifecycle phase to bypass the guard.

## Further candidates, not applied

The loaded connector edits native CDO component pointers but cannot edit the
Blueprint SCS templates whose component variables are null on the CDO. Editing
placed instances would not optimize the reusable copies.

| Component | Observation | Candidate |
| --- | --- | --- |
| WeaponBase / Weapon_base_glow | Movable, shadows and overlap events enabled; QueryAndPhysics, but real gameplay channels Ignore | Disable unused decorative overlaps/collision and shadows in the copy's SCS template. Retain the pickup capsule. |
| WeaponBase pedestal | 2,960 triangles, four material sections, one LOD; already Static with no collision or shadows | Authored lower LOD/material consolidation, with visual comparison. |
| PowerupBase pedestal | Hidden in game, Static, BlockAll and overlaps enabled; shadows already off | Establish the hidden collider's intended gameplay role before changing it. |
| NCPowerupBase_test / Base | Movable, BlockAllDynamic, overlaps and shadows enabled | Review shadows/overlaps/mobility while preserving intended blocking. |
| NCPowerupBase_test / TimerBillboard | NoCollision, overlaps enabled, no distance limit, shadows off | Disable unused overlaps; evaluate distance fading without hiding useful information. |

Stock base/timer particles already have no collision or shadows, 2,048/1,024-unit
draw distances and one-second inactivity timeouts. The observed powerup-owned
point light already has shadows disabled. These were not newly fixed problems.
