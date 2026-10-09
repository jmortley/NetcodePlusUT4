# NetcodePlus Aim Trainer (UE 4.15 / 329)

An opt-in game mode with real animated UT character targets, six 60-second
scenarios, an in-game picker/results HUD and shared UT4Stats top 10 boards.
It does not enable aim assistance or replace the controller in other modes.

## Pop-up movement refresh (2026-10-09)

The IG, Sniper/LG and SACTF pop-up presets now use fresh revision-12 boards.
Link tracking, ordinary headshots and SACTF headshots continue to use their
revision-11 boards. Old pop-up scores remain archived; they are not deleted or
mixed into the tuned preset. Fixed/movement and local/approved-server results
remain separate. Deploy the matching Django update before the new game build;
this board change needs no database migration and adds no revision labels to
the current leaderboard UI.

- Ordinary pop-ups mix 0.24-0.42-second reversals with 0.45-0.75-second holds,
  wider lateral movement and a random initial direction. The rear-center head
  peek and high-right platform target keep their previous movement and seats.
- The middle platform can use either of two side perches. The outer floor
  target alternates between near-left, deep-left and far-right positions. The
  right seat stays ahead of the tall platform so it remains visible. This
  increases location variety while retaining five timed targets and the
  separate permanent foreground dodger, with unchanged target IDs 0-5.
- Left appearances randomly choose ordinary movement, a longer strafe, a
  slide, an angled forward/backward dodge, or a backward dodge into a slide.
  Only one special movement is scheduled per appearance; rejected attempts
  are not retried on a fixed timer. Diagonal input angles vary from 12 to 28
  degrees, and use the selected character's actual UT dodge physics.
- The dodge guard accounts for existing perpendicular velocity, capsule
  radius, landing/slide travel and the resumed strafe band. Unsafe routes or
  actions too close to expiry are skipped. A dodge-to-slide uses UT's actual
  slide input and `ProcessLanded` path, then resumes strafing at its endpoint.
  Native slides/dodges can briefly cross another moving target's sightline.
- Targets disappear after 4.5-5.4 firing intervals, down from 5.5-6.8:
  IG 4.5-5.4 seconds, Sniper/LG 5.85-7.02, SACTF 3.15-3.78. Spawn cadence and
  scoring stay the same; quick target selection now matters more. Full special
  movements still reserve a legal rifle shot before expiry, so the shorter
  SACTF exposure can skip some longer chains.

Validation covers native scheduler/target methods, stock landing-to-slide
transition, capsule/sightline geometry, and Django local/server board routing.
Rebuild and playtest standalone plus a remote client to verify presentation and
the final movement feel; the automated geometry envelope is not an engine run.

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

From the **main menu**, open the console and enter:

```
aimtrain
```

This client command opens standalone practice on the installed stock
`/Game/RestrictedAssets/Maps/WIP/DM-DeckTest` map and shows the trainer's scenario
picker. It requires no hub, login or server credential. The mode constructs the
practice room above the map. The current NCWepMut content pak is still required.
Signed-in players can publish complete standard runs to the separate local-run
leaderboard when UT4Stats has enabled local checkpoint publishing. Signed-out
or disconnected players can still practise without submitting.

The command only runs in the standalone main-menu world. In a match or connected
hub, return to the main menu first. It refuses an already pending map/server
travel, checks the map is installed, closes the front-end UI and uses absolute
travel so previous URL options (including listen, spectator and mutators) are
not inherited. It does not change saved configuration or disable configured
global mutators. Dedicated servers do not register the command.

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
`/Game/Blueprints/Netcode/NCPLinkGun`, `/Game/Blueprints/Netcode/UTNPSniper`,
`/Game/Blueprints/Netcode/UTNPLightningGun` and
`/Game/Blueprints/Netcode/N+InstagibRifle`. The trainer resolves these classes
after paks mount. Missing or incompatible weapon content blocks the selected
scenario with an explanation; tracking requires the real NCP Link Gun and its
secondary beam state rather than substituting a sniper or empty weapon.

