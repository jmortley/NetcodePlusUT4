# NetcodePlus Aim Trainer (UE 4.15 / 329)

An opt-in game mode with real animated UT character targets, three fixed
60-second scenarios, an in-game picker/results HUD and a shared UT4Stats top 10.
It does not enable aim assistance or replace the controller in other modes.

## Planned standalone map

Owner direction, 2026-10-06: expand
[Chatouille's DM-ChatoPractice1](https://utcustomcontent.com/map/759) for the
standalone trainer. UTCC lists version 1.1, released 2019-03-10. This is the
intended base for the future map pass; the current runtime room remains the
prototype while the scenarios are tested.

- Preserve the existing practice areas and credit Chatouille.
- Add dedicated areas for animated-character strafe tracking, headshots behind
  cover, and varied disappearing instagib targets.
- Reuse the scenario picker, timed rounds, results and shared UT4Stats boards.
  Offline practice must continue to work without a hub or website connection.
- Replace the arbitrary-map room placement with authored trainer locations and
  sightlines. Keep free practice separate from the fixed scored presets.
- If distances, cover, target motion or other scoring conditions change, use a
  new preset revision rather than mixing those scores with the prototype board.

Before map authoring, inspect the actual map and determine whether its editable
`.umap` and Blueprint assets are available. The listing and preview have been
reviewed; source assets and the map's internal practice logic have not. A cooked
download is not evidence that editable source is included. No map modification
or redistribution has been performed as part of recording this direction.

## Start practice

Build NetcodePlus for the UE 4.15 client and server. The native game class is:

```
/Script/NetcodePlus.NCAimTrainerGame
```

Open a cooked stock DM map with this game mode, for example from the console:

```
open DM-DeckTest?game=/Script/NetcodePlus.NCAimTrainerGame?Bots=0
```

When opening from another local game, clear inherited URL mutators explicitly:

```
open DM-DeckTest?game=/Script/NetcodePlus.NCAimTrainerGame?Bots=0?mutator=
```

The NCWepMut **content pak** is required for the weapons, but the NCWepMut
**mutator** is not needed. This mode chooses its own pawn and inventory.

Use a map that is installed on your client/server; `DM-DeckTest` is an example.
The mode creates its own enclosed practice room above the map. There is no new
map or Blueprint package to create. Its constructor references the required
stock arena and character content for cooking. **All scenarios require the
current NCWepMut weapon content pak on both client and server**, including
`/Game/Blueprints/Netcode/UTNPShaftLink`, `/Game/Blueprints/Netcode/UTNPSniper`,
`/Game/Blueprints/Netcode/UTNPLightningGun` and
`/Game/Blueprints/Netcode/N+InstagibRifle`. The trainer resolves these classes
after paks mount. Missing or incompatible weapon content blocks the selected
scenario with an explanation; tracking requires the real NCP beam-only Link
class and its firing state rather than substituting a sniper or empty weapon.

The first version supports **one trainee per server instance**. Use separate
instances for simultaneous players. Scores from approved instances share the
same UT4Stats leaderboard.

Click a scenario card or press **1**, **2**, or **3**. Press **Enter** or click
Start for the countdown. **F6** cancels an incomplete run and returns to the
picker. **Enter** retries from the results screen. **Escape** retains the normal
UT menu. Cancelled runs and disconnects do not submit partial scores.

Press **M** or click the movement button in the picker/results screen to enable
optional **movement practice**. Strafe along the room's left/right lane with
normal UT side dodges, jumping and crouching. Forward/backward movement and
forward/back dodges are disabled; side dodges stay aligned with the lane as
you turn your view. Room walls bound the lane. The option applies to all three
scenarios, survives retries and scenario changes, and cannot change mid-run.
Fixed-position aiming remains the default.

Movement-practice runs show local results but never submit to the existing
shared boards, including on an approved network host. Those boards compare
fixed-position runs; jumping/crouching changes the headshot cover challenge.
The HUD labels movement practice and the fixed-position leaderboard separately.

All three scenarios support offline standalone practice. A hub is not required.
Offline results remain on the results screen; only an approved network host
submits ranked scores. A directly connected dedicated server can be used for
network testing without a hub.

## Scenarios and scoring (revision 8)

| Scenario | Exercise | Score |
| --- | --- | --- |
| Link tracking | Hold either fire button to track a strafing, dodging and occasionally sliding character with the NCP Link beam. | Milliseconds of beam contact, up to 60,000. |
| Headshots | Hit wiggling character heads at five cover stations with the NCP Sniper or Lightning Gun. Body hits do not count. | 100 per confirmed headshot. Misses affect accuracy; expired targets do not deduct points. |
| Instagib pop-up | Shoot five moving pop-up characters and a persistent randomly dodging character in the open floor lane. Includes a head peek behind the low block and a forward slide on the high right platform. | 100 per hit, minus 25 per miss and expired pop-up, floored at zero. |

Tracking observes the real authoritative NCP Link beam's selected target, with
a maximum 30 Hz observation rate. It only credits intervals with beam contact
at both endpoints, and discards observation gaps longer than 100 ms. Tracking
accuracy divides beam-contact time by time firing, using the same sampling and
continuity rules for both clocks. Idle time changes neither accuracy nor score;
firing off-target lowers accuracy. Score remains total contact milliseconds
over the 60-second run. The two shooting scenarios use successful hits divided
by fired shots. Weapons retain their normal firing rhythm.

Revision 6 removes point deductions from headshot practice. Earlier revisions
could accumulate miss/expiry deductions while displaying zero, hiding points
from subsequent successful hits. Every confirmed headshot now adds 100 points;
the HUD shows live headshots, shots and expiries. Completed runs also log these
counts in standalone, allowing scoring complaints to be checked against actual
accepted hits. Instagib retains its existing miss/expiry point deductions.

Headshot practice honors the owning player's saved NCP hitscan choice:
`[WeaponSkinsPlus] HitscanChoice=LG` or `Sniper` in `Mod.ini`, selected through
the existing `weaponskins` menu. Selecting a scenario or starting/replaying a
run reads the preference again. The choice travels with that request to the
authority, including when joining a dedicated server. It cannot swap weapons
during countdown or an active run. The HUD identifies the equipped rifle.
Both shipped rifles have the same 1.3-second primary fire interval and 0.95
headshot scales, so they share this board. Scoring reads the equipped weapon's
own shot counter and headshot damage type, including LightningRifleShots and
the Lightning Gun headshot type. Missing LG content blocks the run rather than
silently substituting the sniper.

Tracking uses the existing beam's range, obstruction and server validation.
It does not normalize network latency; use low-ping hosts when comparing runs.
The target now starts 1000 units from the trainee, within the beam's range as
it strafes. Either fire button starts the same beam; releasing one button
while holding the other keeps it active. Plasma and link-pull are disabled by
the existing NCP Shaft weapon class. Targets remain alive and precision shot
counters stay zero in tracking submissions. Accepted damage plays the selected
NCP hitsound at most once per 0.12 seconds. Optional movement practice can still
move out of beam range, where normal misses earn no tracking credit.

The trainer Shaft honors its explicit saved show/hide choice. When absent, it
inherits the current `NCPLinkGun_C` choice, including classic beam offsets. The
separate legacy CSHD Link choice is not used as an alias.

Link tracking uses 75% short strafe holds (0.14–0.34 seconds) and 25% longer holds
(0.45–0.80 seconds), with grounded dodge attempts every 1.8–3.8 seconds. Native
UT dodge cooldowns and landing physics still apply. Ground reversals pause
during a dodge. This is randomized target movement, not a replay of a fixed path.

Revision 8 adds occasional lateral floor slides, scheduled 4–7 seconds apart.
Attempts blocked by grounding or the shared native dodge/slide cooldown retry
after 0.2 seconds. Slides turn inward near the lane edges, retain their direction
through the native 0.7-second duration, then return to normal strafing. They do
not start in the final second of a run. Instagib's forward slide is unchanged.

Accepted Link contact also uses UT's damage-type hit effects on the target,
including its body flash. The trainer's immortal TakeDamage override previously
skipped that native path. LastTakeHitInfo replicates the effects to remote
clients and plays them immediately in standalone, without health loss, armor
effects or knockback. Hiding a pooled target clears its old flash material.

Instagib introduces at most one pop-up every 1.00–1.10 seconds, with a maximum
of five timed pop-ups present. Each stays for 5.5–6.8 seconds, allowing five one-second
refire intervals plus aiming time. Initial spawns, hit replacements and expired
targets share that schedule; a hitch cannot create a catch-up burst. Slower
custom refire values scale the timings and make the run unranked.

A sixth target stays in the open foreground floor lane, making random short
strafes. Its first native dodge attempt is 0.20–0.55 seconds after spawning,
then every 1.15–2.10 seconds. UT's grounding and
dodge cooldown rules still apply; rejected attempts retry after 0.2 seconds.
This target has no expiry penalty and respawns on the next game tick after a
hit, independently of the pop-up schedule. It does not crouch. Its lane stays
clear of the platforms; it can briefly cross a background target's sightline.

Revision 7 makes the high right-hand platform target attempt one native UT
floor slide toward the trainee, 0.8–1.4 seconds after appearing. It spawns
farther back on the platform to leave room for the slide and capsule. The
slide uses normal UT crouch, collision, animation and replicated movement;
after its 0.7-second duration it resumes lateral wiggles at its new position.
It has no separate random crouch. A late attempt is skipped unless enough
time remains for the slide, posture transition and a full rifle refire
interval before target expiry or the run's end. Hits, hiding and reuse clear
the slide state, so a replacement starts standing with a fresh deadline.

Both shooting scenarios use brief 0.12–0.28-second A/D reversals at 220 units
per second. Each seat has a bounded movement range, with room for the capsule
and stopping distance on its platform or behind cover. Headshot targets remain
for 6.5 seconds. The popup layout adds a near-left floor position and a floor
position behind the central block. That block is 176 units high, and the target
on top is offset laterally to keep the rear head peek visible from the fixed
anchor. Movement practice naturally changes those sightlines.

Revision 4 widens only the instagib movement ranges by 10%. Each instagib
appearance has a 65% chance of one short native crouch, scheduled 1.5–3.5 seconds
after spawning and held for 0.25–0.45 seconds. It is skipped if too little
exposure remains for the crouch plus a full rifle refire interval and 0.1 seconds.
Targets behind cover can briefly disappear while crouched. Reappearing targets
start standing; headshot targets and Link tracking do not gain these crouches.

All three use the fixed target model/preset in revision 8. Custom models,
durations or difficulty settings need separate revisions before their scores
can be compared fairly. A server-accepted score is a practice result, not proof
that the player used no automation.

## Shared UT4Stats leaderboard

Reads use `/aimtrainer_leaderboard/?scenario=strafe&revision=8&limit=10` (also
`headshots` and `instagib`). The web page is `/aimtrainer/`. Each board shows one
best run per player. The game fetches a compact board outside active scoring;
it does not stream aiming samples to Django. Revised scenarios submit revision
8; Django retains revision-1/2/3/4/5/6/7 submissions and explicit older boards without
mixing their scores with the new difficulty or accuracy definition. Revision 4 onward
includes `fired_ms` in every result: measured firing time for tracking and zero
for precision scenarios. Older submissions retain their original payloads and
full-run tracking denominator. Apply migration `0066_nc_aimtrainer_fired_ms`
before restarting the updated Django web workers and enabling current hosts.
Revisions 5 through 8 need no additional migration; deploy revision-8 API support
before enabling revision-8 hosts. Rebuild trainer clients and servers together:
revision 8 adds the rifle choice to the trainer's menu/start RPCs and progress
snapshot. These classes are used only by the trainer game mode.

Score submission is an asynchronous server request to `/aimtrainer_entry/`.
The identity comes from the server's player state. Run IDs make retries
idempotent. Three attempts maximum, a 15-second timeout per request and bounded
backoff keep a website failure from blocking gameplay. The results HUD reports
whether an upload was confirmed; local practice remains usable without it.

On an approved server, use the existing server upload token in
`Saved/Config/Mod.ini`:

```ini
[UTPUGS_STATS]
Key=YOUR_EXISTING_SERVER_UPLOAD_TOKEN

[NCAimTrainer]
OnlineEnabled=True
ApiBaseUrl=https://ut4stats.com
```

Keep this token on the host. It is never replicated or included in a client
configuration shipped through the launcher. Standalone practice does not
submit ranked scores. The website must also approve the token owner's account
through its `AIMTRAINER_SERVER_USERS` setting. An ordinary valid API token alone
does not grant score-submission permission.

The accompanying Django change adds an isolated table and migration. Deploy
that code, run its migration, configure the allowed server account usernames,
and restart the web workers before expecting shared scores. Until then the
trainer reports that the online service is unavailable.

## Release verification

Implementation checks completed on 2026-10-06: the UE4.15 Win64 Development
Editor module compiled and linked; 14 native controller/scoring tests passed;
12 Django API/model/render tests and the additive migration's forward/reverse
checks passed. A pre-existing `FVector::Dist2D` use on 329 was replaced with
the equivalent UE4.15 `(A - B).Size2D()` expression to unblock compilation;
its 62 existing fire-anchor checks also passed.

An unattended editor startup was attempted but stalled in existing content
loading before reaching the trainer level. That is not a gameplay pass. This
checkout also lacks the NCWepMut weapon source assets, although the installed
NCWepMut pak contains them. Shipping builds, cooked content and a rendered
client/server playtest remain unverified.

Before enabling ranked hosts, test a packaged 329 client against a 329 dedicated
server. Confirm visible animated targets and cover, all three scoring rules,
miss/expiry handling, cancellation, disconnect/rejoin, and a repeated completed
run appearing only once online. Verify normal game modes are unaffected.
Editor compilation and algorithm/API tests do not replace this multiplayer
playtest or a cooked asset check.

### Startup crash repair (2026-10-06)

A packaged standalone launch reached weapon attachment creation before world
BeginPlay. The supplied minidump resolves to
`AUTWeaponAttachment::AttachToOwnerNative`, reading address `0x3e8`; stock UT
initializes the attachment's `UTOwner` in BeginPlay. The trainer's immediate
match-ready override and manual PostLogin restart bypassed the stock startup
deferral. The repair preserves UT's first-frame readiness guard and delays
trainer pawn spawning/equipping until the world has begun play. Deferred pawn
creation now performs scenario setup from RestartPlayer. This is a game-mode
lifecycle repair; changing the map alone does not resolve it. Retest the
packaged client after rebuilding the plugin.

Repair checks: the UE4.15 Win64 Development Editor module rebuilt successfully;
all 20 trainer native tests passed, including six startup regression cases.
These checks do not establish a packaged runtime pass.

### Target initialization repair (2026-10-06)

The first packaged offline test then reached the picker but could not start:
the native target was copying its animation class from CharacterContent, where
Malcolm's skin deliberately has none. The trainer now copies animation and body
mesh placement from stock BaseUTCharacter, then applies the skin during
PostInitializeComponents so validation succeeds before BeginPlay. Dedicated
server bone updates remain enabled. Startup failures now distinguish missing
target data, room content and weapon/preset problems in the HUD and log instead
of suggesting that offline users need a server cook. Offline runs identify
themselves as practice-only before scoring begins.

The UE4.15 editor module rebuilt successfully. Five target-asset regression
tests cover the native initialization, missing assets, stable scale and server
bone-update settings; the full trainer suite has 25 passing tests. Rebuild the
Shipping plugin and verify visible, animated targets in all three scenarios.

### Fire input and movement practice repair (2026-10-06)

The trainer used `SetIgnoreMoveInput(true)` to hold the trainee in place. Stock
UT's `ApplyDeferredFireInputs()` also checks that flag before starting weapon
fire, so the rifle could not fire even in standalone once targets appeared.
Fixed practice now disables movement physics while keeping the movement
component's deferred-fire drain active. It does not set the shared input lock.

Optional movement practice uses a trainer-only character/movement subclass,
with the same lateral plane and dodge basis on authority and owning client.
Pawn setup restores standing posture at the anchor; the owner also resets
posture/velocity at a fresh countdown. Ordinary progress updates leave jumps
and falling physics alone. Crouching and jumps are accepted by the practice
area guard; fixed-position runs retain their original position check.

Native regression tests exercise the real stock fire queuing/drain functions,
movement option transitions, run eligibility, crouch/jump bounds and trainer
dodge rules. A packaged standalone and dedicated-server playtest is still
required after rebuilding Shipping; these tests do not establish a runtime pass.

Verification: all 36 trainer tests passed and the UE4.15 Win64 Development
Editor module compiled and linked. Adding the movement source exposed a unity
build collision between two existing fire-timing cleanup symbols; the cadence
symbols were given distinct names without changing their behavior.

### Trainee pawn selection repair (2026-10-06)

The next packaged test exposed UT's second pawn-class setting: the trainer set
`DefaultPawnClass`, but `AUTBaseGameMode::InitGame()` then loaded the inherited
`PlayerPawnObject` and replaced it with stock `DefaultCharacter`. That pawn
does not contain `UNCAimTrainerMovement`, so the new startup guard correctly
refused the run. This affects standalone as well as network hosts.

The trainer now sets `PlayerPawnObject` to its native trainee, restores its
`DefaultPawnClass` after base initialization, and returns the trainer class
from the spawn-class selector on every restart. Global `PawnClassOverride`
configuration is preserved. The native trainee also copies
the stock pawn's authored body/first-person animation and mesh placement;
normal possession still applies the player's selected skin. A mismatch logs
the actual pawn and movement classes for diagnosis.

Regression coverage runs the real stock InitGame pawn-selection block before
checking the trainer's final class choice, including configured overrides and
standalone/network startup. Rebuild Shipping to pick up this repair.

Verification: all 44 trainer native tests passed and the UE4.15 Win64
Development Editor module compiled and linked. Packaged offline/network
play and the equipped first-person animations still require a visual retest.

### Weapon presentation, feedback and scenario revision 2 (2026-10-06)

The native trainee now copies `DefaultCharacter`'s completed first-person
defaults. `BaseUTCharacter` has no first-person animation class and a different
arm transform; it is insufficient for the weapon's animated hand attachment.
The trainer's movement component and the player's selected skin are preserved.

Saved NCP weapon hide choices apply to the trainer. An exact instagib preference
wins, including an explicit show choice. If none exists, only the trainer's
`N+InstagibRifle_C` inherits the normal `UTNPShockRifle_C` hide choice. Equipping,
weapon swaps, menu reapplication, beam origin and muzzle flashes use the same
decision. Classic hide uses the saved beam back/down offsets; the other hide
style retains its normal socket origin. No preferences are rewritten and
ordinary matches do not gain this fallback.

Accepted hits explicitly use the NCP hitsound preset, style, pitch and volume,
including mute. The catalog prepares in the picker; no hitsounds mutator is
required. Rejected body hits in the headshot scenario do not play success
feedback. The immortal practice targets no longer emit a false helmet-block
notification after a successful headshot.

Trainer head centers now follow the stock visible head socket and head height
instead of the generic NCP capsule-based fallback. Existing NCP sniper radius,
world obstruction and appearance-time validation stay in place. Headshot
targets are stationary and use the same fixed model on client/server. The
changed geometry and scenario timing use revision 2 on all shared boards.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 73 trainer native tests and 16 Django tests passed. Tests cover hidden/show
overrides, beam offsets, accepted-hit feedback, head-center dispatch, target
movement lifecycle, controllerless dodge landing recovery, popup cadence and
separate revision boards. Rebuild Shipping and check actual first-person
attachment/beam rendering, hitsound playback and head alignment in standalone;
the build and native adapters do not establish a packaged visual pass.

### Link firing and five moving target stations (2026-10-06)

Revision 3 replaces the tracking scenario's sniper display weapon with the
existing NCP Shaft Link Gun. Its installed cooked asset was checked for the
native NCP parent, beam state and 1800-unit range. Both fire buttons hold mode
1, with combined input release handling. Tracking now requires the actual
authoritative beam to be hitting the target. Its closer lane fits the beam's
range without changing normal weapon reach.

The shooting scenarios expand to five stations and add bounded A/D wiggles.
Instagib includes a floor target behind the low central block and another near
the left side. Shared geometry defines the platform/cover sizes and spawn
seats together; geometry regressions check capsule support, sightlines and
head/shoulder exposure throughout their lateral limits. Popup timing still
respects the one-second refire, with longer exposure for the larger group.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 93 trainer tests and 16 Django tests passed. The changed preset submits
revision 3, with older boards retained separately. Rebuild Shipping and test
the beam, wiggles and rear head peek in the packaged standalone game. Visual
animation alignment and multiplayer play remain runtime checks.

### Firing accuracy and instagib crouches (2026-10-06)

Revision 4 separates firing time from total run time. The Link accuracy display
now stays unchanged while idle and falls when firing off target. Points still
measure total contact time, so firing accuracy does not replace the score.
Results show both tracked and fired durations. The new `fired_ms` payload and
Django migration preserve older submissions and their original denominator.

Instagib strafes are 10% wider with occasional short crouches through the native
movement component. Crouches retain normal capsule, animation and replication
behavior, reset between appearances, and avoid the final refire window before
expiry. Headshot and Link target movement remains unchanged.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 105 trainer tests and 21 Django tests passed, including idle/firing accuracy,
native crouch lifecycle, expiry timing, and migration preservation. Rebuild
Shipping for the packaged standalone playtest. Apply Django migration 0066
before restarting updated web workers. Packaged animation and network behavior
have not been exercised by these checks.

### Faster pop-ups and a persistent dodger (2026-10-06)

Revision 5 reduces the pop-up interval to 1.00–1.10 seconds and adds a sixth
target in the clear foreground lane. It stays available throughout instagib,
respawning on the next game tick after a hit with fresh movement/history state.
Native random dodges begin 0.20–0.55 seconds after each appearance and repeat
at 1.15–2.10-second intervals, respecting native grounding and cooldowns.
The dodger never earns an expiry penalty and cannot consume a pop-up spawn
deadline. Headshot and Link scenarios keep their existing target counts.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 111 trainer tests and 21 Django tests passed. Checks include independent
respawning, no timeout, scenario isolation, native dodge retry timing, lane
clearance and revision isolation. Rebuild Shipping to playtest the target's
rendered movement and density. The website needs revision-5 API support but no
new migration beyond revision 4's existing migration 0066.

### Headshot scoring feedback (2026-10-06)

The previous aggregate formula retained all miss and expiry deductions even
when the displayed score was floored at zero. Five expired targets cost 125
points, so the next accepted headshot could still leave the display at zero.
Revision 6 follows the owner's revised rule: every confirmed headshot earns
100 points, with misses reflected only in accuracy and expiries informational.
Instagib scoring remains unchanged. Live precision counts and completed-run
log summaries help separate scoring problems from rejected headshots.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 114 trainer tests and 24 Django tests passed. The scoring regression uses
the real weapon shot-stat reader, authoritative hit handler and score update
to verify that earlier misses/expiries cannot hide the first headshot's points.
Django retains the old scoring rules on revision-1 through revision-5 boards.
Deploy revision-6 API support and rebuild Shipping; no additional database
migration is required. Packaged hit registration and HUD rendering remain
runtime checks rather than conclusions of the scoring tests.

### Upper-platform forward slide (2026-10-06)

Revision 7 adds one native forward floor slide to the elevated right instagib
target. It resumes its short strafes afterward. The target spawns farther
back so the slide has a clear runway, and the other popup positions, persistent
dodger, Link and headshot movement rules remain unchanged. Headshot scoring
continues to award 100 points per confirmed headshot.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 122 trainer tests and 24 Django tests passed. Tests cover the real native
floor-slide impulse/timing, controllerless expiry and replication flags, target
reuse, scenario isolation, rifle timing and platform clearance. Rebuild Shipping
for the packaged visual playtest. Deploy revision-7 API support for shared
scores; no additional database migration is required.

### Link slides, contact feedback and Lightning headshots (2026-10-06)

Revision 8 adds occasional native lateral slides to Link tracking and restores
UT's Link damage-type hit flash on the immortal target. Headshot practice now
uses the owning player's saved NCP Sniper/Lightning preference when selecting
or starting a run. The shared native headshot path and equipped weapon's stat
and damage-type identifiers keep Lightning scoring consistent with Sniper.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 145 trainer tests and 25 Django tests passed. Coverage includes slide timing,
cooldown recovery and lane bounds; native hit-info creation and replication
notification; clearing pooled target flashes; client preference and authority
phase gates; missing Lightning content; and Lightning-specific hit/stat scoring.
GPU effects and the packaged client/server experience still need a playtest.
Rebuild trainer clients and servers together, and deploy revision-8 API support
for shared scores. No additional database migration is required.
