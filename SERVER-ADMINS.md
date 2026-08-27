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

> Engine target: UT4 **4.15** (CL‑3525360). Current plugin build: **328**.
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

**Two weapon‑balance flavors — pick one.** There are two weapon‑replacement paks; both run on the *same*
NetcodePlus netcode and differ only in weapon **balance**:

- **`NCWepMut`** — the **NA‑style** competitive balance (the tuning the UTPugs / North‑American scene
  plays). The default choice for a NetcodePlus hub.
- **`NCStockWeapons`** — **stock UT4 weapon balance** on the NetcodePlus netcode, for admins / leagues
  that want Epic's stock feel with the better hit registration. Primary fire and the normal charged shots
  are **100% stock values**, just lag‑compensated. *Caveat:* the Rocket Launcher keeps the NetcodePlus
  behaviour (spiraling rockets + grenades), so RL is not bit‑for‑bit stock — everything else is.

Pick exactly one and add it like any other token (**don't run both**) — e.g. `?mutator=...,NCStockWeapons`
in place of `NCWepMut`.

Requires clients to run the NetcodePlus plugin (via the launcher) and to have the matching weapon pak —
`NCWepMut` **or** `NCStockWeapons` (both are launcher‑maintained; §2). Neither is auto‑added by anything —
you add the one you want per ruleset.

> **Example — stock CTF, now on NetcodePlus weapons** (keep your existing mutators, add `NCWepMut`):
> ```
> "gameMode": "/Script/UnrealTournament.UTCTFGameMode",
> "gameOptions": "?TimeLimit=20?GoalScore=0?mutator=NCWepMut,dcHitsounds,SetNetSpeed"
> ```
> `SetNetSpeed` still works here because the gamemode is stock (only the NetcodePlus gamemodes ignore it
> — §3.3). StatSQL + ServerShield run hub‑wide via `Game.ini` `ConfigMutators` (§8.1), not in the chain.

> ⚠️ **Known limitation on stock gamemodes — the in‑game menus open without a mouse cursor.**
> On a **stock** gamemode (TDM, DM, stock CTF, Duel…), the F5 menu — and `nchud`, `weaponskins`,
> the cosmetics picker — will appear but the cursor stays captured for camera look, so nothing is
> clickable. **Workaround: press `~` (tilde) to open the console, which frees the cursor; the menu is
> then fully usable.** Closing the console does not re‑capture it while the menu is open.
>
> Why: those panels ask the HUD to release the cursor, and only the **NetcodePlus HUDs** honour that
> request. A stock gamemode uses a stock HUD, which re‑captures the mouse every tick. Running any
> NetcodePlus **gamemode** (§4) has no such issue — the cursor works normally there. Weapons, netcode,
> hit registration and every server‑side feature are unaffected; this is purely the client‑side menu
> cursor. Players who only ever change settings from the main menu never see it.
>
> See **§6.3** for this and the other stuck‑cursor case (launcher join / map change with a menu open),
> both with the same `~` workaround — handy to have in one place when answering tickets.

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
**`NetcodePlus-328.zip`**.

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

~792 MB for the six core paks on a first pull.

> **Optional 7th pak — `NCStockWeapons`** (UTCC **2058**,
> <https://utcustomcontent.com/mutator/2058-NCStockWeapons>). `NCStockWeapons-WindowsNoEditor.pak` is the
> **stock weapon‑balance** alternative to `NCWepMut` (see Start Here) — a small weapon pak the launcher
> also keeps current, so launcher clients already have it; only hubs running a stock‑balance ruleset use
> it. Redirect‑based hubs: add it like the six above — its full `RedirectReferences=` line (with checksum)
> is on the UTCC page linked here.

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

#### Native hitsounds (new in 328) — optional replacement for `dcHitsounds`

328 ships a **native C++ hitsounds mutator** (`AClientHitsounds`, shown in menus as **"Hitsounds"**) that
can replace the `dcHitsounds` blueprint. Same 10 presets and the same damage‑pitched behaviour, plus:

- **Client‑side prediction** — the sound plays on the shooter's own machine at fire time instead of
  waiting a server round‑trip, so it feels tighter at range/ping.
- **Correctness fixes** over the blueprint: no more enemy cue on self‑damage in FFA, pellet weapons
  coalesce into one sound instead of one per shard, damage amps are compensated, and replay
  fast‑forward no longer bursts every recorded hitsound.
- **No content dependency** — it's compiled into the plugin. It reads the same cue assets the paks
  already ship.
- **Players can supply their own sounds** — drop a `UHitsoundPack` data asset (a `DisplayName` plus
  Low/Med/High sounds) into a pak and it appears in the list alongside the built‑ins; a custom pack
  wins a name clash. This is entirely client‑side and needs **no server support or approval**.

**Admin action (optional):** swap the token — use **`ClientHitsounds`** *instead of* `dcHitsounds`, not
alongside it. Both declare the mutator group `Hitsounds`, so UT will refuse to load the second one;
without that guard you'd get every hitsound twice. Players configure it at **F5 → Hitsounds** (or
`ncpmenu hitsounds`).

> Staying on `dcHitsounds` is perfectly fine — it still works on 328. The native one is opt‑in.
> Config keys live under `[ClientHitsounds]` / `[Hitsounds.Enemy]` / `[Hitsounds.Friendly]` in each
> **player's** Mod.ini (appendix); nothing about hitsounds is server‑configured, and no hitsound
> settings ever cross the network.

### 3.2 `MutTeamSkins` → Force Models (client‑side)

Team skins / forced enemy models are now a **client feature** of NetcodePlus called **Force Models**.
It is *purely client‑local* — there is **no server mutator and no `?mutator=` token**, and it is a no‑op
on a dedicated server. Players open it with **F5** or `ncpmenu forcemodels`.

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
| `ClientHitsounds` | **New in 328.** Native C++ hitsounds — optional replacement for `dcHitsounds` (§3.1). Adds client‑side prediction + custom `UHitsoundPack` support. **Use one or the other**, never both (same `Hitsounds` mutator group) | server (compiled in, no pak) |
| `MutInstagibNCP` | Instagib weapon set (makes a CTF mode iCTF) | server (external BP pak) |
| `NCWepMut` | NetcodePlus weapon replacement — turns **any** gamemode into NetcodePlus weapons (see Start Here) | server (pak) |
| `NCStockWeapons` | Same, but **stock** UT4 weapon balance on the NetcodePlus netcode (the stock‑balance alternative to `NCWepMut`; RL keeps NCP spirals + grenades). Run one or the other, not both | server (pak) |
| `MutStatSQL` | Stats upload to ut4stats.com — §9 | server |
| `MutServerShield` | Behavioral anti‑cheat — §10 | server |

There is **no token** for: Force Models (client‑only), the version gate (auto), Warmup Roam (auto),
host/captain pause (the in‑game `pause` command, gated by Mod.ini), or the concede vote (auto — see
below).

> **Concede vote ("gg vote") — new in 328, on by default, no token.** The losing team can end a lost
> match early: a player types **`gg`** (or F1 to confirm / F4 to cancel) and it passes at **>50% of that
> team's humans** (bots don't count). The leading team is told a vote is brewing rather than being
> surprised by a sudden end. Server‑authoritative. Worth knowing before your first "why did that match
> end early?" ticket — and worth mentioning to league admins, since a team can now forfeit a match that
> would previously have been played to the clock.
>
> **To turn it off for a ruleset, add `?AllowConcede=0` to its `gameOptions`** (§7). It is per‑ruleset,
> not per‑gamemode, which is what lets you disable it for **iCTF while leaving regular CTF alone** —
> those two are the same gamemode class, so only the ruleset can tell them apart. A disabled server
> replies "the gg vote is disabled on this server" in chat rather than leaving `gg` as a dead key.
> The setting is read once at map load, so it can't change under a match that is already running.

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
| `UnpauseCountdownSec` | int | `7` | server | "Resuming in N…" countdown before a host/rcon or CTF auto-pause resume takes effect (clamped 0–60; 0 = instant). |
| `VersionReportTimeoutSec` | float | `100.0` | server | Grace window for a joining client to report its plugin build (clamped 1–120). *Mismatch* kicks always fire; the *no‑reply* timeout kick is currently disabled. |
| `AnnouncerPack` | string | *(stock)* | **client** | **New in 328.** Player's chosen announcer voice pack (set at F5, not by admins). Listed here only so you recognise it in a player's Mod.ini — it is client‑side and never affects the server. |
| `ElimEnableAntiCamp` | bool | `true` | server | **ElimPlus only.** Enable the anti-camp watch (flags a player who holds a tight box). Detection is C++; the warn/response is Blueprint. |
| `ElimCampThreshold` | float | `400` | server | **ElimPlus.** Box radius/extent (uu) a player must stay within to be flagged. |
| `ElimCampCheckInterval` | float | `1.0` | server | **ElimPlus.** Seconds between camp position samples. `0` disables the camp timer entirely. |
| `ElimCampWarnCooldown` | float | `5.0` | server | **ElimPlus.** Minimum seconds between camp warnings for a player. |
| `ElimUnevenHealthScaling` | bool | `true` | server | **ElimPlus.** On uneven team counts, spawn the short-handed team tougher / the larger team softer. |
| `ElimUnevenHealthPct` | float | `5.0` | server | **ElimPlus.** Spawn-health swing per missing player (4v5 = ±5%, 4v6 = ±10% …), capped ±50%. |
| `ElimMidGameShuffle` | bool | `true` | server | **ElimPlus.** Re-shuffle teams by PPR on an exact 6-0 blowout (non-PUG + `?BalanceTeams` only; once per match, applied at the next round). |

