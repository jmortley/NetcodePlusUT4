# NetcodePlus

A UT4 plugin focused on improving netcode feel and adding new game modes for competitive Unreal Tournament 4.

Built on UE4 4.15 (UT4 fork, CL-3525360). Targets up to 720Hz on modern hardware.

## What it does

NetcodePlus replaces stock UT4's hit registration and projectile prediction with a hybrid lag-compensation system inspired by UTComp's NewNet, with added performance optimizations for high-refresh-rate displays. It also ships several new game modes and quality-of-life improvements built on top of that netcode layer.

## Features

### Netcode

- **Server-side capsule rewind for hit validation** — every shot is validated against rewound target capsules at the time of fire, scaled by ping. Players at 100ms ping get the same shot effectiveness as players at 20ms.
- **Tiered projectile rewind compensation** — full half-RTT rewind below 100ms ping, linear scale-down to 50% by 150ms, hard cliff above. Prevents extreme-ping shooters from getting unfair compensation while keeping mid-ping play fair.
- **Split prediction system** — visual prediction (0ms) is decoupled from hit validation (~120ms ping-based), so players see enemies in real-time while shots are validated against rewound positions.
- **Bidirectional time-search** — when primary rewind misses on a client-claimed target, server searches ±30ms in 15ms steps to catch hits within the network jitter window.
- **HitScan padding for moving targets** — claimed targets get +40 units of capsule padding at fire time (10 units stationary), compensating for tight UT4 hitboxes vs. visible mesh silhouette.
- **Transactional fire events** — every shot has a unique event index for client/server agreement; resend queue protects against unreliable RPC loss.
- **Strict tap-fire enforcement** — tap-mashing the fire button can no longer fire faster than holding it. Click queueing via retry timer means responsiveness is preserved.
- **Trade-kill grace period** (200ms) — fire RPCs in flight when the shooter dies still register, so reciprocal kills count.
- **Client-side hit detection (CSHD) for link gun beam** — predictive damage with server sanity-trace validation. Less laggy at high ping without giving the client total authority.
- **Quake-style LG accuracy** — stock UT4 ticks `NAME_LinkShots` once per trigger pull, so sustained-beam Hits/Shots ratios explode past 1000%. NetcodePlus adds `NAME_LinkBeamShots` (one per refire interval, server-only) and rate-matches the hit increment via a per-interval flag (`bHitDuringCurrentRefireInterval`). Result: Hits ≤ Shots is mathematically guaranteed, scoreboards / HUD widgets show meaningful 30-70% accuracy in normal play instead of always 100%.

### Performance / High-FPS Support

- **Frame-rate-independent overlay throttle** — 60Hz time-based dirty-marking on character overlay meshes regardless of render rate (works at 480, 720, uncapped).
- **Overlay visibility cull** — armor / UDamage / spawn-protection overlays skip rendering when target is off-screen, occluded, or beyond ~55m. Significant FPS gain in scrum-heavy modes (Wipeout).
- **Per-team collision throttle** — 32/sec instead of every-tick for team collision iteration.
- **Spawn-protection material loop bypass** — cleared per-tick state-transition flag instead of every-frame BodyMI loop (~27K calls/sec saved at 480fps).
- **Shock ball drift correction** — high-FPS floating-point velocity drift snapped back to original fire direction per-tick on zero-gravity projectiles.
- **Spectator rotation smoothing** — separate interp speed for remote pawns to prevent jitter at high refresh rates.
- **Configurable projectile tick rate** — `ut.ProjectileTickRate` console var, 120-720 Hz range.
- **Adjusted character movement smoothing values** — tuned for 480-720fps display rates.

### NetcodePlus Weapons

All weapons inherit lag-compensated hit detection by default. Subclasses provide weapon-specific behavior on top:

| Class | Purpose |
|-------|---------|
| `AUTPlusShockRifle` | Shock primary/alt with combo support, screen-texture ammo display |
| `AUTPlusSniper` | Sniper with rewound headshot detection + Impressive streak tracking |
| `AUTPlusFlakCannon` | Flak primary (shards) and alt (shell) with rewind validation |
| `AUTPlusWeap_RocketLauncher` | RL with charged-state transactional fire + force-fire-on-death |
| `AUTWeap_LinkGun_Plus` | Link gun with CSHD beam damage + reliable yoink |
| `AUTWeap_Minigun_Plus` | Minigun with rewind via virtual dispatch |
| `AUTWeap_Enforcer_Plus` | Enforcer with body-shot rewind, preserves dual-wield |
| `AUTPlusProj_Rocket` | Rocket projectile with rewind hit validation |
| `AUTPlusProj_FlakShell` | Flak shell projectile |
| `AUTPlusProj_ShockBall` | Shock ball with stuck-detection, drift correction, fake/real convergence |

