# Expanded spectator slideout and flag brightness

Source implementation, September 8, 2026. C++ compilation and in-game verification are pending; no content pak was edited or cooked.

## Spectator slideout

F5 → Home → Spectator & Caster → **Expanded spectator slideout** (enabled by default). Save applies the preference immediately. It is stored as `[NetcodePlus] ExpandedSpectatorSlideout=True` in the client's `Mod.ini`.

Open and close the normal spectator slideout as before. Its roster uses 684 design pixels in regular CTF, 596 in iCTF (seven additional columns), or 528 in Wipeout (four additional columns). The expanded slideout renders at 90% of its previous scale, including text, icons, controls and click targets. Camera, flag-view and powerup controls retain their stock layout at that scale. Click anywhere in a player row to follow that player; clicking the selected player opens the existing weapon-stat detail panel. Stock keyboard bindings, camera controls and spectator-window lifecycle remain in use.

The wider presentation requires a **true spectator** (`bOnlySpectator`) in CTF/iCTF or Wipeout. Eliminated players keep the original roster and team visibility rules. Other game modes and HUD subclasses use the stock presentation. An explicitly configured third-party slideout is respected.

| CTF / iCTF | Meaning / source |
|---|---|
| CAP | Captures, replicated PlayerState |
| GRAB | Flag grabs, existing CTFStatsReplicator |
| RET | Returns, replicated PlayerState |
| K/D | Match kills / deaths; kills exclude assists |
| EFF | Kills / (kills + deaths), zero before any kill/death |
| LG% / IG% | Sniper + Lightning Rifle accuracy in regular CTF; instagib rifle in iCTF, matching the existing CTF scoreboard |
| SCORE | Replicated PlayerState score |

| Wipeout | Meaning / source |
|---|---|
| K/D | Match kills / deaths; kills exclude assists |
| DMG | Match damage, existing WipeoutDamageReplicator |
| DMG/L | Match damage / (deaths + 1), integer-truncated |
| SCORE | Replicated PlayerState score |

Health/armor remain live in regular CTF and Wipeout. iCTF omits both values and their header icons and closes the space; its dead/out label uses the weapon-icon area. Wipeout's queued respawn is shown in seconds; other dead/out-of-lives states are labelled. The player row includes a flag-carrier marker or current-weapon icon. Team colours and the selected-player highlight remain visible.

Unavailable replicated values display `-`, and accuracy displays `-` until there are shots. Authority-side PlayerState values provide the supported listen/standalone fallbacks. Existing damage/CTF replicators omit players without a UniqueNetId; remote bot values can therefore be unavailable. No new stats are fabricated and no replicated field, RPC or serialization format is added. UT99's flag-carrier-kill/cover columns are not reproduced: they are not in these existing client snapshots.

Match totals, string formatting and stat-cell font measurement are cached at 5 Hz; their underlying server snapshots retain their existing cadence. Player-name and spectator-number text and measurements are reused until the value or font changes, with immediate invalidation ahead of the stat refresh gate. Live vitals, respawn interpolation and pointer feedback render each frame. Replicator discovery is throttled, cached actors/players are weak references, and world changes clear the match caches. Clicks recheck current roster membership and `CanSpectate` before using the stock `ViewPlayerNum` path.

The input eligibility check does not require `Canvas`: stock `PostDraw` clears that pointer before Slate dispatches mouse clicks. Drawing checks it separately. The initial extension shared that check and rejected row clicks after drawing; this corrects that lifecycle error. `GetDrawScaleOverride` applies the 10% reduction during stock `PreDraw`, so stock camera hitboxes and expanded row hitboxes share the rendered scale.

Wipeout now registers the existing NCPlus slideout subclass, including its carried-loadout weapon panel and replicated accuracy. Stock `ShouldDraw` still opens/closes the interactive spectator window; the extension never bypasses that input bootstrap.

## Flag brightness

F5 → Force Models → **Flag brightness**. Range **1–5**, default **2**. Requires Force Models and Flags to be enabled. Stored as `[ForceModels] FlagBrightness=2.0` in the client's `Mod.ini`. A value of 1 restores the previous recolouring intensity; the control is independent of player-model glow.

The stock `M_CTF_Flag` material's connected `FlagColor` vector contributes to both albedo and emissive output. The new multiplier scales its RGB channels and preserves alpha. It is applied from the configured base colour every update, so brightness does not compound over successive ticks. The material's `EmissiveNear`/`EmissiveFar` parameters exist on a disconnected expression branch; setting them would not affect the inspected material.

Only material slots exposing `FlagColor` are touched. The pole is not recoloured, and cloth simulation, meshes, materials, collision and server state are not replaced. Existing stock emissive masks, lighting and exposure still apply; this is not a conversion to an unlit flag material. Save takes effect through the existing approximately 4 Hz colour update. New flags/material instances after capture or return are picked up automatically.

Original material colours are cached with weak references. Disabling Flags or Force Models restores each managed colour, unless another renderer has since written a different value; expired instances are discarded. Values are clamped and non-finite config values fall back to the default.

## Verification

Completed: source review against the local UT4/UE4.15 method signatures, stock slideout input/draw paths, existing stat-replication producers, and the stock flag material's serialized expression graph; whitespace/diff checks. No C++ build, editor compile/cook, dedicated-server run, or visual game test was performed.

After compiling the client:

1. Join regular CTF, iCTF and Wipeout as a true spectator. Open/close the slideout, click the name and last column, click the selected row for weapon details, and exercise 1P/3P, X-Ray, Auto Cam, flag views, powerup views, keyboard selection and ESC.
2. Compare totals against the scoreboard; verify no-shot/missing snapshots, a late join, a disconnect, map travel and replay seeking. Check 1080p, 1440p and 720p with long names and a full roster. Confirm iCTF has no HP/armor header or values, dead labels fit, and row/camera clicks follow the reduced scale.
3. Toggle the F5 preference off/on and save. Join as an active player and die in Wipeout; confirm the expanded opponent table never appears. Check a non-Wipeout mode that inherits WipeoutHUD and a configured custom slideout.
4. Set a magenta flag on a dark/brown map. Compare brightness 1, 2 and 5 near/far, carried/dropped, and after capture/return. Toggle Flags and the master switch off/on, change team/style, and travel maps. Confirm original colour restoration and unchanged cloth/pole/collision.

These changes require an updated client DLL. Existing compatible 328 servers already publish the consumed stats; an absent snapshot remains visibly unavailable. This source change does not fix or recook the separately identified Wipeout Blueprint self-destruction defect.
