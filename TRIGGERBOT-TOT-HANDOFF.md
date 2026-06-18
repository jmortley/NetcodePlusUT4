# Branch: `feature/triggerbot-tot`

Branched from `dev @5741c3b`. Adds **server-authoritative time-on-target (ToT)
telemetry** to flag a suspected trigger-bot for **human review** (no auto-kick).
Self-contained; does not touch the hit-registration / lag-comp path.

## What it does

For the local client, while holding a precision hitscan weapon in **Elim or
instagib-CTF**, it measures how long the crosshair has continuously rested on a
**visible** enemy (occlusion-checked) before each shot. That dwell time is
sampled at fire and sent to the server, which accumulates a per-player
distribution and emits a summary at match end.

The signal: a trigger-bot fires the instant a target appears, so nearly all its
hits have ~0 ms dwell; a human has a healthy mix of low-dwell flicks and
high-dwell tracking/holding shots. Review the **per-match low-dwell fraction**,
never a single shot.

## Files touched (5)

| File | Change |
|---|---|
| `Source/Public/UTWeaponFix.h` | `bToTDetectActive` (replicated, owner-only gate), `ToTDwellSeconds`, `UpdateToTTracker()`, `ServerReportFireToT()` RPC, `FindBotEventsMutator()` |
| `Source/Private/UTWeaponFix.cpp` | BeginPlay gate; per-frame tracker in Tick; report in FireShot client branch; RPC impl/validate; DOREPLIFETIME |
| `Source/Public/MutBotEvents.h` | `FToTStat`, `ToTStats` map, `ToTLowDwellMs`, `RecordFireToT()`, `PostToTReport()` |
| `Source/Private/MutBotEvents.cpp` | accumulate per player; report at `WaitingPostMatch` (log + POST) |
| `Source/Public/NetcodePlus.h` | `NETCODE_PLUGIN_VERSION` 326 → **327** |

## Key design points

- **Separate UNRELIABLE sidecar RPC** `ServerReportFireToT(uint8 DwellMs)` — NOT
  folded into `ServerStartFireFixed`. The hit-reg / resend path is never touched.
  A dropped telemetry sample only thins the distribution.
- **Gate** `bToTDetectActive` is set server-side in `AUTWeaponFix::BeginPlay` and
  replicated owner-only. True only when **(Elim || instagib-CTF) AND** the weapon
  `IsA` **`AUTPlusSniper` or `AUTPlusShockRifle`** (or child) — i.e.
  instagib / shock / sniper / LG. Excludes minigun/enforcer (hitscan but not
  precision). Dormant (zero cost) everywhere else.
- **Trace:** one `COLLISION_TRACE_WEAPON` line trace per frame (the channel UT's
  own crosshair traces use — blocks on geometry AND characters, so occlusion is
  free in a single trace). ~negligible cost; client-only, one player.
- **Dwell is timed (ms), not frame-count** → identical signal at 60 or 540 fps.
- **Tunable:** `MutBotEvents::ToTLowDwellMs` (default 16 ms) = the "near-zero"
  threshold.

## Client roll

The replicated property + new RPC change `AUTWeaponFix`'s layout, so **327 is a
forced client roll** (version gate + launcher manifest). Old clients get a clean
version mismatch.

## Pending

- **Bot endpoint not built:** server POSTs to `POST /triggerbot_report`
  (UT4IGBot, FastAPI) — that handler does not exist yet. Until it does, the
  review readout is the server log line `[ToT] <name>: shots=N lowDwell(<=16ms)=M
  (P%) meanMs=...` (emitted at Warning, survives Shipping).
- **Build + test in VS** (not built on this branch).

## Merge note

Includes the `NetcodePlus.h` 326→327 bump. If `dev` also bumped the version,
expect a one-line conflict there on merge — resolve to 327 (or higher).
