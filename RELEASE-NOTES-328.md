# NetcodePlus 328 — Release Notes

Compared with **327-clutchwork**, the build currently deployed in production.

| | |
|---|---|
| **Build** | 328 (`NETCODE_PLUGIN_VERSION`) |
| **Engine target** | UT4 4.15, CL-3525360 — unchanged |
| **Audited source range** | `327-clutchwork..28d70df` |
| **Current audited head** | `28d70df` |
| **Audited code delta** | 84 commits, 138 files, +18,854 / −3,366 lines, excluding this release-note document |

328 contains the four fixes that were backported separately to the production branch. It is not a
strict functional superset: the obsolete `NCUTPlus` mutator and FireVal diagnostic paths were
deliberately removed.

> **Release-candidate status:** this document reflects a source audit at `28d70df`. It does not, by
> itself, certify the final DLL, Blueprint parents, replacement arrays, asset registry, or cooked pak
> contents. Complete the deployment checklist before publishing the build.

---

## Read this first — coordinated client and server update required

328 is a lockstep client/server release. It cannot be deployed as a server-only DLL swap.

Two changes make mixed 327/328 matches unsupported:

1. Five network RPC signatures changed — four Start/Stop fire RPCs plus
   `ServerProjectileHitClaim` — and `AuthoritativeFireEventIndex` is no longer replicated.
2. The slope-dodge fix runs in both client prediction and server movement replay.

### Version enforcement

- A client reporting a version other than 328 is disconnected immediately with a non-banning update
  message.
- In a gated NCP match instance, a client that sends no report is checked after
  `[NetcodePlus] VersionReportTimeoutSec` — 100 seconds by default — then receives a final five-second
  handshake grace.
- A client whose NCP pawn has sent movement is treated as functional and is not kicked because its
  version RPC was lost.
- Pure spectators and players currently pawnless because they are eliminated are deferred rather than
  kicked.
- A still-unconfirmed no-report client is disconnected without a ban after those safeguards and may
  install/update and immediately rejoin.
- Hub front ends use advisor mode: they whisper an update reminder instead of kicking. A joined NCP
  match instance still enforces 328.
- Bots and the listen-server host's local controller are exempt.

---

## Highlights

### For players

- Freshly sliding targets can be hit at their visually presented standing height.
- Rocket, minigun, retry, unpause, and weapon-switch firing failures were repaired.
- The Link Gun has a new server-authoritative NetcodePlus beam path.
- NCP Shaft Link has a dedicated authoritative native parent, pending final Blueprint migration and
  cook verification.
- Weapon skins are enabled, network-visible, and support multi-material weapons.
- Shock, Sniper, Lightning Gun, and Link support owner-local custom colors.
- Native hitsounds provide continuous damage-to-pitch feedback when their cue content is mounted.
- Five optional announcer voices can be selected from F5.
- Team modes gain an enabled-by-default concede vote with a per-ruleset opt-out.
- UTComp-style ready-up is available as an opt-in.
- The BSP slope-edge hard-stick is fixed.
- Wipeout no longer ends the match while a winning-kill replay is still active.

### For administrators

- Deploy matching client and server binaries together.
- Several gameplay defaults change immediately after upgrade.
- Link, Minigun, and Shaft functionality depends on final cooked Blueprint assets, not only the native
  DLL.
- Weapon skins and optional announcers require `/Game/NetcodePlusOptional/` content.
- Native built-in hitsound presets rely on the existing `dcSounds` content unless another fallback or
  custom sound pack is present.
- `Mods.db` now uses WAL journal mode.
- Run the release checklist near the end of this document before announcing 328.

---

## Default behavior changes

These changes are active unless explicitly disabled or replaced.

| Change | 328 default | Override or scope |
|---|---:|---|
| Concede vote | On | Add `?AllowConcede=0` to an individual ruleset. `false` and `no` also disable it. |
| Helmet headshot block | On | `ncp.HelmetBlocksHeadshot=0` restores stock behavior. |
| Slide validation grace | 250 ms | `ncp.SlideGraceMs=0` disables it. |
| CTF weighted spawn choice | On | `[UTPUGS_SPAWN] SpawnWeightedRandom=False`. Used only when `MaxPlayers > 4`. |
| CTF auto-pause resume countdown | 7 s | `[NetcodePlus] UnpauseCountdownSec`, clamped 0–60. |
| iCTF dropped-flag lift guard | On | `ncp.ICTFFlagLiftGuard=0` restores Epic's instant-return behavior. |
| Anchored match clock | On where `AUTWeaponFix` is present | `ncp.ClockSync=0`. Stock rulesets receive it only when an NCP weapon-fix actor exists. |
| Projectile accuracy correction | On | Shooting a projectile no longer counts as a weapon accuracy hit. |
| Wipeout sudden-death grace | 1 s | Previously 3 s. |
| Wipeout scoreboard | V/S and HEAL | Eff% was removed. |
| Armor-overlay draw distance | 6500 uu | Previously 5500 uu. |
| SQLite journal mode | WAL | Persists in the shared `Mods.db` file. |

