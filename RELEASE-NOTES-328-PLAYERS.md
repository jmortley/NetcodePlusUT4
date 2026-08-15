# NetcodePlus 328 — Smoother Fights, Fewer Lost Shots

NetcodePlus 328 is a major gameplay and performance update focused on making UT4 feel more reliable
during fast, high-ping fights. It fixes several long-standing weapon failures, improves hit validation,
adds new customization, and removes a large amount of repeated HUD and rendering work.

> **Update required:** 328 changes networking shared by clients and servers. Launch through the UT4
> Launcher and install the update before joining a 328 match. Mixed 327/328 matches are unsupported.
> Being disconnected for an old version is an update prompt, not a ban—you can rejoin as soon as you
> update.

---

## Better Shooting and Weapon Reliability

- Shots against a freshly sliding player now validate against the mostly-standing posture that is still
  visible on your screen.
- Several Rocket Launcher failures were fixed, including swallowed clicks, stuck charge states, and
  loaded volleys failing to release.
- Minigun primary no longer stalls or drops to an abnormally slow server fire rate.
- Re-pressing Minigun during spin-down no longer silently fails.
- Inputs received during weapon equip, retries, clock resynchronization, or rapid press/release sequences
  are less likely to disappear.
- Shots are no longer rejected for several seconds after a match is unpaused.
- Rockets and flak shells retain the correct hit-registration context after you switch weapons.
- The Link Gun beam has a new server-authoritative NetcodePlus firing path while retaining responsive
  first-person effects.
- Destroying a projectile or detonating a Shock core no longer incorrectly counts as a weapon accuracy
  hit. Some displayed accuracy percentages may be slightly lower, but they are now more truthful.
- The iCTF beam appears immediately on the prediction frame and regains its intended thickness at high
  ping.

---

## Movement and Match Flow

- Fixed the hard stick that could catch a dodge on BSP slope seams and convex edges.
- Team modes now support a concede vote by default. Use console command `gg` to start or join a vote,
  F1 to confirm an existing vote, and F4 to withdraw. Only the losing team can concede, and a strict
  majority of its human players is required.
- Servers can optionally enable UTComp-style player ready-up through F5.
- Mid-round ElimPlus joiners and reconnecting players no longer receive an unrestricted free camera.
- Helmet Small Armor now absorbs one headshot by default in supported modes. Wipeout is unaffected
  because it removes Helmet pickups.
- Candy orbs no longer jam lifts and can no longer be shot.

### CTF and iCTF

- Larger matches receive less repetitive weighted spawn selection.
- Overtime respawns now ramp from two seconds to a ten-second cap over time.
- Auto-pause has a visible resume countdown instead of resuming instantly.
- A new disconnect during the countdown cancels the pending resume.
- Dropped iCTF flags are handled more safely around lifts instead of immediately returning when blocked.
- Flag-taken announcements now play on re-grabs.

### Wipeout

- Sudden-death respawn grace is now one second instead of three.
- **FINAL LIFE!** warns when your next death will leave no time to respawn.
- Permanently eliminated players see `X` instead of a countdown that can never finish.
- The scoreboard replaces Eff% with V/S and HEAL.
- Thigh pads remain available, and Vest-to-Shield-Belt substitution now works reliably.
- Siphon placement has been improved to avoid spawning too close to Amp.
- Extreme telefrag and overkill values no longer distort round performance calculations.
- Wipeout now lets the winning-kill replay finish before ending the match, fixing an intermittent
  post-match crash.

---

## Weapon Skins, Colors, and Sound

### Weapon skins

- Weapon skins are enabled and visible to other players, spectators, and viewers of dropped weapons.
- Multi-material weapons such as Flak and Lightning Gun now skin the correct sections.
- Open **F5 → Home → Weapon Skins**, or use the `weaponskins` command.
- Some skins require the optional NetcodePlus content package. Missing optional choices are hidden and
  the stock appearance remains available.

### Custom weapon colors

- Shock beam and core color
- Shared Sniper and Lightning Gun beam color
- Link beam color

Custom colors are deliberately local to your first-person view. Other players, spectators, and replay
playback continue to see stock colors.

### Native hitsounds

Open **F5 → Hitsounds** for:

- separate enemy and teammate sounds;
- volume and pitch controls;
- Absolute, UTComp, and Flat pitch styles;
- optional zero-damage teammate cues;
- live test buttons.

Hitsounds now provide continuous damage-to-pitch feedback, combine a flak spread into one useful sound,
handle amplified damage correctly, and preserve posthumous trade-hit confirmation. An occasional brief
double sound can still occur when the server corrects a prediction to a different audible tier.

### Optional announcers

Five optional voices are supported:

- UT2004 Male
- UT2004 Female
- UT2003
- Classic UT
- Sexy

The selector appears on **F5 → Home** when the optional announcer package is installed. Stacked rewards
such as a multikill followed by a spree are now preserved instead of one announcement deleting the
other.

---

## Smoother Frame Times

328 includes extensive caching and rate-limiting work across HUDs, scoreboards, armor overlays, outlines,
weapon screens, and materials.

- Repeated scoreboard actor searches, sorting, string formatting, and font measurement are cached.
- QuickStats health and armor arcs reuse their geometry instead of rebuilding it every frame.
- Armor material updates are event-driven rather than rewritten for every visible pawn every frame.
- Armor and shield overlays stop drawing beyond 6500 units.
- Shock and Link weapon displays are capped at 30 updates per second rather than following uncapped FPS.
- Shock ammo material values update only when the displayed values change.
- F5 defers asset discovery for tabs you have not opened.
- TacCom and model-outline cleanup runs less often without losing its recovery coverage.

This work targets frame-time consistency and FPS lows in busy matches, while spectating, and with large
scoreboards open. It is optimization rather than an engine renderer replacement, so it should not be
read as a guaranteed increase to average FPS on every system.

---

## Other Fixes and Changes

- Bright forced-player models keep their intended team-color brightness.
- TacCom/X-ray outlines survive character-model swaps more reliably.
- Armor tint updates correctly when your viewing team changes.
- Spectator and caster joins no longer perform a synchronous rating-database lookup on the game thread.
- The displayed match clock stays aligned more closely with server-side pickup timers in supported NCP
  modes.
- Launcher authentication no longer exposes the one-shot login code through the initial command line,
  logs, or crash-report snapshot. This requires the matching launcher update.

---

## Current Known Issues

- Hitsound prediction correction can occasionally produce a brief double sound.
- Shock's separate low-ammo warning color is currently missing; normal ammo count and weapon behavior
  are unaffected.
- Switching from an Invisible Link skin back to a normal skin may leave the weapon's small displays stale
  until the weapon is re-equipped.
- The existing stuck-cursor/ghost-menu issue remains. The console-key workaround is unchanged.

---

Thanks to everyone who tested, supplied traces, reported edge cases, and repeatedly broke the release
candidate in useful ways. The goal of 328 is simple: make the game respond more consistently without
changing what makes UT feel like UT.