### 5.2 `[UTPUGS_STATS]` — CTF rating, respawn, auto‑pause, ELO upload

Shared section (StatSQL reads `Key`/`ServerName`/`SendStats` here too — see §9). All server‑side.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `CTFRespawnWait` | float | `1.5` | Regulation respawn delay (s) in CTF (fractional supported). |
| `CTFRespawnWaitSmall` | float | `1.0` | Respawn delay (s) in small games (≤ `CTFSmallGameMaxPlayers`). |
| `CTFSmallGameMaxPlayers` | int | `2` | At/below this player count a CTF match is "small" (2 = 1v1; raise to 4 for 2v2). |
| `AutoPauseOnDrop` | bool | `true` | Immediately auto‑pause a bot PUG (`?PugId`) when a participant drops. Once all awaited IDs return (or an unpause is requested), resume through `UnpauseCountdownSec`; another tracked drop cancels that countdown. |
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

CTF/iCTF uses a two-stage, NewCTF-style server-side selector above `SpawnSystemThreshold`.
Each team's authored starts are shuffled once. Primary scans that rotating queue and takes
the first start clear of enemy proximity/vision, teammate proximity/vision, nearby flags,
the recent-use tail, the last killer, and NCP's flag-carrier/robbed-base protections. A start
is moved to the queue tail only after a pawn successfully spawns there; preview choices do
not consume it.