CTF/iCTF rulesets with `MaxPlayers > 4` use the new overtime respawn ramp:

| Overtime elapsed | Respawn delay |
|---|---:|
| 0:00–5:59 | 2 s |
| 6:00 | 3 s |
| Each minute afterward | +1 s |
| 13:00 onward | 10 s cap |

Optional ready-up remains disabled unless `[NetcodePlus] bUsePlayerReadyUp=True`.

---

## Hit registration and weapon reliability

### Slide posture grace

A floor slide shrinks the authoritative capsule before the replicated pose has visually finished
transitioning. Shots through the still-visible torso could therefore miss on the server.

328 adds a bottom-aligned standing-capsule validation envelope for the first 250 ms of a slide. The
grace is used by four hitscan and three projectile validation paths.

- `ncp.SlideGraceMs=250` by default.
- `0` restores pre-328 server validation.
- This changes validation only; the live movement capsule remains unchanged.

The client claim generator still uses the raw slide capsule without this grace. Watch `[HitAttrib]`
and `[RenderGate]` diagnostics during dogfood testing for unclaimed slide hits.

### Firing fixes

- Rocket primary recovers a wedged charge state in roughly 0.25 seconds instead of waiting for the
  shared multi-second watchdog.
- The press that detects a rocket wedge now fires instead of being swallowed.
- Loaded rocket volleys are released through their firing state rather than silently discarded.
- Minigun primary no longer collapses to approximately one authoritative bullet every 0.2 seconds.
- Re-pressing minigun during spin-down no longer no-registers.
- Mouse chatter no longer leaves the weapon silent until another physical click.
- Deferred click retries land one or two frames sooner.
- Honest shots are no longer rejected for several seconds after unpausing while client and server
  clocks resynchronize.
- Retried shots preserve the client head offset, allowing legitimate retry headshots.
- Late or duplicate stop packets no longer cancel a newer held-fire sequence.
- Fire starts arriving during equip or non-transactional firing states are queued rather than dropped.

### Projectiles fired before switching weapons

`c8261e2` routes rocket, flak, and shard client impact handling through the weapon that fired the
projectile rather than the weapon held when it lands.

`64d5869` completes the authoritative side for rockets and flak shells: their resolved-projectile
rewind grace is returned to the firing weapon after a switch. Stinger shards use live-projectile
recovery rather than the same resolved-grace buffer.

### Accuracy correction

Detonating a shock core or destroying another projectile no longer increments the stock weapon hit
counter. Damage, combo awards, shot counts, and kill rewards are unchanged. Shock-heavy accuracy
percentages may therefore appear lower but are more accurate.

The Wipeout damage feed also clamps credited per-hit damage to 0–300. This prevents telefrag and
overkill sentinel values of approximately ±100,000 damage from moving the raw
`Kills + Damage / 100` PPR input by roughly ±1,000 in one round.

---

## Weapons

### NCP Link Gun

The continuous secondary beam uses a server-authoritative NCP firing state.

- The owning client traces the cosmetic endpoint.
- The server performs damage tracing and accumulation.
- Remote players and spectators consume the replicated stock effect endpoint.
- Default one-way beam rewind is capped at 40 ms.
- C++ hard-clamps any cooked Blueprint value to 50 ms.
- Client hit claims are not accepted for continuous beam damage.
- Primary plasma retains Epic's predicted-projectile cadence and is intentionally non-transactional.
- Link pull rejects the owner, dead or torn-off targets, and teammates, with a final validation
  immediately before damage and momentum.
- The beam watchdog exists only while a beam effect exists.
- Canvas screen refresh is capped at 30 Hz and steady-state particle writes are change-gated.

The native class alone does not deploy this weapon. The final cook must contain the reparented Link
Blueprint and replacement-array entries.

### NCP Minigun

The native NCP minigun path already existed on the 327 line. The 328 content change is the reparented
Minigun Blueprint and replacement-array wiring that makes it the deployed weapon.

The primary-fire watchdog fix is new and prevents the server stream from being stopped by an
unstamped sentinel timestamp.

### NCP Shaft Link

`AUTWeap_LinkGun_Shaft_NCP` is a new authoritative parent for `UTNPShaftLink`.

- Both inputs use `UUTWeaponStateFiringLinkBeam_NCP`.
- Primary is converted from plasma to the Blueprint-authored mode-1 Shaft beam.
- Both inputs canonicalize to the mode-1 cosmetic path.
- Link pull and primary overheat are disabled.
- Serialized legacy firing-state objects are repaired after Blueprint defaults load.
- Mode-1 damage, cadence, and ammo settings are copied to mode 0.
- `ProjClass[0]` is cleared so neither input can emit plasma.
- Beam shots use `LinkBeamShots`, one sample per refire interval.

This path is inactive until the final `UTNPShaftLink` Blueprint is reparented, compiled, saved, and
recooked. See the deployment checklist and the Shaft accuracy known issue below.

### Weapon skins

Weapon skins are enabled and replicated.

