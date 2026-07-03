# Contributing to NetcodePlus

Thanks for wanting to help. NetcodePlus is a community UT4 plugin, and there's room to
contribute whether or not you write C++ — a lot of the most visible work (custom
characters, HUD styles, maps) is **content** that needs no code build at all.

There are three ways in, with very different setup:

| Path | What you do | Setup needed |
|------|-------------|--------------|
| **Report a bug** | File an issue with a repro | None |
| **Contribute content** | Custom characters, HUD layouts, maps | The UT4 editor + the prebuilt NetcodePlus editor DLL — **no source build** |
| **Contribute code** | C++ features / fixes | Build the plugin as part of the UT4 game — **needs UT4 source access** |

If you're not sure which fits, or you get stuck at any step, ask — the maintainer
([phantaci](https://github.com/jmortley)) is happy to walk you through setup 1:1 for now.

---

## Reporting bugs

Open a [GitHub issue](https://github.com/jmortley/NetcodePlusUT4/issues). There's no
template yet, so please include:

- **Mode + server** — e.g. ElimPlus on a specific hub, or a local listen/standalone game.
- **What happened vs. what you expected**, and **steps to reproduce** if you have them.
- **Plugin version / build** — the number after "NetcodePlus" in your client, or which
  hub you were on.
- **Client log** if it's a crash or something visual — `Saved/Logs/` in your UT4 install.

Server-side behaviour and cvars are documented in [SERVER-ADMINS.md](SERVER-ADMINS.md) if
you're testing on your own hub.

---

## Contributing content (no source build required)

This is the low-barrier path. You do **not** need the game source or a compiler — you
need the UT4 editor with NetcodePlus loaded, and then you author assets like any other UT4
content.

### 1. Get the editor with NetcodePlus loaded

1. Install the **UT4 Editor** (the Community Launcher's *Add-ons* tab installs it, or use
   any UT4 editor build you already have).
2. Put this repo at `<Editor>/UnrealTournament/Plugins/NetcodePlus/` — same layout as a
   normal install (`NetcodePlus.uplugin`, `Source/`, `Binaries/`).
3. The prebuilt editor DLL ships in the repo: `Binaries/Win64/UE4Editor-NetcodePlus.dll`
   (+ `.pdb`). Because it's already built, the editor loads NetcodePlus **without
   compiling anything** — its classes (custom characters, HUD widgets, etc.) are just
   available.
4. Open the editor. If NetcodePlus classes show up in the content browser / class picker,
   you're set.

### 2. What you can make

- **Custom playable characters** (ForceModels-compatible). NetcodePlus can force team
  models client-side, so community characters that follow the UT4 team-material convention
  recolour and ragdoll correctly. Authoring checklist:
  - Body material instances: turn the **`Use Team Colors` static switch ON** (a character
    shipped with it OFF won't recolour and must be recooked).
  - Name the **head / face / eye / hair** material instances so they auto-skip recolouring
    (the skip list matches `head, face, eye, hair, teeth, tongue, mouth, brow`). Rename a
    slot *out* of that set if you want it recoloured (e.g. a helmet).
  - Set **`DMSkin Type = EDMSkin_Base`** (neutral base skin), **`bHideInUI = false`**, and a
    `base_3p` skeleton with `SkeletonMesh` assigned.
  - **Assign a PhysicsAsset** (the stock `base_3p` one is fine) or the character stands
    upright on death — ragdoll needs physics bodies.
  - Fix facing on the content BP's Mesh component (`RelativeRotation` Yaw `-90` matches
    stock) so the model doesn't render 90° off.
  - **Cook it as a content pak** — it mounts `Content/` → `/Game` and is **client-side
    only**. The launcher distributes paks, so you ship a `.pak`, not source.
- **HUD layouts & styles.** NetcodePlus has an in-game HUD editor (`nchud` console
  command) with a JSON layout you can share via the editor's Copy/Paste buttons
  (`Saved/NetcodePlus/HUDLayout.json`). Example layout and the style mockups live in
  [`Docs/HUDLayout/`](Docs/HUDLayout) and [`Docs/HudMockups/`](Docs/HudMockups). New HP/
  armor style ideas or preset layouts are welcome as JSON + a mockup.
- **Maps** — standard UT4 map contribution; nothing NetcodePlus-specific required.

### 3. How content ships

Content paks are client-side and distributed through the Community Launcher, so a content
contributor's deliverable is a **cooked pak (+ source assets)** — no game source, no
compiler. The cook/pak step has a few UT4-specific gotchas; the maintainer will walk you
through it until it's documented here.

---

## Contributing code (build from source)

NetcodePlus is a single C++ **Runtime** module that compiles as part of the UT4 game.
There is no separate editor module — the one module builds into every target (editor,
client, server).

### Prerequisites

- **Access to the official UT4 source repository** (access-gated). This is required to
  build any of the C++ — the plugin links against UT4's game modules. Getting set up isn't
  something we can document publicly yet; **ask the maintainer and they'll help you get
  access and a working tree.**
- **Visual Studio 2017** (Windows).
- For the **Linux dedicated-server `.so`**: the UE4 Linux cross-toolchain (clang 3.9.0 /
  centos7) with the `LINUX_MULTIARCH_ROOT` environment variable pointing at it
  (e.g. `C:\UnrealToolchains\v8_clang-3.9.0-centos7\`). This cross-compiles the Linux
  server from Windows — no Linux box needed.

The repo ships a curated set of prebuilt binaries (editor/client/server) so you can run and
do content work without compiling; other build output is gitignored. To change **code**,
you rebuild the affected artifacts yourself.

### Build steps

1. Set up the official UT4 source tree (run its `Setup.bat` once).
2. Clone this repo into `<UT4>/UnrealTournament/Plugins/NetcodePlus/` (the repo root **is**
   the plugin directory).
3. From the UT4 tree, run `GenerateProjectFiles.bat` (or right-click
   `UnrealTournament.uproject` → *Generate Visual Studio project files*).
4. Open `<UT4>/UE4.sln` in Visual Studio and build the target you need:

   | Target (config) | Produces |
   |-----------------|----------|
   | `UnrealTournamentEditor` (Development) | `Binaries/Win64/UE4Editor-NetcodePlus.dll` — for editing/testing |
   | `UnrealTournament` (Shipping) | `Binaries/Win64/UE4-NetcodePlus-Win64-Shipping.dll` — client |
   | `UnrealTournamentServer` (Win64 Shipping) | `Binaries/Win64/UE4Server-NetcodePlus-Win64-Shipping.dll` — Windows server |
   | `UnrealTournamentServer` (Linux Shipping) | `Binaries/Linux/libUE4Server-NetcodePlus-Linux-Shipping.so` — Linux server (cross-compiled) |

The Linux server is just the standard UBT Linux Shipping config from the same solution —
there is no hand-rolled `Build.sh` in the repo.

Layout: `Source/Public/` = headers, `Source/Private/` = impl (~60 pairs). Dependencies are
in [`Source/NetcodePlus.Build.cs`](Source/NetcodePlus.Build.cs). See the README's
[Building from source](README.md#building-from-source) and **Development notes** for the
full picture.

### Coding conventions

These are load-bearing — several exist because breaking them **crashes stock clients** or
poisons the UE4 4.15 unity build. The README's *Development notes* has the exhaustive list;
the must-follows:

1. **UE containers only — no STL / iostream in gameplay code.** Use `TArray` / `TMap` /
   `FString`, never `std::vector` / `std::string` / `<iostream>` (they pull `<Windows.h>`
   on MSVC and poison the PCH chain). The one vendored library (Tron's TeamGlicko-2) uses
   `std::` internally but is confined behind a Pimpl and included in a single `.cpp`;
   `FElimPlusRatingSystem` is the pattern to copy if you ever vendor something.
2. **`#include "NetcodePlus.h"` first.** PCH mode is `UseExplicitOrSharedPCHs`, so
   `NetcodePlus.h` (or `UnrealTournament.h`) must be the first include in every `.cpp`, and
   first in any plugin header that pulls in UT engine types, or unity-bundle reshuffles
   trigger cascade errors.
3. **Never subclass a replicated engine class** (e.g. `AUTGameState`). The class-identity /
   ABI mismatch crashes stock clients on level load. For replicated game-mode state, spawn a
   separate replicated `AInfo` actor instead (the `*StatsReplicator` / `ANCVersionGate`
   pattern). Corollary: reach replicated engine members through their vtable-dispatched
   accessor — `GetFlagBase(idx)`, not `CTFGameState->FlagBases`. Bind delegates by
   `UFUNCTION` **name**, not by member-function pointer.
4. **Know your change's net-safety class** (see below).
5. Client-only code (rendering, force-models, hitsounds) early-returns on
   `NM_DedicatedServer`; periodic client re-assertion runs on the shared ~4 Hz slow-tick
   ticker, not per-frame; pawn/PlayerState caches use `TWeakObjectPtr`.

### Net-safety: will my change need a version bump?

`NETCODE_PLUGIN_VERSION` (a single `#define` in `NetcodePlus.h`) is compiled into both
client and server; the version gate kicks mismatched clients. **Classify every change
before you PR:**

- **Touches replicated data / adds a replicated class / RPC / `UPROPERTY`** → bump
  `NETCODE_PLUGIN_VERSION`, and it **must ship client + server together**.
- **Pure server-side logic** → server-only, no bump, no client roll.
- **Pure client-render / client-prediction** → client-only, no bump.
- **Behaviour-only cvar gate that touches no replicated state** → no bump.

When in doubt, say so in the PR — getting this wrong either silently breaks compatibility
with live hubs or needlessly forces everyone to update.

---

## Branch & PR model

- **`dev`** — active development. Branch off `dev` and open your PR against `dev`.
- **`main`** — stable releases; `dev` merges here at release boundaries.
- Keep PRs focused (one feature/fix), and note the net-safety class of the change.
- The maintainer reviews and integrates. **Releases are signed and shipped by the
  maintainer** through the launcher's update manifest — contributors don't cut releases, so
  you don't need to touch versioning/signing infrastructure.

You'll see other branches (`feature/*`, `hotfix/*`, dogfood/build branches) — those are
maintainer working branches; `dev` is the one to target.

---

## License

NetcodePlus's original source is licensed under the **[Apache License 2.0](LICENSE)**. By
submitting a contribution, you agree that it will be licensed under the same terms
(inbound = outbound).

The license covers the plugin source authored for this project. NetcodePlus builds against
Epic Games' UT4 / Unreal Engine 4 code, which is governed by the
[Unreal Engine EULA](https://www.unrealengine.com/eula) — this license grants no rights to
Epic's code. Third-party components (the vendored Glicko-2 library, © Tron, used with
permission) retain their own copyright; see [NOTICE](NOTICE).

---

## Related projects

- **[ClientDemos](https://github.com/jmortley/ClientDemos)** — client-side demo recording (separate plugin).
- **StatSQL** — match-stats database integration feeding ut4stats.com (separate plugin).