The first version supports **one trainee per server instance**. Use separate
instances for simultaneous players. Scores from approved instances share the
same UT4Stats leaderboard.

Click a scenario card or press **1** through **6**. Press **Enter** or click
Start for the countdown. **F6** cancels an incomplete run and returns to the
picker. **Enter** retries from the results screen. **Escape** retains the normal
UT menu. Cancelled runs and disconnects do not submit partial scores.

The starting countdown speaks **3, 2, 1** through the owning player's current
announcer, including the voice pack selected in NCP settings and the normal
announcer volume. It uses the existing `CD3`, `CD2` and `CD1` cues, so no new
sound pak or recook is required. Standalone account verification finishes before
the voice countdown begins. Speech follows the displayed authority countdown;
repeated updates do not repeat a number, and delayed updates do not replay
missed numbers. Queued speech expires if the countdown advances or is cancelled.
This presentation change does not alter scenario timing, scoring or revisions.

The picker and results screen show the selected scenario's UT4Stats top 10,
including player names, points and accuracy. Click **Local runs** or
**Approved servers** to browse either source without leaving the trainer.
Standalone defaults to local runs; network sessions default to approved servers.
The source selection persists when changing scenarios. Browsing a board never
changes where the current run is eligible to submit.

Viewing scores does not require signing in. The menu distinguishes loading,
an empty board and a service failure, and keeps these messages separate from
run authentication, setup and submission status. Requests are cached and
throttled per scenario/source/movement setting; drawing the menu every frame
does not send an HTTP request every frame. A confirmed submission refreshes
its own board.

Menu verification on 2026-10-07: 55 controller, startup, leaderboard and local
recording tests passed, including late responses after changing scenario/source,
submission invalidation during an in-flight read, cached failures and teardown.
Win64 Development Editor, Win64 Shipping client and Linux Shipping server module
builds passed. Rebuild the 329 trainer client and host together for the scoped
submission-refresh notification. The packaged menu layout still needs an in-game
visual check; scores depend on deploying the accompanying Django endpoints.

Press **M** or click the movement button in the picker/results screen to enable
optional **movement practice**. Strafe along the room's left/right lane with
normal UT side dodges, jumping and crouching. Forward/backward movement and
forward/back dodges are disabled; side dodges stay aligned with the lane as
you turn your view. Room walls bound the lane. The option applies to all six
scenarios, survives retries and scenario changes, and cannot change mid-run.
Fixed-position aiming remains the default.

Revision 10 gives movement practice its own leaderboard for each scenario and
score source. The movement button also selects which board the menu displays.
Fixed-position and movement results never compete on the same board;
jumping/crouching changes the headshot cover challenge. Movement runs follow
the same eligibility and submission checks as fixed-position runs.

All six scenarios support offline standalone practice. A hub is not required.
Eligible local results use the separate account-authenticated checkpoint board.
An approved network host submits to the approved-server board. A directly
connected dedicated server can be used for network testing without a hub.

## Scenarios and scoring (revision 11)

| Scenario | Exercise | Score |
| --- | --- | --- |
| Link tracking | Hold either fire button to track a character mixing strafes, dodges, slides and brief crouches with the NCP Link beam. | Milliseconds of beam contact, up to 60,000. |
| Headshots | Hit wiggling character heads at five cover stations with the NCP Sniper or Lightning Gun. Body hits do not count. | 100 per confirmed headshot. Misses affect accuracy; expired targets do not deduct points. |
| Instagib pop-up | Shoot five moving pop-up characters and a persistent randomly dodging character in the open floor lane. Includes a head peek behind the low block and a forward slide on the high right platform. | 100 per hit, minus 25 per miss and expired pop-up, floored at zero. |
| Sniper/LG pop-up | The pop-up layout with the selected NCP Sniper or Lightning Gun, TeamArena character dimensions and rifle-paced appearances. The first accepted body hit or headshot retires that appearance. | 100 per hit, plus 50 per confirmed headshot, minus 25 per miss and expired pop-up, floored at zero. |
| SACTF headshots | The head-peek exercise using the actual SACTF sniper and SACTF movement profile. Body hits do not count. | 100 per confirmed headshot; misses affect accuracy only. |
| SACTF pop-up | The moving pop-up exercise using the actual SACTF sniper. First accepted hit retires the target. | 100 per hit, plus 50 per confirmed headshot, minus 25 per miss and expired pop-up, floored at zero. |

