# NCPLinkGun copy + reparent notes (328 RC)

## Design decision

`AUTWeap_LinkGun_NCP` is the non-CSHD Link Gun. It derives from `AUTWeaponFix`
only so the stock continuous beam reaches NetcodePlus's rewind-aware
`FireInstantHit()` / `HitScanTrace()` through virtual dispatch.

Both inputs retain stock firing-state timing:

- Mode 0 plasma calls `AUTWeapon::StartFire`, `StopFire`, `FireShot`,
  `SpawnNetPredictedProjectile`, and the stock delayed-fake callback. Its rapid
  cadence does not create NetcodePlus per-shot transactions, ACKs, retry gates,
  pending fake-event entries, or projectile-hit claims.
- Mode 1 uses `UUTWeaponStateFiringLinkBeam_NCP`, a stock-faithful copy of
  `UUTWeaponStateFiringLinkBeam` retargeted to the new weapon class. The owning
  client traces the cosmetic endpoint; the server performs the damage trace and
  accumulation; remote players and spectators consume the stock replicated
  flash endpoint/effects.
- The raw-validation/rollback rewind is server-selected and capped to 40ms
  one-way by default. `LinkBeamMaxRewindMs` may be tuned only from 0-50ms, and
  C++ hard-clamps it to 50ms even if stale cooked data contains a larger value.
  With `ncp.RenderCredit=1`, remote-human damage target selection instead uses
  one estimated render-time capsule (`half server-measured RTT +
  ncp.HitAttribRenderExtraMs`, capped at 250ms). It replaces raw history rather
  than being unioned with it.
  `bTrackHitScanReplication` is explicitly false and the Link class disables
  the claim-only bidirectional time-search fallback. Its `FireInstantHit`
  override also clears the inherited received-claim cache immediately before
  every beam trace, so no stale or unsolicited client-named target, claim
  padding, or CSHD rescue can participate in beam damage.
- The discrete stock link-pull RPC still retains Epic's bounded client target
  fallback (`100uu + target radius` from server aim with a world-geometry
  clearance test). That is separate from continuous beam damage and is retained
  for stock pull feel, but the hinted target must pass the same live enemy-only
  validation as the authoritative trace.
- Link pull rejects the owner, dead/torn-off targets, and teammates. The check
  uses `AUTGameState::OnSameTeam()` plus a direct non-255 team-number comparison,
  is applied to local readiness, the server trace, the client-hint fallback, and
  once more immediately before authoritative damage/momentum.
- The new class copies the stock `ServerSetPulseTarget` RPC with the same
  unreliable signature. It adds no custom fire or hit RPC.
- `AUTWeaponFix::Tick()` is intentionally bypassed. Its generic firing watchdog
  depends on transactional `LastFireTime`; using it on a non-transactional Link
  beam would falsely stop a healthy beam after roughly 0.25 seconds.

The existing CSHD Link asset/class (`UTNPLinkGun`, `AUTWeap_LinkGun_Plus`, and
`UUTWeaponStateFiringLinkBeamPlus`) remains independent and must not be copied
or wired into the stock-adjacent `NCPLinkGun` path.

`UTNPShaftLink` now has a dedicated migration path: reparent it to
`AUTWeap_LinkGun_Shaft_NCP`. That class derives from `AUTWeap_LinkGun_NCP`,
installs `UUTWeaponStateFiringLinkBeam_NCP` in both fire-mode slots (primary is
converted from plasma to beam), and canonicalizes both fire inputs to the
Blueprint-authored mode 1. That preserves the mode-1 beam actor, muzzle,
looping sound, animations, and replicated cosmetic mode instead of combining
mode-0 projectile cosmetics with beam damage. It disables Link pull/primary
overheat and keeps two unused legacy tuning-property aliases so the existing
Shaft BP can recompile without its old CSHD defaults becoming missing-property
errors. The NCP beam state records `LinkBeamShots` once per refire interval,
matching the existing Shaft HUD, scoreboard, and shared accuracy replicator;
stock `LinkShots` is intentionally not used as the beam denominator. At
runtime, guarded `PostInitProperties()` and `PostInitializeComponents()` passes
replace either serialized legacy firing state after Blueprint defaults load.
This is required because UE4 keeps the inherited `FiringState` objects from the
Blueprint's previous parent when the asset is reparented. It also copies the
proven mode-1 beam
damage/cadence/ammo data to mode 0 and clears `ProjClass[0]`, so neither stale
state objects nor plasma-era mode-0 arrays can undo the conversion.

## Why one blank Blueprint is not sufficient

The stock `/Game/RestrictedAssets/Weapons/LinkGun/BP_LinkGun` Blueprint owns the
Link Gun's components, event/function graphs, animation/effect references, and
tuned class defaults. A blank child of the new native class would have none of
that content.

The current elimination weapon is itself a child of `BP_LinkGun`:

`/Game/Blueprints/UT+/UT+/UTPlusNew/ElimVersion/UT+LinkGunElim`

Reparenting that child directly to the native class would remove the stock
Blueprint layer it inherits. Preserve both layers with two duplicated assets.

## Post-build editor procedure

The editor cannot reparent to `AUTWeap_LinkGun_NCP` until the user builds the
module and restarts the UE4.15 editor.

1. Duplicate `/Game/RestrictedAssets/Weapons/LinkGun/BP_LinkGun` to
   `/Game/Blueprints/Netcode/NCPLinkGunBase`.
2. Reparent `NCPLinkGunBase` to native `UTWeap_LinkGun_NCP`; compile and save.
3. Verify `FiringState[1]` is an instance of
   `UTWeaponStateFiringLinkBeam_NCP`, while `FiringState[0]` remains the stock
   looping projectile state.
4. Duplicate `UT+LinkGunElim` to `/Game/Blueprints/Netcode/NCPLinkGun`.
5. Reparent `NCPLinkGun` to the Blueprint class `NCPLinkGunBase`; compile and
   save. This retains the elimination child's own graphs/defaults while the
   duplicated base supplies the stock Link Blueprint content.
   Recheck both firing-state slots on this final child as well: a serialized
   stock `UTWeaponStateFiringLinkBeam` hard-casts its owner to the unrelated
   stock `AUTWeap_LinkGun` and is unsafe on the new native parent.
6. Confirm `ProjClass[0]` is the intended Link plasma generated class and that
   both authoritative and fake projectiles use that identical class. No custom
   NetcodePlus Link projectile class is required by this design.
7. Compare the resulting CDO against the source child. At minimum preserve its
   current ammo, fire intervals, beam half-size, pull damage, bring-up/put-down
   timing, materials, effects, animations, and Blueprint-owned variables.
   Confirm `LinkBeamMaxRewindMs=40` (never above 50; this is the raw rollback
   cap, not the default render-authoritative sample) and
   `bTrackHitScanReplication=False`.
8. Only after the new Blueprint compiles cleanly, replace Link index 4 in both
   `CustomWeaponClasses` and `WarmUpInv` of
   `/Game/Blueprints/ElimPlusStuff/NCWepMut` with `NCPLinkGun_C`.
9. Recook the production mutator/content PAK and verify the new base, child, and
   all referenced projectile/effect assets are actually present in that cook.

Do not wire an incomplete duplicate into a production mutator before the native
class exists and both Blueprint layers compile.

## High-ping primary test note

This design deliberately accepts Epic's stock primary prediction behavior.
`AUTWeapon` has one delayed-fake timer/payload per weapon, so at ping high enough
to enter its delayed-fake path, closely spaced plasma shots can omit some local
fake visuals. The authoritative server bolts still fire. That is distinct from
the NetcodePlus transaction/ACK disappearance class, because Link primary never
enters that machinery. Test shooter visuals and authoritative hits separately
at the intended maximum ping before release.

## Shaft Link migration

After building the module and restarting the editor:

1. Make a backup duplicate of `/Game/Blueprints/Netcode/UTNPShaftLink`.
2. Reparent `UTNPShaftLink` to native `UTWeap_LinkGun_Shaft_NCP`.
3. Compile and save. The two legacy CSHD tuning fields referenced by the asset
   remain available as inert compatibility properties; do not add any CSHD
   beam-hit RPC graph to the new parent.
4. Verify `FiringState[0]` and `FiringState[1]` are both
   `UTWeaponStateFiringLinkBeam_NCP`. No slot may retain
   `UTWeaponStateFiringLinkBeamPlus` or a looping projectile state at runtime.
   The authoritative `[StateLayout] UTNPShaftLink_C` log line must report the
   NCP state in both slots; the source asset may retain an unused legacy class
   reference in its serialized dependency table after reparenting.
5. Verify both inputs render and sound like the intended mode-1 Shaft beam and
   that neither produces a plasma projectile. Link pull/yoink must remain
   disabled.
6. Because the asset path is unchanged, the existing `NCShaftArena`
   `ShaftLinkClass` or `[NCShaftArena] WeaponClass=...UTNPShaftLink_C` setting
   remains valid. Recook the gameplay pak that owns `UTNPShaftLink`.

## Static verification before the user build

- New source files have no compile/link dependency on `UTWeap_LinkGun_Plus`,
  `UTWeaponStateFiringLinkBeamPlus`, CSHD processing, or a custom beam-hit RPC.
- Primary dispatch is explicitly qualified to `AUTWeapon`, preventing virtual
  fallback into `AUTWeaponFix::SpawnNetPredictedProjectile`.
- Beam state damage/accumulator and pulse behavior follow the stock UE4.15
  source; only the owning weapon cast/class name changes.
- No Blueprint assets or production replacement arrays are modified until the
  post-build editor pass.
