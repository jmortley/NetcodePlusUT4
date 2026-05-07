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

- **NCPlusCTF** — Capture the Flag with adopted NewCTF advantage/OT mechanics, instant-end on flag-home, 5-min OT cap, ping-compensated spawning. HUD scorebar shows count-up overtime clock with "Overtime" label (replicated via `ANCPlusCTFOTInfo`); spectator list deduplicates the engine's stock count text.
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
  - `PickBalancedTeam` override + full pre-match `TeamBalancer` rebalance at `HandleMatchHasStarted` (fires after engine bot-fill so the pool is complete). Respects `?BalanceTeams=true/false` URL flag.
  - Custom Canvas-drawn pre-match team-preview overlay with team rosters + ELOs + team-strength totals (replaces unreliable scoreboard auto-show).
  - Bot-match cap (±5 ELO) for matches where the human never faced human opposition. Skips never-played connections (e.g. plugin-mismatch kicks) at flush.
  - Lifetime PPR persistence (`TotalPoints` + `RoundsPlayed` columns in `NCRatingElimPlus`, queryable via sqlite).
  - Match-winning-kill instant replay via `ClientPlayInstantReplay` with conditional 7s EndGame delay (skipped in standalone PIE).
  - Hidden-while-respawning portrait visuals + last-man-standing pulse.
- **Wipeout** — Team elimination with respawn waves, portrait-strip HUD, side-by-side scoreboard with player portraits, K/D + B/A tracking, sudden death OT, alternating-team-first round spawning.
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

- **9-anchor grid** (TopLeft / TopCenter / TopRight / CenterLeft / Center / CenterRight / BottomLeft / BottomCenter / BottomRight) plus per-element offset, scale, opacity, color overrides, font selector.
- **Drag-drop visual editor** — repositioning happens in viewport, not text fields.
- **Five HP/Armor visual styles** (MinimalTypography / SegmentedBars / RadialArcs / HexChevrons / VerticalPills), per-element via the `style` extra.
- **Custom split WeaponBar** — left/right columns with per-weapon picker (decide which weapons live in which column; remaining weapons hide entirely).
- **Live preview** — every edit applies to the active HUD without a restart. Reset-per-row and Reset-All snap back to engine defaults snapshotted at startup.
- **JSON share via clipboard** — Copy / Paste buttons in the editor footer serialize the layout to/from the system clipboard. Validation + confirm dialog on paste. Same payload as `Saved/NetcodePlus/HUDLayout.json`.
- **Multi-mode** — single layout file applies across all NetcodePlus modes (ElimPlus / Wipeout / NCPlusCTF / ShockDom / NCLeagueDuel / NCShaftArena).
- **Movable engine widgets** too — the alias table covers stock CTF flag-status, crosshair, killfeed, spectator score, announcements, voice-chat status, etc. Position overrides write straight to the widget's `ScreenPosition` / `Position` / `Origin` fields.

Open the editor in-game with the registered console command (see `NetcodePlus.cpp` for the bind name). Layout persists to `Saved/NetcodePlus/HUDLayout.json`.

### Mutators

- **NCUTPlus** — primary mutator that replaces stock weapons with their NetcodePlus variants. Configurable per-weapon hide/show via `weaponskins` console command.
- **ClientHitsounds** — client-side hit prediction with batched server confirmation. Configurable hitsound packs.
- **ElimPlusMutator** — adds the ElimPlus-specific behaviors (rating/replicator hookup, BP CheckRelevance for placed-pickup filtering). Required when running `ElimPlus` game mode.

### Utilities

- **`weaponskins` console command** — opens Slate UI for per-weapon hide/show and skin selection.
- **`weaponhand [right|left|center|hidden]`** — direct console command (writes to ProfileSettings).
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

Per-player settings persist in `Mod.ini` under `[NetcodePlus.WeaponSettings]`:

- `Hide.<WeaponClassName>=1/0` — hide/show first-person mesh per weapon
- `Skin.<WeaponSkinTag>=<AssetPath>` — applied skin per weapon family (currently disabled in code; see `bSkinsEnabled` gate)

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