If every primary candidate is blocked, secondary chooses the non-cycle start with the
greatest capped weighted distance from all living players. Teammates contribute less and
enemy flag carriers contribute more. If secondary is disabled or no tagged team start is
available, Epic's selector is used.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `SpawnUseNewCTFSelection` | bool | `true` | Master switch. `false` restores the pre-port weighted/tie-band selector. |
| `SpawnSystemThreshold` | int | `4` | Use Epic's selector at or below this many connected competitors. Spectators do not count; dead players and bots do. |
| `SpawnEnemyHardRadius` | float | `1200` | An enemy this close blocks primary regardless of visibility. `0` = off. |
| `EnemyLOSBlockRange` | float | `3000` | An enemy with clear LOS inside this range blocks primary. Also remains the legacy LOS-scoring range. |
| `SpawnFriendlyBlockRange` | float | `150` | A teammate this close blocks primary. |
| `SpawnFriendlyVisionBlockRange` | float | `150` | A teammate with clear LOS inside this range blocks primary. |
| `SpawnFlagBlockRange` | float | `750` | An enemy flag carrier or an unheld home/dropped flag this close blocks primary. |
| `SpawnMinCycleDistance` | int | `1` | Number of most recently used team starts excluded from both primary and secondary. |
| `SpawnExtrapolateMovement` | bool | `true` | Project remote players by half RTT (capped at 250 ms RTT) for distance/LOS checks. |
| `SpawnSecondaryEnabled` | bool | `true` | Use the weighted-distance fallback when primary finds no safe start. |
| `SpawnSecondaryMaxDistance` | float | `2000` | Cap each player's distance contribution to a secondary candidate. |
| `SpawnSecondaryOwnTeamWeight` | float | `0.2` | Secondary distance multiplier for teammates. |
| `SpawnSecondaryCarrierWeight` | float | `2.0` | Secondary distance multiplier for enemy flag carriers. |
| `SpawnEnemyBelowZ` | float | `190` | An enemy at least this far *below* a start counts as floor‑separated: no proximity penalty, and it clears the hard radius above when he also has no line of sight. `0` = off. |
| `SpawnKillerAvoidRadius` | float | `2500` | Last killer inside this range blocks primary. `0` = off. |
| `SpawnFlagCarrierLOSAvoidRadius` | float | `3500` | Extends the primary LOS exclusion specifically for the enemy carrying your flag. `0` = off. |
| `SpawnRobbedBaseAvoidCount` | float | `2` | Size of the nearest-own-base set whose one rotating member is blocked while your flag is out. Kept as a float for compatibility; fractional values are truncated. |
| `LogSpawnChoices` | bool | `false` | Log primary/secondary route, block counts, selected start, and actual spawned location. |

