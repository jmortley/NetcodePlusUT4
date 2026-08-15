# Aim Assist v1 — Proposal (no code written yet)

Status: PROPOSAL for review. Branch plan: `aimassist-v1` off `327-clutchwork`.
Date: 2026-07-22. All file:line references verified against the current working
tree (stock UT 4.15 source, UE4.15 engine fork, NetcodePlus plugin @ `327-clutchwork`,
UT3 script source 1.5 from the mod pack).

## 1. Summary

Client-side controller aim assist, default OFF, two mechanisms:

- **Target friction** (primary): while the reticle is over/near a visible enemy,
  gamepad look sensitivity is reduced. Implemented as a runtime scale on the
  `Gamepad_RightX/Y` axis sensitivity — the same per-key `AxisConfig` mechanism the
  engine itself applies in `UPlayerInput::MassageAxisInput`.
- **Target magnetism** (secondary, default strength 0): gentle rotation toward the
  tracked target while the player is actively moving/aiming with the sticks.
  Implemented as `RotationInput` injection (`AddYawInput`/`AddPitchInput` from the
  pawn), consumed by the engine's own `UpdateRotation` next frame.

Core guarantee: **assist never touches the fire path.** Both mechanisms modify only
what feeds the engine's single input→rotation pipeline; the shot continues to fire
exactly where the crosshair points, because camera, fire capture, and server
validation all read the same `ControlRotation` that pipeline produces.

Algorithm pedigree: direct port of UT3's shipped console aim assist
(`UTConsolePlayerInput::ApplyTargetFriction` / `ApplyTargetAdhesion`, extracted
verbatim from `UT3Port\ref\ut3_2.1\UT3ScriptSource_1.5`), with UT3's tuning values
as defaults, minus UT3's third layer (`InstantAimHelp` bullet magnetism — explicitly
out of scope here, as it bends the shot rather than the view).

## 2. Findings for the pre-implementation questions (traced facts)

**Q1 — Where is gamepad look input bound?**
All input is bound on the PlayerController; the pawn binds nothing
(`SetupPlayerInputComponent` does not exist anywhere in the UT module, and the
plugin never overrides it). `AUTPlayerController::SetupInputComponent`
(UTPlayerController.cpp:478-484) binds `Turn`/`LookUp` (mouse, → engine
`AddYawInput`/`AddPitchInput`) and `TurnRate`/`LookUpRate` (gamepad, →
`TurnAtRate`/`LookUpAtRate` at UTPlayerController.cpp:1722-1732, which apply
`Rate * BaseTurnRate(=45, hardcoded) * DeltaSeconds`). **Gamepad look bindings ship
in DefaultInput.ini:31-34** (`TurnRate=Gamepad_RightX`, `LookUpRate=Gamepad_RightY`)
with AxisConfig DeadZone 0.25 / Sensitivity 1.0 / Exponent 1.0 (DefaultInput.ini:4-26).
Gamepad buttons/movement come from profile defaults (`UUTProfileSettings::
GetDefaultGameActions`, UTProfileSettings.cpp:186-355). No DefaultInput.ini work is
needed — the right stick already turns the view in stock builds.

**Q2 — Per-frame ordering on the local client** (engine PlayerController.cpp,
LevelTick.cpp, all traced):

```
PC TickActor
  └ PlayerTick
      ├ TickPlayerInput → ProcessInputStack     axis delegates fire here;
      │                                          AddYaw/PitchInput accumulate
      │                                          RotationInput (InputYawScale=2 /
      │                                          InputPitchScale=-2 applied at add)
      ├ UpdateRotation                           DeltaRot=RotationInput; base =
      │   ├ PlayerCameraManager->ProcessViewRotation   GetControlRotation();
      │   ├ SetControlRotation(ViewRotation)     pitch clamped; ControlRotation set
      │   └ Pawn->FaceRotation
      └ PC Tick(); RotationInput zeroed (PlayerController.cpp:4087)
UUTCharacterMovement::TickComponent              (bTickBeforeOwner prereq)
  ├ Super::TickComponent → movement
  └ PC->ApplyDeferredFireInputs()                fire flush — StartFire/StopFire →
                                                 FireShot samples GetViewRotation()
                                                 LIVE (UTWeaponFix.cpp:1053) =
                                                 ControlRotation set above
ATeamArenaCharacter::Tick                        ← assist logic runs HERE
World: "Update cameras last" (LevelTick.cpp:1429-1433) → DoUpdateCamera
                                                 rendered rotation = ControlRotation
```