- Open `F5 → Home → Weapon Skins`, or use `weaponskins`.
- Selections are validated by the server.
- Other players, spectators, respawns, re-equips, and dropped weapons see the selected skin.
- Flak and Lightning Gun use corrected first- and third-person material slots.
- Skin application supports up to 32 material slots.
- Invisible skins can cover all weapon sections.
- Weapon render scale is preserved for unskinned weapons.
- Missing optional skins are skipped without disabling the catalog.

The server and client must both mount `/Game/NetcodePlusOptional/`. The required catalog contains 20
assets; if any required entry is missing, the catalog remains unavailable. Optional entries are
currently `InvisibleIGRifle`, `PinkLG`, and `RocketPink`.

### Owner-local weapon colors

The Weapon Skins panel supports:

- Shock beam/core color;
- shared Sniper and Lightning Gun hitscan color;
- Link beam color.

These colors are intentionally local to the weapon owner's first-person view. Remote players,
spectators, and replay playback see stock colors. Saved settings publish a new local color generation,
so active effects can refresh without another weapon grant.

The final cooked Blueprints must call the matching native bridges from their effect graphs. Verify
Shock weapon/core/combo, Sniper, Lightning Gun, and Link wiring in the final cook.

### Other weapon changes

- The iCTF beam renders immediately on the prediction frame.
- High-ping iCTF receives a second local additive beam layer, restoring intended thickness.
- The “Show Own Beam” toggle now applies after the next respawn or weapon pickup rather than the next
  shot.
- Enforcer padding requires line of sight to the real capsule.
- Team-projectile blocking follows stock behavior where enabled.
- Projectile-ignore retries are bounded.
- Swept trace impact points are placed on the target surface.
- Shock ammo-glow material values are cached and written only when ammo changes.
- Shock LCD updates remain capped at 30 Hz while recently rendered.

---

## Game modes

### Concede vote

Concede is enabled by default in these six mode families:

- NCPlusCTF, including iCTF;
- Wipeout;
- ElimPlus;
- NCLeagueDuel;
- NCShaftArena;
- ShockDom.

It is not enabled in Clutch.

- Console `gg` opens or joins a vote.
- F1 confirms an existing vote.
- F4 withdraws.
- Chat text is not parsed as a vote.
- Only the strictly losing team may concede.
- Required votes are `Humans / 2 + 1`; bots do not count.
- A tied match cannot be conceded.
- A lead change voids the vote.
- Departed players and team switchers are pruned.
- A successful vote uses the normal `EndGame(..., "Concede")` path so ratings, stats, and lineups
  complete normally.

Disable concede per ruleset with:

```text
?AllowConcede=0
```

The spellings `false` and `no` are also accepted, case-insensitively. Missing or unrecognized values
leave concede enabled.

### Optional player ready-up

Set:

```ini
[NetcodePlus]
bUsePlayerReadyUp=True
```

This replaces host-controlled start with an active-player ready check.

- Ready and Not Ready controls appear in F5.
- Scoreboards show READY / NOT READY.
- Bots and spectators do not block start.
- Changing teams clears readiness.
- Once every active human is ready, the normal `StartDelay` begins and readiness locks.
- Only an entirely empty server cancels the locked countdown.

### CTF and iCTF

- Weighted spawn selection replaces deterministic best-start selection when `MaxPlayers > 4`.
- Rulesets with `MaxPlayers <= 4` use Epic's normal picker.
- Anti-repeat memory increases from two starts to three.
- A nearby live enemy is the highest-priority avoidance criterion, not an absolute prohibition. If
  every start violates the radius, selection continues using the remaining safety tiers.
- Enemies sufficiently below a start count as floor-separated only when line of sight also fails.
- Invalid weighted settings are corrected on load. Base is nonnegative; non-finite values fall back to
  defaults; weighted mode always uses a positive spread.
- Auto-pause has an authoritative HUD banner and a per-second resume countdown.
- Another tracked disconnect cancels a pending auto-resume.
- A pause survives an empty server and is restored when the first participant reconnects.
- The flag-taken alarm now plays on re-grabs, with one-second per-team deduplication.
- iCTF dropped flags are relocated safely around lifts instead of immediately returned.
- Spectator and caster joins no longer run a synchronous rating database query on the game thread.

### Wipeout

- Sudden-death respawn grace is reduced from three seconds to one.
- A permanently dead player sees `X` instead of an unreachable countdown.
- “FINAL LIFE!” warns when the next death cannot respawn before sudden death.
- Eff% is removed from the scoreboard; V/S and HEAL are added.
- Thigh pads remain available.
- Vest-to-Shield-Belt substitution is rebuilt so it works on live maps.
- Siphon prefers a map-authored non-Amp powerup location, then the sniper base farthest from the nearest
  Amp when it clears `SiphonMinAmpDistance`.
- Siphon and Belt appear roughly one second into warmup so players can scout the selected location.
- Damage credit is clamped to 0–300 per hit.
- Buff banners use the authoritative server transform.

### Shared match flow

