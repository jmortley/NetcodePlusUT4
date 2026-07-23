# NetcodePlus Controller Aim Assist - Community Discussion Proposal

**Status:** Discussion draft only. No aim-assist gameplay code has been approved or implemented.

**Purpose:** Give players, server operators, and competitive organizers a concrete proposal to debate before NetcodePlus commits to an implementation or default policy.

## Why discuss this now?

Unreal Tournament has a steep controller learning curve, while NetcodePlus currently offers no controller-specific aiming support. A carefully bounded system could make controller play more approachable without changing how weapons register hits or granting assistance to mouse aim.

Aim assist is also a legitimate competitive concern. Even modest assistance can change duels, weapon balance, and perceptions of fairness. The goal of this proposal is therefore not to announce a finished feature. It is to define the narrowest credible design, expose its tradeoffs, and let the community challenge it before gameplay code is written.

## Proposed principles

1. **The crosshair remains authoritative.** A shot goes exactly where the player can see the crosshair pointing.
2. **No bullet magnetism.** A near miss is never converted into a hit, and target collision is never enlarged for an assisted shot.
3. **Mouse aim is untouched.** Mouse sensitivity, acceleration, rotation, firing, and hit validation keep their existing paths.
4. **Assistance is controller-look specific.** A gamepad button, trigger, D-pad, or movement stick cannot grant assistance to mouse look.
5. **Friction comes first.** The first gameplay experiment would only slow right-stick rotation near a valid target. It would not move the view on its own.
6. **Magnetism is a separate decision.** Any gentle camera pull would be independently configurable, default to zero during initial testing, and require additional community approval.
7. **Servers retain policy control.** A server must be able to disable aim assist completely or permit friction while forbidding magnetism.
8. **The feature must fail closed.** Spectating, replays, line-ups, disabled firing, dead players, invalid targets, uncertain input ownership, or lost visibility all disable assistance.

## What UT3 did - and what is not being copied

Unreal Tournament 3 combined three distinct forms of aiming help:

- **Target friction:** reduced controller turn speed while aiming near a target.
- **Target adhesion:** added some visible camera movement toward a target while the player was moving.
- **Instant aim help:** allowed certain instant-hit near misses to be processed as hits.

This proposal considers the first mechanism, leaves the second open for later discussion, and rejects the third outright. NetcodePlus will not bend a shot, expand an enemy for hit registration, or report a hit somewhere other than the visible crosshair direction.

## Phase 0: establish a neutral NC PlayerController

Before adding aim assist, the proposal is to establish a safe C++ PlayerController foundation.

NetcodePlus already contains an older experimental controller that rewrites deferred firing input. It is not a neutral base and will not be activated or reused. A new `ANCPlayerController`, derived directly from the stock UT controller, would be created instead.

The foundation rollout would be deliberately uneventful:

1. Add the empty native C++ class without assigning it to a game mode.
2. Add only the stock camera-manager constructor setup required by this UT build.
3. Assign it through a default-off URL canary in one native mode, proposed `NCShaftArenaGame`.
4. Add super-only input overrides that change no values.
5. Add debug-only observation of mouse-look and right-stick-look sources, still without changing rotation.

No Blueprint child would be created or reparented. Existing project history shows that editor-side PlayerController Blueprint reparenting is fragile, while direct native assignment is the path the engine already supports.

Each rung must be reviewed before widening the canary. The foundation is valuable only if keyboard/mouse play and stabilized firing behavior remain unchanged.

## Phase 1 candidate: target friction

Target friction reduces right-stick sensitivity while the reticle is close to a valid, visible enemy. It does not rotate the camera by itself. If the player releases the right stick, the view does not move.

The candidate system would:

- operate only on the `Gamepad_RightX` and `Gamepad_RightY` look paths;
- rank targets by angular distance from the crosshair rather than by world-space distance alone;
- use the target's rendered position, including visual mesh offsets;
- require a clear weapon-channel visibility trace;
- exclude teammates, dead players, feigning players, and invalid or hidden targets;
- retain a target briefly with hysteresis so the slowdown does not flicker between nearby players;
- scale angular thresholds with field of view so the on-screen region remains consistent;
- restore the player's unmodified controller sensitivity immediately when any eligibility gate fails.

The initial strength would be a playtest value, not a promise. One candidate is `0.6` applied to a UT3-derived friction curve, with the client master switch off until the player opts in and the server permits it.

## Phase 2 question: visible magnetism

Magnetism means a small, visible rotation of the camera toward the selected target. Because the crosshair moves with the camera, the shot would still follow the crosshair; this is not bullet magnetism.

This is the most controversial part of the proposal and is intentionally separated from friction. During initial work:

- magnetism strength would default to `0`;
- friction could be tested and evaluated without it;
- servers could cap it at zero even if a client requests it;
- enabling a nonzero default would require a later, explicit decision.

If eventually tested, magnetism would require active right-stick deflection in the current frame and player movement activity. An idle stick would produce no pull. It would be disabled while zoomed for the first version, rate-limited, frame-rate independent, and accumulated carefully so high frame rates do not silently change its strength.

## Mixed-input protection

The system must not use a generic "last device was a gamepad" flag. That design allows an unrelated controller input to arm assistance while the player continues aiming with a mouse.

The proposed ownership rules are:

1. Only meaningful `Gamepad_RightX` or `Gamepad_RightY` input may establish controller-look ownership.
2. Controller buttons, triggers, the D-pad, and the movement stick never arm aim assist.
3. Any meaningful `MouseX` or `MouseY` input disables assistance immediately for that frame.
4. If mouse and right-stick look occur in the same frame, the mouse veto wins.
5. Mouse look begins a short lockout, with `0.75` seconds proposed as a starting point.
6. After the lockout, the player must hold the right stick above its deadzone for a short re-arm period before assistance fades back in.
7. Friction scales only right-stick look. It never scales combined rotation input where mouse and controller values could be mixed.
8. Magnetism, if ever enabled, additionally requires live right-stick deflection in the current frame.