Consequence: anything the assist writes during pawn Tick (frame N) is consumed by
frame N+1's `UpdateRotation` — i.e. **all assist output flows through the exact same
single point where stick input becomes rotation.** Camera, movement, fire capture,
and `ServerStartFireFixed`'s `ClientViewRot` can never see different rotations,
because there is only one writer. (Direct `SetControlRotation` from pawn Tick was
rejected: it lands after this frame's fire flush but before this frame's camera —
a one-frame render/fire divergence, the precise thing the brief forbids.)

**Q3 — Hook point.** Chosen: **ATeamArenaCharacter::Tick** (already overridden,
already has local-player-gated blocks at TeamArenaCharacter.cpp:1244) +
input-layer writes:
- Friction: `PC->PlayerInput->SetAxisProperties(Gamepad_RightX/Y, …)` — API exists
  in 4.15 (PlayerInput.h:306/309), self-invalidates the private axis-props cache.
- Magnetism: `AddYawInput/AddPitchInput` with values divided by
  `InputYawScale`/`InputPitchScale` (sign matters: pitch scale is −2).

Alternatives rejected with evidence:
- (b) Custom `APlayerCameraManager` without a custom PC: **impossible** —
  `PlayerCameraManagerClass` is a non-config UPROPERTY (PlayerController.h:231-233)
  read only by `SpawnPlayerCameraManager` from PC `PostInitializeComponents`; no
  config/GameMode/ini override path exists in this vintage.
- (c) Pawn-side axis re-binding: **cannot work for interception** — the PC's
  InputComponent is processed first ("first dibs", PlayerController.cpp:2341-2343 +
  top-down walk PlayerInput.cpp:1029-1032) and consumes the gamepad keys
  (bConsumeInput default true); the pawn's component (processed last) receives 0.
- Counter-rotation reconstruction in pawn Tick: fragile (must byte-match the
  TurnAtRate math forever) and still splits the frame. Rejected.

