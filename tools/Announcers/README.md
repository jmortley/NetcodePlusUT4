# UT3 announcer import

The optional pack ID and selector label are `UT3`. Its class is
`/Game/NetcodePlusOptional/Announcers/UT3/BP_NCPAnnouncer_UT3.BP_NCPAnnouncer_UT3_C`.

The local UT4 editor content contains 108 SoundWaves: all 58 reward recordings
from the base game and expansion, plus 50 selected status recordings for
DM, CTF, rounds, and countdowns. Warfare/campaign status, expansion Leviathan
lines, and unidentified alternate recordings are excluded.

`BP_NCPAnnouncer_UT3` derives from UTAnnouncer (copied from the existing
UT2004 Male variant). Its explicit arrays contain 76 reward keys and 81 status
keys, including exact spoken-name aliases. UT4's stock audio paths and prefixes
remain the fallback for calls without a matching UT3 recording.
The one-kill status key is `DM_Kills1`.

`/Game/Blueprints/Netcode/MutAnnouncers` references the new class as the sixth
AnnouncerArray entry. All five existing announcers and every other class
default were preserved. Both Blueprints compiled and saved without diagnostics.
Every imported wave's duration, sample rate, and channel count matched its WAV.

## Local content and audit

Editor project:
`C:/GDriveUT4/LAEditorUT4/UnrealTournamentEditor/UnrealTournament`

New assets live under `Content/NetcodePlusOptional/Announcers/UT3`.
The updated cooking anchor is `Content/Blueprints/Netcode/MutAnnouncers.uasset`.
These editor assets are separate from this source repository.

The run directory is `Saved/NetcodePlus/Announcers/UT3/20260912-import01` under
the editor project. It contains the extracted OGG files, converted WAVs,
`import-manifest.json` with hashes and lookup mappings, `import-results.json`,
and `asset-verification.json`. `Backup` contains the original MutAnnouncers
asset and its class-default dump.

## Preparing another fresh import

Use UModel's sound export on the local UT3 installation's English packages:
`Sounds/INT/A_Announcer_Reward.upk`, `Sounds/INT/A_Announcer_Status.upk`, and
`UT3G/Sounds/INT/A_Announcer_UT3G.upk` beneath `UTGame/CookedPC`.
Export into a new run directory's `Extracted` folder, preserving UModel's
package/SoundNodeWave subdirectories.

Run `prepare_ut3_announcer.py` with `--run`, `--sox`, `--content` (the target
UT4 Content folder), and `--ut3-cooked` (the source UTGame/CookedPC folder).
It uses SoX to create 16-bit PCM WAVs while retaining sample rates and channels.
It does not normalize loudness, import assets, compile code, or cook a PAK.
It refuses existing target assets and staged WAVs; use a fresh staging directory
and unpopulated target content when reproducing the preparation.

Import the manifest's WAVs with overwrite disabled. Set the announcer's
RewardAudioList and StatusAudioList from the manifest mappings, retain its
stock fallback fields, and append its class to the cooking anchor.
Compile, save, and verify both Blueprints and all asset references.

## Release validation still required

The C++ selector entry is prepared on `328-release-candidate` in its isolated
worktree. No C++ build, PAK cook, deployment, or in-game listening test was run.
Build that branch and recook MutAnnouncers into the announcer PAK before
checking UT3 selection, multikills, sprees, CTF calls, countdowns, and stock
fallback calls in the game.
