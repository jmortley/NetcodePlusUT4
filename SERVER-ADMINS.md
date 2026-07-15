# NetcodePlus — Server Admin Guide

A practical guide for hub and server operators who want to run **NetcodePlus** (and its companion
plugins **StatSQL** and **ServerShield**) the way the UTPugs hubs run them. It covers what to install,
how clients get the content, what replaced the old stock mutators, and every server‑side knob the
plugin exposes — with example rulesets you can copy.

This is a community effort and is **not** an official Epic project. Nothing here is endorsed by Epic
Games.

> **New to hosting?** This guide covers the NetcodePlus layer on top of a *working UT4 hub*. If you
> don't have a dedicated server running and listed in the Hub browser yet, set one up first with the
> community **UT4 Hub Setup Guide** (neckutter1 / UT4ever) —
> <https://downloads.ut4ever.box.ca/server/HubGuide.pdf> — then come back here. Quick prerequisites
> checklist in §1.0.

> Engine target: UT4 **4.15** (CL‑3525360). Current plugin build: **327**.
> Repos: [`jmortley/NetcodePlusUT4`](https://github.com/jmortley/NetcodePlusUT4) ·
> [`jmortley/StatSQL`](https://github.com/jmortley/StatSQL) · ServerShield (private — see §10).

---

## Start here — `NCWepMut` turns any gamemode into NetcodePlus

You don't have to rebuild your hub around the NetcodePlus gamemodes to get the netcode. The
weapon‑replacement mutator **`NCWepMut`** drops the NetcodePlus weapons into **any** gamemode — stock
CTF, TDM, DM, Duel, Showdown, or your existing custom rulesets — by adding one token:

```
?mutator=...,NCWepMut
```

It swaps the stock Sniper / Shock / Flak / Rocket (and, configurably, Link / Bio / Enforcer / Minigun)
for the NetcodePlus equivalents, and **that weapon path is where the real improvements live**:
lag‑compensated hit registration, projectile direct‑hit rewind, client projectile prediction, the
shock‑core drift fix, and client‑informed headshot validation. Keep your gamemode, your maps and your
rules — just add `NCWepMut` and the gunplay is NetcodePlus.

- **The fast path:** add `NCWepMut` (plus `dcHitsounds`) to an existing stock ruleset. Everything else
  in this guide still applies — the lag‑comp cvars (§6), StatSQL (§9), ServerShield (§10).
- **The full experience:** the dedicated NetcodePlus **gamemodes** (ElimPlus, Wipeout, iCTF, League
  Duel, Shaft Arena, Shock Domination — §4) layer ELO, fair‑spawn logic, the custom HUD, end‑of‑match
  cap replays, host/captain pause and the version gate **on top of** those same weapons.

Requires clients to run the NetcodePlus plugin (via the launcher) and to have the `NCWepMut` pak (§2).
`NCWepMut` is **not** auto‑added by anything — you add it per ruleset.

> **Example — stock CTF, now on NetcodePlus weapons** (keep your existing mutators, add `NCWepMut`):
> ```
> "gameMode": "/Script/UnrealTournament.UTCTFGameMode",
> "gameOptions": "?TimeLimit=20?GoalScore=0?mutator=NCWepMut,dcHitsounds,SetNetSpeed"
> ```
> `SetNetSpeed` still works here because the gamemode is stock (only the NetcodePlus gamemodes ignore it
> — §3.3). StatSQL + ServerShield run hub‑wide via `Game.ini` `ConfigMutators` (§8.1), not in the chain.

---

## 0. TL;DR — migrating a hub off stock

If you already run a UTPugs‑style hub, the move to NetcodePlus is mostly **swapping mutators** and
**dropping three legacy ones**:

| Old (remove) | New (use instead) | Why |
|---|---|---|
| `MutHitsounds` / `Mutator_AbsoluteHitSounds` pak | **`dcHitsounds`** | `MutHitSounds` was rewritten as **dcHitsounds** to work with NetcodePlus weapons (+ the "FAHHH" meme sound). Ships in the content paks. |
| `MutTeamSkins` mutator + pak | **Force Models** (built into the client plugin) | Team skins are now a **client‑side** feature of NetcodePlus. There is no server mutator to add — just stop running `MutTeamSkins`. |
| `SetNetSpeed` mutator — remove from **NetcodePlus‑gamemode** rulesets | **`[ElimPlus] NetSpeed`** in Mod.ini (for the NetcodePlus gamemodes) | `SetNetSpeed` doesn't work on the NetcodePlus native gamemodes — they read `[ElimPlus] NetSpeed` instead. It **still works on stock gamemodes**, so keep it where you've only added `NCWepMut` (see §3.3). |

Then:

1. **Install the plugin binaries** on the server (Windows **and** Linux ship in one zip) — §1.
2. **Make sure clients can get the 6 content paks** — via the launcher or `Game.ini` redirects — §2.
3. **Point your rulesets at the NetcodePlus gamemodes** (or just add `NCWepMut`) and use `dcHitsounds`;
   add **StatSQL + ServerShield hub‑wide** via `Game.ini` `ConfigMutators` (§8.1) — §4, §11.
4. **Remove the stale redirects** for `Mutator_AbsoluteHitSounds` and `MutTeamSkins` from `Game.ini`.

---

## 1. Installing the plugin

### 1.0 Prerequisites — a working UT4 hub

NetcodePlus is the layer you add on top of an already‑running hub. This guide assumes you have a UT4
dedicated server (a "hub") that:

- runs the UT4 dedicated‑server build **CL‑3525360** (Linux or Windows), has generated its configs from
  a first launch, and appears in the in‑game Hub browser;
- is registered to the community master server (`[OnlineSubsystemMcp.*] Domain=master-ut4.timiimit.com`
  in `Engine.ini`);
- has an rcon password set (`[/Script/UnrealTournament.UTGameEngine] RconPassword=…`);
- serves custom content to clients via **UTCC redirects** (`RedirectReferences=` in `Game.ini`) — the same
  mechanism the 6 NetcodePlus paks use (§2);
- loads its rulesets from `…/UnrealTournament/Saved/Config/Rulesets/rulesets.json`.

If any of that isn't in place yet, set the hub up first with the community **UT4 Hub Setup Guide**
(neckutter1, UT4ever) — a full walkthrough from a bare Linux box through master‑server registration and
UTCC redirects: **<https://downloads.ut4ever.box.ca/server/HubGuide.pdf>**. Add NetcodePlus once that hub
is live.

### 1.1 Get the binaries

Download the latest plugin zip from the releases page:

**<https://github.com/jmortley/NetcodePlusUT4/releases>** → tag **`plugin-latest`** → asset
**`NetcodePlus-327.zip`**.

Every release zip contains **all** build targets — you don't need a separate "server build":

```
NetcodePlus/
├── NetcodePlus.uplugin
└── Binaries/
    ├── Win64/        UE4-NetcodePlus-Win64-Shipping.dll  (dedicated server + client)
    │                 + editor DLLs
    └── Linux/        libUE4-NetcodePlus-Linux-Shipping.so (dedicated server)
```

### 1.2 Where it goes

Extract so the folder lands at:

```
<UnrealTournament install>/UnrealTournament/Plugins/NetcodePlus/
```

- **Windows hub:** `...\UnrealTournament\UnrealTournament\Plugins\NetcodePlus\`
- **Linux hub:** `.../UnrealTournament/UnrealTournament/Plugins/NetcodePlus/`

The dedicated‑server `.so` / `.dll` is what your hub loads; the same zip's client DLL is what the
**launcher** ships to players. Restart the server after extracting. (To automate this, see §12.)

### 1.3 Version gate

NetcodePlus auto‑spawns a **version gate** on every match (no mutator token needed). When a client
joins, it reports its plugin build; a **mismatched** build is kicked with a reason and can rejoin
after updating. Players with **no** plugin are currently **not** auto‑kicked by the gate (the no‑reply
timeout is disabled), so a hub can run mixed if you want — but a forced client roll (a build bump)
will kick out‑of‑date clients until they update.

> Tunable: `[NetcodePlus] VersionReportTimeoutSec` (Mod.ini). See §5.

---

## 2. The content paks (what clients need)

NetcodePlus's C++ lives in the plugin DLL that the **launcher** installs on each client. The
**gamemodes and mutators** that your rulesets reference (the instagib rifle, the ElimPlus/Wipeout
gamemode blueprints, the sounds, etc.) live in **6 content paks**. These are exactly the paks the
[NetcodePlus launcher](https://github.com/jmortley/netcodeplus-launcher) verifies and keeps current on
every client.

| Pak (package name) | UTCC id | Used by / token | Role |
|---|---|---|---|
| `MutInstagibNCP-WindowsNoEditor` | 2037 | `MutInstagibNCP` | Instagib weapon set → turns NetcodePlus CTF into **iCTF** |
| `ElimPlusMutator-WindowsNoEditor` | 1996 | `ElimPlusMutator` + `ElimPlus.ElimPlus_C` | **ElimPlus** (Team Arena) gamemode + mutator + sounds |
| `WipeoutMutator-WindowsNoEditor` | 2036 | `WipeoutMutator` + `WipeoutPlus.WipeoutPlus_C` | **Wipeout** gamemode + mutator |
| `SdomMutator-WindowsNoEditor` | 2040 | `SdomMutator` | **Shock Domination** content |
| `NCWepMut-WindowsNoEditor` | 2042 | `NCWepMut` | NetcodePlus **weapon replacement** — drops NetcodePlus weapons into **any** gamemode (the on‑ramp, see Start Here) |
| `UTPlus-WindowsNoEditor` | 852 | `UTPlus` | UT+ weapon/movement content |

~792 MB total on a first pull.

### 2.1 How clients get them — two paths

1. **The launcher (recommended).** The NetcodePlus launcher maintains all 6 paks in
   `Documents/UnrealTournament/Saved/Paks/DownloadedPaks/` so a player always has them, **even if a
   server forgets to push them**. This is the fix for the long‑standing "missing ElimPlus sounds"
   problem (some hubs sent the pak, some didn't). Point players at the launcher and you can stop
   worrying about redirects.

2. **`Game.ini` redirects (for non‑launcher clients).** UT4 will auto‑download any pak listed under
   `[/Script/UnrealTournament.UTBaseGameMode] RedirectReferences=` when a server requires it. The NYC
   hub already redirects all 6 from UTCC, e.g.:

   ```ini
   [/Script/UnrealTournament.UTBaseGameMode]
   RedirectReferences=(PackageName="MutInstagibNCP-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/2037/00140c90ae0395f8107e1b157a4ee042/MutInstagibNCP-WindowsNoEditor.pak",PackageChecksum="00140c90ae0395f8107e1b157a4ee042")
   RedirectReferences=(PackageName="ElimPlusMutator-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/1996/7e4567dd7ffc3d08c93b776828d1cd27/ElimPlusMutator-WindowsNoEditor.pak",PackageChecksum="7e4567dd7ffc3d08c93b776828d1cd27")
   RedirectReferences=(PackageName="UTPlus-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/852/5468c86e8aad1c17a5b8d9cd3b7b6835/UTPlus-WindowsNoEditor.pak",PackageChecksum="5468c86e8aad1c17a5b8d9cd3b7b6835")
   RedirectReferences=(PackageName="WipeoutMutator-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/2036/095ad00d157cbcaa4f887aededc2141a/WipeoutMutator-WindowsNoEditor.pak",PackageChecksum="095ad00d157cbcaa4f887aededc2141a")
   RedirectReferences=(PackageName="SdomMutator-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/2040/dff4e3d673b721cd051a1f9593475fc7/SdomMutator-WindowsNoEditor.pak",PackageChecksum="dff4e3d673b721cd051a1f9593475fc7")
   RedirectReferences=(PackageName="NCWepMut-WindowsNoEditor",PackageURLProtocol="http",PackageURL="utcustomcontent.com/redirect/2042/2a17b9bf73fe01efddcb55403fe420d3/NCWepMut-WindowsNoEditor.pak",PackageChecksum="2a17b9bf73fe01efddcb55403fe420d3")
   ```

   The `<md5>` in each redirect URL is the pak's checksum — UTCC bakes it into the path. Keep the paks
   on the server too (in `.../UnrealTournament/Content/Paks/` or your hub's pak dir) so the gamemode
   classes resolve server‑side.

### 2.2 Remove these stale redirects

These two are **superseded** and should be deleted from `Game.ini` so old content stops downloading:

```ini
; DELETE — old hitsounds, replaced by dcHitsounds (bundled in the paks)
RedirectReferences=(PackageName="Mutator_AbsoluteHitSounds-WindowsNoEditor", ...)
; DELETE — team skins are now client-side Force Models, no pak needed
RedirectReferences=(PackageName="MutTeamSkins-WindowsNoEditor", ...)
```

---

## 3. What replaced what

### 3.1 `MutHitsounds` → `dcHitsounds`

`MutHitSounds` (the old `Mutator_AbsoluteHitSounds` pak) was rewritten as **`dcHitsounds`** so it works
correctly against NetcodePlus's client‑prediction weapon path, and the popular **"FAHHH"** sound was
added. The content ships inside the NetcodePlus paks, so launcher users already have it.

**Admin action:** in your `?mutator=` chains, replace the token `MutHitsounds` with `dcHitsounds`.
Players tune their own hitsounds in‑game with **`mutate hitsounds`**.

> A native C++ hitsounds port (`AClientHitsounds`, config under `[ClientHitsounds]` / `[Hitsounds.Enemy]`
> / `[Hitsounds.Friendly]`) is compiled into the plugin but is **not yet the shipping path** — the live
> hitsounds is the `dcHitsounds` blueprint. Those Mod.ini keys are listed in the appendix for
> completeness but don't drive anything yet.

### 3.2 `MutTeamSkins` → Force Models (client‑side)

Team skins / forced enemy models are now a **client feature** of NetcodePlus called **Force Models**.
It is *purely client‑local* — there is **no server mutator and no `?mutator=` token**, and it is a no‑op
on a dedicated server. Players open it with **F5** or `mutate teamskins` / `mutate forcemodels`.

**Admin action:** just stop running `MutTeamSkins` (and drop its pak/redirect). There is nothing to add
server‑side. The per‑player settings live in each client's Mod.ini under `[ForceModels]` (documented in
the appendix for reference).

### 3.3 `SetNetSpeed` — keep it on stock gamemodes, drop it on the NetcodePlus gamemodes

The old standalone `SetNetSpeed` mutator (`/Game/ZohStuff/NetSpeed/...`) sets each client's net rate.
Whether you keep it depends on the gamemode:

- **NetcodePlus native gamemodes** (ElimPlus, Wipeout, iCTF / NetcodePlus CTF, League Duel, Shaft Arena,
  Shock Domination): `SetNetSpeed` **does not work** here — the netspeed logic was rebuilt into the
  gamemode paks, which set the rate themselves. **Remove `SetNetSpeed` from these rulesets** and set the
  rate once in Mod.ini:

  ```ini
  [ElimPlus]
  WinByTwo=true
  NetSpeed=72000
  ```
  All of the NetcodePlus gamemode paks read that one `[ElimPlus]` section.

- **Stock / standard gamemodes you've added `NCWepMut` to** (stock CTF, TDM, DM, Duel, …): the gamemode
  is stock, so `SetNetSpeed` **still works** — keep using it for the net rate. `[ElimPlus] NetSpeed` does
  **not** apply here (only the NetcodePlus gamemode paks read it).

The server‑side rate ceilings in `Engine.ini` apply either way — see §8.

> The `[ElimPlus]` keys are read by the **gamemode paks (blueprint)**, not the NetcodePlus C++ DLL, so
> they are not in the cvar/Mod.ini tables below.

---

## 4. Wiring a ruleset

### 4.1 NetcodePlus gamemodes

NetcodePlus ships six C++ gamemodes. In a hub ruleset you normally point `gameMode` at the **blueprint
subclass** that lives in the content pak (it's reparented to the C++ class and carries the BP defaults),
e.g. `/Game/Blueprints/ElimPlusStuff/ElimPlus.ElimPlus_C`. The native class path
(`NetcodePlus.<Class>`) also works for the non‑abstract ones.

| Mode | C++ class | In‑game name | Typical `gameMode` reference | Mod.ini section |
|---|---|---|---|---|
| ElimPlus / Team Arena | `AElimPlusGame` | "Team Arena" | `/Game/Blueprints/ElimPlusStuff/ElimPlus.ElimPlus_C` | *(pak)* |
| NetcodePlus CTF / iCTF | `ANCPlusCTFGameMode` *(abstract)* | "NetcodePlus CTF" | `/Game/Blueprints/Netcode/NCP-IGCTF.NCP-IGCTF_C` | `[UTPUGS_STATS]`, `[UTPUGS_SPAWN]` |
| Wipeout | `AUWipeoutGame` | "Wipeout" | `/Game/Blueprints/ElimPlusStuff/WipeoutPlus.WipeoutPlus_C` | *(pak)* |
| League Duel | `ANCLeagueDuelGame` | "NetcodePlus League Duel" | `/Game/Blueprints/Netcode/bp_NCLeagueDuel.bp_NCLeagueDuel_C` | `[NCLeagueDuel]` |
| Shaft Arena | `ANCShaftArenaGame` | "NetcodePlus Shaft Arena" | `NetcodePlus.NCShaftArenaGame` (or BP subclass) | `[NCShaftArena]` |
| Shock Domination | `AShockDomGameMode` | "Shock Domination" | `/Game/Blueprints/Netcode/ShockDomGM.ShockDomGM_C` | — |

Notes:
- `ANCPlusCTFGameMode` is **abstract** — you must reference a concrete subclass (the `NCP-IGCTF_C` BP for
  iCTF, or your CTF BP), never the bare base.
- **iCTF is not a separate gamemode** — it's NetcodePlus CTF + the external `MutInstagibNCP` mutator. The
  plugin detects instagib at runtime.
- `AUWipeoutGame` really has the double‑U.
- Each mode auto‑spawns the version gate; CTF/iCTF also auto‑adds an internal **Warmup Roam** mutator.
  You don't add either.

### 4.2 NetcodePlus / companion mutator tokens

| Token (`?mutator=`) | What it is | Side |
|---|---|---|
| `dcHitsounds` | Damage hitsounds (replaces `MutHitsounds`) | client content, added to chain |
| `MutInstagibNCP` | Instagib weapon set (makes a CTF mode iCTF) | server (external BP pak) |
| `NCWepMut` | NetcodePlus weapon replacement — turns **any** gamemode into NetcodePlus weapons (see Start Here) | server (pak) |
| `NetcodePlus.NCUTPlus` | Native weapon‑replacement mutator (`ANCUTPlus`); usually a BP subclass classpath | server |
| `MutStatSQL` | Stats upload to ut4stats.com — §9 | server |
| `MutServerShield` | Behavioral anti‑cheat — §10 | server |

There is **no token** for: Force Models (client‑only), the version gate (auto), Warmup Roam (auto), or
host/captain pause (the in‑game `pause` command, gated by Mod.ini).

> **StatSQL and ServerShield are best added hub‑wide, not per ruleset** — put them in `Game.ini` as
> `ConfigMutators` (§8.1) so they run on every match. Don't also list them in a ruleset `?mutator=`
> (avoid a double‑add), and drop the legacy `Global_StatSQL-v2` blueprint so stats aren't
> double‑reported. (`EnabledByDefault` in the `.uplugin` only loads the module; it does **not** attach
> the mutator — `ConfigMutators` is what attaches it.)

---

## 5. `Mod.ini` reference — sections NetcodePlus owns

Mod.ini lives at `<UnrealTournament>/UnrealTournament/Saved/Config/Mod.ini` on the server. Only the
sections below are read by the NetcodePlus **C++ plugin**. Other sections in your Mod.ini
(`[NoCampers]`, `[DefaultWeaponSkins]`, `[UTPlus]`, `[ElimPlus]`, `[LogoSplash]`, `[UTPUGS_STATS]`
weapon stuff, etc.) belong to **other** mutators/paks and are not covered here.

### 5.1 `[NetcodePlus]` — core server behavior

| Key | Type | Default | Scope | Meaning |
|---|---|---|---|---|
| `bAllowHostPause` | bool | `false` | server | Let the recognized match host (`?HostId`) pause/unpause with the `pause` command. |
| `bAllowCaptainPause` | bool | `false` | server | Let bot‑designated team captains (`?Captains`) pause in PUGs. |
| `CaptainPauseCooldownSec` | int | `8` | server | Min unpaused seconds between captain pauses (0 = no cooldown). |
| `UnpauseCountdownSec` | int | `7` | server | "Resuming in N…" countdown before an unpause takes effect (0 = instant). |
| `VersionReportTimeoutSec` | float | `100.0` | server | Grace window for a joining client to report its plugin build (clamped 1–120). *Mismatch* kicks always fire; the *no‑reply* timeout kick is currently disabled. |

### 5.2 `[UTPUGS_STATS]` — CTF rating, respawn, auto‑pause, ELO upload

Shared section (StatSQL reads `Key`/`ServerName`/`SendStats` here too — see §9). All server‑side.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `CTFRespawnWait` | float | `1.5` | Regulation respawn delay (s) in CTF (fractional supported). |
| `CTFRespawnWaitSmall` | float | `1.0` | Respawn delay (s) in small games (≤ `CTFSmallGameMaxPlayers`). |
| `CTFSmallGameMaxPlayers` | int | `2` | At/below this player count a CTF match is "small" (2 = 1v1; raise to 4 for 2v2). |
| `AutoPauseOnDrop` | bool | `true` | Auto‑pause a bot PUG (`?PugId`) when a participant drops, until they rejoin or an admin unpauses. |
| `CTFPerfEnabled` | bool | `true` | Enable the CTF performance‑rating model (false = legacy K/D only). |
| `CTFRatingShadow` | bool | `true` | Shadow mode: live Glicko delta uses legacy perf; new perf is only observed/uploaded. |
| `CTFPerfObjectiveWeight` | float | `1.0` | Multiplier on the objective (flag‑play) half of the CTF perf score. |
| `CTFFlagFeederPenalty` | float | `1.0` | Multiplier on the "grabbed repeatedly without capping" penalty. |
| `CTFRatingMinPresenceFrac` | float | `0.5` | Min fraction of the match a leaver must have played to be rated (rage‑quit guard). |
| `CTFRoleAware` | bool | `true` | Weight CTF perf by offense/defense lean. |
| `CTFRoleWeightStrength` | float | `0.25` | Spread of the role multipliers (`Aoff = 1 + s·OffLean`). |
| `CTFRoleCombatWeight` | float | `4.0` | Weight a kill/death location adds to role‑dwell vs a 1.0 one‑second presence sample. |

### 5.3 `[NCShaftArena]` — Shaft Arena 1v1

| Key | Type | Default | Meaning |
|---|---|---|---|
| `GoalScore` | int | `10` | Frag goal. |
| `MinWinMargin` | int | `2` | Win‑by margin. |
| `SiphonPercent` | float | `0.5` | Fraction of damage dealt healed back (vampirism). |
| `HealCap` | int | `199` | Max health reachable via siphon. |
| `WeaponClass` | string | *(BP default)* | Override the shaft/link weapon class path. |

### 5.4 `[NCLeagueDuel]` — League Duel 1v1

| Key | Type | Default | Meaning |
|---|---|---|---|
| `MinKillerSpawnDistance` | float | `2500.0` | Min respawn distance from your last killer. |
| `MinimumEnemySpawnDistance` | float | `2400.0` | Min respawn distance from the enemy. |
| `ShieldBeltExclusionCount` | int | `2` | Spawn points near the shield belt excluded from the duel spawn pair. |

### 5.5 `[BOT_EVENTS]` — PUG bot integration (optional)

Fallbacks for the launch‑URL options of the same name (see §7). Leave empty unless you run the PUG bot.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `BotApiUrl` | string | *(empty)* | Base URL the match‑lifecycle events POST to. |
| `BotApiToken` | string | *(empty)* | Auth token for those POSTs. |

### 5.6 `[UTPUGS_SPAWN]` — CTF spawn‑selection tuning (advanced)

NetcodePlus CTF uses a geometry‑aware spawn scorer. **The defaults are tuned — only touch these if you
know what you're doing.** Keys (all float unless noted, server‑side, defaults live in the gamemode
constructor):

`FlagCarrierSpawnPenalty`, `DroppedFlagSpawnPenalty`, `FlagCarrierLOSPenalty`, `EnemyBlockRange`,
`EnemyBlockPenalty`, `EnemyLOSBlockRange`, `EnemyLOSPenalty`, `FlagBaseProximityRadius`,
`FlagSpawnPenaltyRadius`, `SpawnRecentPenaltyMultiplier`, `SpawnNearLastRadius`, `SpawnNearLastPenalty`,
`SpawnTieBandWidth`, `SpawnFreshnessBonus`, `SpawnFreshnessWindow`, `SpawnFlagVicinityRadius`,
`SpawnKillerAvoidRadius`, `SpawnFlagCarrierLOSAvoidRadius` (default `3500`), `SpawnRobbedBaseAvoidCount`.

---

## 6. Console variables (cvars)

Set live via rcon (`rconexec <cvar> <value>`) or in an exec config. **`ut.*` / `ncp.*` are NetcodePlus
cvars.** Most are live‑toggleable; none are cheat‑gated.

### 6.1 Server‑side (hit registration / lag comp)

| Cvar | Default | Meaning |
|---|---|---|
| `ut.RocketLagComp` | `1` | Master switch for rocket/flak‑shell **direct‑hit** lag compensation. |
| `ut.RocketLagCompMaxWindowMs` | `200` | Max rewind window (ms) for projectile direct‑hit validation. |
| `ut.RocketLagCompGraceMs` | `200` | Grace buffer (ms) so a late hit claim can still rescue an already‑exploded rocket. |
| `ut.RocketLagCompMaxPingMs` | `150` | Reject projectile hit claims from shooters above this RTT (anti‑abuse). 150 covers EU/Israel→NYC. |
| `ncp.HeadCapsuleDrop` | `20` | Headshot sphere **centre** distance below the capsule top (lower = sphere moves up). **Server value is authoritative; must match the client value.** Calibrate with `ncp.DebugHeads`. |
| `ncp.HeadBandLow` | `45` | Headshot validation: head‑centre lower bound (capsule top − N uu). |
| `ncp.HeadBandHigh` | `5` | Headshot validation: head‑centre upper bound. |
| `ncp.HeadBandXY` | `22` | Headshot validation: max head‑centre off‑axis offset (uu). |
| `ncp.HeadSlackScale` | `1.0` | High‑ping head slack = `targetSpeed · rewindTime · scale` (velocity‑gated; 0 disables). |
| `ncp.HeadSlackMax` | `25` | Hard cap (uu) on the high‑ping head slack. |
| `ncp.ShockServerTickHz` | `0` | Server shock‑core tick rate (0 = 240 Hz; >0 = that Hz, 30–720; read at spawn). |
| `ncp.CTFReplayMinDemoSeconds` | `200` | Min server‑demo age before the end‑of‑match decisive‑cap replay fires (0 = always — not advised; can crash the killcam in an early match). Requires a replay server (§8). |
| `ncp.CTFReplayBuildupSeconds` | `8` | Seconds of run‑up shown before the featured cap. |
| `ncp.ShockDebug` | `0` | Shock‑core diagnostics logging (0 off, 1 events). |

> The rocket trio `MaxWindowMs` / `GraceMs` / `MaxPingMs` are meant to be kept **matched** (defaults
> 200/200/150). Bumping `ut.RocketLagCompMaxPingMs` to 150 is the live fix for high‑ping no‑reg.

### 6.2 Client‑side (rendering / prediction — for player support)

These are set by players, not the server, but admins should know them for troubleshooting:

| Cvar | Default | Meaning |
|---|---|---|
| `ut.ProjectileTickRate` | `240` | Client projectile sim rate (snapped to 60s, clamped 120–720). Server always ticks at 120 Hz. |
| `ut.EnableProjectilePrediction` | `1` | Visual prediction for projectile weapons (0 = raw server positions). |
| `ncp.PredStabFastDrop` | `30` | Prediction‑stability interp rate when the lead must drop (jukers). |
| `ncp.PredStabSlowRise` | `4` | Prediction‑stability interp rate when re‑arming the lead. |
| `ncp.ShockDriftCorrect` | `1` | Shock‑ball per‑tick heading re‑assert (0 = stock‑like). |
| `ncp.ShockMatchFakeDot` | `0.5` | Shock fake/real pairing tolerance (0.95 = stock). |
| `ncp.HideArmorShield` | `0` | Force Models: hide the armour shield overlay instead of recolouring it. |
| `ncp.DebugHeads` | `0` | Draw the headshot sphere (green) vs mesh head (red) — calibration aid for `ncp.HeadCapsuleDrop`. |
| `ncp.FlagDebug` | `0` | Log CTF flag/mesh state (~1.5 s) for Force Models flag‑recolour debugging. |

---

## 7. Launch / URL options (ruleset `gameOptions`)

The `?Key=Value` tokens NetcodePlus itself parses (in addition to stock UT4 options like `?TimeLimit`,
`?GoalScore`, `?BalanceTeams`, `?MaxSpectators`):

| Option | Type | Default | Modes | Meaning |
|---|---|---|---|---|
| `?AdvantageMaxDuration` | int s | `300` (min 60) | CTF/iCTF | Max flag‑advantage period before the match force‑ends. |
| `?GracePeriod` | int s | `10` (min 3) | CTF/iCTF | Grace window after a flag event before advantage/OT logic acts. |
| `?HalftimeDuration` | int s | *(BP)* | CTF/iCTF | Halftime length (1v1/2v2 only; auto‑off for 3v3+). |
| `?MaxPoints` | int | *(BP)* | ShockDom | Number of control points to spawn. |
| `?GoalScore` | int | *(BP)* | ShockDom, Wipeout | Score target (re‑read by the plugin to keep the BP default when absent). |
| `?PugId` | int | `-1` | CTF/iCTF, Wipeout | Marks a bot PUG; enables auto‑pause‑on‑drop and `?PugTeams` pinning. |
| `?PugTeams` | string | *(empty)* | CTF/iCTF, Wipeout | Bot‑balanced roster (`<id>:<team>,…`, team 0=red/1=blue) — pins players to a side. |
| `?Captains` | string | *(empty)* | all NCPlus | Per‑team captains (`<id>,<id>`) allowed to pause; needs `[NetcodePlus] bAllowCaptainPause=true`. |
| `?BotApiUrl` / `?BotApiToken` | string | *(→ `[BOT_EVENTS]`)* | any (bot events) | PUG‑bot event sink + auth. |

Stock options the plugin **reads/honors** but doesn't parse itself (listed so you know they matter):
`?HostId` (host pause / scoreboard host badge), `?BalanceTeams` (false skips the Glicko rebalance),
`?RespawnWait` — **ignored for CTF**, which uses `[UTPUGS_STATS] CTFRespawnWait`/`CTFRespawnWaitSmall`
instead.

> Not parsed by the plugin (don't expect them to do anything NetcodePlus‑specific):
> `?RandomSubsetSize`, `?MapLockoutDuration`, `?WTR`.

---

## 8. Engine.ini / Game.ini server settings

These are stock UT4 server settings (not NetcodePlus), but they're part of running a good hub and
several interact with NetcodePlus features.

**`Engine.ini`:**

```ini
[/Script/UnrealTournament.UTGameEngine]
RconPassword=<your-rcon-pass>
ServerMaxPredictionPing=100          ; cap on server-side prediction (ms)

[/Script/OnlineSubsystemUtils.IpNetDriver]
NetServerMaxTickRate=120             ; hub tick rate — keep at 120
MaxInternetClientRate=72000          ; per-client net rate ceiling (the netspeed cap)
MaxClientRate=72000

; Replay server — REQUIRED for the CTF/iCTF end-of-match decisive-cap replay (§6.1)
[HttpNetworkReplayStreaming]
ServerURL="https://replayserver.azurewebsites.net/"
[NetworkReplayStreaming]
DefaultFactoryName=HttpNetworkReplayStreaming
```

- `MaxInternetClientRate` / `MaxClientRate` are the **server‑side ceiling** for the per‑gamemode netspeed
  set in the paks (§3.3). Keep them ≥ the value you set in the mode sections (72000 is standard).
- The end‑of‑match cap replay (`ncp.CTFReplay*`) records to a **replay server**; without
  `[HttpNetworkReplayStreaming]` configured, the replay can't play.

**`Game.ini`:** holds the pak `RedirectReferences` (§2.1), your `NoSpawnProtectionMutator` / gamemode
cache config, and the hub‑wide mutator list below.

### 8.1 Hub‑wide mutators — `ConfigMutators` (where StatSQL + ServerShield go)

UT4 has a built‑in hook for **mutators that run on every match**: the `ConfigMutators` array on
`AUTGameMode` (engine `UTGameMode.cpp::InitGame` adds each entry via `AddMutator`). This is the right
home for **StatSQL and ServerShield** — you want them on every game, not pasted into each ruleset. In
`Game.ini`:

```ini
[/Script/UnrealTournament.UTGameMode]
ConfigMutators=MutServerShield
ConfigMutators=MutStatSQL
```

- Short class names are enough — `MutStatSQL` / `MutServerShield`, no `ModuleName.` prefix (and no `+`
  needed in a single `Game.ini`).
- Add them here **once** instead of in every ruleset's `?mutator=` — and don't do both (avoid a
  double‑add).
- **Per‑ruleset tuning still works:** a ruleset can put `?Tournament=1` / `?SSFlagThreshold=…` /
  `?StatSQLKey=…` in its `gameOptions`; those URL options are read from the match URL no matter where
  the mutator was added.
- Remove the legacy `Global_StatSQL-v2` blueprint from your rulesets so stats aren't double‑reported.
- The hub/lobby gamemode isn't an `AUTGameMode`, so it won't run these — exactly what you want (no
  stats/anticheat on the lobby itself).

---

## 9. StatSQL — stats to ut4stats.com

**StatSQL** is the C++ mutator that snapshots every match and POSTs it to **ut4stats.com** (works for
every NetcodePlus mode). Class `AMutStatSQL`. **Add it hub‑wide as a `ConfigMutators` entry in
`Game.ini`** (§8.1) — `ConfigMutators=MutStatSQL` (short class name, no module prefix) — rather than in
each ruleset, and drop the legacy `Global_StatSQL-v2` blueprint to avoid double‑reporting.

Config under `[UTPUGS_STATS]` (Mod.ini), with launch‑option overrides:

| Mod.ini key | URL override | Default | Meaning |
|---|---|---|---|
| `Key` | `?StatSQLKey` | *(empty)* | API auth token. Sent as `Authorization: Token <Key>`. **Required to upload.** If you ran the old Global Stats mutator, this is already set — StatSQL reuses it. |
| `ServerName` | — | *(→ Game.ini ServerName)* | Server name reported with the match (used for correlation). |
| `SendStats` | `?StatSQLEnabled` | `true` | Master enable for uploads. |
| `Debug` | — | `false` | Verbose logging. |
| `AllowNameChange` | — | `true` | Allow players `mutate setname <name>` (rate‑limited ~60 s, profanity‑filtered). |
| — | `?StatSQLUrl` | `https://ut4stats.com` | API base URL override. |
| — | `?PugId` | `-1` | PUG id (set by the bot) so ut4stats can correlate the match to its PUG. |

> **Build codependency (important):** StatSQL **hard‑links ServerShield** at build time — it includes
> ServerShield's headers and reads its per‑hit data for the hitplot upload. Both binaries (`.dll`/`.so`)
> **must be deployed together**; a hub with StatSQL but no ServerShield `.so` will fail to load the
> module and can crash at boot. See §10.

---

## 10. ServerShield — behavioral anti‑cheat (brief)

**ServerShield** is a **server‑side** behavioral analytics plugin. It watches play, scores it across
several behavioral detectors, and **flags suspicious players for human review** — it writes a per‑match
report and fires a review event. It is deliberately **review‑only: there is no auto‑kick or auto‑ban
anywhere in the plugin.** Conviction is always a human looking at the report + a demo. (Detector
internals are intentionally not documented here.)

Class `AMutServerShield`. **Add it hub‑wide as a `ConfigMutators` entry in `Game.ini`** (§8.1) —
`ConfigMutators=MutServerShield` (short class name). Its per‑match behaviour is still tuned per ruleset
via the URL options below in `gameOptions`.

Admin surface — **URL options** (set per ruleset):

| Option | Default | Meaning |
|---|---|---|
| `?Tournament` | `0` | Stricter analysis for tournament play. |
| `?SSLogLevel` | `1` | Log verbosity 0–3. |
| `?SSAnalysisInterval` | `15` | Seconds between analysis cycles (5–120). |
| `?SSFlagThreshold` | `90` | Suspicion score (0–100) to tag a player for review. *(Omitting the option uses 90, not 50.)* |
| `?SSInstaCleanPct` / `?SSInstaCheatPct` / `?SSInstaMinShots` | `57` / `70` / `90` | Instagib‑only accuracy‑floor screen (clean %, cheat %, min shots). |

Admin **mutate commands** (rcon‑admin only): `mutate ss_status`, `mutate ss_analyze`,
`mutate ss_detail <PlayerName>`, `mutate ss_tournament_on`, `mutate ss_tournament_off`.

**Output:** per‑match files in `<UnrealTournament>/UnrealTournament/Saved/ServerShield/` —
`SS_<map>_<matchid>.txt` (summary), `.csv` (per‑player), `_timeline.csv`, `_hits.csv`. UTF‑8, not
auto‑pruned.

### 10.1 StatSQL ⇄ ServerShield are codependent — ship them together

As of the current builds the two plugins are **codependent**: StatSQL build‑depends on ServerShield
(§9), and ServerShield build‑depends on NetcodePlus. **Run all three together** — a server should have
NetcodePlus + StatSQL + ServerShield `.so`/`.dll` all present, or StatSQL won't load.

**Distribution:** ServerShield's repo is **private**, so server owners can't build it themselves. Both
StatSQL and ServerShield binaries are published **together** from the **StatSQL releases page** so
admins get a matched pair in one download:

**<https://github.com/jmortley/StatSQL/releases>**

Extract both `StatSQL/` and `ServerShield/` into your server's `Plugins/` directory (alongside
`NetcodePlus/`). Windows (Win64) + Linux dedicated‑server builds are included; debug symbols (`.pdb`)
are excluded.

---

## 11. Example rulesets (migrated from the NYC hub)

These show the **de‑stocked** form: NetcodePlus gamemodes, `dcHitsounds` (not `MutHitsounds`), no
`MutTeamSkins`, no `SetNetSpeed` (these native gamemodes set the net rate from `[ElimPlus] NetSpeed`). **StatSQL + ServerShield are not in these chains** — they run hub‑wide
via `Game.ini` `ConfigMutators` (§8.1). Keep whatever community quality mutators you already run
(`RemoveGrenadeLauncher`, `MutNSFF`, `MutNoESP`, `HiddenWeaponsUTPL`, …).

### 11.1 ElimPlus (Team Arena) — 4v4

```json
{
  "uniqueTag": "ElimPlusNCP_4v4",
  "categories": ["PUGS/Practice"],
  "title": "ElimPlus (NetcodePlus)",
  "toolTip": "PLUGIN REQUIRED",
  "mapPrefixes": [],
  "defaultMap": "/Game/Mogno/Maps/DM-Cheops-Mog/DM-CheopsRemastered",
  "maxPlayers": 8,
  "maxTeamCount": 2,
  "maxTeamSize": 4,
  "displayTexture": "Texture2D'/Game/RestrictedAssets/UI/GameModeBadges/GB_TDM.GB_TDM'",
  "gameMode": "/Game/Blueprints/ElimPlusStuff/ElimPlus.ElimPlus_C",
  "gameOptions": "?MaxPlayers=8?TimeLimit=30?GoalScore=10?ForceSpawn=true?BalanceTeams=false?MaxSpectators=2?MaxPlayerWait=65?ForceNoBots=1?mutator=ElimPlusMutator,dcHitsounds,MutNSFF,MutNoESP,RemoveGrenadeLauncher?FFPercent=-0.33?RFFMomentumEnabled=1?ForceWarmup=1",
  "requiredPackages": [
    "/Game/Blueprints/ElimPlusStuff/ElimPlus.ElimPlus_C",
    "/Game/Blueprints/ElimPlusStuff/ElimPlusMutator.ElimPlusMutator_C"
  ],
  "bTeamGame": true,
  "optionFlags": 65535,
  "bHideFromUI": false
}
```

Set the net rate in Mod.ini: `[ElimPlus]` → `NetSpeed=72000`.

### 11.2 iCTF (NetcodePlus CTF + instagib) — 5v5

```json
{
  "uniqueTag": "5v5iCTFNCP",
  "categories": ["PUGS/Practice"],
  "title": "5v5 iCTF (NetcodePlus)",
  "mapPrefixes": ["CTF"],
  "defaultMap": "/Game/CTF-Acrony/CTF-Acrony-v08",
  "maxPlayers": 10,
  "maxTeamCount": 2,
  "maxTeamSize": 5,
  "displayTexture": "Texture2D'/Game/RestrictedAssets/UI/GameModeBadges/GB_CTF.GB_CTF'",
  "gameMode": "/Game/Blueprints/Netcode/NCP-IGCTF.NCP-IGCTF_C",
  "gameOptions": "?TimeLimit=15?GoalScore=0?BalanceTeams=true?AdvantageMaxDuration=60?GracePeriod=5?mutator=MutInstagibNCP,dcHitsounds?Difficulty=0",
  "requiredPackages": [],
  "bTeamGame": true,
  "optionFlags": 65535,
  "bHideFromUI": false
}
```

CTF respawn + small‑game tuning is in Mod.ini `[UTPUGS_STATS]` (`CTFRespawnWait`, `CTFRespawnWaitSmall`,
`CTFSmallGameMaxPlayers`). For tournaments add `?Tournament=1` for ServerShield.

### 11.3 Wipeout — 4v4

```json
{
  "uniqueTag": "4v4WipeNCP",
  "categories": ["PUGS/Practice"],
  "title": "Wipeout (NetcodePlus)",
  "toolTip": "PLUGIN REQUIRED",
  "defaultMap": "/Game/Mogno/Maps/DM-Cheops-Mog/DM-CheopsRemastered",
  "maxPlayers": 8,
  "maxTeamCount": 2,
  "maxTeamSize": 4,
  "displayTexture": "Texture2D'/Game/RestrictedAssets/UI/GameModeBadges/GB_TDM.GB_TDM'",
  "gameMode": "/Game/Blueprints/ElimPlusStuff/WipeoutPlus.WipeoutPlus_C",
  "gameOptions": "?MaxPlayers=8?TimeLimit=30?GoalScore=4?ForceSpawn=true?BalanceTeams=false?MaxSpectators=2?ForceNoBots=1?mutator=WipeoutMutator,dcHitsounds,MutNSFF,RemoveGrenadeLauncher?FFPercent=-0.33?RFFMomentumEnabled=1?ForceWarmup=1",
  "requiredPackages": [
    "/Game/Blueprints/ElimPlusStuff/WipeoutPlus.WipeoutPlus_C",
    "/Game/Blueprints/ElimPlusStuff/WipeoutMutator.WipeoutMutator_C"
  ],
  "bTeamGame": true,
  "optionFlags": 65535,
  "bHideFromUI": false
}
```

### 11.4 Duel (League Duel) — 1v1

Use the **NetcodePlus League Duel** gamemode (not stock Duel) — a Duel subclass with the NetcodePlus
fair‑spawn picker + Glicko2 rating, referenced by its blueprint. The duel weapon set comes in via the
stock `WeaponReplacement` mutator + `NCWepMut`, with the first‑spawn weapon pair set by `?WTR=<A>:<B>`:

```json
{
  "uniqueTag": "PUG_DUEL_NCP",
  "categories": ["UT+ Pickup Games"],
  "title": "Duel (NetcodePlus League Duel)",
  "displayTexture": "Texture2D'/Game/RestrictedAssets/UI/GameModeBadges/GB_Duel.GB_Duel'",
  "gameMode": "/Game/Blueprints/Netcode/bp_NCLeagueDuel.bp_NCLeagueDuel_C",
  "bTeamGame": true,
  "maxPlayers": 2,
  "minPlayersToStart": 2,
  "gameOptions": "?BotFill=0?RequireFull=1?TimeLimit=10?MaxSpectators=3?GoalScore=0?WTR=/Game/RestrictedAssets/Weapons/GrenadeLauncher/BP_GrenadeLauncher.BP_GrenadeLauncher_C:/Game/RestrictedAssets/Weapons/BioRifle/BP_BioRifle.BP_BioRifle_C?mutator=WeaponReplacement,dcHitsounds,NCWepMut,NoSpawnProtectionMutator?AnalyticsLogged=true?ForceNoBots=1",
  "optionFlags": 65535,
  "bHideFromUI": false
}
```

Spawn distances are in Mod.ini `[NCLeagueDuel]`; net rate comes from `[ElimPlus] NetSpeed` like the
other NetcodePlus gamemodes (no `SetNetSpeed` — it's a native gamemode).

> For a **non‑instagib NetcodePlus CTF**, use your CTF subclass BP (concrete subclass of
> `ANCPlusCTFGameMode`) without `MutInstagibNCP`. ShockDom uses `SdomMutator` + the
> `/Game/Blueprints/Netcode/ShockDomGM.ShockDomGM_C` BP and honors `?MaxPoints` / `?GoalScore`.

---

## 12. Keeping a hub updated

The plugin's signed manifest lists the current build's URL + SHA‑256. These scripts read it, verify the
download, back up the old plugin, and extract the new one. They only touch the **NetcodePlus plugin** —
update StatSQL/ServerShield from their release (§10) when a matched pair is published.

**Ready‑to‑run copies live in [`tools/`](tools/):** [`tools/update-hub.sh`](tools/update-hub.sh) (Linux)
and [`tools/update-hub.ps1`](tools/update-hub.ps1) (Windows) — identical to the listings below.

> The canonical trust root is the launcher's minisign‑signed manifest. These scripts verify the
> **SHA‑256** from that manifest (fetched over HTTPS), which is enough for a server‑side convenience
> tool; they don't check the minisig. **Stop the server before running them** (a mounted DLL can't be
> replaced).

### 12.1 Linux (`update-hub.sh`)

```bash
#!/usr/bin/env bash
# Usage: ./update-hub.sh /path/to/UnrealTournament/UnrealTournament/Plugins
set -euo pipefail
PLUGINS_DIR="${1:?usage: update-hub.sh <.../UnrealTournament/Plugins>}"
MANIFEST="https://github.com/jmortley/netcodeplus-launcher/releases/download/updates-latest/manifest.json"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$MANIFEST" -o "$tmp/manifest.json"

read -r URL SHA VER < <(python3 - "$tmp/manifest.json" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]))["channels"]["stable"]["plugin"]
print(p["url"], p["sha256"], p["version"])
PY
)
echo "Latest NetcodePlus build: $VER"
curl -fsSL "$URL" -o "$tmp/ncp.zip"
echo "${SHA}  ${tmp}/ncp.zip" | sha256sum -c -

dest="$PLUGINS_DIR/NetcodePlus"
[ -d "$dest" ] && mv "$dest" "${dest}.bak.$(date +%Y%m%d%H%M%S)"
mkdir -p "$dest"
unzip -q "$tmp/ncp.zip" -d "$dest"
echo "Installed build $VER -> $dest . Restart the server."
```

### 12.2 Windows (`update-hub.ps1`)

```powershell
# Usage: .\update-hub.ps1 -PluginsDir "C:\...\UnrealTournament\UnrealTournament\Plugins"
param([Parameter(Mandatory)][string]$PluginsDir)
$ErrorActionPreference = 'Stop'
$manifestUrl = 'https://github.com/jmortley/netcodeplus-launcher/releases/download/updates-latest/manifest.json'

$tmp = Join-Path $env:TEMP ("ncp-update-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $m   = (Invoke-WebRequest -Uri $manifestUrl -UseBasicParsing).Content | ConvertFrom-Json
    $p   = $m.channels.stable.plugin
    Write-Host "Latest NetcodePlus build: $($p.version)"

    $zip = Join-Path $tmp 'ncp.zip'
    Invoke-WebRequest -Uri $p.url -OutFile $zip -UseBasicParsing
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $p.sha256.ToLower()) { throw "SHA-256 mismatch: expected $($p.sha256), got $got" }

    $dest = Join-Path $PluginsDir 'NetcodePlus'
    if (Test-Path $dest) { Rename-Item $dest "NetcodePlus.bak.$(Get-Date -Format yyyyMMddHHmmss)" }
    New-Item -ItemType Directory -Path $dest | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Write-Host "Installed build $($p.version) -> $dest . Restart the server."
}
finally { Remove-Item $tmp -Recurse -Force }
```

---

## Appendix A — client‑side settings (what players control)

These live in each **player's** Mod.ini and are set in‑game (F5 menu / `mutate` commands). Listed so
admins understand what's player‑side vs server‑side. None of these are server‑enforced.

- **`[NetcodePlus]`** (client): `StockBottomBar` (use stock HUD bar), `HighResScreenshotPostMatch`,
  `OwnFootstepVolume` (iCTF own‑footstep volume).
- **`[InstagibCTF]`** (client): `RagdollTime`, `bAllowGib`, `bShowRagdoll` (corpse/ragdoll behavior).
- **`[ForceModels]`** (client): `Enabled`, `Models`, `HUD`, `Armour`, `Flags`, `DarkenBodies`,
  `Cosmetics`, `Style`, `AllowAnyModel`, `HiddenModels`, `AllowModels`, `RecolorParams`,
  `SkipMaterials`, `BakedMaterials`, `HideTestModels`; per‑side `[ForceModels.Model.<Red|Blue>]`
  (`Class`, `H`/`S`/`V`, `Brightness`, `Complimentary`, `ArmourMode`); `[ForceModels.FlagWind]`
  (cloth animation). Migrated once from dc's legacy `[TeamSkins.Enable]`.
- **`[WeaponSkinsPlus]` / `[NetcodePlus.WeaponSettings]`** (client): `LGColor`, `ShockBeam`,
  `HitscanChoice`, per‑weapon `Skin.<tag>`, `Hide.<weapon>`, `HiddenBeamBack`/`HiddenBeamDown`.
- **`[NetcodePlus.Cosmetics]`** (client): saved cosmetic selections.
- **`[ClientHitsounds]` / `[Hitsounds.Enemy]` / `[Hitsounds.Friendly]`** — config for the *future*
  native C++ hitsounds port; **not the live path** (live hitsounds = `dcHitsounds` BP, `mutate
  hitsounds`).

## Appendix B — quick checklist

- [ ] A working UT4 hub first — master server + rcon + UTCC redirects (§1.0)
- [ ] Plugin DLL/.so in `Plugins/NetcodePlus/` (Windows + Linux) — §1
- [ ] All 6 paks reachable: launcher and/or `Game.ini` redirects — §2
- [ ] Stale `Mutator_AbsoluteHitSounds` + `MutTeamSkins` redirects removed — §2.2
- [ ] Rulesets: `dcHitsounds` (not `MutHitsounds`), no `MutTeamSkins`, no `SetNetSpeed` — §0, §11
- [ ] StatSQL + ServerShield added hub‑wide via `Game.ini` `ConfigMutators` (not per‑ruleset); legacy `Global_StatSQL-v2` removed — §8.1
- [ ] `[UTPUGS_STATS] Key=` set for ut4stats uploads — §9
- [ ] StatSQL **and** ServerShield binaries both deployed (codependent) — §10.1
- [ ] Net rate: `[ElimPlus] NetSpeed` for the NetcodePlus gamemodes, keep `SetNetSpeed` on stock+`NCWepMut`; `Engine.ini` ceilings ≥ that — §3.3, §8
- [ ] Replay server configured if you want end‑of‑match cap replays — §8
```
