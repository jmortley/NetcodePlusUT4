# Custom Hitsounds — Creator Guide (NetcodePlus 328+)

NetcodePlus hitsounds are fully client-side: every player picks their own sounds,
volume, pitch and style in **F5 → Hitsounds**, with separate choices for enemy and
teammate hits and a preview button. Custom packs need **no server support** — they
work on every server, and nobody else hears yours.

There are two ways to get your own sounds in.

---

## The easy way — submit your sounds

You don't need the editor for this. Send in:

1. **1–3 short sound files** — one each for low / medium / high damage hits.
   One sound is fine (it will be used for all tiers).
   - WAV, 16-bit PCM, mono preferred
   - Short and punchy (under ~half a second), leading silence trimmed —
     any silence at the start *is* delay on your hit feedback
2. **A name** for the menu (this is permanent — see *DisplayName* below).

Submissions get built into the community hitsounds pak and delivered through the
launcher like any other content pak — your pack shows up in everyone's F5 menu.

> **Where to submit:** _(fill in: Discord channel / thread)_

---

## The creator way — build a pack yourself

### Prerequisites

A UT4 Editor install with the NetcodePlus plugin in it. The plugin zip already
contains the editor module, so:

- Copy `UnrealTournament\Plugins\NetcodePlus` from your **game** install into your
  **editor's** `UnrealTournament\Plugins\` folder, and restart the editor.
- (A future launcher update will do this for you from the Editor tab.)

You'll know it worked when **HitsoundPack** shows up in the Data Asset class picker.

### Steps

1. **Import your sounds.** Content Browser → **Import** → select your `.wav` files
   (16-bit PCM). Put them in their own folder to keep things tidy.

2. **Create the pack.** Content Browser → **Add New → Miscellaneous → Data Asset**
   → choose **HitsoundPack**.

3. **Fill in the fields:**

   | Field | What it does |
   |---|---|
   | **DisplayName** | The name shown in the menu — and the identity your selection is saved under. Pick something unique and **never rename it** (renaming resets every user's saved choice). Naming it the same as a built-in preset (case-insensitive) deliberately **replaces** that built-in — that's how you override "Default" with your own cues. |
   | **Low / Med / High** | Your sound for low / medium / high damage hits. Any slot may be left empty — gaps are auto-filled from the slots you did set. All three give the full damage sweep on the Absolute style. Slots accept a SoundWave directly; a SoundCue is only needed if you want extra processing. |
   | **DefaultVolume** | Starting volume for users who pick your pack. **Set 2.0** to match the loudness of the built-in packs — the field defaults to 1.0, which will feel half as loud next to them. Users can adjust it afterwards. |
   | **DefaultPitch** | Starting pitch. Built-ins use 1.9. The engine clamps final pitch to 0.4–2.0. |

4. **Cook it into a pak.** The shipped game only loads cooked content — a raw
   `.uasset` will not work. Any pak that contains your HitsoundPack (and its
   sounds) is enough; the game discovers packs by **class** through the Asset
   Registry, so the content path inside the pak does not matter.
   The stock-editor route: reference your HitsoundPack from a small test level
   (e.g. a level blueprint variable) and use the editor's **Share** flow — the
   cook pulls in every referenced asset.

5. **Install and test.** Drop the pak in
   `Documents\UnrealTournament\Saved\Paks\MyContent\`, restart UT4, open
   **F5 → Hitsounds**. Your pack appears by its DisplayName; use the preview
   button to hear it at different damage values.

### Troubleshooting

- **Pack doesn't appear:** check the log for
  `Custom hitsound pack '<name>' has no sounds assigned — skipped.` — at least one
  of Low/Med/High must actually be set. If the catalog was empty at startup, the
  game rescans about every 30 seconds.
- **Pack appears but is silent/quiet:** raise the volume slider, and check your
  WAV isn't 24-bit/32-bit float — 4.15 wants 16-bit PCM.

---

## Reference — how damage turns into sound

Three styles (per-user setting, F5 → Hitsounds):

- **Absolute** — continuous pitch sweep across your Low/Med/High cues by damage
  dealt. The classic "musical" hitsound.
- **UTComp** — hyperbolic damage→pitch curve on a single cue.
- **Flat pitch** — one cue, fixed pitch, no damage feedback.

Damage range 1–190 maps across the sweep; final pitch is clamped to 0.4–2.0 by
the engine. Enemy and teammate hits are configured independently, each with its
own pack/volume/pitch.
