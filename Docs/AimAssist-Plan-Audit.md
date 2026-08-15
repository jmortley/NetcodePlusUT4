# Aim Assist — Audit of Codex's Initial Plan vs Claude's Proposal

Audited: `Docs/AimAssist-Codex-Initial-Plan.md` against traced evidence
(stock UT 4.15 module, UE4.15 engine fork, NetcodePlus @ `327-clutchwork`,
UT3 script source). Companion doc: `Docs/AimAssistV1-Proposal.md` (Claude).
Verdicts: VERIFIED (checked against source), CORRECT-UNVERIFIED-BY-CODEX
(true, but Codex asserted it without evidence), PENDING (research still
running), GAP (missing from Codex's plan), DISAGREE (with reasoning).

## 1. Claim-by-claim verification of Codex's plan

| # | Codex claim | Verdict | Evidence |
|---|---|---|---|
| 1 | `ANPPlayerController::PlayerTick` rewrites `DeferredFireInputs` (same-frame Start→Stop pairs deferred to next tick) — "not neutral" | **VERIFIED** | NPPlayerController.cpp:16-94: Step 1 front-injects `DeferredStopFires`, Step 2 removes same-frame pairs and defers the Stop. This mutates the exact queue `ApplyDeferredFireInputs` flushes (UTCharacterMovement.cpp:653-660) — genuinely conflicts with the frozen fire path and the just-landed `ncp.StopClearsPending` semantics. Codex's "fresh ANCPlayerController, leave ANPPlayerController dead" is the right call. |
| 2 | BP children of the old PC hang the editor; C++-only, never a BP child | **VERIFIED + root cause now known** | NPPlayerController.cpp:11-12 warning confirmed. Root cause traced: pump-less O(N²) reinstance/recompile in 4.15 (no `FScopedSlowTask` in the compile→reinstance→GC path; `GetDependentBlueprints` scans every loaded BP) presenting as a hang on the **editor-UI reparent** path — NOT the bridge (its BP verbs postdate the Apr-2026 hang by 3 months). Distinct from the weapon-reparent CRASH: PCs have no instanced subobjects, so no `ClassWithin` assert. Full detail in proposal §9. |
| 3 | "Git history contains multiple removal/restoration iterations around the camera-manager assignment" | **VERIFIED** | Confirmed: `1a8302c` add → `da48977` remove → `abc57fe` restore, plus the `c9a1899`→`da48977`→`abc57fe` three-variant sequence on 2026-04-13 (all hung within 16 min). Constructor body proven irrelevant to the hang (empty ctor hung too). |
| 4 | `InitGame()` runs before `Login()` spawns `PlayerControllerClass`; mutator BeginPlay / `GameModePostLoginEvent` too late | **VERIFIED — and InitGame is REQUIRED, not merely sufficient** | Sequence confirmed (`InitGame` before `Login`→`SpawnActor(PlayerControllerClass)`, GameModeBase.cpp:658). Stronger finding Codex's plan understates: the shipped **hub modes are BP children** of the C++ modes (SERVER-ADMINS.md — `gameMode`=`ElimPlus_C`/`NCP-IGCTF_C`/`WipeoutPlus_C`/…; only NCShaftArenaGame is native; ANCPlusCTFGameMode is `Abstract`). Serialized BP CDOs **stomp constructor-set classes** — proven in-repo by ClutchGameMode.cpp:255-258 re-asserting HUDClass in InitGame. So **constructor-only assignment is insufficient for hub modes**; must (re)assign in InitGame. Codex proposed InitGame but for the weaker "before Login" reason, not the stomp reason. |
| 5 | Stock look-input separation: mouse→`Turn`/`LookUp`→`AddYaw/PitchInput`; pad→`TurnRate`/`LookUpRate`→`TurnAtRate`/`LookUpAtRate` | **VERIFIED** | UTPlayerController.cpp:478-484 (bindings), :1722-1732 (rate handlers, `BaseTurnRate=45` hardcoded), DefaultInput.ini:31-34 (mappings). Matches Claude's findings exactly. |
| 6 | Friction hook: override `TurnAtRate()`/`LookUpAtRate()` | **VERIFIED-VIABLE, with a check Codex skipped** | These are `virtual` (UTPlayerController.h:990, :996) — Codex asserted the override plan without verifying virtualness. It holds: axis-binding delegates dispatch virtually for virtual methods. Had they been non-virtual, rung 4 would have silently done nothing. Also virtual: `UpdateRotation` (:1117), `InputAxis` (:349), `SetupInputComponent` (:170). |
| 7 | `InputAxis()` receives physical `FKey` + `bGamepad` — clean device-source signal | **VERIFIED** | UTPlayerController.h:348-349, .cpp:775-788. Genuine advantage of a custom PC: event-driven attribution vs Claude's `GetKeyValue` polling. |
| 8 | UpdateRotation happens before movement flushes deferred fire; view-rotation assist naturally becomes both the rendered and the validated direction | **VERIFIED** | Full order traced in Claude proposal §2/Q2 (PlayerController.cpp:2027-2075, UTCharacterMovement.cpp:653-660, UTWeaponFix.cpp:1053/1181-1183). Both plans share this foundation. |
| 9 | UT3 mechanism taxonomy (friction / adhesion / InstantAimHelp) and the decision not to port near-miss promotion | **VERIFIED** | Matches the verbatim extraction (UTConsolePlayerInput.uc:653-794, :508-642; UTWeapon.uc:2263-2345). Codex's reading of `InstantAimHelp` details (stationary ×0.5, zoom-reduced modifier) is accurate. |
| 10 | UT3's device gate was unfinished (`AimingHelp()` returns true; comment proposes permanent mouse disqualification) | **VERIFIED** | UTConsolePlayerController.uc:76-92, comment block quoted in extraction. |
| 11 | Mixed-input rules (right-stick-only arming, same-frame mouse veto, 0.75s lockout + 0.2s re-arm, friction scales only the rate path, never combined RotationInput) | **CONVERGENT** | Independently identical philosophy to Claude's state machine (§4 of proposal). Parameter deltas are trivial (Claude: 0.75s stick-hold re-arm + 0.5s ramp-in). Convergence from two independent designs is good evidence the shape is right. Claude adopts Codex's rule 9 (optional strict mode: mouse look disables assist until respawn) as an extra cvar. |
| 12 | Versioning: no bump while unassigned/local; bump 327→328 before any network deployment because old clients can't resolve the new replicated PC class | **VERIFIED + one fact Codex lacked** | Correct reasoning. Missing fact: the bump is already owed — `327-clutchwork` is 218 commits ahead of shipped `origin/main` with `AClutchRoundState` (15+ replicated properties) while all branches still read 327. Next release bumps regardless; both plans' network features ride it free. |
| 13 | FireVal: exclude assisted sessions client-side unless a later bump tags samples | **CONVERGENT** | Same decision as Claude's, same single gate point available (UTWeaponFix.cpp:1153→:1173). |
| 14 | "Do not alter the module's PlayerController-free launcher direct-connect path" | **VERIFIED** (context found) | `NetcodePlus.cpp:444-447`: the `-ncpconnect` travel is deliberately PC-free because "routing this through a PlayerController (even our own ANPPlayerController) crashes the editor" — a second, independent PC-adjacent editor fragility the team already routes around. Confirms the caution; unaffected by opt-in rungs. |

## 2. Errors and gaps in Codex's plan

1. **GAP — the high-fps silent-drop trap.** `AController::SetControlRotation`
   drops per-frame changes < 0.001° (Controller.cpp:115, `Equals(NewRotation,
   1e-3f)`). Weak magnetism at 240–480 fps produces sub-tolerance per-frame
   deltas; without a remainder accumulator the pull silently dies exactly when
   frame rate is highest. Codex's UpdateRotation-stage magnetism has the same
   exposure as any other write path. Fix (both plans): accumulate and emit only
   when |accum| ≥ 0.005°, unit-tested at simulated 60 vs 480 fps.
2. **GAP — magnetism must require per-frame stick deflection**, not just
   established "look ownership". Codex rule 8 lets eligibility persist while the
   right stick is idle; combined with (1) this either dead-bands or, worse,
   permits pull without live aiming input (drift toward "releasing the stick
   never moves the view" violation). Claude's rule: deflection ≥ threshold this
   frame is a hard gate and scales the pull.
3. **GAP — no zoom/FOV treatment.** "Likely disabled initially for sniper zoom"
   is listed but unspecified. Traced fact both plans must handle: this build
   FOV-scales MOUSE sensitivity only (engine PlayerInput.cpp:1629) — zoomed
   gamepad look is NOT slowed stock. Claude spec: angular thresholds scale by
   CurrentFOV/DefaultFOV; UT3's zoomed moving-target radius collapse ported;
   magnetism off while zoomed.
4. **GAP — no line-up guard.** `AUTPlayerController::UpdateRotation` early-outs
   during line-ups (UTPlayerController.cpp:4447-4451). Implementation guidance
   for the PC route: inject magnetism into `RotationInput` BEFORE calling Super
   (inherits the early-out and pitch clamping); never `SetControlRotation` after
   Super (bypasses `ProcessViewRotation` clamps).
5. **GAP — unspecified target selection.** Deliberately deferred by Codex's
   stop-point, but the hard requirements (rendered capsules via
   `ShotIntersectsRenderedCapsule` refactor, occlusion via the
   `COLLISION_TRACE_WEAPON` FireValTrace pattern, team/dead/feign filters,
   hysteresis, trace budget) are fully specified in Claude's §3 and are
   PC-agnostic — reusable verbatim by either architecture.
6. **Minor — feign-death, Mod.ini persistence, menu integration** absent
   (consistent with Codex's narrower approved scope; specified in Claude's plan).

## 3. The real fork: PC-first (Codex) vs pawn-now (Claude)

Both plans agree on: fire path untouched, mouse path untouched, UT3 friction as
the primary mechanism, magnetism optional/default-0, mixed-input defense shape,
FireVal exclusion, no bullet magnetism, eventual v328.

The genuine disagreement is sequencing and blast radius:

**Codex (PC-first)** — aim assist waits for a proven ANCPlayerController.
- Pros: cleanest possible hooks once proven — friction is a local multiply
  inside virtual `TurnAtRate` (no engine axis-state discipline needed);
  magnetism in `UpdateRotation` is same-frame (zero latency vs Claude's
  one-frame); `InputAxis(bGamepad)` gives event-driven device attribution; the
  PC foundation also unlocks future non-assist features.
- Cons: assist is hostage to the riskiest item in the repo's institutional
  memory (editor hazards around PC classes — even if the hang is BP-only, the
  canary ladder + approvals serialize a long pipeline); the custom PC spawns for
  EVERY player in an assigned mode including KBM players (Super-only overrides
  are behaviorally identical, but any future PC bug hits everyone — process
  risk, not per-frame divergence); rollout requires wiring every game mode +
  bump BEFORE first networked use; per-mode canary (`?NCPC=1` in one mode)
  means assist can't reach the main hub modes until full PC deployment.

**Claude (pawn-now)** — assist ships on `ATeamArenaCharacter::Tick` +
`SetAxisProperties`/`RotationInput`, no new spawned classes.
- Pros: works in every mode from day one with no per-mode wiring; KBM players'
  code path is literally unmodified (they never execute a line of assist-adjacent
  code); no dependency on the editor-hazard track; the only wire change is the
  policy flag riding the already-owed bump.
- Cons: friction lever is engine axis-state (needs the restore-on-disengage +
  CDO-base-re-read discipline — specified, but it IS more moving parts than a
  local multiply); magnetism is one frame late (imperceptible but real);
  device detection is polling-based.

## 4. Recommended synthesis (two tracks, one core)

1. **Track A — ship aim assist v1 on the pawn** (Claude architecture). No PC
   dependency, all modes, smallest blast radius. The assist CORE (device state
   machine, target selection, friction/magnetism math) is built as PC-agnostic
   pure functions in `NCAimAssist.h/.cpp` — inputs: view point/rot, key values,
   dt, candidate list; outputs: sensitivity factor + rotation delta.
2. **Track B — PC foundation proceeds independently** (Codex rungs 1–5,
   unchanged, including the fresh-class decision and stop-point). Its goal is
   the foundation itself + the editor-hang answer, not aim assist.
3. **v2 convergence option:** if/when ANCPlayerController is proven, re-home the
   SAME assist core behind `TurnAtRate`/`LookUpAtRate`/`UpdateRotation` with a
   thin adapter — gaining same-frame magnetism and `InputAxis(bGamepad)`
   attribution — and retire the axis-properties lever. The two plans then merge
   rather than compete; nothing built in Track A is throwaway.
4. Cross-adoptions now: Codex takes the 1e-3 accumulator, per-frame deflection
   gate, FOV scaling, line-up guard, feign rule, bump-owed fact, and the
   target-selection spec; Claude takes Codex's optional strict-lockout policy
   cvar and (for Track B) the InputAxis attribution plan.

## 5. Editor-hang research — RESOLVED (was blocking Track B)

Complete; full detail in proposal §9. Verdicts that change the plan:
- **C++-only custom PC is safe.** The hang is an editor-UI-reparent phenomenon
  (pump-less O(N²) reinstance/recompile presenting as frozen Slate), never on the
  C++ InitGame-assignment path. Not a true infinite loop; likely bounded minutes.
- **The constructor is not the culprit** (empty ctor hung too) — so Codex's rung-2
  "constructor only" build will not itself reproduce the hang; the hazard is
  strictly BP-child creation/reparent, which the plan already forbids.
- **Hub modes are BP-wrapped** → Track B rung 3 must assign in **InitGame**
  (constructor alone is stomped by serialized BP CDOs), and NCShaftArenaGame is
  the correct canary mode precisely because it's the one natively-launched hub
  mode. Codex's choice was right; the reason is firmer than stated.
- **Add a PC entry to the MapForge fragile-class reparent gate**
  (MapForgeBridgeServer.cpp:2100-2101) so the bridge can't headlessly reopen this
  hang — cross-references [[reparent-verb-crashes-weapons]].
- **Codex worktree "canary" is unrelated** (server-side aim audit); no PC-hang
  work has actually been attempted — the Codex plan is design-only.

## 6. Open items blocking final sign-off (product decisions, not research)

- Track selection: adopt the two-track synthesis (§4), or Codex's PC-first
  sequencing, or Claude's pawn-only?
- Foundation class if Track B proceeds: fresh `ANCPlayerController` (Codex's
  recommendation, endorsed) vs reusing ANPPlayerController (rejected — non-neutral).
- From Claude proposal §8: feign-death exclusion; commit the pending
  `StopClearsPending` working-tree change on `327-clutchwork` before branching;
  default friction strength (0.6 proposed); adopt strict-lockout cvar (default
  off).
