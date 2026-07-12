# Charged Rocket Release Transaction Plan

Status: Deferred for later implementation. No implementation changes have been made.

## Problem

At high ping, the client and server can disagree about the selected loaded-rocket mode and loaded count. The server can begin or continue a charged burst with `CurrentRocketFireMode == 0` even though the client selected grenades, producing an authoritative standard rocket during the grenade burst.

The current charged release mixes the stock `ServerStopFire` flow, the custom `ServerStopFireFixed` flow, retries, and a separate incremental `ServerCycleRocketMode()` RPC. This leaves multiple ordering and duplicate-processing paths.

## Recommended fix

Use one charged-release transaction:

```cpp
ServerReleaseLoadedRockets(
    SelectedMode,
    LoadedCount,
    ReleaseTimestamp,
    TransactionId
);
```

The server must:

- Validate and clamp the selected mode and loaded count.
- Reject or safely ignore duplicate and stale transaction IDs.
- Latch the validated mode and count for the complete burst.
- Use the latched mode for every projectile in that burst.
- Keep `ActiveBurstMode` immutable until the final scheduled projectile has fired.

Additional changes:

- Replace incremental `ServerCycleRocketMode()` with absolute `ServerSetRocketMode(NewMode)`.
- Include the selected mode again in the release RPC so release is self-contained.
- Do not send stock `ServerStopFire` for the charged state; use only the custom charged-release transaction.
- Clear the latched burst state only after the complete burst or an explicit validated abort.

## Validation

The server remains authoritative. It should clamp:

- `SelectedMode` to supported and enabled rocket modes.
- `LoadedCount` to `MaxLoadedRockets`, available ammo, and the maximum count permitted by authoritative charge timing.
- `ReleaseTimestamp` to the accepted lag-compensation window.

The transaction ID should be monotonic with wrap-safe comparison. Replays of an already processed release must not spawn additional projectiles.

## Why a mode latch alone is insufficient

A smaller `ActiveBurstMode` latch prevents the mode from changing in the middle of a burst. It does not fix a burst that begins with the wrong server mode. The release RPC must carry the selected mode to make the transaction self-contained and close that failure path.

## Rollout

This changes the client/server RPC contract and therefore requires a coordinated client and server rollout.
