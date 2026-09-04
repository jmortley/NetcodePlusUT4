# Stock UT4 Netcode Failure Modes at High Client Framerates

**Why NetcodePlus exists, with line-number receipts.**

Every mechanism described here was verified by reading the stock source in this fork
(UT4 UE4.15, CL-3525360) on 2026-09-03. Line numbers cite that tree:

- `UT/…` = `UnrealTournament/Source/UnrealTournament/`
- `Engine/…` = `Engine/Source/Runtime/Engine/`

The *mechanisms* below are source-verified. The specific framerate thresholds
(~400–420 fps for visible projectile curving, ~700 fps for unreliable hitscan) are
empirical, from A/B testing with RTSS caps; the mechanisms explain why thresholds in
that range exist, not why they land on those exact numbers.

---

## Symptoms

| Client fps | Symptom | Root cause | Section |
|---|---|---|---|
| Any | First (aimed) shot is the worst-synchronized shot | Fire RPC always beats the rotation that goes with it | §2 |
| ~400+ | Projectiles visibly curve / warp mid-flight | Fake↔real spawn-rotation divergence, snapped and amplified | §3 |
| ~700 | Hitscan intermittently doesn't go where you aim | Unreliable RPC drops + a resend fallback that fires along the wrong rotation | §4 |
| High fps | Spread weapons hit a different pattern than shown | Client/server random-seed desync | §5 |
| Below ~400 | "Feels fine" | Forgiveness systems mask errors up to their padding | §6 |

---

## 1. How aim reaches the server in stock

The server fires your shots along **its** copy of your view rotation, not yours.

- Server hitscan/projectile direction = `GetBaseFireRotation()` → `UTOwner->GetViewRotation()`
  = the pawn's ControlRotation on the server (`UT/Private/UTWeapon.cpp:1641-1656`, via
  `GetAdjustedAim` at `:1658`).
- That ControlRotation is written in exactly one place: when a **rotation-bearing move RPC**
  is processed — `ProcessServerMove` calls `PC->SetControlRotation(ViewRot)`
  (`UT/Private/UTCharMovementReplication.cpp:833`).
- The fire RPCs carry **no rotation at all**: `ServerStartFire(FireModeNum, FireEventIndex,
  bClientFired)` and the `ZOffset` variant, which adds only a compressed eye-height Z
  (`UT/Public/UTWeapon.h:607-612`, impls at `UT/Private/UTWeapon.cpp:625-665`).

So the entire question is: *how fresh is the move-stream rotation when the fire RPC lands?*
Stock's answer:

**Moves are throttled into bursts.** `UTCallServerMove` delays sending while
`TimeStamp - ClientUpdateTime < NetMoveDelta`, where `NetMoveDelta` is **17 ms** for
"important" moves (accel/mode changed vs. last acked move) and **33 ms** otherwise
(`UT/Private/UTCharMovementReplication.cpp:693-708`, threshold at `:701`).

**Backfilled moves carry no rotation.** When a burst finally goes out, every accumulated
move except the newest is sent as `UTServerMoveQuick(TimeStamp, Accel, Flags)` — no yaw,
no pitch (`:748-764`, quick path at `:761`; server side processes it with no rotation
update, `:972-1009`). Only moves flagged by `NeedsRotationSent()` — dodge, slide, or
`bShotSpawned` (`:622-625`) — go as `UTServerMoveSaved` with view angles. The final move
of the burst goes as full `UTServerMove` with yaw+pitch (`:780-791`).

**Lost rotation is never recovered.** The resend path for old moves,
`UTServerMoveOld(OldTimeStamp, OldAccel, OldYaw, OldMoveFlags)`, carries **yaw only — pitch
is not in the signature** (`UT/Public/UTCharacter.h:317`, call site
`UTCharMovementReplication.cpp:745`).

**Everything is unreliable.** All four move RPCs are `UFUNCTION(unreliable, server)`
(`UT/Public/UTCharacter.h:312-325`). So are `ServerStartFire`, `ServerStartFireOffset`,
`ServerStopFire`, and `ServerHitScanHit` (`UT/Public/UTWeapon.h:599-631, :1377`). A drop
is silent; the only recovery paths are the yaw-only old-move resend and the fire-event
resend described in §4.

At 700 fps, roughly 11 of every 12 moves during combat (17 ms important cadence) — or 22
of 23 at the 33 ms cadence — are rotation-less Quick moves.

---

## 2. Failure mode A — the first shot loses the race, by design

Epic's mechanism for synchronizing fire-frame rotation is the `bShotSpawned` flag: a move
flagged with it is never delayed and always carries rotation
(`UTCharMovementReplication.cpp:607, :618-625, :694-696`). The flag comes from
`AUTWeapon::WillSpawnShot` → `UUTWeaponStateFiring::WillSpawnShot`:

```cpp
// UT/Private/UTWeaponStateFiring.cpp:56-59
return IsPendingFire(CurrentFireMode)
    && (GetTimerRemaining(RefireCheckHandle) < DeltaTime);
```