Tracking observes the real authoritative NCP Link beam's selected target, with
a maximum 30 Hz observation rate. It only credits intervals with beam contact
at both endpoints, and discards observation gaps longer than 100 ms. Tracking
accuracy divides beam-contact time by time firing, using the same sampling and
continuity rules for both clocks. Idle time changes neither accuracy nor score;
firing off-target lowers accuracy. Score remains total contact milliseconds
over the 60-second run. The five shooting scenarios use successful hits divided
by fired shots. Weapons retain their normal firing rhythm.

Revision 6 removes point deductions from headshot practice. Earlier revisions
could accumulate miss/expiry deductions while displaying zero, hiding points
from subsequent successful hits. Every confirmed headshot now adds 100 points;
the HUD shows live headshots, shots and expiries. Completed runs also log these
counts in standalone, allowing scoring complaints to be checked against actual
accepted hits. Instagib retains its existing miss/expiry point deductions.

Headshot practice and Sniper/LG pop-up honor the player's saved NCP hitscan choice:
`[WeaponSkinsPlus] HitscanChoice=LG` or `Sniper` in `Mod.ini`, selected through
the existing `weaponskins` menu. Selecting a scenario or starting/replaying a
run reads the preference again. The choice travels with that request to the
authority, including when joining a dedicated server. It cannot swap weapons
during countdown or an active run. The HUD identifies the equipped rifle.
Both shipped rifles have the same 1.3-second primary fire interval and 0.95
headshot scales, so they share each scenario's board. Scoring reads the equipped weapon's
own shot counter and headshot damage type, including LightningRifleShots and
the Lightning Gun headshot type. Missing LG content blocks the run rather than
silently substituting the sniper.

**[5] SACTF Headshots** and **[6] SACTF Pop-up** load
`/Game/Blueprints/Netcode/SACTFSniper.SACTFSniper_C` from the installed
**MutSaCTF content pak**. NCWepMut alone does not contain that weapon. Missing
content blocks these two modes with an explicit message; the trainer does not
substitute the normal sniper or apply the player's Lightning preference.

The audited SACTF asset fires every **0.7 seconds**, with 70 body damage,
140 headshot damage, 1.75 headshot scales, and head sphere padding of 6 while
moving / 2 while stationary. These are the weapon's real native NCP hit rules;
practice points are separate from damage. The trainer leaves the asset's
firing, zoom, hit validation and cosmetic behavior intact. Pop-up appearances
are spaced 0.70–0.77 seconds apart and last 3.85–4.76 seconds. An altered firing
interval makes either SACTF preset practice-only.

Both SACTF presets use native trainer pawns matching the audited
`SaCTFCharacter`: TeamArena-sized 40-by-108 capsule, 940 walk speed and 5000
acceleration, but **900 sustained slide speed**, with 1350 initial slide speed.
Separate class defaults preserve those settings through crouch restoration and
network possession. Fixed-position and movement runs have separate SACTF
leaderboards under each existing local/approved-server source. Revision 11
adds these boards while preserving revision-10 results and APIs.


Tracking uses the existing beam's range, obstruction and server validation.
It does not normalize network latency; use low-ping hosts when comparing runs.
The target now starts 1000 units from the trainee, within the beam's range as
it strafes. Either fire button starts the same beam; releasing one button
while holding the other keeps it active. Both buttons select secondary fire;
Link pull is disabled for trainer pawn owners on both client and authority.
Targets remain alive and precision shot counters stay zero in tracking
submissions. Every accepted native damage batch plays the selected NCP
hitsound, without an additional trainer timer suppressing confirmations.
Optional movement practice can still move out of the normal 1800-unit beam
range, where normal misses earn no tracking credit.

The trainer uses the regular `NCPLinkGun_C` saved show/hide choice, including
classic beam offsets. The separate Shaft and legacy CSHD Link choices do not
override it.

