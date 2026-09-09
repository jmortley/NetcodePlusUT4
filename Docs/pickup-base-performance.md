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

**Runtime acceptance has not passed.** A standalone NCStockWeapons test reached
the new hook, but all 16 exact-source bases on Example_Map were kept by the
preservation guard; no optimized replacement was observed. Further headless
tests crashed before the hook or stalled during startup. More specific guard
diagnostics are now in source and need another owner build before retesting.
Do not treat the editor activation as release acceptance or measured FPS savings.

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
  property that differs. VeryVerbose additionally prints that property's
  instance/archetype values. The checks and their order are unchanged.

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
  The owner supplied the native build containing the new parent. No native
  build or cook was run by this audit; the diagnostic follow-up is uncompiled.
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
- The active Example_Map remains clean with 518 actors; it was not saved.
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

Next: rebuild/deploy the diagnostic DLL, use a normal rendered play test with
`-LogCmds="LogGameMode VeryVerbose"`, and capture the first keep reason for an
unmodified newly placed base as well as an existing map base. Establish actual
swaps before the server/client and lighting checks above. Recook the six
configured/copied assets only after runtime acceptance.

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
