# Carried AMP ambient sound

September 8, 2026. Source implementation; compilation and in-game listening checks are pending.

Stock `BP_UDamage` and the inspected `GamePlay_UDamage` Blueprint have damage-scaling and firing-sound logic, but no carried ambient-loop assignment. Siphon explicitly assigns its own loop from C++. This change restores the **hum on the AMP carrier**, using the stock `A_Powerup_UDamage_PowerLoop_Cue`. The cue's serialized graph connects its root to a looping node and then the mono AMP power-loop wave, and retains its authored attenuation and sound class.

`ATeamArenaCharacter` drives a separate, non-replicated audio component from the existing replicated weapon-overlay bits. It matches the exact stock AMP overlay material (or its legacy stock counterpart), rather than treating every powerup overlay as AMP. This works for relevant remote characters and spectators without requiring their owner-only inventory. The current `BP_UDamage` and `GamePlay_UDamage` defaults both resolve to the supported `M_UDamageSkin_3P` material. A custom AMP using an unrelated overlay is outside this fallback.

The component follows the character and stops when the AMP bit clears, the character dies, or the actor is destroyed/leaves the world. It does not write the character's weapon, flag, or Siphon ambient channels. An existing ambient assignment of the same stock cue suppresses the extra component to avoid doubling it. The cue loads only when a client first needs it; a missing asset produces one warning and disables this fallback for that process.

Only an updated **client DLL** is needed for this audio change. Servers already send the consumed overlay state. No replicated field, RPC, Blueprint edit, new sound asset, or pak recook is introduced. This does not change the sound at an uncollected pickup on the map.

Playing loops are not restarted every frame. If the audio system rejects/culls a voice, retries are limited to 4 Hz and tolerate replay time rewinds. A new pickup can start immediately.

## Validation

- Read the stock timed-powerup acquisition/removal paths and unconditional replication of `WeaponOverlayFlags`; verified the public overlay lookup and audio component APIs against the local engine source.
- Inspected live editor AMP defaults and exported the AMP/Siphon graphs read-only. Decoded the stock cue's serialized looping-node connections and wave reference.
- Source/diff checks only; no C++ build, pak cook, or live listening test was run.

After compiling, listen as the carrier, a nearby opponent and a spectator. Check pickup, expiry, drop/re-pickup, death, map travel, weapon switching and continuous fire. Pick up Siphon alone (no AMP loop) and both powerups together (independent loops); confirm the AMP hum ends independently. Check near/far attenuation and an existing server with the updated client.