### Game Modes

- **NCPlusCTF** — Capture the Flag with adopted NewCTF advantage/OT mechanics, instant-end on flag-home, 5-min OT cap, ping-compensated spawning. Used for both regular CTF and iCTF (instagib).
  - HUD scorebar shows count-up overtime clock with "Overtime" label (replicated via `ANCPlusCTFOTInfo`); spectator list deduplicates the engine's stock count text.
  - **Size-keyed respawn** — `MaxPlayers <= CTFSmallGameMaxPlayers` (default 2 = 1v1 / w00t) uses `CTFRespawnWaitSmall` (1.0s); larger uses `CTFRespawnWait` (1.5s). Works for bot-hosted PUGs and hub rulesets without anyone needing `?RespawnWait`.
  - **`mutate warmup`** — warmup-only roam mode (`AWarmupRoamMutator`, auto-added). Toggles invulnerability + disables firing on the calling player so they can learn the map without dying or fragging others. Re-asserted on respawn; stripped from everyone the instant the match leaves `WaitingToStart`, so it can never carry into live play.
  - **Flag-status banners** (`UNCPlusHUDWidget_CTFFlagStatus`) — full nchud control of the world-projected carrier indicator (orange flag over enemy carrier's head), the yellow "You have the flag!" banner, and a new red "Enemy has your flag, recover it!" banner (engine had the text but never drew it; restored as a UT99-style alarm for audio-off players). Each piece independently positionable/hideable/colorable/styleable.
  - **Crosshair flag-grab flash suppressed** by default (the engine's team-colored flag that pops over the crosshair for 3s after a grab). Opt back in via nchud → CTF section. Implemented as a `LastFlagGrabTime` save/restore around `Super::DrawHUD()` — touches only the one flash path, no other side effects.
- **NCLeagueDuel** — Strict 1v1 with paired-weapon first-spawn fairness:
  - Map's weapon pickups grouped into pairs (Sniper↔Shock, Rocket↔Flak, Mini↔Link); first-spawn picker assigns A and B from the same pair so neither player gets a weapon-class advantage out of the gate.
  - Shield-belt-adjacent PlayerStarts excluded from first-spawn while the belt is active.
  - 1v1 Glicko-2 ratings persisted in `NCRatingDuel` (Mods.db). Match-end pushes per-player pre/post ELO + delta.
  - Scoreboard: K/D, Quake-style LG accuracy column, total damage, and a 4-icon armor-pickup row (Belt/Vest/Pads/Helm) with anti-timing reveal delay (4s hold per armor type, instant on damage / match reset).
- **NCShaftArena** — 1v1 link-gun-only beam fight, win-by-2:
  - Shaft-only loadout (BP weapon class, configurable via Mod.ini `[NCShaftArena] WeaponClass=...`).
  - Always-on vampirism (`SiphonPercent=0.5`, `HealCap=199`) — shaft hits heal the attacker.
  - Glicko-2 1v1 ratings in `NCRatingShaftArena`.
  - Link-pull yoink disabled (no "GET OVER HERE"), pickup timers stripped (no respawn-ring world spam).
- **ElimPlus** — Round-based 4v4 team elimination, formerly TeamArena. Full competitive scoring stack:
  - Vendored TeamGlicko-2 ratings (per-player Rating / RD / Sigma persisted in `Saved/Mods.db`), per-round in-memory updates, frozen display ELO during a match → animated count-up at match end on the portrait HUD chip (green/red color tween).
  - **Carry-aware lobby-impact blend** — per-round PPR (Kills + Damage/100, no deaths — matches the community PPR board) z-scored across BOTH teams, blended into the Glicko update via `eff = 0.9·logistic(2.5·z) + 0.1·W/L`. Within-team performance scaling alone could only ever dampen the W/L delta (never flip its sign), so good players plateaued near the seed (~1600). The blend rescues a hard carry from a losing team and discounts a passenger on a winning team. Validated offline on 90k PHX Abs-Elim + UTPugs ElimPlus matches: Spearman 0.937 vs the community PPR board. Wipeout uses the same blend.
  - `PickBalancedTeam` override + full pre-match `TeamBalancer` rebalance at `HandleMatchHasStarted` (fires after engine bot-fill so the pool is complete). Respects `?BalanceTeams=true/false` URL flag.
  - Custom Canvas-drawn pre-match team-preview overlay with team rosters + ELOs + team-strength totals (replaces unreliable scoreboard auto-show).
  - Bot-match cap (±5 ELO) for matches where the human never faced human opposition. Skips never-played connections (e.g. plugin-mismatch kicks) at flush.
  - Lifetime PPR persistence (`TotalPoints` + `RoundsPlayed` columns in `NCRatingElimPlus`, queryable via sqlite).
  - Match-winning-kill instant replay via `ClientPlayInstantReplay` with conditional 7s EndGame delay (skipped in standalone PIE).
  - Hidden-while-respawning portrait visuals + last-man-standing pulse.
- **Wipeout** — Team elimination with respawn waves, portrait-strip HUD, side-by-side scoreboard with player portraits, K/D + B/A tracking, sudden death OT, alternating-team-first round spawning. Same carry-aware Glicko blend as ElimPlus.
- **ShockDom** — 4v4 Shock-Domination (3 control points). Includes match clock HUD, opposing-side cluster spawning at match start, configurable scoring tick.

### Spawn System (Wipeout + ElimPlus)

Both team-elimination modes share the same spawn picker:
- Multi-axis side detection at map load — five candidate splits (E-W, N-S, two diagonals, principal eigenvector of the spawn covariance), pick whichever maximizes the minimum cross-team distance.
- Per-team curated pool of N spawns (N = team size, currently 4) sorted by distance from the other side's centroid.
- Dynamic per-player scoring at spawn time within the pool: cluster teammates, separate from enemies, hard floor at `MinimumEnemySpawnDistance` (3600u default), occupancy check.
- 3-tier fallback: curated pool → curated best-of-bad (relax enemy threshold) → entire-map best-of-bad. Never returns null.
- Capped enemy-distance bonus (5000u) so the score doesn't explode when no enemies have spawned yet (first team in a round).

### HUD Editor

NetcodePlus ships an in-game HUD layout editor (`SNCPlusHUDEditor`) with a live-preview JSON layout system that's not present in any official UT release.

- **9-anchor grid** (TopLeft / TopCenter / TopRight / CenterLeft / Center / CenterRight / BottomLeft / BottomCenter / BottomRight) plus per-element offset, scale, opacity, color overrides.
- **In-viewport repositioning** — `nchud_drag` preview overlay lets you see element bounds in-place rather than picking through text fields blind.
- **Per-element font picker + FontSz slider** — Tier A (engine built-ins: Tiny / Small / Medium / Large / Huge / Number / Chat) and Tier B (lazy-loaded UT4 fonts: Exo2 Bold, Lato, Ambex, Positec, Extreme) on every text-rendering alias. `FontSz` is a separate multiplier (0.5–2.0 slider, 0.25–4.0 hard cap) so you can dial in apparent text size at 4K without re-importing the UFont at a different `LegacyFontSize`. Lives on scorebar / score_kda today; portraits get it for HP/Armor numbers + respawn timers; the CTF banners get it too.
- **Five HP/Armor visual styles** (MinimalTypography / SegmentedBars / RadialArcs / HexChevrons / VerticalPills), per-element via the `style` extra. The font picker on `hp_armor` covers both the numbers and the HEALTH / ARMOR labels.
- **Custom split WeaponBar** — left/right columns with per-weapon picker (decide which weapons live in which column; remaining weapons hide entirely).
- **CTF flag-status banners** — `ctf_carrier_indicator` (orange flag over enemy carrier), `ctf_you_have_flag` (yellow), `ctf_enemy_has_flag` (red, new in v326), and `ctf_flag_status` (top silhouettes) are four independent draw-call aliases — each independently hideable/positionable, banners with their own color + font.
- **Optional opt-in overlays** (default OFF; appear in the editor with the Hide box pre-checked):
  - `damage_flash` — full-screen tint when you take damage. Tunable color, intensity (via opacity), and `flash_duration` (default 0.30s, linear fade).
  - `server_info` — small server-name plate (font/size/color/opacity). Reads `GameState->ServerName`, falls through to `ServerDescription`, then a string literal. Supports an explicit `name_override` Extras key for hub setups where `ServerName` is overwritten with the ruleset/match label.
  - `crosshair_flag_grab` — bring back the engine's grab flash if you want it.
  - `speedometer`, `minimap`, `accuracy`, `heal_ability` — pre-existing opt-ins.
- **HOST badge on scoreboards** — gold "HOST" tag next to the player who'll press Enter to start the match, on every NCPlus scoreboard. Warmup-only — disappears the instant the match starts. Identifies the host via `AUTPlayerState::bIsMatchHost` with replicated `AUTGameState::HostIdString` as a fallback.
- **Live preview** — every edit applies to the active HUD without a restart. Reset-per-row and Reset-All snap back to engine defaults snapshotted at startup.
- **JSON share via clipboard** — Copy / Paste buttons in the editor footer serialize the layout to/from the system clipboard. Validation + confirm dialog on paste. Same payload as `Saved/NetcodePlus/HUDLayout.json`.
- **Preset gallery** — 3 curated presets (Streamer Friendly / Comp Minimal / Quake Live Throwback) plus user save/delete, with procedural Slate thumbnails. First-run seeds Streamer Friendly so new installs get a polished baseline instead of stock UT defaults.
- **Multi-mode** — single layout file applies across all NetcodePlus modes (ElimPlus / Wipeout / NCPlusCTF / ShockDom / NCLeagueDuel / NCShaftArena).
- **Movable engine widgets** too — the alias table covers stock crosshair, killfeed, spectator score, announcements, voice-chat status, etc. Position overrides write straight to the widget's `ScreenPosition` / `Position` / `Origin` fields.
- **FSE-safe color picker** — opens via a snapshot-and-restore window-mode swap (Fullscreen → WindowedFullscreen → restore on close) so the OS doesn't minimize the game and trap the picker in a bounce loop.

Open the editor in-game with the `nchud` console command. Layout persists to `Saved/NetcodePlus/HUDLayout.json`.

### Mutators

- **NCUTPlus** — primary mutator that replaces stock weapons with their NetcodePlus variants. Configurable per-weapon hide/show via `weaponskins` console command.
- **ClientHitsounds** — client-side hit prediction with batched server confirmation. Configurable hitsound packs.
- **ElimPlusMutator** — adds the ElimPlus-specific behaviors (rating/replicator hookup, BP CheckRelevance for placed-pickup filtering). Required when running `ElimPlus` game mode.
- **AWarmupRoamMutator** — auto-added by NCPlusCTF (incl. iCTF). Powers the `mutate warmup` console command: warmup-only invuln + firing-disable so players can learn the map. Stripped from everyone at match start via `NotifyMatchStateChange`; can never carry into live play.

### Utilities

- **`weaponskins` console command** — opens Slate UI for per-weapon hide/show, skin selection, hitscan choice (Sniper / LG), **beam colors** (Sniper/LG via `LGColor`, Shock via `ShockBeam` — FSE-safe color picker, format matches the BP defaults' `Convert String to Linear Color` parser so the weapon BPs read the new color at spawn without parser changes), and **hidden-weapon beam origin** (`Back` / `Down` spinners — the tracer/beam spawn point relative to the camera when the weapon is hidden; defaults 10 / 35 reproduce stomach-height; try 10 / 20 for chest or 10 / 60 for hip-fire feel).
- **`weaponhand [right|left|center|hidden]`** — direct console command (writes to ProfileSettings).
- **Plugin-version gate (`ANCVersionGate`)** — server-spawned per-player `AInfo` in `PostLogin`, owner-only replication. Client's `PostNetInit` reports `NETCODE_PLUGIN_VERSION` immediately; server `KickPlayer`s mismatches with a clear "server v325, you are vN — update via launcher" message. Server-side timeout (default 10s, `[NetcodePlus] VersionReportTimeoutSec`, clamp 1–60) catches clients that don't have the gate class at all and never respond. Wired into every NCPlus mode's `PostLogin`. Bots + listen-host local PC exempt.
- **Custom siphon powerup** — life-steal pickup spawning at sniper location with 90s timer.
- **Hit-plot replicator** — server analyzes hit positions for ServerShield-style debug visualization.
- **Stats integration** — replicates per-player accuracy + damage + armor counts via mode-specific `AInfo` replicators (`ANCLeagueDuelStatsReplicator`, `ANCShaftArenaStatsReplicator`, `AElimPlusStatsReplicator`, `ACTFStatsReplicator`, `AWipeoutDamageReplicator`, `AShockDomReplicator`) so dedicated-server clients can read stats that are server-only on `AUTPlayerState`.
- **Local + global ELO** — every rated mode (Duel, Shaft, ElimPlus) writes per-player Glicko-2 ratings to the hub's local `Saved/Mods.db` SQLite. On match end, hubs also push results to ut4stats.com (via StatSQL's HTTP layer) for a cross-hub global ELO leaderboard.
- **Anti-timing armor delay** (duel scoreboard) — armor pickup counts on the scoreboard hold for 4 seconds before incrementing, so opponents / spectators / streamers can't time pickups by watching the column. Per-armor-type timer; decreases (match reset) update instantly.

## Installation

1. Place the `NetcodePlus/` folder in `<UT4Install>/UnrealTournament/Plugins/NetcodePlus/`
2. Verify the directory contains `NetcodePlus.uplugin`, `Source/`, and `Binaries/`
3. Launch UT4 — the plugin loads automatically

For server admins: ensure `Binaries/Linux/` and `Binaries/Win64/` are committed if running dedicated servers across platforms.

## Configuration

### Console Variables

| CVar | Default | Description |
|------|---------|-------------|
| `ut.ProjectileTickRate` | 240 | Client-side projectile sim rate (Hz). Range 120-720. |
| `ut.EnableProjectilePrediction` | 1 | Visual prediction for non-hitscan weapons. Set 0 for raw server positions. |

### Mod.ini

Lives at `<Saved>/Config/Mod.ini`. Most plugin-side knobs go here; some are per-player (client `Saved/`), some are per-server (server `Saved/`), some apply to both — noted per section.

**`[NetcodePlus]`** (server-side):
- `VersionReportTimeoutSec=10.0` — how long the version gate waits for a client to report its NETCODE_PLUGIN_VERSION before kicking. Default 10s; clamped 1–60s. Garbage / missing value falls back to default.

**`[NetcodePlus.WeaponSettings]`** (per-player):
- `Hide.<WeaponClassName>=1|0` — hide/show first-person mesh per weapon.
- `HiddenBeamBack=10.0` — when a weapon is hidden, how far behind the camera the tracer/beam spawns (units). Clamped 0–100.
- `HiddenBeamDown=35.0` — how far below the camera. Defaults 10 / 35 = stomach height; 10 / 20 = chest; 10 / 60 = hip-fire.
- `Skin.<WeaponSkinTag>=<AssetPath>` — applied skin per weapon family (currently disabled in code; see `bSkinsEnabled` gate).

**`[WeaponSkinsPlus]`** (per-player):
- `HitscanChoice=Sniper|LG` — hitscan weapon preference (Sniper Rifle vs Lightning Gun). Set via the `weaponskins` menu toggle.
- `LGColor=(R=...,G=...,B=...,A=...)` — beam color for both Sniper and LG. Format is `FLinearColor::ToString` output; read by the BP `Convert String to Linear Color` node in `UTNPLightningGun` / `UTNPSniper` defaults.
- `ShockBeam=(R=...,G=...,B=...,A=...)` — beam color for the Shock primary. Same format; read by `UTNPShockRifle`.

**`[UTPUGS_STATS]`** (server-side — CTF perf + respawn tuning):
- `CTFRespawnWait=1.5` — regulation CTF respawn for normal-sized matches (`MaxPlayers > CTFSmallGameMaxPlayers`). Fractional values honored.
- `CTFRespawnWaitSmall=1.0` — respawn for small games (1v1 / w00t).
- `CTFSmallGameMaxPlayers=2` — `MaxPlayers <= this` uses `CTFRespawnWaitSmall`. Raise to 4 if you want 2v2 to also get the fast respawn.
- Other knobs in this section tune the live CTF perf scoring used for ELO + the live Glicko system; see `LoadCTFPerfConfig` in `NCPlusCTFGameMode.cpp` for the full list.

**`[UTPUGS_SPAWN]`** (server-side — CTF spawn picker tuning): per-knob list in `LoadSpawnConfig` (penalties for flag-carrier proximity, enemy LOS, freshness window, etc.).

### URL Flags (server command line)

Standard UT4 URL flags that NetcodePlus modes honor:

- `?BalanceTeams=true|false` — enables / disables the auto-balancer in ElimPlus (gates both `PickBalancedTeam` override and the pre-match `RebalanceTeamsForMatchStart`). Default true.

### ElimPlus testing knobs (`Game.ini` or BP defaults)

```ini
[/Script/NetcodePlus.AElimPlusGame]
bRandomizeBotElo=true     ; default true — give each bot a random ELO in [BotEloMin, BotEloMax]
BotEloMin=1400
BotEloMax=1600
MinimumEnemySpawnDistance=3600.0
```

Mods.db schema for ELO data (server-only, gated by `USE_SQLITE = UE_SERVER`). One table per rated mode; all live in `Saved/Mods.db`:

```sql
-- ElimPlus (team-based Glicko-2 + per-round PPR)
SELECT UniqueId, Rating, RD, Sigma, TotalPoints, RoundsPlayed,
       (TotalPoints / NULLIF(RoundsPlayed, 0)) AS LifetimePPR
FROM NCRatingElimPlus;

-- NCLeagueDuel (1v1 Glicko-2)
SELECT UniqueId, Rating, RD, Sigma, GamesPlayed, Wins, Losses, Draws
FROM NCRatingDuel;

-- NCShaftArena (1v1 Glicko-2, beam-only)
SELECT UniqueId, Rating, RD, Sigma, GamesPlayed, Wins, Losses, Draws
FROM NCRatingShaftArena;
```

Hubs use these tables as the local source of truth. A parallel push to ut4stats.com (via StatSQL) feeds a cross-hub global ELO leaderboard so a player's rating accumulates across all participating hubs.

### Lag Compensation Tunables (BP-editable on weapon classes)

| Property | Default | Purpose |
|----------|---------|---------|
| `MaxRewindMs` | 250 | Max one-way rewind cap |
| `FudgeFactorMs` | 20 | Ping jitter buffer |
| `ProjectilePredictionCapMs` | 120 | Max projectile fast-forward |
| `HitScanPadding` | 40-45 | Capsule padding for claimed moving targets |
| `HitScanPaddingStationary` | 10 | Capsule padding for claimed stationary targets |
| `bEnableProjectileRewind` | (BP per-weapon) | Master toggle for projectile hit-claim validation |
| `ProjectileRewindMaxScale` | 1.0 | Full half-RTT rewind at low ping |
| `ProjectileRewindFullPingMs` | 100 | Below this ping, full rewind applied |
| `ProjectileRewindMaxPingMs` | 150 | Above this ping, no rewind (cliff) |
| `ProjectileRewindMinScale` | 0.5 | Rewind scale at the upper boundary |

## Building from source

NetcodePlus is a C++ plugin. To rebuild:

1. Have UT4's editor build set up (Visual Studio 2017 / Linux toolchain)
2. Right-click `UnrealTournament.uproject` → Generate Visual Studio project files
3. Build `UnrealTournamentEditor` from VS for development, `UnrealTournament` for shipping
4. For dedicated server: build `UnrealTournamentServer-Linux-Shipping` or `-Win64-Shipping`

Plugin code is under `Source/Public/` (headers) and `Source/Private/` (impl). Module name is `NetcodePlus`.

## Development notes

- **UE4 4.15 quirks**: no `FString::TrimStartAndEnd()` (use `.Trim()`), `ServerMutate()` is on `AUTPlayerController` not `APlayerController`, PCH mode is `UseExplicitOrSharedPCHs` so `.cpp` files must include `UnrealTournament.h` (or `NetcodePlus.h`) before UT headers. Same rule applies to plugin **headers** that include UT engine types — `#include "NetcodePlus.h"` must be the first include or unity-bundle reshuffles trigger UDataAsset / ULocalMessage cascade errors.
- **Class layout / ABI**: never subclass `AUTGameState` in plugin C++ (engine class layout mismatch crashes on level load). Use a separate replicated `AInfo` actor for game-mode state.
- **FlagBases access**: never directly access `CTFGameState->FlagBases` — use `GetFlagBase(idx)` accessor (vtable-dispatched, ABI-safe).
- **Vendored libraries**: hide non-UE4 C++ libs (e.g. Tron's TeamGlicko-2) behind a Pimpl in your wrapper header. Strip any `<iostream>` / `<fstream>` / stdio includes from vendored sources — they pull `<Windows.h>` on MSVC and poison the unity-build PCH chain. `FElimPlusRatingSystem` is the canonical example.

## Related projects

- **ClientDemos** — Client-side demo recording (UT99/UTComp-style) — separate plugin repo at github.com/jmortley/ClientDemos
- **StatSQL** — Database integration for match stats — separate plugin

## License / Credits

- Maintained by [phantaci](https://github.com/jmortley)
- Built on top of Epic Games' UT4 codebase
- Design influenced by UTComp's NewNet projectile sync and lag-comp patterns

## Branch policy

- `main` — stable releases
- `dev` — active work; merged to `main` at release boundaries