- Candy orbs no longer block lift movement and cannot be shot.
- Warmup roam and `mutate host` extend to Wipeout and ElimPlus.
- Helmet Small Armor blocks one headshot when enabled. Wipeout remains unaffected because it strips
  Helmet pickups.
- Clock anchoring keeps pickup timers and displayed time aligned in worlds where an `AUTWeaponFix`
  instance creates the clock actor.

### Wipeout post-match replay crash

Wipeout could call `EndGame` while a winning-kill replay still held references to the winning pawn,
causing an intermittent post-match `EXCEPTION_ACCESS_VIOLATION`.

328 now waits seven seconds when a valid replay will play and uses a 0.5-second finish delay when no
replay is available. This brings Wipeout in line with the guarded behavior already active in ElimPlus.

---

## Client experience

### Native hitsounds

Open `F5 → Hitsounds`, `ncpmenu hitsounds`, or `mutate hitsounds`.

Enemy and teammate channels each support sound selection, volume, pitch, Absolute/UTComp/Flat pitch
models, optional zero-damage teammate cues, and live test buttons.

Behavioral improvements include:

- continuous damage-to-pitch feedback;
- no enemy cue for self-damage;
- flak pellets coalesced into one summed-damage sound;
- amplified damage reflected in pitch;
- full spectator hit streams;
- posthumous trade-kill confirmation;
- no false high-damage teammate prediction on no-friendly-fire servers;
- authoritative correction when prediction resolves to a different audible tier.

An occasional correction double-blip remains possible, especially around flak.

The native code does not contain the stock preset audio. Built-in presets load primarily from
`/Game/Blueprints/Netcode/Hitsounds/dcSounds/`; `/NetcodePlus/Hitsounds` fallback assets or a custom
`UHitsoundPack` can also provide cues. Without any of those sources, the client has no playable
hitsound preset. Dedicated servers do not load sound assets.

### Announcer packs

Optional voices are UT2004 Male, UT2004 Female, UT2003, Classic UT, and Sexy.

The selector appears on `F5 → Home` only when optional announcer content is mounted. Without it,
Stock UT4 remains active.

- Applying a pack does not cut off the announcement currently playing.
- Stacked multikill and spree rewards are preserved.
- Flag-taken announcements are protected from unrelated queue cancellation.
- ElimPlus and Wipeout controllers are repaired client-side when a foreign announcer replaces the
  selected pack.

### HUD, scoreboard, and Force Models

- Bright character materials retain their authored HDR compensation.
- TacCom/X-ray outlines survive model swaps.
- The process-wide orphan CustomDepth cleanup remains active at one hertz, including while Force Models
  outline mode is disabled.
- Armor and shield overlays draw out to 6500 uu.
- Armor tint follows viewer team changes.
- HUD and scoreboard actor walks, sorting, string formatting, and texture decoding are cached.

Expected bounded cache delays:

| Item | Maximum expected delay |
|---|---:|
| CTF instagib warmup column layout | 0.5 s |
| Spectator weapon list after pickup | 0.1 s |
| Heal keybind label after live rebind | 1 s |
| Fresh-join stats columns | Approximately 1 s |

Force Models outline remains config-only and is not exposed in F5.

---

## Movement

The slope-dodge hard-stick on BSP seams and convex slope edges is repaired.

The fix removes only the horizontal component that points back into the supporting slope after Epic's
vertical adjustment.

- Vertical movement remains stock.
- Cross-slope movement is preserved.
- Uphill caps and steep-descent branches remain stock.
- Walking movement is untouched.
- A near-vertical-normal fallback closes the degenerate-normal gap.
- The fix applies to `ATeamArenaCharacter` modes, including ElimPlus, NCPlusCTF, ShockDom, and Wipeout.

---

## Performance and frame-time stability

At 480 fps, one per-pawn-per-frame operation runs 480 times per second for each pawn, or 4,800 times
per second across ten pawns.

- **Armor overlays:** tint writes move from per-pawn/per-frame to dirty-state updates. With ten visible
  pawns at 480 fps, eliminating two steady-state MID writes per pawn removes roughly 9,600 material
  writes per second.
- **Overlay rendering:** armor/shield overlays are hidden when the owning mesh has not rendered recently
  or is farther than 6500 units from the local camera.
- **Outline recovery:** the process-wide orphan CustomDepth sweep is capped at 1 Hz. It remains active
  independently of the hidden Force Models outline option because stock TacCom can create the same
  leaked components.
- **Link and Shock displays:** visible render targets update at no more than 30 Hz rather than once per
  rendered frame.
- **Shock ammo glow:** scalar parameters are written only when ammo, max ammo, or the live material
  instance changes.
- **QuickStats:** radial HP/armor geometry is cached. Steady state removes up to 704 `Sin` plus 704
  `Cos` calls — 1,408 trig calls total — and associated per-frame allocation.
- **Scoreboards:** ElimPlus, CTF, Wipeout, Clutch, and ShockDom cache row data, strings, measurements,
  sorts, and repeated actor lookups until inputs change.
