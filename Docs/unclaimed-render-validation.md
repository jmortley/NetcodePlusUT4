# Unclaimed hitscan validation in 328

The server's unclaimed render gate now requires position and posture evidence at
its estimated presentation time. It applies only to a selected pawn hit from a
remote human using a claim-capable exact hitscan mode when no target claim arrived.
Claimed primary hits, padding, time-search rescues, and Link/Minigun targeting keep
their existing paths.

This change uses existing server history and CPP-local trace state. It changes no
RPC, replicated property, serializer, public class layout, or client asset. Existing
328 client payloads remain compatible; no client update is needed for this patch.
That is a source-level compatibility assessment, not a mixed-version runtime test.

## Enforced checks

With `ncp.UnclaimedRenderGate 1` (the existing default), the central sample must:

1. Have a server-observed RTT and a finite requested age within 0..250 ms. The age
   remains half RTT plus `ncp.HitAttribRenderExtraMs`; an unsupported age is rejected
   rather than clamped to a different time.
2. Have a real older/newer position bracket for a historical sample, with no
   teleport marker in the bracket or between the render and primary validation
   epochs. The shared helper's stationary newest-endpoint exception is not used.
3. Have a known historical capsule posture from `ATeamArenaCharacter`. Missing
   samples, a teleport, or a slide transition inside the posture bracket fail.
4. Intersect that single reconstructed capsule, using the existing
   `ncp.UnclaimedRenderSlack` and the original world-clipped shot segment.

A zero-age sample uses the current authoritative capsule. Historical samples use
the recorded physical half-height, including ordinary crouch. Historical sliding
uses the established bottom-aligned `SlideTargetHeight` shape, without a standing
alternative or slide-grace expansion. Current slide state does not choose the shape.
Capsule radius and `SlideTargetHeight` are not recorded historically; their current
values remain in use. Exact skeletal animation and client smoothing are unknown.

An enforced failure retains the existing world-impact demotion and blocks the
later head-sphere fallback. It does not retry another pawn. A target without NCP
posture history cannot establish this unclaimed hit. Legitimate unclaimed shots
near spawn/history warm-up or a posture transition can therefore become misses.
Increasing radius slack cannot make missing evidence valid.

`ncp.UnclaimedRenderGate 0` is the existing live kill switch for **all** unclaimed
render enforcement, including the earlier geometric check. It does not restore
only the old posture behavior. With hit attribution enabled, the strict verdict
continues to log in shadow mode.

## Timing probe: logs only

Use these commands on the server console after installing the server build:

```text
ncp.UnclaimedRenderGate 1
ncp.HitAttribDebug 1
ncp.UnclaimedRenderProbeMs 10
```

Keep the existing slack override (for example, the investigated server used `4`).
This patch does not change the default or any configured slack/search padding.

The probe runs only after the central check passes, while hit attribution is
enabled. It tests the **same** target and shot segment at the estimated age minus
and plus the configured interval. At `10`, the three sample ages are central,
central minus 10 ms, and central plus 10 ms. The interval is exploratory and is
not a measured client uncertainty bound. It is clamped to 0..50 ms per side;
`0` disables probe work. Invalid neighboring ages are reported, not clamped.

Probe results never feed the central verdict, target selection, damage, or demotion
flag. There is no probe-enforcement cvar. This adds at most two posture/position
queries and capsule-distance tests for one already selected target; it adds no
pawn scan, skeletal evaluation, or network message. CPU cost is unbenchmarked.
`ncp.HitAttribDebug 0` stops hit-attribution logs and all probe work.

The existing `[HitAttrib]` fields are retained and these fields are appended:

| Field | Meaning |
| --- | --- |
| `renderChkReason` | `pass` or `miss` for a measured capsule; `no-timing`, `invalid-age`, `no-history`, `no-posture`, or `invalid-capsule` when evidence is unavailable; `na` outside this gate. |
| `renderChkPosture` | `slide` or `non-slide`; non-slide includes crouching. `na` when no capsule was measured. |
| `renderChkHalfHeight`, `renderChkRadius` | Half-height of the tested shape and the capsule component's radius, in uu. Effective geometric radius is their minimum. |
| `renderChkSlack` | Applied central/neighbor radius tolerance in uu; `na` outside this gate. |
| `renderProbeMs` | Applied interval on each side, or zero when not running/configured off. |
| `renderProbe` | `pass`: both neighboring capsules also intersect. `shadow-fail`: both are measurable and at least one misses. `unknown`: at least one neighbor lacks evidence. `base-fail`: central check already failed, so no neighbors were tested. `off`: probe disabled. `na`: gate not applicable. |
| `renderYounger`, `renderOlder` | Each neighboring sample's reason, using the same vocabulary as `renderChkReason`; `na` if not tested. |
| `renderYoungerMissBy`, `renderOlderMissBy` | Distance beyond the unpadded capsule (including shot trace radius), in uu; compare with `renderChkSlack`. `na` means unmeasured. |

`renderChkMissBy` also becomes `na` for unmeasured central samples instead of a
numeric sentinel. A negative measured miss distance means the ray is inside the
estimated capsule. It does not prove intersection with the client's visible mesh.
The summary probe verdict is `unknown` even if the other measured neighbor misses;
the individual reason fields preserve that evidence.

To estimate the additional impact of a future temporal-agreement rule, count
`renderChk=pass renderProbe=shadow-fail` by shooter/ping, and report `unknown`
separately. Agreement means all three sampled capsules intersect the ray. It
neither searches for a favorable time nor establishes continuous agreement at
every instant between samples. Server-only logs measure policy impact, not what
the client actually rendered.

## Validation status

The patch is intended for server deployment with existing 328 clients. Source
review and static checks cover its scope and isolation of the diagnostic probe.
No Unreal build or live-match validation was run for this change, following the
request to leave builds to the operator. Before evaluating match behavior, the
server build must compile and run with the updated code.
