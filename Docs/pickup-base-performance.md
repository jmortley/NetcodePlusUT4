# Pickup base and spectator performance audit — 2026-09-09

## Current status

The optimized bases are **saved copies**, not edits to stock content. All three
original class-default dumps match their captured baselines. All three copies
compile without messages and newly created instances inherit a 0.05-second
actor tick interval.

The shared native `ANCPickupBaseMutator::CheckRelevance_Implementation` is
prepared in source. **Replacement is not activated yet.** The running editor
does not have this new native class. `NCWepMut`, `NCStockWeapons` and
`WipeoutMutator` still inherit stock `UTMutator`; their graphs were only read.
Native compilation, the parent/class-reference assignment and runtime acceptance
remain pending.

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

## Relevance integration prepared

Use the shared native class as the parent of all three existing mutator BPs.
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
  No native C++ build or runtime replacement test was performed. The new parent
  must be built/deployed to the editor before assigning it to the mutators.
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