Link tracking uses 75% short strafe holds (0.14–0.34 seconds) and 25% longer holds
(0.45–0.80 seconds), with grounded dodge attempts every 1.8–3.8 seconds. Native
UT dodge cooldowns and landing physics still apply. Ground reversals pause
during a dodge. This is randomized target movement, not a replay of a fixed path.

Each run seeds its pseudorandom stream from a new run ID. Hold durations and
dodge/slide choices vary, while ordinary strafes alternate direction and lane
boundaries force inward turns. The target does not react to the trainee's aim.
Revision 9 adds brief 0.20–0.45-second native crouches to Link tracking, with a
random 6–10-second standing interval before each one. Blocked attempts wait
for grounding or an active slide to finish. A/D strafing continues at native
crouched speed; dodges and slides wait until the target can stand again.
Crouches do not start without time to finish before the run ends, and hiding
targets clears their pending crouch schedule.

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

Sniper/LG pop-up uses the same scheduling policy with its normal 1.3-second
refire interval: appearances every 1.30–1.43 seconds, lasting 7.15–8.84 seconds.
It uses TeamArena movement and capsule values instead of the instagib profile.
Only a headshot confirmed by the equipped rifle's damage type earns the extra
50 points; there is no separate, more generous trainer head trace.

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

All five shooting scenarios use brief 0.12–0.28-second A/D reversals with the
character profile's normal 940-unit walking limit and 5000 acceleration. Short
holds and early turns limit the distance traveled, rather than a custom speed
cap. Each seat has a bounded movement range, with room for the capsule and
stopping distance on its platform or behind cover. Headshot targets remain
for 6.5 seconds. The popup layout adds a near-left floor position and a floor
position behind the central block. That block is 160 units high, and the target
on top is offset laterally to keep the rear head peek visible from the fixed
anchor. Movement practice naturally changes those sightlines.

Revision 10 adds random slides to both left floor pop-ups in both pop-up
scenarios, with a 45% chance per appearance. The rear-left target slides
forward; the near-left target slides inward along the lane to avoid the
persistent dodger. The rear-left target also has an independent 65% chance
of a long strafe: its half-width grows from 99 to 180 units for 0.50–0.75 seconds,
starting 2.3–2.9 seconds after appearing. It uses normal movement rather than
teleporting, then returns to short strafes. Long strafes wait for an active
slide to finish. Both actions require enough remaining exposure for a full
rifle refire interval afterward. The high right-hand slide remains guaranteed
when the target lives long enough and grounding permits it.

Revision 4 widened instagib movement ranges by 10%; both pop-up scenarios now
use those ranges. Appearances without a scheduled slide or long strafe have a
65% chance of one short native crouch, scheduled 1.5–3.5 seconds
after spawning and held for 0.25–0.45 seconds. It is skipped if too little
exposure remains for the crouch plus a full rifle refire interval and 0.1 seconds.
Targets behind cover can briefly disappear while crouched. Reappearing targets
start standing; headshot targets do not gain these crouches. Link tracking uses
the separate, less frequent schedule described above.

All six use the fixed target model/preset in revision 11. Custom models,
durations or difficulty settings need separate revisions before their scores
can be compared fairly. A server-accepted score is a practice result, not proof
that the player used no automation.

## Shared UT4Stats leaderboard

Reads use `/aimtrainer_leaderboard/?scenario=strafe&revision=11&movement=0&limit=10`
(also `headshots`, `instagib`, `precision_popup`, `sactf_headshots` and
`sactf_popup`). Add `scope=local_checkpoints`
for local results; the default source is `approved_servers`. Use `movement=1`
for movement practice. The web page is `/aimtrainer/`. Each board shows one best
run per player, isolated by scenario, revision, source and movement setting.
Successful menu reads are cached for one minute. Approved hosts send aggregate
results; authenticated local runs send the checkpoints described below.

