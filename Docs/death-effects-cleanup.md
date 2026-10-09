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
existing Show Ragdoll, Darken Bodies and iCTF Ragdoll Time behavior; visible corpses
retain their current lifetime.

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

Hidden corpse cleanup is automatic under the existing Show Ragdoll, Ragdoll Time
and Darken Bodies settings. There is no additional cleanup checkbox.

The blood switch leaves living-character hit effects and already-created decals
alone. Death decals normally bypass the damage type's bCausesBlood flag. These are
local rendering/lifetime changes and require the updated client plugin; installing
only the server binary does not apply them to existing clients.

## Diagnostic console overrides

| CVar | Default | Values |
| --- | --- | --- |
| `ncp.HiddenCorpseCleanup` | `1` | `1`: clean up eligible hidden bodies; `0`: retain stock corpse lifetime for comparison. |
| `ncp.DeathBloodDecals` | `-1` | `-1`: follow F5; `0`: force new death/corpse blood decals off; `1`: allow stock decals regardless of F5. |

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