Legacy-only controls used when `SpawnUseNewCTFSelection=false` remain supported:
`SpawnWeightedRandom`, `SpawnRandomBase`, `SpawnRandomSpread`, `SpawnTieBandWidth`,
`SpawnFreshnessBonus`, `SpawnFreshnessWindow`, `SpawnFlagVicinityRadius`,
`SpawnRecentPenaltyMultiplier`, `SpawnNearLastRadius`, `SpawnNearLastPenalty`,
`FlagCarrierSpawnPenalty`, `DroppedFlagSpawnPenalty`, `FlagCarrierLOSPenalty`,
`EnemyBlockRange`, `EnemyBlockPenalty`, `EnemyLOSPenalty`, `FlagBaseProximityRadius`,
and `FlagSpawnPenaltyRadius`.

This section is loaded once when live play first starts. Change `Mod.ini` before the
next map/match; halftime does not reload it. On maps that swap sides, the team queues
are rebuilt after the swap so their authored team assignments stay correct.

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
| `ncp.HitscanFudgeMs` | `10` | Full‑RTT buffer subtracted before halving the server‑observed RTT for hitscan target rewind. `10` places the primary sample 5 ms newer than half RTT; set `20` for the previous behavior. Hitscan-only: projectile catch-up/delayed-fake timing still uses its existing weapon property. |
| `ncp.HitscanPrimaryPadding` | `40` | Extra radius (uu) granted to the specifically client‑claimed **moving** target at the primary hitscan epoch. Stationary primary padding remains the weapon's `HitScanPaddingStationary` value. |
| `ncp.HitscanSearchPadding` | `40` | Extra radius (uu) at every claimed-target ±15/30/45 ms time-search rung. Set `45` for the previous behavior. |
| `ncp.UnclaimedRenderGate` | `1` | Server rejects a hitscan hit the shooter's own client never claimed, by reconstructing what that shooter actually had rendered. This is the "gifted shots at high ping" fix. Shipped to production **on 327** (backported), not new in 328. **Leave at `1`** — if honest aim is being demoted, widen `ncp.UnclaimedRenderSlack` rather than disabling the gate. |
| `ncp.UnclaimedRenderSlack` | `20` | Extra tolerance (uu) the render check allows before demoting a hit. Raise if live logs show demotes clustered on honest body aim. |
| `ncp.HitAttribRenderExtraMs` | `30` | Extra time (ms) added to the exact-hitscan render-position estimate (`half RTT + extra`). Used only by the unclaimed Shock/Sniper render gate and hit-attribution telemetry; it no longer tunes Link/Minigun. This remains an estimate, not a client-supplied frame timestamp. |
| `ncp.RenderCredit` | `1` | Legacy name for render-authoritative targeting on opted-in, claimless fire (NCP Link beam and Minigun primary). At `1`, the estimated **render-time capsule replaces raw rewind history as the sole target-selection sample**; it is not `raw OR render`. Ray, spread, world clipping, and timing estimate remain server-owned. `0` is the live rollback to raw-rewind-only behavior. Link/Minigun use server ACK-derived RTT rather than trusting client-reported `ExactPing`; pawn damage fails closed during the connection's brief RTT/history warm-up. With `ncp.HitAttribDebug=1`, `[RenderAuthority]` accepted-target lines appear under `Log LogUTWeaponFix Verbose`; this is high-volume for a held beam. |
| `ncp.RenderCreditExtraMs` | `15` | Link/Minigun-only presentation-delay estimate added beyond half the server-observed RTT. Kept separate from `ncp.HitAttribRenderExtraMs` because claimless continuous/spread fire uses the client proxy-prediction path. Lower values sample a newer target position; changing this does not move the Shock/Sniper unclaimed-render epoch. |
| `ncp.RenderCreditSlack` | `0` | Extra radius (uu) around the render-authoritative capsule. Keep at `0` unless evidence shows the server-side render estimate needs spatial tolerance. |
| `ncp.SlideGraceMs` | `250` | **New in 328.** Window after a floor slide starts during which validation accepts the standing capsule. A slide shrinks the server capsule instantly, but the shooter still sees a standing body for one replication interp plus the anim blend — without this, shots through the visible torso were server-side air. `0` = off (pre-328 behaviour). |
| `ncp.HitAttribDebug` | `0` | Per‑shot hit‑attribution telemetry (`[HitAttrib]` log lines). Diagnostic only — **high volume on a populated server**, leave `0` unless investigating a specific report. |
| `ncp.ShockServerTickHz` | `0` | Server shock‑core tick rate (0 = 240 Hz; >0 = that Hz, 30–720; read at spawn). |
| `ncp.CTFReplayMinDemoSeconds` | `200` | Min server‑demo age before the end‑of‑match decisive‑cap replay fires (0 = always — not advised; can crash the killcam in an early match). Requires a replay server (§8). |
| `ncp.CTFReplayBuildupSeconds` | `8` | Seconds of run‑up shown before the featured cap. |
| `ncp.ShockDebug` | `0` | Shock‑core diagnostics logging (0 off, 1 events). |