Revision 10 and 11 submissions require a Boolean `movement` field. Django retains
revision-1 through revision-9 submissions and explicit older boards, treating
them as fixed-position results. Older payload digests remain unchanged.
Revision 4 onward includes `fired_ms`: measured firing time for tracking and
zero for precision scenarios. Apply migrations through
`0068_nc_aimtrainer_movement` and deploy revision-11 API support before enabling
revision-11 hosts. Revision 11 needs no additional migration. Rebuild trainer clients and servers together for the new
scenario and movement-scoped submission notification. These classes are used
only by the trainer game mode.

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
configuration shipped through the launcher. Standalone practice never uses
this token or submits to the approved-server board. The website must approve the token owner's account
through its `AIMTRAINER_SERVER_USERS` setting. An ordinary valid API token alone
does not grant score-submission permission.

The accompanying Django change adds an isolated table and migration. Deploy
that code, run its migration, configure the allowed server account usernames,
and restart the web workers before expecting shared scores. Until then the
trainer reports that the online service is unavailable.

## Account-authenticated local scores (2026-10-07)

Windows standalone runs can publish to a **separate local-run leaderboard**.
There is no new password prompt and no server upload key in the client. The
trainer reads the existing signed-in UT account through OnlineSubsystem. The
start endpoint verifies that account with the UT community master server and
returns a short-lived, account-bound run ticket. It takes the account ID and
display name from that verification, never from score payload fields.

The game waits for verification before its three-second countdown. Signed-out
players, disabled publishing, unsupported client platforms or service failures
fall back to ordinary local practice. Standard fixed-position and movement runs
are eligible for their respective boards; modified presets are not.
`[NCAimTrainer] OnlineEnabled=False` disables online scores.

Only the fixed `https://ut4stats.com/aimtrainer_local/start/` endpoint receives
the game account bearer. Subsequent requests to `/aimtrainer_local/checkpoint/`
use only the run ticket. The local credential path ignores `ApiBaseUrl` and the
server's `[UTPUGS_STATS] Key`. The Windows transport verifies certificates using
the OS trust store, requires TLS, disables redirects, and does not use UE4's
permissive Curl settings or its HTTP header debug logging. No global engine
networking configuration is changed. Other platforms remain local practice
until an equivalently verified transport is implemented.

Checkpoint zero establishes the receiving server's clock; the timed challenge
waits for its acknowledgement so a lost initial request cannot put the client
ahead of the server's clock. Every five seconds,
the client sends ordered shot/hit/expiry events or approximately 30 Hz Link
firing/contact samples. UT4Stats accepts sequential, immutable batches and
computes the result itself. Checkpoint 12 closes exactly 60 seconds. The service
checks elapsed time against its own receipt clock, firing cadence, one hit per
shot and appearance, event bounds and complete coverage. It rejects edited
retries, another run's ticket and partial completion. Identical retries are
idempotent, including a lost final response.

Uploads are asynchronous, with at most four queued/in-flight batches, four-second
request timeouts and three attempts using one/two-second backoff. Leaving the
map, cancelling, starting another run or changing the scenario cancels pending
work. A missed schedule, queue overflow or failed upload makes the result
practice-only; there is no late offline score upload. A new authenticated start
revokes the account's previous unfinished ticket so restarting does not require
waiting for its three-minute expiry.

The board scope is `local_checkpoints`; historical and approved host scores
stay in `approved_servers`. The standalone HUD and website identify these
sources explicitly. These checks bind identity and make accepted history
immutable, but **cannot prove that a player-controlled client honestly produced
its observations**. Local results must not be promoted to the approved-server
board based only on these checks.

Deploy Django migrations through `0068_nc_aimtrainer_movement` (including
`0067_nc_aimtrainer_local`), configure
an acknowledged shared rate-limit cache, and enable
the environment variable `AIMTRAINER_LOCAL_ENABLED=1` after staging verification. Publishing defaults
off on the website. See the backend's `docs/aimtrainer.md` for rate limits,
database routing and deployment checks. A real signed-in Windows run and the
PostgreSQL concurrency checks remain release gates, separate from native and
SQLite regression tests. Do not add ordinary players to `AIMTRAINER_SERVER_USERS`.