- **CTF HUD:** absence of the overtime-info actor is cached and re-probed once per second rather than
  scanned every frame.
- **ElimPlus HUD:** pawn/vitals resolution is consolidated into one per-frame snapshot.
- **F5 menu:** tab-specific asset discovery is deferred until that tab is opened.

These changes primarily reduce game-thread, Slate, and render-thread churn and should improve frame-time
consistency in HUD-heavy or many-player scenes. They do not replace an engine-level renderer upgrade.

---

## Security

### Launcher exchange code

The launcher no longer places the one-shot Epic exchange code in the process command line before engine
logging and crash-report snapshots.

328 reads `NCP_AUTH_PASSWORD` from the environment, validates it as a bare token, appends it only to the
live command line, and clears the environment value. This protection requires the matching launcher
release.

### Mid-round free camera

A player joining or reconnecting during a live ElimPlus round is held inactive and assigned a living
teammate as view target. A one-hertz server sweep keeps the restriction in place for `bOutOfLives`
joiners.

The pre-existing stock case where an already-dead player can invoke `ServerViewSelf` remains open.

### Debug head spheres

`ncp.DebugHeads` is now cheat-gated. Default remains 0.

---

## Logging

No high-volume diagnostic cvars are enabled by default. Several rare failure warnings were added.

| Cvar | 328 default |
|---|---:|
| `ncp.HitAttribDebug` | 0 |
| `ncp.RocketPrimaryDiag` | 0 |
| `ncp.AnnouncerTrace` | 0 |

Spawn and respawn flow diagnostics were demoted to `Verbose`. `[RenderGate] DEMOTED` is also
`Verbose`; parsers expecting it at normal `Log` verbosity must opt in.

Useful live commands:

```text
Log LogUTWeaponFix Verbose
Log LogNCLeagueDuel Verbose
```

Rare warning categories to monitor during rollout include `[FireBlock]`, `[WedgeArmed]`,
`[StateLayout]`, `[TimeDesync]`, and rejected server stop-fire sequence jumps.

---

## Removed or relocated

| Change | Upgrade impact |
|---|---|
| `NetcodePlus.NCUTPlus` removed | Remove it from mutator URLs and Blueprint parents. Weapon replacement is handled by `NCWepMut` / `NCStockWeapons`. |
| `ncp.FireValDump` removed | Command is no longer recognized. |
| `ncp.FireValReplayCsv` removed | Command is no longer recognized. |
| 16 catalog skins removed | Saved selections fall back to Default. |
| Updater backups relocated | Backups now live in a `PluginBackups/` sibling of `Plugins/`, preventing UE4 from loading an old duplicate plugin first. |

Removed skin names:

`BlackDeath`, `FlakDefault`, `LinkBee`, `LinkBeeElim`, `LinkFreedom`, `LinkMint`, `Rocket99`,
`Rocket99Elim`, `RocketBee`, `RocketBurnElim`, `RocketMahogany`, `RocketTiger`,
`ShockBlueBirdElim`, `ShockFreedom`, `SniperRedBird`, `SniperSport`.

---

## Configuration reference

### New or newly active cvars

| Cvar | Default | Effect |
|---|---:|---|
| `ncp.SlideGraceMs` | `250.0` | Standing-capsule validation grace after slide start. |
| `ncp.HelmetBlocksHeadshot` | `1` | Small Armor blocks one headshot. |
| `ncp.ClockSync` | `1` | Enables anchored clock behavior where the clock actor is present. |
| `ncp.ICTFFlagLiftGuard` | `1` | Protects dropped flags around lifts. |
| `ncp.HitsoundCorrection` | `1` | Allows an authoritative hitsound through dedup when its audible tier differs. |
| `ncp.FlagTakenGuarantee` | `1` | Protects flag-taken lines from unrelated cancellation. |
| `ncp.SkinTiming` | `0` | Weapon-skin diagnostics. |
| `ncp.AnnouncerTrace` | `0` | Client announcer diagnostics. |
| `ncp.RocketPrimaryDiag` | `0` | Rocket firing diagnostics. `1` traces M1/primary-fire lifecycle; `2` adds fake-delay, charged-state, and other-mode detail. |
| `ncp.ClickBufferMs` | `0.0` | Experimental click buffering. Keep at `0`; delayed shots do not yet preserve an aim snapshot. |
| `ncp.FlakShellPairDebug` | `0` | Flak fake/authority pairing diagnostics. |
| `ncp.FlakShellMatchFakeMaxDist` | `1250.0` | Maximum candidate separation. |
| `ncp.FlakShellMatchFakeMaxPhase` | `0.60` | Maximum inferred ballistic phase separation. |
| `ncp.FlakShellMatchFakeMinHorizDot` | `0.98` | Minimum horizontal velocity-direction dot. |
| `ncp.FlakShellMatchFakeMaxPosError` | `256.0` | Maximum phase-compensated position residual. |
| `ncp.FlakShellMatchFakeMaxVelError` | `200.0` | Maximum velocity residual. |

Current hit-attribution defaults remain:

| Cvar | Default |
|---|---:|
| `ncp.HitAttribDebug` | `0` |
| `ncp.UnclaimedRenderGate` | `1` |
| `ncp.UnclaimedRenderSlack` | `20` |
| `ncp.HitAttribRenderExtraMs` | `30` |

`ncp.UnclaimedRenderGate` is already deployed on 327 through its production backport; it is not a new
328 behavior.

### `[UTPUGS_SPAWN]`

These keys affect NCPlusCTF/iCTF rulesets with `MaxPlayers > 4`.

| Key | Default | Meaning |
|---|---:|---|
| `SpawnWeightedRandom` | `true` | Enables weighted selection. |
| `SpawnRandomBase` | `20` | Best-start draw ceiling. Negative finite values clamp to 0; non-finite values fall back to 20. |
| `SpawnRandomSpread` | `1.0` | Ceiling lost per score point. Non-finite values fall back to 1; weighted mode forces a non-positive value to 1. |
| `SpawnEnemyHardRadius` | `1200` | Highest-priority nearby-enemy avoidance radius; not an absolute no-spawn guarantee. |
| `SpawnEnemyBelowZ` | `190` | Vertical separation used by the below-floor test. |
| `LogSpawnChoices` | `false` | Logs each live spawn selection. |

`SpawnRandomBase=0` is valid and deliberately drops weighted selection into the deterministic
best-score fallback.

### `[NetcodePlus]`

| Key | Default | Meaning |
|---|---:|---|
| `bUsePlayerReadyUp` | `False` | Enables player ready-up. |
| `VersionReportTimeoutSec` | `100.0` | Version-report deadline. Positive values clamp to 1–120 seconds; missing, garbage, or non-positive values fall back to 100. |
| `UnpauseCountdownSec` | `7` | Host/rcon and CTF auto-pause resume countdown, clamped 0–60. |
| `SiphonMinAmpDistance` | `3000` | Minimum preferred Siphon-to-Amp distance for sniper-base fallback. `0` removes only the distance requirement; it does not disable Siphon. |

### Ruleset option

| Option | Default | Meaning |
|---|---:|---|
| `?AllowConcede` | `1` | Set to `0`, `false`, or `no` to disable concede for that ruleset. |

### Per-player settings

- `[WeaponSkinsPlus] LinkBeam=(R=...,G=...,B=...,A=...)`
- `[NetcodePlus] AnnouncerPack=...`
- Native hitsound choices, volume, and pitch remain client-local.

### New mutator token

`ClientHitsounds` enables the native hitsound implementation.

Do not run it alongside `dcHitsounds`; both occupy the same mutator group and would otherwise duplicate
sounds.

---

## Content and deployment requirements

### Required content

| Feature | Required content |
|---|---|
| Weapon skins and local-color selector | `/Game/NetcodePlusOptional/` on client and server |
| Optional announcers | `/Game/NetcodePlusOptional/Announcers/UT2004/` on clients |
| Native built-in hitsound presets | `/Game/Blueprints/Netcode/Hitsounds/dcSounds/` on clients, unless fallback/custom packs are supplied |
| NCP Link | Reparented Link Blueprint plus replacement-array entries |
| NCP Minigun | Reparented Minigun Blueprint, correct firing-state slots, and replacement-array entries |
| NCP Shaft | `UTNPShaftLink` reparented to `AUTWeap_LinkGun_Shaft_NCP` |

A server without the weapon-skin catalog rejects every selection even if clients have the assets.

### Final Blueprint/cook checklist

- [ ] Build the final 328 native module.
- [ ] Restart the editor against that exact binary and verify no duplicate staging plugin is loaded.
- [ ] Compile and save the Link Blueprint.
- [ ] Verify Link mode 0 is stock plasma and mode 1 uses `UUTWeaponStateFiringLinkBeam_NCP`.
- [ ] Verify Link replacement entries in both live and warmup swap arrays.
- [ ] Verify Link impact-effect slots, muzzle attachment, both LCD materials, watchdog, and color bridge.
- [ ] Compile and save the Minigun Blueprint.
- [ ] Verify its intended primary and alternate firing-state classes and swap entries.
- [ ] Reparent, compile, and save `UTNPShaftLink` to `AUTWeap_LinkGun_Shaft_NCP`.
- [ ] Verify both Shaft firing-state slots are `UUTWeaponStateFiringLinkBeam_NCP`.
- [ ] Verify both Shaft inputs produce the mode-1 beam and no plasma projectile.
- [ ] Verify `[StateLayout] UTNPShaftLink_C` reports NCP beam states in both slots.
- [ ] Verify Shock weapon/core/combo, Sniper, Lightning Gun, and Link color bridge calls.
- [ ] Verify all 20 required weapon-skin assets load on client and dedicated server.
- [ ] Verify optional `InvisibleIGRifle`, `PinkLG`, and `RocketPink` degrade independently if absent.
- [ ] Verify the five announcer classes load.
- [ ] Verify all intended hitsound choices resolve from the shipped cue content.
- [ ] Recook every gameplay or mutator pak that owns or references these assets.
- [ ] Inspect final pak asset registries rather than relying on editor visibility.
- [ ] Test a clean 327 client against a 328 instance and confirm the mismatch message.
- [ ] Test a plugin-less client through the timeout and non-banning kick.
- [ ] Test listen server, dedicated server, spectator, replay, and standalone paths.
- [ ] Run a final full clean build and startup smoke test.