> **328 render-estimate migration:** if an existing server set
> `ncp.HitAttribRenderExtraMs=15` specifically to tune Link/Minigun, restore that
> key to `30` (or remove the override) and use `ncp.RenderCreditExtraMs=15`.
> Leaving the old override at `15` now moves only the unclaimed Shock/Sniper
> render check and its attribution telemetry.

> The rocket trio `MaxWindowMs` / `GraceMs` / `MaxPingMs` are meant to be kept **matched** (defaults
> 200/200/150). Bumping `ut.RocketLagCompMaxPingMs` to 150 is the live fix for high‑ping no‑reg.

> **Compiled default (not a cvar):** the server‑side hitscan **time‑search fallback** — the ±window the
> server sweeps after primary capsule rewind to catch a client‑claimed hit inside the jitter window — is
> now **±45 ms in fixed 15 ms rungs (±15 / ±30 / ±45), uniform across every weapon** (it was 30 ms base,
> with shock the only 45 ms exception). It's baked into the build; there is no runtime knob for it.

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
| `ncp.CrossModeRetry` | `1` | Re‑fire a held primary that landed mid‑cycle after a shock ball / cross‑mode switch (fixes the "hold M1, nothing comes out" beam stall). `0` = legacy drop (kill‑switch). |
| `ncp.GhostFix` | `0` | **Leave at 0 — do not enable.** Parked held‑fire‑across‑switch experiment; the current version breaks consecutive held weapon switches. A pawn‑level v2 is pending. |
| `ncp.FireDebug` | `0` | Client fire‑input diagnostics: `1` logs every StartFire / StopFire / retry decision (traces the held‑M1 beam stall). Pure logging, no behaviour change. |
| `ncp.ShockConverge` | `1` | Client fake→real shock‑ball convergence interp (the ~700 ms, 60 uu‑capped pull of the rendered fake toward the real). `0` = fake renders its own predicted path. |
| `ncp.ShockHandoff` | `1` | Client stuck‑ball handoff: when the real ball stops, destroy the fake and reveal the real so the shooter sees the stop. `0` = no reveal. |
| `ncp.WarmupSpawns` | `1` | Warmup‑only spawn‑point markers (team‑colored, facing tick + distance) in CTF / iCTF, as a learning aid. `0` = off. |