Verification on 2026-10-07: all 172 existing trainer regression tests and the
11 new local-recording tests passed. The real UE automation test
`NetcodePlus.AimTrainer.LocalSession.Transport` passed, including initial
checkpoint acknowledgement, bounded retries, cancellation, final responses
and credential URL/header restrictions. Final module builds compiled and
linked for Win64 Development Editor, Win64 Shipping client and Linux Shipping
server with `NCP_AIM_ASSIST_TEST_BUILD=0`. Django passed 67 tests; two tests
requiring PostgreSQL were skipped on SQLite. Migration state and forward/reverse
preservation checks passed. No live account credentials were used in these
tests, and no production deployment was performed.

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
server. Confirm visible animated targets and cover, all four scoring rules,
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

### Gameplay character profiles (2026-10-06)

Revision 9 copies the inspected movement/size values from the owner's cooked
`IGCharacterFootsteps` and `TeamArenaCharacter` Blueprints into the trainer's
native classes. `NCP-IGCTF` in the supplied `MutInstagibNCP-WindowsNoEditor.pak`
selects `IGCharacterFootsteps`. The native profile is a frozen snapshot of this
content, not a live Blueprint dependency or execution of its event graph.
Future gameplay Blueprint tuning needs a corresponding profile/preset update.

| Setting | Instagib | Sniper, Lightning and Link |
| --- | --- | --- |
| Capsule radius / half-height | 38 / 103 | 40 / 108 |
| Pawn body mesh scale / Z | 0.95 / -110 | 1.0 / -110 |
| Standing eye offset | 80 | 83 |
| Maximum walk / crouched speed | 940 / 315 | 940 / 315 |
| Ground acceleration / friction | 5000 / 14 | 5000 / 14 |
| Crouched capsule half-height | 72 | 72 |
| Dodge air control | 0.60 | 0.55 |
| Initial maximum / sustained slide speed | 1350 / 1100 | 1350 / 1100 |

Both the trainee and targets use the selected profile. The instagib classes
have distinct class defaults because native uncrouching restores the capsule
from the class default, and applying a skin restores the default mesh scale.
Scenario changes replace the affected pawns outside a run; they do not patch
live capsule sizes or change other NCP game modes. Respawn heights and the
fixed-position guard use the selected class's standing height, including when
a pooled target was crouching during its previous appearance.

The slide speeds include each Blueprint's BeginPlay assignments, not only its
serialized defaults. The profile also preserves the distinct initial crouched
eye heights and inherited runtime crouched eye height. It keeps the trainer's
working first-person animations, attachments and weapon visibility preferences.

Targets no longer override movement with 500-unit tracking speed, 220-unit
wiggle/crouch speed or 7000 acceleration. The bounded A/D driver reverses before
its stopping distance reaches the seat edge. The central instagib block is
lowered to 160 units to preserve the rear head peek for the smaller model. The
Link setup requires the shipped 1800-unit beam range for the faster slide lane.
Existing input restrictions and movement-practice ranking rules remain intact.

The matching Django change adds revision 9 support without a migration and
retains explicit revision 1–8 boards. Deploy the updated backend and rebuild
trainer clients/servers together. Shipping and an in-game posture/skin/weapon
check are still required; native tests and an Editor module build do not prove
rendered alignment or network play.

Verification: the UE4.15 Win64 Development Editor module compiled and linked;
all 158 trainer native tests and 25 Django tests passed. Coverage includes
scenario class replacement, failed-spawn recovery, pooled target standing
heights, skin scale, posture callbacks, full-speed braking at 30–700 Hz, and
profile-aware cover/platform clearance. Shipping and packaged offline/network
verification remain outstanding.

### Regular NCP Link beam and damage confirmations (2026-10-06)

Revision 9 also replaces the narrower Shaft asset with the shipped
`NCPLinkGun` asset. Its secondary beam has a 12.5-unit trace half-width,
28 damage per 0.25-second interval and 1800-unit range, compared with Shaft's
5-unit half-width and 25 damage. These values come from the actual weapon
content; the trainer does not override the trace or rewind rules.