This predicts *refire-timer* shots. It structurally cannot flag the **click frame**:

1. Input processes → `StartFire` → the shot spawns synchronously and `ServerStartFire`
   is queued. The refire timer resets to its full interval.
2. Later that same frame, the movement component builds the move and evaluates
   `WillSpawnShot(DeltaTime)` (`UTCharMovementReplication.cpp:607`). Timer remaining ≈ full
   refire interval > DeltaTime → **false**.

Consequences, per click:

- The fire RPC leaves in that frame's packet.
- The move carrying the click-frame rotation is unflagged, therefore *delayable* — it can
  sit in the burst queue up to 17/33 ms behind the fire RPC that already left.
- The server processes the fire with **previous-burst rotation**, aged anywhere from 0 to
  a full burst interval.

Held-fire chains (link, stinger, follow-up rockets) get flagged moves and good sync. The
single deliberate flick — rocket, shock, sniper click — never does. The aimed shot is the
worst-synced shot in the stock game.

---

## 3. Failure mode B — why projectiles start curving around 400 fps

Client-side, `SpawnNetPredictedProjectile` spawns a fake projectile immediately along your
current view (`UT/Private/UTWeapon.cpp:2097-2197`). Server-side, the real projectile
spawns along the stale rotation from §1/§2, then gets **fast-forwarded by your prediction
time**: `CatchupTick` ticks it ahead by `GetPredictionTime()` before anyone sees it
(`:2170-2183`). Angular error × catchup distance = displaced spawn.

When the real projectile replicates back, the linker compares it to your fake: if the real
is *ahead* of the fake along its velocity, the **fake is snapped onto the real's path**
(`UT/Private/UTProjectile.cpp:380-413`; error measured at `:383`, direction test `:384`,
snap via `PostNetReceiveLocationAndRotation` at `:392-394`; ongoing re-sync at
`:592-622`). That snap is the visible curve/warp.

The divergence angle is (rotation staleness) × (your turn rate at fire time). Why fps makes
it worse:

- At 60–144 fps, the 17 ms important gate is ~1–3 frames. The rotation the server fires
  with is at most a frame or two older than the view you actually saw when you clicked —
  divergence between "what you saw" and "what the server did" stays around one frame.
- At 400–700 fps your view and your fake are 1.4–2.5 ms fresh, but the server's rotation
  is still on the 17/33 ms burst schedule, with only rotation-less Quick moves in between.
  The see-vs-server gap approaches the full burst interval.
- The error is then *amplified* by the server's catchup fast-forward and *revealed* by the
  fake-snap. ~400–420 fps is empirically where typical flick-speed × divergence-window
  crosses the visibility threshold; nothing in the code picks that number, the window just
  grows past what the snap can hide.

The very-high-ping path makes the intent explicit: with enough latency the client doesn't
even spawn the fake immediately (`GetProjectileSleepTime` delay, `:2119-2134`,
`:2199-2231`) — the whole system is built around tolerances that high fps quietly exceeds.

---

## 4. Failure mode C — at 700 fps, hitscan becomes a lottery

Three compounding pieces:

**a) The client throttles its own uplink.** The client's outgoing token bucket is its own
`ConfiguredInternetSpeed = 15000` bytes/s (`UnrealTournament/Config/DefaultEngine.ini:105`;
engine default is 10000, `Engine/Config/BaseEngine.ini:1726`). Server-side
`MaxInternetClientRate=72000` raises what the **server** will accept, but a stock client
never asks for more. Quick-move backfill alone costs roughly 14–18 wire bytes per move —
at 700 fps that's ~10–12 KB/s before the rotation-bearing bursts, fire traffic, hitscan-hit
claims, and channel acks. In fights, the bucket runs dry.

**b) Dry bucket = silent drops.** Every RPC in the fire and move path is unreliable (§1).
A saturated connection drops them with no error. The move-side backstop is yaw-only (§1);
the fire-side backstop is:

**c) The resend path fires along the wrong rotation.** Fire events are tracked by
`FireEventIndex`, and a lost one is re-sent via `ResendServerStartFire`. That handler is
the **only place `bNetDelayedShot` is ever set** (`UT/Private/UTWeapon.cpp:682, :705`;
default false at `:57`). The delayed path makes the shot reach back into the pawn's
`SavedPositions` history for the entry flagged `bShotSpawned`, and fire with that stored
rotation/position — but only within `MaxShotSynchDelay = 0.2 s`
(`UT/Private/UTCharacter.cpp:192`; scan at `:408-423`, position variant `:391-406`).

For a refire-chain shot, that works. For a **first shot there is no flagged entry** (§2) —
the scan falls through to the fallback at `UTCharacter.cpp:422`: live `GetViewRotation()`
*at resend-arrival time*. The server fires your click along wherever you were aiming an
RTT-plus later. That is the "sometimes it just doesn't go where I aimed" experience:
per-shot roulette between on-time-but-burst-stale and resent-along-future-aim.