---

## Known issues and release gates

1. **Shaft accuracy bookkeeping must be reconciled before enabling the migrated Shaft weapon.** The NCP
   beam writes `LinkBeamShots`, and the Shaft HUD/scoreboard replicators read it, but
   `NCShaftArenaGame::ComputeLinkAccuracyPct()` and `BuildMatchSummary()` still read `LinkShots`.
   Rating and uploaded summaries can therefore use the wrong denominator.
2. **NCP Link screen restoration after an invisible skin needs validation or repair.** Its screen MIDs
   are created after the base class captures original materials, while it has no
   `SetupSpecialMaterials()` override. Returning from an all-slot invisible skin can restore stale
   display materials until the weapon is reattached.
3. **Shock's former slot-3 low-ammo warning is not covered by the native ammo-glow cache.** Native code
   updates the slot-2 ammo/max-ammo parameters. With the old Blueprint tick disconnected, the separate
   low-ammo material transition is absent.
4. **The Blueprint cook remains load-bearing.** Native Link, Minigun, Shaft, and local-color code does
   not prove that the shipped paks contain the required parents, state slots, swap arrays, and graph
   calls.
5. Keep `ncp.ClickBufferMs=0`; buffered shots do not store an aim snapshot and can use a later view
   rotation.
6. `ncp.GhostFix` remains known-broken and should remain `0`.
7. Force Models outline remains Mod.ini-only and is absent from F5.
8. Authoritative hitsound correction can occasionally produce a double-blip when prediction selected a
   different audible tier.
9. The stuck-cursor/ghost-menu issue remains; the console-key workaround is unchanged.
10. Final DLL and cooked-content verification remain release gates. Source review alone does not prove
    the shipped paks.

---

## Documentation follow-ups

The 328 admin guide now documents `AllowConcede` and correctly lists the hit-attribution defaults.
Remaining documentation work includes:

- add `/Game/NetcodePlusOptional/` to the server/client pak table;
- remove contradictory language saying native hitsounds are only a future path;
- clarify that hitsound code is native but built-in cue content is external;
- add `bUsePlayerReadyUp` to the server configuration table;
- add `SiphonMinAmpDistance`;
- remove the nonexistent announcer “legacy immediacy” toggle;
- document `LinkBeam` and current local-color behavior;
- replace the obsolete “Flak + LG only” skin-slot description;
- correct README claims that the no-reply version kick is disabled, RenderGate is new in 328, and
  `UnclaimedRenderSlack` defaults to 40;
- document `ncp.ClickBufferMs`, `ncp.RocketPrimaryDiag`, `ncp.MouseDebounceCap`, and the flak-shell
  pairing cvars.

---

## Not shipping in 328

The aim-assist documents are discussion proposals only. No aim-assist gameplay code is present in
`Source/`.

---

## Audited code commit list through `28d70df`

<details>
<summary>84 commits, oldest first</summary>

