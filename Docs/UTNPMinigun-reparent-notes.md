# UTNPMinigun — copy + reparent notes (2026-07-21)

Working copy of the live minigun for the Minigun_Plus migration.

- **Source:** `/Game/Blueprints/UT+/UT+/UTPlusNew/ElimVersion/UT+MinigunElim` ("Pro+ Stinger Minigun")
- **Copy:** `/Game/Blueprints/Netcode/UTNPMinigun` (bridge StaticDuplicateObject, compiled, saved)
- **Source parent:** native `UTWeap_Minigun` (NOT a BP parent — every asset ref below is stored
  in the BP itself, so nothing is inherited-and-losable from a parent blueprint)

## Why the reparent to AUTWeap_Minigun_Plus is low-risk

`AUTWeap_Minigun_Plus`'s constructor is a byte-for-byte copy of stock `AUTWeap_Minigun`'s
(verified line-by-line, incl. the `UUTWeaponStateFiringSpinUp` FiringState0 subobject class).
Delta-serialization can only "reset" a property to the new parent's default — which is the
same value the old parent had. The BP-local overrides that must survive are listed below;
verify them against this file after reparenting.

## Defect found + fixed in the copy (the predicted "issue or two", #1)

`MuzzleFlash(0)/(1)` survived duplication still pointing at the ORIGINAL BP's component
templates (`UT+MinigunElim_C:ParticleSystemComponent_2/_1972`). Cross-class template refs
don't remap at instance construction → muzzle flashes would silently never fire on the copy.
**Fixed:** repointed to the copy's own templates
(`UTNPMinigun_C:ParticleSystemComponent_2` = P_Stinger_MF_01_1P, `_1972` = P_Stinger_MF_02_1P,
both RelativeRotation Yaw=90). `Mesh1P`, `DummyRoot`, all weapon states, and the spin-up
instance remapped correctly on their own.

## BP-local overrides that must survive the reparent (checklist)

| Property | Value |
|---|---|
| Ammo / MaxAmmo | 70 / 140 (stock 80/240) |
| AmmoCost(1) | 5 |
| FireInterval | 0.088 primary / 0.45 alt (stock 0.10) |
| InstantHitInfo(0) | 10 dmg, UTDMG_Minigun_Primary_C, 25000 range |
| Spread(0) | 0.05 |
| ProjClass(1) | `UT+Minigun_ProjectileNew_C` (parent: native `UTProj_StingerShard2`; BP adds "Random Spray (Degrees)" + DamageImpactedActor override) |
| FiringState(0) | BP-instanced `UTWeaponStateFiringSpinUp_1`: WarmupShotIntervals (0, 0.06, 0.12, 0.12, 0.12, 0.12), CoolDownTime 0.27, warmup/cooldown/loop montages |
| FOVOffset | (1,1,1) (stock 0.01,1,1.6) |
| WeaponRenderScale | 0.85 |
| HUDViewKickback | (0,0) (stock 0.03,0.05) |
| BringUp/PutDownTime | 0.3 / 0.3, RefirePutDownTimePercent 0.65 |
| DisplayName | "Pro+ Stinger Minigun" |
| bFPFireFromCenter / bFPIgnoreInstantHitFireOffset | True / True, FireOffset (75,0,0) |
| Full 1P anim set | idle/run/jump/land/slide/dodge/wallrun/AO_Lag/BS_Lean + hands montages (all explicit refs in the BP; see OBJ DUMP) |
| Event graph | BeginPlay + OnStartedFiring → cast owner to UTPlayerController → `ServerNegotiatePredictionPing(DesiredPredictionPing)`; both call Parent. Vars: MyUTPC, DesiredPredictionPing (+ legacy MF/MF2). Export: scratchpad `minigun-eventgraph.t3d` (12 nodes) |

## Reparent procedure (manual — no bridge op for this)

1. Open `/Game/Blueprints/Netcode/UTNPMinigun` → File → **Reparent Blueprint** →
   `UTWeap_Minigun_Plus` (abstract native parents are valid for BPs).
2. Compile + Save.
3. Verify vs the checklist (bridge `OBJ DUMP Default__UTNPMinigun_C` and the spin-up
   subobject dump) — especially FiringState(0) keeping the `UTWeaponStateFiringSpinUp_1`
   instance and the anim set.
4. If anything reset, restore via `bridge_set_class_defaults` from this file's values.

## Pipeline after reparent (agreed order)

1. ~~Copy + fix refs~~ (done) → reparent to `AUTWeap_Minigun_Plus` (= stock feel + rewind
   hitscan on primary).
2. Mode-split transactional alt-fire in `AUTWeap_Minigun_Plus` (mode 1 →
   `AUTWeaponFix::StartFire`, mode 0 stays `AUTWeapon::StartFire`; sniper precedent:
   `AUTPlusSniper : AUTWeaponFix` with non-fire zoom mode) + C++ shard
   (`UTPlusProj_*` fake-pairing pattern, parent `UTProj_StingerShard2`), reparent
   `UT+Minigun_ProjectileNew` onto it.
3. Validated client direct-hit claim for the shard: stock
   `ServerNotifyProjectileHit → NotifyClientSideHit` exists but is combo-only and has
   ZERO server validation (Epic `@TODO FIXMESTEVE`); implement with timestamp/position
   tolerance + capsule rewind, and add the same validation to the shock-combo path.