(Related, movement-side: the engine's saved-move buffer handles overflow by freeing the
**entire list** — `MaxSavedMoveCount = 96`, wholesale reset at
`Engine/Private/Components/CharacterMovementComponent.cpp:9259-9267` — and UT throttles
good-move acks to `MinTimeBetweenClientAdjustments = 0.1 s`
(`UT/Private/UTCharacterMovement.cpp:143`, gate at
`UTCharMovementReplication.cpp:496-507`), so high fps + real ping walks the unacked count
toward that cliff. NetcodePlus patches the saved-move layer separately; this document
focuses on the weapon path.)

---

## 5. Failure mode D — spread weapons show one pattern, apply another

Spread is applied server-side AND client-side from a synchronized seed:

```cpp
// UT/Private/UTWeapon.cpp:1729-1736
FMath::RandInit(10000.f * UTOwner->GetCurrentSynchTime(bNetDelayedShot));
```

For normal (non-resent) shots, `GetCurrentSynchTime` returns the **movement timestamp of
the last processed move** (`UT/Private/UTCharacter.cpp:371-389`). The client seeds with its
fire-frame move time; the server seeds with whatever move it had processed when the fire
RPC arrived. They agree only if the fire-frame move beat the fire RPC in — nearly always
at 60 fps (the burst gate ≈ frame time), nearly never mid-burst-window at 400+. Different
seed → different pellet pattern: enforcer/stinger/flak visually land where they didn't hit.

---

## 6. Why it feels fine below ~400 — the masking layer

Stock carries real forgiveness that absorbs small aim-data error:

- **Rewind retest**: after the strict trace, `HitScanTrace` re-tests every pawn against its
  rewound position along the server's ray (`UT/Private/UTWeapon.cpp:1823-1880`, rewind
  lookup `:1830`, best-target override `:1881-1897`).
- **Client hit claims**: `ServerHitScanHit` lets the client name the character it hit; that
  target gets +40 uu of extra capsule padding in the retest (`:1818`, `:1828`).
- **Headsphere assist**: a second search via `ChooseBestAimTarget` (0.7 aim cone, 150 uu
  offset) catches headshots the capsule trace missed (`:1918-1926`; target-guess variant
  with 0.85 cone at `:1696-1727`, implementation
  `UT/Private/UTGameplayStatics.cpp:315-410`).

All of these correct the *target test*, not the *ray*. They hide rotation staleness until
the error exceeds the padding — which is why the breakdown feels like a cliff rather than
a slope: below it the forgiveness eats the error, above it shots start missing outright.

---

## 7. What NetcodePlus changes

The stock resend path (§4c) is the telling artifact: Epic knew a late-executing fire event
needed to carry its moment with it, and built exactly that — but only for resends, and
only for refire-chain shots. NetcodePlus makes that the rule for every shot:

```cpp
// Plugins/NetcodePlus/Source/Public/UTWeaponFix.h:745-747
UFUNCTION(Server, Reliable, WithValidation)
void ServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp,
    FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset);
```

Point by point against the stock failure modes:

| Stock | NetcodePlus |
|---|---|
| Fire RPC carries no rotation; server uses burst-stale ControlRotation (§1, §2) | Every fire event carries `ClientViewRot` — the ray is the one you saw |
| Unreliable fire RPCs, drop-and-reroll resend semantics (§4) | `Reliable` fire funnel, plus an explicit resend queue with identical payload validation |
| `uint8` FireEventIndex wraparound ambiguity | `int32` event index with sequence validation and server-pushed correction (`ClientConfirmFireEvent`) |
| Seed/timing desync from move-stream races (§5) | `ClientTimestamp` in the payload; server validates against refire cooldown rather than inferring timing |
| Blanket +40 uu claim padding (§6) | Claim padding split and validated: moving vs. stationary (`HitScanPaddingStationary`), cvar-controlled |
| Uncapped catchup amplification (§3) | Forward-prediction capped (`ProjectilePredictionCapMs`, `MaxCatchupTime`) |

Rotation is latched at the RPC edge *before* `SetPendingFire`/state entry, because stock's
`UUTWeaponStateActive::BeginState` auto-fires latched pending fire without re-consulting
anything downstream (see `UTWeaponFix.h:368-373` and the fire-funnel notes in the plugin
docs).

---

## Appendix: reproduction

A/B with RTSS caps on the same map/hardware:

1. Cap 144 → stock feels fine. Forgiveness margin >> divergence window.
2. Cap 420+ → aimed projectiles (rocket/shock at range, fired during a flick) visibly
   correct mid-flight on stock; NetcodePlus does not.
3. Cap 700 → on stock, hitscan flicks intermittently miss despite the crosshair being on;
   worst during heavy combat (uplink saturation → drops → §4c fallback). Spread weapons
   show pattern mismatch. NetcodePlus unaffected.

Measured example (2026-09-03, RTSS 700, same elim scenario): stock-lineage elim 669.8 avg /
501 1% low with an oscillating frametime baseline; NetcodePlus 699.1 avg / 583 1% low,
flat baseline — the netcode fixes also remove measurable per-tick overhead, but the aim
fidelity difference above is the reason the plugin exists.