An optional strict competitive policy could keep assistance disabled until respawn or the next round after mouse look is detected.

### Honest limitation

Client gameplay code cannot prove that a signal labeled `Gamepad_RightX/Y` came from a physical analog stick. Hardware adapters or remappers can present mouse motion as a virtual stick. At that point the input is indistinguishable from controller input at this layer.

The proposal addresses accidental and ordinary mixed input, including the failure mode where pressing a gamepad button arms mouse assistance. It does not claim to solve deliberate device spoofing. Server-side disable/cap controls and a friction-first, magnetism-default-zero policy limit the incentive and impact while broader competitive policy is considered separately.

## States where assistance must be inert

Assistance would be disabled when any of the following is true:

- the local player or server has disabled it;
- input ownership is keyboard/mouse or uncertain;
- the right stick is idle where live deflection is required;
- the player is dead, feigning death, spectating, in a line-up, or unable to fire;
- a replay or killcam world is playing;
- the candidate is a teammate, dead, feigning, occluded, or outside the allowed cone;
- the game is in a state where normal player view rotation is suspended;
- the player is zoomed, for magnetism in the initial proposal.

No assistance would run for bots or remote pawns.

## Server and player controls

The exact names are not final, but the intended control surface is:

### Player controls

- Master aim-assist on/off switch, initially off.
- Friction strength slider.
- Magnetism strength slider, initially zero and subject to the server cap.
- Optional debug display for dogfood testing only.

### Server controls

- Allow or disallow controller aim assist.
- Permit friction while forcing magnetism to zero.
- Cap any permitted magnetism strength.
- Optionally use the strict mixed-input lockout policy.

The active server policy should be visible to players rather than silently changing behavior.

## Rollout proposal

1. **Controller foundation canary:** no aim behavior; validate spawning, input parity, menus, travel, standalone, listen, and dedicated-server behavior.
2. **Input-attribution canary:** debug-only device transitions; verify that buttons and movement never arm assistance and mouse look always vetoes it.
3. **Friction dogfood:** opt-in, server-controlled, conservative strength, magnetism fixed at zero.
4. **Public friction evaluation:** publish tuning and collect controller and keyboard/mouse feedback.
5. **Magnetism decision:** decide whether it should remain unavailable, remain server-opt-in, or proceed to a separate dogfood test.
6. **Default-policy decision:** only after measured playtests and community review.

The release-candidate line is already versioned as NetcodePlus 328. Any network-visible controller or policy state would ship only with matching version-gated clients and servers. A later network-layout change after 328 ships would require another coordinated version bump.

## Validation requirements

Before any public default changes, testing must cover:

- keyboard/mouse parity with assist both enabled and disabled;
- controller feel at low and high frame rates;
- release-stick behavior with no camera drift;
- mixed mouse/controller input in the same frame;
- gamepad buttons and left-stick movement never arming assistance;
- targets behind walls and around corners;
- teammates, FFA opponents, dead players, and feigning players;
- zoomed weapons;
- spectating, killcams, replays, line-ups, and post-match states;
- standalone, listen-server, dedicated-server, reconnect, and travel paths;
- shots continuing to land only where the visible crosshair points;
- server disable and cap settings taking effect predictably.

No compile or playtest result should be claimed until it has actually been observed.

## Risks and tradeoffs

- **Competitive perception:** Some players will consider any assistance unacceptable, even friction-only. Server policy and transparent settings are necessary but may not resolve that disagreement.
- **Tuning advantage:** Excessive friction can become target detection or make tracking unnaturally stable. Conservative limits and occlusion/target filters are required.
- **Device spoofing:** Virtual-stick remapping cannot be reliably distinguished from a physical controller by this client layer.
- **Controller skill expression:** Too much friction or magnetism can flatten the difference between new and expert controller players.
- **Input accessibility:** Very strict mouse lockouts can inconvenience legitimate mixed-input or accessibility setups.
- **Foundation blast radius:** Assigning a custom PlayerController affects every player in that mode, including keyboard/mouse users. That is why the neutral, behavior-free canary comes first.

## Questions for community feedback

1. Should NetcodePlus support controller target friction at all?
2. Is friction-only acceptable in competitive modes?
3. Should visible magnetism remain permanently unavailable, be server-opt-in, or be allowed with a conservative cap?
4. Should aim assist be disabled by default globally, enabled by the player where the server permits it, or decided per playlist?
5. How strict should the mouse-to-controller re-arm rule be?
6. Should any mouse look disable assistance only briefly, until respawn, or until the next round?
7. Should friction remain available while zoomed, or should all assistance turn off during zoom?
8. Should feigning players be excluded, accepting that the absence of friction might itself reveal information?
9. What server-policy information should be shown in the menu or HUD?
10. Which controller skill levels, frame rates, and accessibility setups must be represented in playtesting?

Useful feedback should distinguish between:

- opposition to any aim assist in principle;
- concern about a particular mechanism, such as magnetism;
- concern about tuning strength;
- concern about mixed-input abuse or device spoofing;
- a reproducible controller usability problem the proposal does not address.

## Current decision boundary

This document does not authorize aim-assist gameplay implementation. The proposed next engineering step is only the neutral C++ PlayerController canary. Results from that foundation and community feedback should be reviewed before target selection, friction, magnetism, replication, or settings UI are implemented.
