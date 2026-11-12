# Client death effects and corpse cleanup

The inspected NCP-IGCTF defaults use IGCharacterFootsteps and N+InstagibRifle. Its
NCPlusUTDmg_Instagib damage Blueprint calls CleanUpRagdoll at the configured
RagdollTime, but the character Blueprint's event execution output is disconnected.
The existing C++ fallback hides the mesh; hiding alone leaves the corpse's physics,
skeletal updates, components and Blueprint timers alive. Stock visible-death cleanup
first checks after 15 seconds. This describes the inspected editor assets, not a
verification of every deployed pak.

## C++ behavior

ATeamArenaCharacter now follows its existing corpse visibility deadline with
deferred cleanup on online clients. No Blueprint changes are required. It uses the
existing Show Ragdoll and iCTF Ragdoll Time behavior; visible corpses retain their
current lifetime. Darken Bodies never hides a body (see below).

Cleanup requires a dead/torn-off pawn whose mesh remains explicitly invisible. It
waits while a local controller possesses or views the pawn, either side of a camera
blend references it, a carried-object component remains attached, or the stock
death sound is still queued. The sound is queued for 0.25 seconds and survives
owner destruction once playback starts. With RagdollTime=0.1, the body still hides
after 0.1 seconds; deletion waits for audio and any other dependencies to clear.
Retries run every 0.1 seconds without adding work to the character's frame tick.

Normal Destroy teardown removes the corpse's physics, components, weapon
attachments and actor timers. Early cleanup does not run on dedicated servers,
listen servers, standalone worlds or replay playback. While a client records a
replay, cleanup waits for the recording channel to close after its final tear-off
update. It never forces a flag detach or changes a camera target.

The initial death/ragdoll setup still happens. This change targets continuing work
from hidden corpses; its effect on frame times requires an online comparison.

## F5 controls

Use **F5 -> iCTF -> Gore Settings -> Show Death Blood**, then **Save**. The checkbox
defaults to checked and persists `[InstagibCTF] bShowDeathBlood` in `Mod.ini`. Save
refreshes the cached preference immediately; Cancel, Escape and closing with F5
discard unsaved checkbox changes. The decal path uses the cached value, so it does
not repeatedly read config while bodies collide.

Hidden corpse cleanup is automatic under the existing Show Ragdoll and Ragdoll Time
settings. There is no additional cleanup checkbox.

### Darken Bodies (2026-10-15)

**F5 -> Home -> Force Team Models -> Darken Bodies** (with Enable Force Models), then
**Save**. A body keeps its ragdoll but loses its live-player highlight at death: every
team-colour parameter its materials expose drops to the same hue at low saturation and
value, and the `Emissive Max` / `Emission Power` highlight goes to zero (eyes included).
Faces, eyes and hair keep their colours. Teammates and enemies are treated alike, as are
forced models, tint-only sides and natural models.

The work is one pass over the body's existing material instances when it dies: no
material is created and nothing runs per frame. Only parameters a material has and that
actually change are written. A team change, viewer team change or F5 Save no longer
repaints a corpse; the F5 Save re-skin skips dead bodies entirely. A body-material rebuild
after death (in any mode) is darkened again on the next tick. Any overlay a body keeps
(armour, or a powerup such as UDamage) is dimmed the same way, including your own body
on a listen host or offline. A body that dies during spawn protection also drops its
full-body flash.

Two things are deliberately left alone. The stock hit flash from the killing blow still
plays: it is kill feedback, it only writes `HitFlashColor`, and it clears itself when its
curve ends, leaving the body dark. A powerup body skin (for example invisibility, via
`ReplicatedBodyMaterial`) renders as designed while it lasts; the darkened body materials
come back when it ends.

Darken Bodies used to hide bodies, but only when Show Ragdoll had never been saved. The
F5 Save always writes Show Ragdoll, so it never took effect from the menu. Hiding is now
Show Ragdoll's job only: untick it to hide bodies about a second after death.

The blood switch leaves living-character hit effects and already-created decals
alone. Death decals normally bypass the damage type's bCausesBlood flag. These are
local rendering/lifetime changes and require the updated client plugin; installing
only the server binary does not apply them to existing clients.

## Diagnostic console overrides

| CVar | Default | Values |
| --- | --- | --- |
| `ncp.HiddenCorpseCleanup` | `1` | `1`: clean up eligible hidden bodies; `0`: retain stock corpse lifetime for comparison. |
| `ncp.DeathBloodDecals` | `-1` | `-1`: follow F5; `0`: force new death/corpse blood decals off; `1`: allow stock decals regardless of F5. |
| `ncp.DarkenBodiesValue` | `0.1` | Darken Bodies brightness: the value corpse team colours drop to (0 = black, clamped 0-1). Applies to fresh deaths. |

F5 edits the saved blood preference without changing a diagnostic console override.
After testing with `0` or `1`, use `ncp.DeathBloodDecals -1` to return control to F5.

Changing cleanup to 0 cannot resurrect corpses already deleted. Compare fresh deaths
after each setting change, and allow existing corpses to expire between captures.

## Validation

Source review checked UE4.15 timer teardown, camera blends, carried-object
attachments, delayed sound ownership and client replay tear-off ordering. No build
or runtime test was run; the user handles builds.

After building, verify on a dedicated-server connection:

- With RagdollTime=0.1 and cleanup enabled, nearby remote bodies hide promptly and
  death sounds remain audible. Compare fresh deaths with cleanup disabled.
- Die while carrying a flag; verify the dropped flag stays visible and can be picked
  up. Check own death camera and spectating a player through death and respawn.
- Repeat while recording a replay; verify recorded deaths and playback camera
  transitions still work. Playback itself retains stock cleanup.
- Uncheck Show Death Blood and Save, then check fresh deaths on a surface that
  normally receives blood. Existing stains should persist; new death/corpse stains
  should stop. Reopen F5 and restart the client to check persistence. Change the
  checkbox and Cancel to confirm unsaved changes do not apply.
- Verify the blood CVar at `0` and `1` overrides the saved checkbox, and `-1` follows
  it again. The checkbox should continue to show the saved preference.
- Keep the same cap, viewpoint and effects settings when comparing frame-time
  captures. The screenshots establish brief drops, not their allocation by subsystem.
- Darken Bodies on, with a bright forced enemy colour (high Glow): fresh enemy and
  teammate bodies turn dark and desaturated at death and stay that way. Living players
  keep their colours. Check a natural-model side (Enemy Only style) and a tint-only side.
- With a body on screen, switch teams and press F5 Save: the body must stay dark, and
  living players must re-skin as before.
- Darken Bodies off: bodies keep their live colours (previous behavior). Show Ragdoll
  unticked still hides bodies after about a second, with Darken Bodies on or off. Check
  Show Ragdoll both with the key absent from `Mod.ini` (bodies stay) and explicitly False.
- Tune `ncp.DarkenBodiesValue` (try 0.05 and 0.2) on fresh deaths.
- Listen host or offline: die while wearing armour (for example by a fall or suicide,
  which can keep the armour value). Your own body's armour shell must dim with it.
- Kills with a visible hit flash: the flash still plays, then the body is dark. Also a
  death during spawn protection: no lingering full-body glow.
- Bodies of players who had UDamage or invisibility: the overlay or skin looks sane.
- Replay: seek across a death and back; the body is dark after the seek.
- xTDM with Force Models on: bodies from all four teams darken, hues still distinct.
- With bodies on screen, untick Enable Force Models and Save: existing bodies keep their
  death-time look, and fresh bodies keep their live colours.
