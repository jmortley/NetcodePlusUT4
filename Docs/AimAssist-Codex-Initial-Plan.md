# Controller Aim Assist — Codex Initial Plan

**Status:** Pre-implementation proposal; no gameplay code or branch changes made  
**Starting branch:** `327-clutchwork`  
**Proposed feature branch:** `codex/aimassist-v1`  
**Immediate scope:** Establish and validate an NC C++ PlayerController foundation first. Stop before implementing aim assist.

This document records Codex's initial design so it can be compared with Claude's plan before implementation begins. It distinguishes verified code-path findings from proposed decisions.

## Decision summary

| Area | Proposed decision | Reason |
|---|---|---|
| Controller foundation | Create a fresh `ANCPlayerController` derived from `AUTPlayerController` | The existing `ANPPlayerController` is not neutral: it rewrites deferred fire input for an older debounce experiment. |
| Existing controller | Leave `ANPPlayerController` untouched and unassigned | Enabling it would implicitly alter firing behavior and inherit its documented editor/camera-manager history. |
| Blueprint use | C++ only; never create or reparent a Blueprint child | Existing comments and history identify Blueprint CDO reconstruction hangs around the old custom controller. |
| Initial assignment | Opt-in canary in one native NC mode, proposed `NCShaftArenaGame`, through a server URL option such as `?NCPC=1` | `InitGame()` runs before `Login()` spawns `PlayerControllerClass`; the canary remains isolated and default-off. |
| Gamepad identification | Determine eligibility exclusively from right-stick look axes | A generic “last device was gamepad” flag allows a controller button to grant mouse aim assist. |
| Mouse behavior | Keep the stock mouse bindings and rotation path untouched | Mouse input must remain bit-identical with aim assist enabled or disabled. |
| Friction hook | Override controller-only `TurnAtRate()` and `LookUpAtRate()` | Stock UT already separates rate-based gamepad look from direct mouse deltas. |
| Magnetism hook | Reserve `UpdateRotation()` for optional, later magnetism | It can adjust the visible view before the fire path captures `GetViewRotation()`. |
| Fire/hit path | Do not modify it | Shots must continue to go exactly where the assisted crosshair points. |
| UT3 near-miss help | Do not port it | It promoted near misses to hits without moving the crosshair and conflicts with NetcodePlus validation. |
| Versioning | No bump for an unassigned local canary; bump `327` to `328` before network deployment of the new controller | Older clients do not contain the new replicated PlayerController class. |

## Verified findings

### Existing `ANPPlayerController` is not an empty base

`ANPPlayerController::PlayerTick()` scans and rewrites `DeferredFireInputs`, removing some same-frame `StopFire` entries and reinserting them on the next tick. That was an older low/zero-debounce mouse experiment. Activating this controller as the aim-assist foundation would therefore change firing behavior even before aim assist was added.

This conflicts with the current requirement to preserve the stabilized `StartFire`/`StopFire` behavior. The new controller should not inherit or copy that code.

The existing class also explicitly assigns `AUTPlayerCameraManager` and warns that Blueprint children hang during reparent CDO reconstruction. Its Git history contains multiple removal/restoration iterations around that camera-manager assignment. A new C++ class still needs the staged canary process; it must never have a Blueprint child.

### Safe PlayerController assignment point

The engine sequence is:

1. Construct the selected native or Blueprint-derived game mode.
2. Call its `InitGame()`.
3. Later, during player `Login()`, spawn `PlayerControllerClass`.

Consequently, an NC game mode can enforce the canary class from `InitGame()` after Blueprint defaults are loaded but before the first player controller is spawned.

A mutator `BeginPlay()` or the existing global `GameModePostLoginEvent` is too late to select the controller for the arriving player. Replacing an already spawned controller is outside the initial plan.

### Stock look-input separation already exists

The stock bindings are:

- `MouseX` → `Turn` → `APlayerController::AddYawInput`
- `MouseY` → `LookUp` → `APlayerController::AddPitchInput`
- `Gamepad_RightX` → `TurnRate` → `AUTPlayerController::TurnAtRate`
- `Gamepad_RightY` → `LookUpRate` → `AUTPlayerController::LookUpAtRate`

`AUTPlayerController::InputAxis()` also receives the physical `FKey` and a `bGamepad` argument. This lets the NC controller observe the real look-axis source without treating an unrelated controller button as proof that mouse aim deserves assistance.

### Rotation reaches the existing fire path correctly

Local input processing and `UpdateRotation()` happen before character movement flushes deferred fire input. NetcodePlus then captures the player's view rotation for the shot and validates it against the server-side rewound target representation.

Therefore, view rotation assistance applied at the controller input/rotation stage naturally becomes both:

- the direction shown by the crosshair; and
- the direction claimed and validated for the shot.

No weapon trace, projectile direction, RPC, or hit-validation modification is required.

## What UT3's aim assistance did

UT3 contained three different mechanisms that should not be conflated.

### Target friction

While the reticle was on or near a target, UT3 scaled down `aTurn` and `aLookUp`. This reduced controller sensitivity around the target but did not independently generate view movement.

This is the primary behavior worth adapting.

### Target adhesion

UT3 could add DeltaTime-scaled yaw and pitch toward the current target while the player was moving. This changed the visible view rotation, so the crosshair followed the assisted direction.

This is comparable to the proposed optional magnetism, but the NC version should remain separately tunable and default to zero.

### Bullet-level `InstantAimHelp()`

After an instant-hit trace missed a valid projectile target, UT3 called `InstantAimHelp()` and checked the shot ray against an expanded target cylinder:

- `AimingHelpRadius` was added outside the normal collision cylinder.
- The allowance was halved for a stationary target.
- The allowance was reduced while zoomed.
- A qualifying near miss replaced the final impact entry with the target and was processed as a hit.

This was near-miss promotion rather than visible camera assistance. The crosshair did not need to move onto the target. It will not be carried into NetcodePlus.

The available UT3 source also has an unfinished device gate: `AimingHelp()` returns `true`, with comments proposing that mouse use should permanently disqualify a player from assistance. The aiming feel can be studied, but that eligibility implementation should not be copied.

## Preventing mixed-input mouse aim assist

The design must not maintain a single generic state such as `bUsingGamepad` that any controller event can activate. Aim-assist eligibility will be based on **look-source ownership**, not the last device that produced any input.

### Proposed rules

1. Only nontrivial `Gamepad_RightX` or `Gamepad_RightY` input may qualify controller look.
2. Controller buttons, triggers, D-pad input, and left-stick movement never arm assistance.
3. Any nontrivial `MouseX` or `MouseY` input disables assistance immediately for the current frame.
4. If mouse and controller look occur in the same frame, assistance is disabled for the whole frame.
5. Mouse look begins a proposed 0.75-second lockout.
6. After the lockout, right-stick input must remain above the configured deadzone for approximately 0.2 seconds before assistance rearms.
7. Friction scales only the right-stick rate passed through `TurnAtRate()`/`LookUpAtRate()`. It never scales the combined `RotationInput`, where mouse and controller deltas could already be mixed.
8. Optional magnetism requires established controller-look ownership. Left-stick strafing may modulate its strength but cannot establish eligibility by itself.
9. A stricter competitive policy may make mouse look disable assistance until respawn or the next round.

These rules prevent the specific “touch a controller button, then aim with the mouse under controller aim assist” failure. They also allow mixed accessibility input for movement without granting mouse-look assistance.

They cannot prove that a right-stick signal came from genuine controller hardware. An adapter or remapper that deliberately presents mouse motion as `Gamepad_RightX/Y` is indistinguishable at this layer and belongs to anti-cheat/device-attestation policy. The server-side aim-assist setting is competitive policy, not a security boundary.

References:

- [EA: Apex Legends aim assist](https://help.ea.com/en/articles/apex-legends/aim-assist/)
- [EA: Breach anti-cheat update](https://www.ea.com/games/apex-legends/apex-legends/news/breach-anti-cheat-update)

## Proposed NC PlayerController canary ladder

Each rung gets its own build and validation. No Blueprint child will be created.

### Rung 1 — Empty class

- Add `ANCPlayerController : AUTPlayerController` with no behavior.
- Do not assign it yet.
- Build the plugin and confirm Unreal Header Tool/class registration succeeds.

### Rung 2 — Constructor only

- Add only the constructor.
- Explicitly set `PlayerCameraManagerClass` to stock `AUTPlayerCameraManager`, following the currently working PIE requirement documented by the old class.
- Build again.

### Rung 3 — Opt-in spawn canary

- Add a default-off server URL option such as `?NCPC=1` to one native NC game mode, proposed `NCShaftArenaGame::InitGame()`.
- When absent, retain the stock controller.
- When present, set `PlayerControllerClass = ANCPlayerController::StaticClass()` before login.
- Build and verify standalone, listen-server, and dedicated-server controller spawning without touching the hub/front-end path.

### Rung 4 — Super-only virtual seams

- Override only the virtual functions needed by the future feature:
  - `InputAxis()`
  - `TurnAtRate()`
  - `LookUpAtRate()`
- Each override forwards to `Super` without changing data or behavior.
- Build and test mouse/controller input parity.
- Do not add `UpdateRotation()` until magnetism work actually requires it.

### Rung 5 — Read-only device telemetry

- Record nonzero mouse-look and right-stick-look observations.
- Ignore controller buttons and movement axes for aim eligibility.
- Add debug-only device transition logging.
- Make no rotation changes.
- Build and validate simultaneous-input classification.

### Stop point

After Rung 5, report results and obtain another approval before implementing target selection, friction, magnetism, replicated server policy, or settings persistence.

## Later aim-assist architecture, not yet authorized

If the controller foundation passes its canaries, the proposed next phase is:

- friction inside controller-only `TurnAtRate()` and `LookUpAtRate()`;
- optional magnetism during `UpdateRotation()`;
- target selection using rendered enemy positions rather than replicated actor anchors;
- visibility/occlusion gating matching existing NetcodePlus fire-validation traces;
- no teammates, dead players, spectators, killcam/replay, feigning, or firing-disabled windows;
- explicit zoom behavior, likely disabled initially for sniper zoom;
- DeltaTime-scaled math validated at 60 and 240+ FPS;
- client cvars defaulting the master feature and magnetism to off;
- an owner-only server allowance/clamp if competitive server policy is added;
- exclusion of assisted sessions from FireVal sampling unless a later protocol bump explicitly tags the samples.

## Versioning decision

Creating the class while it remains unassigned and used only in a local canary does not require an immediate protocol bump.

Before any network server spawns `ANCPlayerController`, `NETCODE_PLUGIN_VERSION` should change from `327` to `328`. Even with no new replicated properties or RPCs, older clients do not contain the new replicated actor class and cannot safely resolve it.

Any later owner-only replicated server policy flag would independently require the same coordinated version change if it had not happened already.

## Repository-safety notes

- Preserve the pre-existing unstaged `Source/Private/UTWeaponFix.cpp` ghost-fire fix exactly as-is.
- Do not modify `StartFire`, `StopFire`, `ServerStartFireFixed`, or `ServerStopFireFixed`.
- Do not activate or silently inherit the legacy deferred-fire behavior in `ANPPlayerController`.
- Do not alter the module's PlayerController-free launcher direct-connect path.
- Do not claim successful compilation or runtime testing until each result has actually been observed.

## Approval boundary

The first implementation approval should cover only:

1. creating `codex/aimassist-v1` from `327-clutchwork`;
2. implementing and building Rungs 1–5 one at a time; and
3. documenting the results.

Aim-assist behavior itself should require a separate review and approval after the NC PlayerController foundation is proven stable.
