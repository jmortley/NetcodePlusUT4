# Experimental remote character animation URO

`ncp.RemoteAnimationURO` defaults to `0`. After building this client, enable the experiment with:

```text
a.URO.Enable 1
ncp.RemoteAnimationURO 1
```

Return to authored mesh settings with `ncp.RemoteAnimationURO 0`. No character Blueprint checkbox or asset change is required. Leave existing Blueprint URO settings alone: meshes with authored URO or a custom scheduling policy are excluded rather than overwritten. The engine's global `a.URO.Enable` must be on and `a.URO.ForceAnimRate` must be zero for NCP to acquire a policy.

## Scope and priority

This experiment affects eligible simulated third-person character bodies on an online client. It does not change movement ticks, replication, projectile prediction or dedicated-server simulation. Standalone, listen-server and replay worlds are excluded.

The initial policy uses fixed conservative thresholds:

| Condition | Animation policy |
| --- | --- |
| Local pawn or a local view target | No reduced updates |
| Target sphere within 2,500 Unreal units of any local camera | Full rate |
| Target sphere intersects a 15-degree cone around any local view direction | Full rate |
| Projected sphere diameter is at least approximately 10% of view height | Full rate |
| Zoom, camera cut, large camera/aim disagreement, or invalid view data | Full rate |
| Current or smoothed wall-frame duration exceeds 1/240 second | Full rate |
| Small distant peripheral target remains eligible for 0.25 seconds | At most every other frame, using engine interpolation |

Promotion to full rate occurs at the next pre-movement decision; demotion must wait through continuous eligibility again. Camera information is shared across characters once per world/frame. The engine resolves the camera later in the frame, so the decision uses its latest available camera, with conservative zoom/cut/aim guards. There are no visibility traces or new tick prerequisites.

Existing offscreen pose visibility rules remain in effect. The policy sets the non-rendered URO rate to one, so it does not introduce additional offscreen frame skipping. Death, ragdoll, root motion, unsupported components and another active animation consumer exclude the body. The normal sleeping first-person mesh has a narrow exception, with explicit settlement before first-person view transitions. Custom secondary meshes with animation instances are excluded even when hidden or master-pose driven; some character/overlay combinations may therefore receive no reduction.

## Engine integration

`UTeamArenaCharacterMovement::TickComponent` selects the policy before `Super`, ahead of body pose work already ordered after movement. An unmanaged body is acquired only after it becomes a potential reduction candidate. Once acquired, promotion changes scheduling inputs between one and two frames instead of repeatedly toggling the component flag.

UE4.15 shares animation-rate parameters across components owned by the same actor. It also adds stored `AdditionalTime` to a pose even when the URO flag is off. The state helper therefore restores ordinary disables through a single acknowledged catch-up update. It does not manually tick animation. Mesh replacement, simulation changes and secondary-mesh activation discard the old pose's pending interval rather than transferring it to a different consumer. At the imposed two-frame rate, that interval is at most one skipped animation frame.

## Validation and profiling

Development profiling exposes `stat NCPURO`: total policy CPU time, managed characters, and characters assigned the two-frame policy. The last counter describes the requested policy, not proof that every corresponding animation evaluation was skipped; engine visibility, controller and cache rules still apply. These stats are compiled out of the normal Shipping build.

The native automation tests are under `NetcodePlus.Performance.RemoteAnimation`. They cover view geometry, invalid inputs, priority hysteresis and pending animation-time calculation. During implementation, four unchanged test groups ran through a standalone native runner using the real engine Core types: 53 assertions passed. Focused Development/Shipping syntax checks passed for the changed implementation. This does not validate a linked client or engine runtime transitions.

For the online ElimPlus/Wipeout comparison, use the same build, map, players, camera route and FPS cap; alternate the NCP CVar between zero and one. Confirm that ordinary remote characters are actually receiving a two-frame policy before interpreting the result. Compare total game-thread time, animation work, policy overhead and frame-time percentiles; at a 700 FPS cap, additional headroom and better lows matter more than average FPS.

Exercise close fighting, long-range sniper aim, fast camera turns, zoom, spectating/behind-view changes, respawns, ragdolls and armor/outline activation. Toggle both the NCP option and engine master switch while a distant character is moving. Check head alignment, weapon sockets/muzzle effects and animation-driven sounds. Test the below-240 FPS fallback separately. Runtime correctness and a net frame-time improvement remain unmeasured; the experiment stays off by default.