### 6.3 Known issues players will report — "my mouse is stuck / the menu won't click"

Two **different** bugs share one symptom and one workaround. Neither loses progress, neither affects
gameplay, hit registration or anything server‑side, and in both cases:

> **Workaround: press `~` (tilde).** Opening the console frees the mouse cursor. The menu is fully
> usable afterwards, and closing the console does not re‑capture it.

| What the player did | What happens | Why |
|---|---|---|
| Opened **F5** (or `nchud` / `weaponskins` / cosmetics) on a **stock** gamemode — stock TDM, DM, stock CTF, Duel… typically while running `NCWepMut` / `NCStockWeapons` on a stock ruleset | Menu draws, but the cursor stays captured for camera look, so nothing is clickable | Those panels ask the HUD to release the cursor and only the **NetcodePlus HUDs** honour it. A stock gamemode runs a stock HUD, which re‑captures the mouse every tick. **Not an issue on any NetcodePlus gamemode** (§4). |
| **Joined from the launcher** (`-ncpconnect`), or the server changed map, **while the front‑end menu was open** | A "ghost" main menu is left behind: cursor stuck, `Esc` does nothing, input dead | A stock UE4/UT ordering race — the deferred menu close is dropped when the loading movie finishes on the same event the map load rides. A fix exists but was pulled during unrelated crash triage and is **deliberately not in 328**; the `~` workaround stands. |

If a player reports either, the answer is the same one line: **press `~`**. Worth pinning in your
hub's Discord/rules channel — it's the single most common "the game is broken" ticket that isn't.

---

## 7. Launch / URL options (ruleset `gameOptions`)

The `?Key=Value` tokens NetcodePlus itself parses (in addition to stock UT4 options like `?TimeLimit`,
`?GoalScore`, `?BalanceTeams`, `?MaxSpectators`):

| Option | Type | Default | Modes | Meaning |
|---|---|---|---|---|
| `?AdvantageMaxDuration` | int s | `300` (min 60) | CTF/iCTF | Max flag‑advantage period before the match force‑ends. |
| `?AllowConcede` | bool | `1` (on) | all NCPlus team modes | `0` / `false` / `no` (any casing) disables the **gg concede vote** for that ruleset — see §4.2. Absent or unrecognised = **on**, so a typo can't silently disable it. This is the per‑ruleset switch, and it is the only way to separate **iCTF from regular CTF**: they are the same gamemode class (`ANCPlusCTFGameMode` + the instagib mutator), so nothing keyed on the mode itself can tell them apart. |
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
download, back up the old plugin **outside `Plugins/`**, and extract the new one. They only touch the **NetcodePlus plugin** —
update StatSQL/ServerShield from their release (§10) when a matched pair is published.

**Ready‑to‑run copies live in [`tools/`](tools/):** [`tools/update-hub.sh`](tools/update-hub.sh) (Linux)
and [`tools/update-hub.ps1`](tools/update-hub.ps1) (Windows) — identical to the listings below.

> **Why the backup lands in a sibling `PluginBackups/`, not next to the plugin:** UE4 scans *everything*
> under `Plugins/` for `.uplugin` files. A leftover `NetcodePlus.bak.*` **inside** `Plugins/` is a second
> copy of the plugin — on Linux the scan order is arbitrary, so the stale backup can be discovered first
> and **shadow the live plugin** (you'll see a `second location will be ignored` line in the log and the
> server silently keeps running the OLD DLL). The scripts move the old build to
> `<UnrealTournament install>/UnrealTournament/PluginBackups/` (a sibling of `Plugins/`), sweep any legacy
> in‑`Plugins/` backups out on the way, and keep the two most recent. Older copies of these scripts kept
> the backup in `Plugins/` — if you ran one, the next run relocates that stray for you.

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
# Backups must live OUTSIDE Plugins/: UE4 scans everything under Plugins/ for
# .uplugin files, and on Linux directory order is arbitrary — a leftover
# NetcodePlus.bak.* INSIDE Plugins/ can be discovered first, shadowing the real
# plugin ("second location will be ignored") so the server silently runs the
# OLD DLL. Same failure class as the launcher's client-side .old leftovers.
backup_root="$(dirname "$PLUGINS_DIR")/PluginBackups"
mkdir -p "$backup_root"