```text
578e600  release: initialize 328 release candidate
d9d2305  feat(concede): gg vote — losing team forfeits at >50% of its humans (zo BP port)
0a58fdd  feat(concede): tell the leading team a concede vote is brewing
f159046  fix(movement): keep slope-dodge slide parallel (BSP slope-edge stick)
527c4fa  refactor(movement): drop redundant guard around slope-dodge rescale
d2834f6  fix(movement): close near-vertical-normal hole in slope-dodge slide fix
eb1f953  docs(aim-assist): add community discussion proposal
a0d29ac  refactor(net): remove unused native NCUTPlus mutator
aef3700  refactor(net): freeze fire RPC payloads
8419fb5  feat: add network-visible preloaded weapon skins
a823f4b  fix: target correct material slots for Flak and Lightning Gun skins
abc30c5  fix(rocket): fast wedge recovery — first click fires, volleys never eaten
e9a977b  feat(hitreg): per-shot hit-attribution telemetry behind ncp.HitAttribDebug
1887220  tune(hitreg): HitAttrib default ON for 328-RC dogfood + client claim line
fc62561  feat(hitreg): unclaimed-hit render check — server reconstructs the claim
617106a  tune(hitreg): UnclaimedRenderGate defaults to enforce
a90bad4  fix(hitreg): render-gate audit fixes — resurrection, beam flood, posture
b67365b  feat: two-tier weapon-skin manifest — optional entries degrade gracefully
2548c3d  feat(hitsounds): native hitsounds end to end — catalog, pitch models, Slate tab
bba7e6b  fix(hitsounds): throttle the mutate-hitsounds menu request per player
7067c8e  fix(hitsounds): adversarial-review fixes — 4.15 API, dedup, parity, menu
b0059e3  docs: refresh README + SERVER-ADMINS to 327 / seq-42 reality
b424726  fix(tools): hub updater backups move OUT of Plugins/ (UE4 plugin-scan shadow)
098f238  docs(server-admins): add NCStockWeapons (stock weapon balance) to Start Here
1f7e345  fix(client): preserve xray outlines across model swaps
c89782b  wip(hud/weapons): in-progress ElimPlus HUD + weapon fix changes
9721d07  feat(security): take the launcher login code off the command line
1429b72  docs(aim-assist): add the aim-assist planning set
d1ce2b3  fix(security): deny the free-roam camera to mid-round joiners
4ffac3f  feat(announcer): add legacy immediacy and optional packs
2c9f215  fix(wipeout): anchor buff banner to server transform
05c4cb3  feat(wipeout): X unreachable respawns in OT; swap Eff for Heal + Vest/Siphon
2d97f66  fix(build): qualify slide-posture call, keep inherited Eff fields, 4.15 trim
b6aaa0a  docs: record the stock-gamemode menu-cursor limitation
0dfe5d5  docs: bring README + SERVER-ADMINS up to 328
3ba06da  docs: consolidate the two stuck-cursor cases into a player-support section
6036909  fix: stabilize gameplay prediction and announcers
1ba73aa  fix(instagib): detect IGPlusRifle for immediate beam
52e9d7c  fix: preserve bright forced-model materials
c28f334  feat: add server-authoritative NCP link gun
62f1057  fix: stabilize native weapon integration
39aabb1  fix: stop tick watchdog killing minigun primary fire stream
a823721  fix(wipeout): ScoreDamage credited negative / +100k damage around telefrags
b515d7c  tune(logging): demote spawn/respawn flow logs to Verbose; cheat-gate ncp.DebugHeads
64f3006  feat(wipeout): Siphon spawns at the weapon base farthest from Amp, live in warmup
d1963e4  fix: harden release-candidate gameplay systems
34e2c99  feat(clocksync): anchored match-clock sync beacon (ncp.ClockSync)
92abf2e  fix(wipeout): rebuild vest->belt substitution on the Siphon replace pattern; keep thigh pads
9b7c1e9  fix(candy): lifts can no longer jam on PreventDeath orbs; orbs no longer shootable
1892301  feat(warmup): iCTF warmup roam in Wipeout and ElimPlus
d2650a9  fix(build): own header first in NCPClockSync + NCPCandyLiftGuard
41ffbbd  feat(helmet): ncp.HelmetBlocksHeadshot ships on by default
d63e05f  fix(ctfrating): spectator joins stop touching the rating DB; WAL journal
81182ed  feat(ready-up): add UTComp-style player start flow
7ef1814  feat(spawns): IG+ weighted-random pick, 3-deep memory, vertical awareness
8d3cb3f  perf(ictf): cache the per-shot instagib beam checks
0a62bfb  fix(rewind): slide posture applied in all three projectile hit tests
8c30e41  perf(hud): cache scoreboard data and render resources
7521123  fix(stats): detonating projectiles no longer counts as an accuracy hit
282f769  feat(ctf): make auto pause authoritative
bd82c2e  feat(ctf): overtime respawn ramp - hold 6 min, then +1s per minute to 10s
d014d9b  feat(wipeout): siphon takes over authored powerup spots
911535c  fix: clear the 328 pre-cut audit blockers
b43e4f8  feat(ictf): dropped-flag lift guard — swept relocation instead of Epic's instant return
8389067  feat(weapons): add native local color pipeline
bafda02  fix(menu): correct iCTF tab Slate closure
764dc83  fix(shock): restore LCD and local color behavior
e215bcf  perf(shock): cache ammo glow updates
7fa1551  Fix optional weapon skin catalog
7ede8fb  tune(wipeout): sudden-death respawn grace 3s -> 1s
adb3f85  Expand optional weapon skin whitelist
47c62f4  fix(ctf): validate weighted spawn config
ac7740a  fix: keep candy pickups from blocking lifts
63cbd30  fix: preserve stacked announcer rewards
7459874  Optimize outline, armor overlays, and Link effects
ab23a48  Preserve TacCom outline leak cleanup
c8261e2  Weapon skin multi-slot support; projectile hitsound/claim routing fixes
7937805  Recognize NCP invisibility weapon material
ebe7158  Add RocketPink weapon skin to manifest
64d5869  fix(projectiles): preserve rewind grace after weapon switch
eb11bd2  Add authoritative NCP Shaft Link parent
c931794  feat(concede): per-ruleset AllowConcede opt-out
38a0277  fix(replay): stop firing EndGame mid-replay (post-game crash)
28d70df  docs(server-admins): document AllowConcede; correct hitreg cvar defaults
```

</details>

---

## Build-verification status

At least one full build occurred earlier in the range and exposed compile failures that were subsequently
fixed. No final clean build of audited head `28d70df` was performed as part of this release-note revision.
Final DLL and cooked-content verification remain required.
