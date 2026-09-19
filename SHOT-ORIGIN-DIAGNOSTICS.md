# Server shot-origin diagnostics (328)

Build the updated server plugin, then run on the **server**:

```text
ncp.ShotOriginDebug 1
```

Reproduce moving/landing precision shots and keep the server log. Disable with
`ncp.ShotOriginDebug 0` afterwards. This works with existing 328 clients; it adds
no RPC parameters, replicated properties, origin correction or extra rewind.
The diagnostic is off by default. It observes Shock/Instagib/Sniper damaging,
non-cone hitscan modes inside an actual `FireShot`, including both Instagib beams.
It excludes projectile/zoom modes and non-shot queries such as weapon removal.

Optional server `ncp.FireProvenance 1` also shows accepted, blocked and cancelled
requests. Optional client `ncp.FireDebug 1` shows retries and successful
`[NCFireMoveFlush]` submissions. Those switches can produce substantial logs.
Client/server clock values cannot simply be subtracted to measure packet delay.

## What the code currently does

- With `bNetDelayedShot` false, stock builds the firing origin from the server's
  current pawn/view location and the applicable fire-height offset. A late
  initial RPC does not automatically switch to a historical marker.
- With `bNetDelayedShot` true, stock searches `SavedPositions` newest first for
  `bShotSpawned`, then shifts the origin by that saved body position minus the
  current body position. It does not match the marker to the RPC fire event ID.
- The fixed retry wrapper enables delayed mode during its call. Equip completion
  can enable it independently. A retry queued for later dispatch may no longer
  have the wrapper's delayed flag when it actually fires.
- `NotifyPendingServerFire()` can set the latest sample's shot flag during server
  equip/refire handling. The flag alone does not identify its setter or prove a
  client click occurred at that sample.
- Stock checks the shot flag **before** checking sample age. A flagged sample at
  the first over-age entry can still be selected. Its search does not stop at a
  teleport. These diagnostics preserve both behaviors.

## Reading `[NCShotOrigin] ORIGIN`

Each row describes one actual `GetFireStartLoc` query, not necessarily a unique
shot or a hit. Group by owner, weapon, mode, event and generation within one
server capture. `query` distinguishes repeated origin queries during that shot.
There are at most eight rows per shot; a `LIMIT` row marks truncation.

| Field | Meaning |
|---|---|
| `source` | Actual dispatch path, such as `FixedInitial`, `FixedRetry` or `DeferredEquip`. |
| `acceptedRoute` | Whether the accepted request came through the initial or retry RPC, preserved through deferral. `untracked` means no numbered accepted context. An initial RPC can itself have suffered network delay. |
| `acceptT`, `queueMs` | Server acceptance time and server wait until this origin query. They do not measure network travel time; `-1` means no accepted context. |
| `delayed` | The weapon's delayed-shot flag at the query. It does not prove packet loss or retry delivery. |
| `lookup=current_pawn` | This origin calculation did not request a historical body position. |
| `lookup=stock_marker` | The observed stock lookup returned the position of the identified newest shot-marked sample. |
| `lookup=current_fallback` | The observed stock lookup found no eligible marker and returned the current pawn position. |
| `lookup=unobserved_or_multiple` | The exact lookup could not be resolved, for example a pawn outside the instrumented TeamArena hierarchy. Multiple calls are also left unresolved. |
| `lookup=stock_result_mismatch` | The observed result disagrees with the stock-history description. Do not attribute it to that marker. |
| `lookupCount`, `saved`, `marker` | Observed stock lookup count, history size at that lookup and zero-based selected index. The index changes as history is pruned. `saved=-1` means no history lookup was observed; `marker=-1` means no marker was selected. |
| `markerT`, `moveStamp`, `markerAgeMs` | Server movement-record time, associated client movement timestamp and age since the server recorded it. **Marker age is not packet latency or click age.** `-1` means no selected marker. |
| `maxAgeMs`, `overAge`, `ageCutoff` | Configured stock search age, whether the selected marker exceeds it, and whether an unflagged old sample stopped the search. |
| `newerTeleport`, `markerTeleport` | A newer visited sample has a teleport flag, or the selected sample itself has one. These describe history; they do not claim a through-wall hit. |
| `body`, `lookupPos`, `bodyShift` | Current pawn body position, stock lookup's returned position and their difference. When no lookup is observed, the latter two vectors are placeholders, not a world-origin selection. |
| `origin` | The final returned firing origin, including applicable eye/fire offsets and stock geometry adjustment. It is distinct from the saved body position. |
| `fireZ`, `zFresh` | Resolved fire-height field and whether its stock 60 ms freshness condition holds. Applying it also depends on the first-person/center-fire path. |

The TeamArena hook returns `Super::GetDelayedShotPosition()` unchanged and then
describes the stock selector's result. It does not invent an origin for custom
pawn overrides. No marker is consumed, retagged or associated with an event by
this observer. A marker recurring for different events is useful evidence to
review, not proof those requests were delayed or incorrectly validated.

These logs can establish which server origin was used. They cannot reconstruct
the client's exact click-time world position or establish Banko's reported cause
without a matching repro. The move flush improves local submission order, not
network delivery guarantees.