The existing two-button hold handling still selects only secondary fire.
`SupportsLinkPull()` rejects trainer character owners on both roles, including
the server RPC admission path, while normal matches and Shaft retain their
existing behavior. The native beam's minimum damage batch remains intact.
The extra 120 ms trainer confirmation timer is removed, so accepted batches
use the normal NCP damage-confirmation playback and saved settings. Offline
practice follows the same authoritative acceptance path without needing a hub.

Fixed-position target paths fit within the regular range. Movement practice
can put the trainee and target at opposite lateral edges beyond that range;
those are genuine out-of-range misses, with no added reach or scoring credit.

Verification: the updated UE4.15 Win64 Development Editor module compiled and
linked, and all 168 trainer native tests passed. The added coverage checks
trainer-only pull admission and release behavior, normal-owner pull behavior,
consecutive damage confirmations, startup range validation and tracking crouch
timing, posture, conflicts, cleanup and recovery. Packaged
standalone contact, rendering and audio still need an in-game check.

### Revision 9 profile-switch camera correction (2026-10-06)

Switching into or out of Instagib replaces the trainee pawn to select the
correct capsule and movement defaults. Stock respawning initially points the
camera in the map PlayerStart's direction. ConfigurePawn now resets both the
authority control rotation and the owning client's camera toward the practice
lane after placing the pawn. The same correction covers a retry after a failed
replacement spawn.

The startup regression suite passes all 26 tests. It models a nonzero map spawn
rotation, checks both profile-switch directions and failed-spawn recovery, and
verifies that removing either camera reset makes those tests fail. The preceding
revision-9 review passed all 168 trainer tests and 25 Django tests. Final UE4.15
module builds pass for Win64 Development Editor, Win64 Shipping client and Linux
Shipping server. Packaged standalone and multiplayer checks remain necessary.

### Revision 10 pop-up rifles and movement boards (2026-10-07)

Both left pop-ups can slide and the rear-left target can make an occasional
long strafe. The fourth menu option uses Sniper/LG on the pop-up layout, with
100-point body hits and 150-point headshots. Normal rifle cadence sets its
appearance and exposure schedule. All four scenarios now have separate
fixed-position and movement boards in both score sources.

Verification: all 198 native trainer regression tests passed. The Django suite
passed 74 tests, with two PostgreSQL-only tests skipped on SQLite; migration
preservation checks also passed. UE4.15 module builds passed for Win64
Development Editor, Win64 Shipping client and Linux Shipping server with
`NCP_AIM_ASSIST_TEST_BUILD=0`. The engine automation test
`NetcodePlus.AimTrainer.LocalSession.Transport` passed, including revision-10
start encoding and rejecting movement-mismatched acknowledgements.

Deploy the accompanying Django code and migrations through 0068, then rebuild
trainer clients and hosts together. No production deployment or packaged
playtest was performed here. Slide visuals/collision, both rifle choices,
dedicated-server play and real authenticated score publication still need
in-game validation.

### Revision 11 SACTF practice (2026-10-07)

The picker adds SACTF Headshots and SACTF Pop-up, using the actual MutSaCTF
rifle asset and a dedicated native SACTF character/target profile. The normal
Sniper/LG preference does not replace this weapon. Target opportunities match
its 0.7-second refire. Headshot-only scoring is 100 per confirmed headshot;
SACTF pop-up uses 100 per hit with a 50-point headshot bonus and the existing
miss/expiry penalties. Both modes support separate fixed/movement boards in
each score source. Existing scenarios and old revision APIs remain available.

Verification: 207 native trainer tests pass, including the updated pawn-class
adapter for the new subclasses. The Django suite passed 81 tests with two
PostgreSQL-only tests skipped. Win64 Development Editor, Win64 Shipping client
and Linux Shipping server modules built with the aim-assist fixture disabled.
The engine's LocalSession.Transport automation passed both new scenario
payloads under revision 11. Read-only review found no scoring, scenario-routing
or leaderboard-scope mismatch between the game and Django.

Deployment requires revision-11 game and Django code plus the MutSaCTF content
pak on the trainer host/client. No new migration beyond 0068 is needed. This
verification did not include a cooked playtest or real authenticated upload.
