# Third-person weapon outline shadows

## Finding

UT4's `AUTWeaponAttachment::UpdateOutline()` duplicates the visible `Mesh3P`
component, enables custom depth, and disables the main pass. It does not disable
shadow casting. The live editor defaults inspected on 2026-09-09 have
`CastShadow`, `bCastDynamicShadow`, and `bCastStaticShadow` enabled for Shock,
Rocket, Flak, Sniper, Minigun, and Link attachments. Instagib uses the same
`Shock_Rifle_3p` mesh and also enables these shadow flags.

This differs from UT's `CreateCustomDepthOutlineMesh()` helper, used by character
and first-person weapon outlines, which explicitly calls `SetCastShadow(false)`.

The local UE4.15 renderer confirms that the attachment copy remains eligible for
shadow rendering:

- `FPrimitiveSceneProxy` copies the component's shadow flags independently of
  `bRenderInMainPass`. `IsShadowCast()` checks shadow and visibility flags, not
  main-pass participation.
- `FSkeletalMeshSceneProxy::GetViewRelevance()` reports shadow relevance and
  main-pass relevance separately. Its section shadow flags also depend on the
  component's `CastShadow` flag.
- `FProjectedShadowInfo::GatherDynamicMeshElementsArray()` gathers skeletal
  shadow geometry using shadow relevance and dynamic relevance without requiring
  main-pass relevance. Grouped lighting does not deduplicate matching meshes.

This establishes redundant shadow eligibility, not a measured frame-time cost.
Actual work depends on shadow settings, section/material eligibility, lighting,
visibility, and whether an outline copy is registered. The six inspected mesh
assets have `ShadowPhysicsAsset=None`; UE4.15 does not fall back to their ordinary
physics asset for capsule-shadow shapes. Their capsule flags therefore do not
establish duplicated capsule geometry. No gain is claimed for scenes with shadows
disabled or no active weapon outlines.

## NCP correction

`TeamArenaCharacter.cpp` disables `CastShadow` on the attachment's existing
custom-depth-only duplicate immediately after both calls to stock
`UpdateOutline()`: the ordinary path and the deferred character-model rebuild.
Only that distinct duplicate is eligible for the correction. The source weapon
mesh, materials, master pose, bounds, stencil, visibility, and registration
lifecycle retain their existing behavior.

Stock's only native call to the third-person attachment outline update is in
`AUTCharacter::UpdateOutline()`. A replacement attachment is handled when its
outline is first created through that path. Repeated updates are a flag check;
`SetCastShadow(false)` dirties render state only when the flag changes. The
deferred path keeps its existing registration safeguards and runs the correction
after its own stock call. Turning an outline off still unregisters it. Stock does
not create holstered attachment outlines through this path, so the patch does not
introduce them or scan unrelated components.

The stock getter returns a const pointer to the mutable runtime component, so the
local helper uses a narrow `const_cast` to call the exported component setter. It
does not modify an asset, CDO, engine source, public class layout, replicated
property, RPC, or plugin version. This is client rendering behavior and can be
tested with a rebuilt dogfood client against an existing 328 server using NCP
characters. No Blueprint replacement or pak change is required.

## Validation

Source trace and diff checks completed. No Unreal build, PIE test, runtime shadow
capture, or FPS measurement has been run for this patch.

After building the client/editor DLL:

1. Enable spectator X-ray or player outlines while viewing other players. Run
   `ncp.PawnDump`. The existing `[PawnDbg][DEPTH]` rows now include `castShadow`
   and `masterPoseCastShadow`.
2. For an outlined weapon attachment whose normal mesh casts shadows, expect
   `castShadow=0 masterPoseCastShadow=1`. Compare the visual weapon outline and
   normal weapon shadow with the previous build under the same lighting.
3. Repeat after weapon changes, respawns, force-model/team changes, outline
   toggles, and replay/killcam transitions. Check that no orphan outline remains.
4. Compare the same scene and camera between builds with shadows and outlines
   enabled; inspect GPU shadow timing/draws. Toggling X-ray alone also changes
   custom-depth and animation work, so it does not isolate this correction.