# Sweep legacy in-Plugins backups left by older versions of this script.
for old in "$PLUGINS_DIR"/NetcodePlus.bak.*; do
  [ -d "$old" ] && mv "$old" "$backup_root/" && echo "Relocated legacy backup: $(basename "$old")"
done

[ -d "$dest" ] && mv "$dest" "$backup_root/NetcodePlus.bak.$(date +%Y%m%d%H%M%S)"
mkdir -p "$dest"
unzip -q "$tmp/ncp.zip" -d "$dest"

# Keep the two newest backups, prune the rest.
ls -1dt "$backup_root"/NetcodePlus.bak.* 2>/dev/null | tail -n +3 | xargs -r rm -rf

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
    # Backups must live OUTSIDE Plugins\: UE4 scans everything under Plugins\
    # for .uplugin files — a leftover NetcodePlus.bak.* inside it can shadow
    # the real plugin and the server silently runs the OLD DLL.
    $backupRoot = Join-Path (Split-Path $PluginsDir -Parent) 'PluginBackups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

    # Sweep legacy in-Plugins backups left by older versions of this script.
    Get-ChildItem -Path $PluginsDir -Directory -Filter 'NetcodePlus.bak.*' -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "Relocated legacy backup: $($_.Name)"; Move-Item $_.FullName $backupRoot }

    if (Test-Path $dest) { Move-Item $dest (Join-Path $backupRoot "NetcodePlus.bak.$(Get-Date -Format yyyyMMddHHmmss)") }
    New-Item -ItemType Directory -Path $dest | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dest -Force

    # Keep the two newest backups, prune the rest.
    Get-ChildItem -Path $backupRoot -Directory -Filter 'NetcodePlus.bak.*' |
        Sort-Object Name -Descending | Select-Object -Skip 2 | Remove-Item -Recurse -Force

    Write-Host "Installed build $($p.version) -> $dest . Restart the server."
}
finally { Remove-Item $tmp -Recurse -Force }
```

---

## Appendix A — client‑side settings (what players control)

These live in each **player's** Mod.ini and are set in‑game (F5 menu / `mutate` commands). Listed so
admins understand what's player‑side vs server‑side. None of these are server‑enforced.

- **`[NetcodePlus]`** (client): `StockBottomBar` (use the stock bottom HUD bar instead of the NCPlus one),
  `StockTeamPanel` (stock slanted team‑roster panel instead of the portrait strip), `ScoreboardOpacity`
  (scoreboard background alpha, default `0.3`, clamped 0.05–1.0), `HighResScreenshotPostMatch`,
  `OwnFootstepVolume` (iCTF own‑footstep volume). The three HUD toggles are written by the in‑game HUD
  editor (`nchud`), which also repositions stock widgets — including the **killfeed**, which now holds its
  custom spot mid‑match. A fresh install with no saved layout defaults to the stock bar/panel.
- **`[InstagibCTF]`** (client): `RagdollTime`, `bAllowGib`, `bShowRagdoll` (corpse/ragdoll behavior).
- **`[ForceModels]`** (client): `Enabled`, `Models`, `HUD`, `Armour`, `Flags`, `DarkenBodies`,
  `Cosmetics`, `Style`, `AllowAnyModel`, `HiddenModels`, `AllowModels`, `RecolorParams`,
  `SkipMaterials`, `BakedMaterials`, `HideTestModels`; per‑side `[ForceModels.Model.<Red|Blue>]`
  (`Class`, `H`/`S`/`V`, `Brightness`, `Complimentary`, `ArmourMode`); `[ForceModels.FlagWind]`
  (cloth animation). Migrated once from dc's legacy `[TeamSkins.Enable]`.
- **`[WeaponSkinsPlus]` / `[NetcodePlus.WeaponSettings]`** (client): `LGColor`, `ShockBeam`,
  `CustomShockBeam` (gate set by the F5 shock‑colour picker so the weapon reads the custom `ShockBeam`
  at spawn), `HitscanChoice`, per‑weapon `Skin.<tag>`, `Hide.<weapon>`, `HiddenBeamBack`/`HiddenBeamDown`.
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