**Q4 — Gamepad detection.** None exists: `UUTGlobals::GetIsUsingGamepad()` is
hardcoded `return false` (UTGlobals.h:112); the UMG input-change delegates are
commented out; no `bUsingGamepad` state is stored anywhere (traced). We build
last-input-device tracking from `PlayerInput->GetKeyValue()` reads (post-massage;
mouse keys are unaffected by our scaling, and for pad keys we divide our own known
friction factor back out; deadzone-filtering comes free). Raw key-state reads are
not blocked by input-stack consumption (consumption only zeroes what later input
components' delegates receive).

**Q5 — Target selection uses rendered positions + occlusion.**
Reuse `ShotIntersectsRenderedCapsule`'s visual-offset math (UTWeaponFix.cpp:136-192):
rendered capsule center = `ActorLocation + (MeshComponentLocation − (ActorLocation +
ActorQuat.Rotate(BaseTranslationOffset)))`, with the floor-slide height special case.
Refactor: extract a `GetRenderedCapsule(Target, OutCenter, OutHalfHeight, OutRadius)`
static on `AUTWeaponFix` (allowed by the brief) used by both the existing shot test
and the assist. Occlusion gate: the `UpdateFireValTracker` pattern verbatim
(UTWeaponFix.cpp:1925-1951): `LineTraceSingleByChannel` on `COLLISION_TRACE_WEAPON`,
simple collision, owner ignored — first blocking hit is either the wall (no assist)
or the candidate (assist). Team filter: the FireShot pattern
(`MyTeam==255 || ThTeam==255 || MyTeam!=ThTeam`, UTWeaponFix.cpp:1153-1161); dead
filter `!IsDead()`; additionally skip `bFeigningDeath` targets so assist cannot be
used as a feign-death detector (decision point — see §14).

**Q6 — FireVal interaction: client-side exclusion, no wire change.**
The only sample send site is `ServerReportFireValidation(...)` at
UTWeaponFix.cpp:1173, fronted by `if (bFireValActive)` at :1153. We add one condition:
skip sampling while the device state is GamepadArmed and assist master is on.
Assisted dwell-time samples would contaminate the collector's KBM-calibrated
distributions; exclusion is one client-local line. (Tagging samples instead would
be wire-free *now* — the release must bump anyway, see Q8 — but it also touches the
Django ingest contract, which is out of scope. Revisit when analytics wants
gamepad segmentation.)

**Q7 — Server policy: bFireValActive pattern, replicated owner-only flag.**
Server decides in `ATeamArenaCharacter::BeginPlay` (authority branch) from Mod.ini
`[NetcodePlus] AllowAimAssist` (default 1) and `AimAssistMagnetismCapPct`
(default 100); replicates `bServerAllowsAimAssist` (bool) + `ServerMagnetismCapPct`
(uint8) with `DOREPLIFETIME_CONDITION(..., COND_OwnerOnly)` — exactly the
bFireValActive pattern (decision UTWeaponFix.cpp:453-464, condition :2214, client
read :1828). Client gates all assist on the flag and clamps effective magnetism.
This is competitive policy, not anti-cheat: client-side assist adds no capability
aimbots don't already have, and full HID-level device spoofing is accepted as out
of scope (see §6 for what we DO defend against).

**Q8 — Version bump: YES, 327→328 — and it is already owed.**
Adding replicated properties changes net layout ⇒ bump required
(NCPlusVersionGate.h:23-25 warning; gate kicks mismatches at
NCPlusVersionGate.cpp:255-262). Traced fact: `327-clutchwork` is 218 commits ahead
of shipped `origin/main` and already adds `AClutchRoundState` with 15+ replicated
properties while all branches still read `NETCODE_PLUGIN_VERSION 327` — the next
release from this line must bump regardless of aim assist. The policy flag rides
that bump at zero incremental cost. (Bump-free channels were inventoried and
rejected: no replicated field has safe spare bits; actor-presence and
sentinel-RPC-arg tricks exist but carry semantic-hygiene costs with no remaining
benefit.)

**Q9 — Inert states.** Assist runs only when ALL hold:
- `ncp.AimAssist=1` AND `bServerAllowsAimAssist`
- `IsLocalPlayerPawn()` (TeamArenaCharacter.cpp:160-164 — NOT IsLocallyControlled;
  standalone-bot-safe) → structurally inert for spectators (different pawn class)
- NOT `World->DemoNetDriver->IsPlaying()` (killcam/replay guard, FireShot precedent
  UTWeaponFix.cpp:1010-1020)
- NOT `IsDead()`, NOT own `bFeigningDeath`, NOT `IsFiringDisabled()`
  (UTCharacter.h:925/978/1250)
- NOT `UTPC->IsLineUpActive()` — UpdateRotation early-outs during line-ups
  (UTPlayerController.cpp:4447-4451), so injected RotationInput would pool and dump
  on line-up end; we must not inject during it
- Zoom: friction stays active but the moving-target radius collapse from UT3
  (`radius *= 0.05` when target speed >200 and zoomed) is ported; magnetism is
  fully disabled while zoomed. Note: this build applies FOV sensitivity scaling to
  MOUSE ONLY (engine PlayerInput.cpp:1629 `if MouseX||MouseY`) — zoomed gamepad
  look is NOT slowed in stock UT4. We do not change that (out of scope; flag as a
  possible stock-feel fix later). All assist angular thresholds scale by
  `CurrentFOV / DefaultFOV` so cones stay screen-consistent under zoom.

**Framerate independence (Q9 of the brief's numbering).** Friction is a
sensitivity multiplier on a `°/s × DeltaSeconds` path — dt-correct by construction.
Magnetism uses the exponential form `delta = err × (1 − exp(−k·dt))` (identical
convergence at 60 and 480 fps), with a °/s cap. **Trap found and handled:**
`AController::SetControlRotation` silently drops per-frame changes < 0.001°
(Controller.cpp:115 `Equals(NewRotation, 1e-3f)`). Weak magnetism at 240–480 fps
produces sub-tolerance per-frame deltas, so we accumulate into a float remainder
and emit only when |accum| ≥ 0.005°. This must be covered by a unit test at
simulated 60 vs 480 fps.

## 3. UT3 → NCP port map

| UT3 (shipped values) | NCP v1 |
|---|---|
| Friction: cylinder inflated by distance ramp 320/1500/3000uu, PeakRadiusScale 1.0 / PeakHeightScale 0.2, multiplier lerp 0.3–0.4 by horizontal closeness; `aTurn *= (1−f)` | Same geometry against the **rendered** capsule; factor × `ncp.AimAssistFriction`; applied as Gamepad_RightX/Y sensitivity scale |
| Friction had no LOS check | We add the occlusion trace (brief requires no assist through walls/smoke) |
| Adhesion: only while `aForward != 0`; cylinder ×0.65; only when aim OUTSIDE cylinder; band AimDistY/Z 128/96uu; rate lerp 0.3–2.85 capped at Pct 0.8; `+= DeltaRot × rate × dt`; LOS-checked | Same, generalized activity gate (move-stick OR look-stick deflection), × `ncp.AimAssistMagnetism` × server cap, °/s-capped, exponential dt form, RotationInput injection |
| TargetFrictionOffset Z=32 (aim at chest) | Same offset on rendered center |
| Per-weapon enables/ranges (flak 64/128/200 etc.) | v1 global (weapon-agnostic); per-weapon scala later if wanted |
| ZoomedTurnSpeedScalePct 0.85/0.45 | NOT ported (stock UT4 has no gamepad zoom scaling; changing feel is out of scope) |
| InstantAimHelp bullet magnetism | NOT ported — violates the shot-goes-where-crosshair-points invariant |
| View acceleration / auto-center | NOT ported (stock UT4 gamepad path is linear; nothing to suppress) |

Target acquisition (UT3's was native C++, unrecoverable): iterate `AUTCharacter`s,
filter self/dead/feigning/team/distance/cone (dot ≥ ~0.9, FOV-scaled), rank by
angular distance to rendered center, occlusion-trace only the best candidate + the
current target (≤2 traces/frame), 20% hysteresis before switching, weak-ptr held
(FireValLastTarget precedent).

## 4. Device detection & anti-mixed-input (the "Apex exploit")

States: `KBM` (default) ↔ `GamepadArmed`.

- Any mouse-look delta or mouse-button press → `KBM` **immediately**; also vetoes
  magnetism the same frame.
- `KBM → GamepadArmed` requires ~0.75s (`RearmTime`) of cumulative right-stick
  deflection ≥ 0.15 (post-deadzone) with **zero** mouse deltas; assist strength then
  ramps in over ~0.5s.
- Magnetism additionally requires this-frame stick deflection ≥ threshold AND
  movement activity (UT3's `aForward != 0` rationale: motion masks the pull), and
  scales with deflection. Idle stick ⇒ zero pull ⇒ "releasing the stick never moves
  the view" holds by construction.
- Friction is structurally mouse-proof: it only rescales gamepad key sensitivity;
  the mouse rotation path is untouched, so a mouse aimer gains nothing from it.
- Honest limit: full HID spoofing (Cronus/reWASD mouse→virtual-stick) is
  indistinguishable client-side and stays out of scope per the brief. Backstops:
  hub policy flag (disallow entirely, or cap magnetism to 0 = friction-only), and
  v1 tuning (magnetism default 0) keeps the spoof payoff far below Apex-class
  assist. UT3 shipped with no device gating at all; this is strictly tighter.
- `ncp.AimAssistDebug=1` logs every device-state transition and draws candidate /
  friction factor / applied delta.

## 5. Configuration surface

Client cvars (TAutoConsoleVariable file-statics, plugin style UTWeaponFix.cpp:50-134):

| cvar | default | meaning |
|---|---|---|
| `ncp.AimAssist` | 0 | master (0/1) |
| `ncp.AimAssistFriction` | 0.6 | friction strength 0..1 (×UT3 curve); effective only with master on |
| `ncp.AimAssistMagnetism` | 0.0 | magnetism strength 0..1; server-capped |
| `ncp.AimAssistDebug` | 0 | debug draw + device-state logging |

Master default 0 ⇒ the feature is OFF for everyone until opted in.
Persistence: Mod.ini `[NetcodePlus]` keys `AimAssist`, `AimAssistFriction`,
`AimAssistMagnetism` — read once client-side (static latch, LoadWeaponSettings
pattern UTWeaponFix.cpp:282-355) and pushed into the cvars; `SUTNCPlusMenu`
checkbox + sliders are a follow-up using the existing GConfig read/write pattern
(SUTNCPlusMenu.cpp:889-892/:930-932).
Server Mod.ini keys: `AllowAimAssist` (default 1), `AimAssistMagnetismCapPct`
(default 100).

Friction sensitivity write discipline (protects user config): base sensitivity is
re-read every frame from the `UInputSettings` CDO (so the stock
`SetGamepadSensitivityLeft/Right` execs — UTPlayerController.cpp:3853/3883 — stay
authoritative); the scaled value is written ONLY to the live per-instance
`UPlayerInput` via `SetAxisProperties`. `UPlayerInput::AxisConfig` is not a config
UPROPERTY, and nothing SaveConfigs the instance — while the UT paths that DO
persist (`InputSettings->SaveConfig()` in the sensitivity execs and profile apply)
write only the CDO we never touch. On any gate loss the base value is restored
once and writes stop.

## 6. Files to change (planned)

| file | change |
|---|---|
| `Source/Public/NCAimAssist.h` + `Source/Private/NCAimAssist.cpp` (new) | device state machine, target selection, friction/magnetism math (pure functions where possible, for tests) |
| `Source/Public/TeamArenaCharacter.h` / `.cpp` | `FNCAimAssistState` member + `UpdateAimAssist(Dt)` call in Tick; `bServerAllowsAimAssist`/`ServerMagnetismCapPct` replicated (COND_OwnerOnly) + BeginPlay server decision + GetLifetimeReplicatedProps additions |
| `Source/Private/UTWeaponFix.cpp` / `Public/UTWeaponFix.h` | extract `GetRenderedCapsule(...)` static from ShotIntersectsRenderedCapsule (:136-192); add assist-exclusion condition to the FireVal gate (:1153) |
| `Source/Public/NetcodePlus.h` | `NETCODE_PLUGIN_VERSION 327 → 328` (owed by clutch replication anyway — coordinate: the bump ships with the release, not necessarily with this branch's first commit) |
| `Source/Private/Tests/AimAssistTests.cpp` (new) | dt-independence (60 vs 480 fps), state-machine transitions, accumulator/1e-3 behavior, friction factor curve |
| `Docs/AimAssistV1-Proposal.md` | this document |

Deliberately untouched: StartFire/StopFire/ServerStartFireFixed/ServerStopFireFixed,
all weapon states, movement, NPPlayerController, any Blueprint asset.

## 7. Test plan (honest about what runs where)

Runnable by the implementing session (no editor, no gamepad):
1. Full plugin compile (UBT, UnrealTournament + UnrealTournamentServer targets).
2. Automation unit tests (ClutchTests.cpp precedent) for the pure math: friction
   factor curve, magnetism convergence 60 vs 480 fps equality, remainder
   accumulator vs the 1e-3 drop, device state machine (mouse veto, re-arm timing,
   threshold gating).
3. Static verification that KBM path is untouched: assist writes touch only
   Gamepad_* axis properties + RotationInput; no mouse-key or Turn/LookUp code path
   modified (reviewable by diff).

Requires the user (gamepad + play session) — mapped to the brief's list:
KBM byte-identical regression; friction feel over strafing bot; release-stick
no-drift; wall/occlusion; team/FFA; zoom behavior; 60 vs 240 fps feel; spectate/
killcam/replay inertness; fire-path sanity (shots land on crosshair, FireVal
accept rates unchanged); standalone/listen/dedicated matrix; Mod.ini + policy-flag
behavior with `AllowAimAssist=0` hub config.

## 8. Risks / open questions

1. `SetAxisProperties` per-frame cost is a map reset + lazy rebuild of the whole
   axis-props cache (PlayerInput.cpp:426, :1727-1742) — trivial in absolute terms
   (runs once per frame only while friction factor changes), but will be verified.
2. UT mouse-acceleration interaction: `AUTPlayerController::UpdateRotation`
   (UTPlayerController.cpp:4445-4483) applies optional acceleration to ALL of
   RotationInput when `AccelerationPower>0` (off by default). Gamepad players with
   mouse-accel enabled would have magnetism deltas accelerated too (stock already
   treats their stick input this way, so behavior is consistent; noted, not fixed).
3. Feign-death targets excluded from assist = assist users get a subtle
   info-DISADVANTAGE vs KBM (no friction on feigners); including them = feign
   detector. Proposal: exclude. Flag for decision.
4. Hysteresis/tuning constants are UT3's; first playtest may want a tuning pass —
   constants live in one struct for easy iteration.
5. The uncommitted `ncp.StopClearsPending` change in UTWeaponFix.cpp predates this
   work and sits on `327-clutchwork`. Branching takes it along; it should be
   committed on `327-clutchwork` first (its own feature) so `aimassist-v1` starts
   clean. Decision needed.
6. Device detection reads `GetKeyValue` (post-massage). If a 4.15 raw-value
   accessor turns out to be public (`GetRawKeyValue`), we use it and drop the
   divide-out-our-own-factor step; otherwise the division is exact anyway.

## 9. NPPlayerController / editor-hang track (separate work stream)

Aim assist v1 deliberately does NOT depend on this. Investigation complete;
findings below. Bottom line: a **C++-only custom PlayerController is safe to
establish**, the hang is an editor-UI-reparent phenomenon that never occurs on the
C++ assignment path, and the one non-obvious requirement is to assign the class in
`InitGame`, not only in constructors.

### What the git history proves (traced)

The class had live assignments then was disabled, with a documented hang saga
(all commits by the repo owner):
- `0ba499d` (2026-03-29): introduced ANPPlayerController WITH live
  `PlayerControllerClass` assignments in NCPlusCTFGameMode + WipeoutGame; empty
  constructor.
- `1a8302c` (same day): added `PlayerCameraManagerClass` to fix a PIE
  "SpawnActor failed because no class was specified".
- `cee6ca7` (same day): commented out both assignments ("ready but not tested").
- `c9a1899` → `da48977` → `abc57fe` (2026-04-13, within 16 minutes): tried
  **three** constructor variants — guarded assignment, no assignment, restored
  assignment — **all hung the editor on BP reparent/child creation**. Final state:
  assignment restored, warning added ("BP children hang the editor but direct C++
  usage via game mode constructor is fine").

Two independent conclusions from this: (a) the constructor body is irrelevant to
the hang — an empty constructor hung too; (b) the trigger was the **editor UI
reparent path**, because the LiandriMapForge bridge's `create_blueprint` /
`reparent_blueprint` verbs did not exist until 2026-07-13/07-21 — three months
after the hang. So this was manual BP-child creation/reparent in the editor, the
path that runs the extra `EnsureBlueprintIsUpToDate` conform passes and the full
reinstancer.

### Why it hangs (ranked; mechanism traced in 4.15 engine source)

1. **Pump-less long reinstance/recompile presenting as a hang** (highest
   confidence). There is NO `FScopedSlowTask`/progress pump anywhere in the
   compile→reinstance→GC cascade (KismetReinstanceUtilities.cpp, Kismet2.cpp
   CompileBlueprint, KismetCompiler.cpp), and per-compile dependency discovery
   iterates every loaded Blueprint (`GetDependentBlueprints` →
   `GetObjectsOfClass(UBlueprint, derived=true)`, BlueprintEditorUtils.cpp:2852),
   giving O(compiled × all-loaded) work with recursive child/dependent recompiles.
   AUTPlayerController is a huge, widely-referenced class; with hub content loaded,
   minutes of game-thread work freeze Slate = indistinguishable from a true hang.
   A `while (RecompilationQueue.Num())` guard rules out a literal infinite loop.
2. **PC instantiated during reinstance with a transient/REINST CDO** whose
   `PlayerCameraManagerClass` was null → the recorded "no class was specified"
   warning → degenerate camera-manager work. (Commit 1a8302c's "CDO construction
   can lose the parent's reference" is best read as this.)
3. **Hot-reload class staleness** — plugin commits prebuilt Binaries; a BP
   parented to a `HOTRELOADED_`/`REINST_` class is a classic 4.x wedge. No
   artifacts survive to confirm.

Critically, this is a **hang**, distinct from the separate weapon/projectile
scripted-reparent **crash** ([[reparent-verb-crashes-weapons]]): APlayerController's
constructor creates NO instanced subobjects (only AController's
`TransformComponent0`), so the PC family cannot hit the `ClassWithin` fatal assert
that the instanced weapon state-machines (`StateActive`/`FiringState%i`) trigger.
Different failure, different cause.

### The safe path (verified against findings)

1. **Never create or reparent a BP child of any custom PC.** No BP child is ever
   needed: 0 of 23,243 Content assets reference any PlayerController class; every
   NCP HUD/menu is native C++/Slate; input seeding uses the `UUTPlayerInput` CDO.
   C++-only is fully sufficient. (Corollary caveat: there is NO known-good BP-PC
   baseline in this project — stock UT wires all PCs in C++ — so "BP children of
   AUTPlayerController work fine" is unsupported here; don't rely on it.)
2. **Assign at `InitGame`, not only in the constructor.** The shipped hub modes
   are BP children of the C++ modes (SERVER-ADMINS.md: `gameMode` points at
   `ElimPlus_C`, `NCP-IGCTF_C`, `WipeoutPlus_C`, …; only NCShaftArenaGame launches
   natively; ANCPlusCTFGameMode is `Abstract` so a BP child is mandatory). A
   serialized BP CDO can stomp a constructor-set class — proven in-repo by
   `ClutchGameMode.cpp:255-258` re-asserting `HUDClass` in InitGame for exactly
   this reason. `InitGame` runs before every `Login`→`SpawnActor(
   PlayerControllerClass)` (GameModeBase.cpp:658), and all 7 NCP modes already
   override InitGame Super-first — 7 ready insertion points.
3. **Opt-in canary first**: a default-off URL option (Codex's `?NCPC=1`) in
   NCShaftArenaGame (the one natively-launched hub mode = lowest risk), then widen.
4. **Bump 327→328 before any networked server assigns it** (owning clients must
   resolve the replicated PC class) — free, since the bump is already owed (§Q8).
5. **Extend the bridge's fragile-class reparent gate to PlayerController chains.**
   The MapForge safety gate (MapForgeBridgeServer.cpp:2100-2101) currently refuses
   only `UTWeapon`/`UTProjectile` headless reparents; it would happily reparent a
   PC BP today. Adding PC classes closes the one door back to this hang.

### Choice of class (product decision, not a safety one)

Both ANPPlayerController and Codex's proposed fresh `ANCPlayerController` are
equally editor-safe (both trivial constructors). The difference is behavioral:
ANPPlayerController's `PlayerTick` rewrites the deferred fire queue (its whole
purpose — the low-debounce fix, NPPlayerController.cpp:16-94), which would alter
firing the moment it's assigned and collides with the frozen fire path +
`ncp.StopClearsPending`. Codex's "fresh neutral ANCPlayerController, leave ANP
dead" is the correct call. They can even coexist, mode-selected by URL option.

### Cheapest confirmation (if the PC track proceeds)

One debugger session discriminates all hypotheses at once: reproduce the hang on a
throwaway branch, attach, capture the game-thread callstack after 60s (a stack in
`FBlueprintCompileReinstancer`/`CompileBlueprint`/`GetDependentBlueprints` confirms
#1; `SpawnPlayerCameraManager` confirms #2), and simply waiting 10-15 min
distinguishes slow-from-infinite. Also: create a BP child of STOCK
AUTPlayerController in a scratch project — if that also hangs, the mechanism is
PC-family-generic, not ANP-specific (expected).

### Codex worktree note

The `private/327-aimcanary` worktree's untracked `NCAimCanary.cpp` is an unrelated
**server-side anti-cheat aim audit** (`#if UE_SERVER`, "evidence collector, not an
enforcement path"), NOT a PC-hang canary experiment. No PC-hang work has been
attempted anywhere yet — the Codex plan doc is design-only.
