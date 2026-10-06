# NetcodePlus Aim Trainer (UE 4.15 / 329)

An opt-in game mode with real animated UT character targets, three fixed
60-second scenarios, an in-game picker/results HUD and a shared UT4Stats top 10.
It does not enable aim assistance or replace the controller in other modes.

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
stock arena and character content for cooking. **The shooting scenarios also
require the existing NCWepMut weapon content pak on both client and server**,
including `/Game/Blueprints/Netcode/UTNPSniper` and
`/Game/Blueprints/Netcode/N+InstagibRifle`. The trainer resolves these classes
after paks mount. A source editor without those weapon assets can exercise
unarmed tracking; shooting scenarios report the missing content and refuse to
start instead of substituting a different weapon.

The first version supports **one trainee per server instance**. Use separate
instances for simultaneous players. Scores from approved instances share the
same UT4Stats leaderboard.

Click a scenario card or press **1**, **2**, or **3**. Press **Enter** or click
Start for the countdown. **F6** cancels an incomplete run and returns to the
picker. **Enter** retries from the results screen. **Escape** retains the normal
UT menu. Cancelled runs and disconnects do not submit partial scores.

All three scenarios support offline standalone practice. A hub is not required.
Offline results remain on the results screen; only an approved network host
submits ranked scores. A directly connected dedicated server can be used for
network testing without a hub.

## Scenarios and scoring (revision 1)

| Scenario | Exercise | Score |
| --- | --- | --- |
| Strafe tracking | Keep the crosshair on a full-size strafing character as it changes direction. Firing is not required. | Milliseconds on target, up to 60,000. |
| Headshots | Hit real character heads above cover with the sniper rifle. Body hits do not count. | 100 per headshot, minus 25 per miss and expired target, floored at zero. |
| Instagib pop-up | Hit full-size characters before they disappear. Positions, distance, height and exposure timing vary. | 100 per hit, minus 25 per miss and expired target, floored at zero. |

Tracking uses a server line trace and the actual target collision, with a
maximum 30 Hz observation rate. It only credits intervals with target contact
at both endpoints, and discards observation gaps longer than 100 ms. Accuracy
means time on target for tracking, and successful hits divided by fired shots
for the two shooting scenarios. Weapons retain their normal firing rhythm.

Tracking revision 1 measures the server's current target against received
view rotation. It does not reconstruct the client's rendered frame or
normalize network latency. Use low-ping hosts when comparing tracking runs;
equal skill at different latencies can produce different scores. The shooting
scenarios use the normal NCP weapon hit-validation path.

All three use the fixed target model/preset in revision 1. Custom models,
durations or difficulty settings need separate revisions before their scores
can be compared fairly. A server-accepted score is a practice result, not proof
that the player used no automation.

## Shared UT4Stats leaderboard

Reads use `/aimtrainer_leaderboard/?scenario=strafe&revision=1&limit=10` (also
`headshots` and `instagib`). The web page is `/aimtrainer/`. Each board shows one
best run per player. The game fetches a compact board outside active scoring;
it does not stream aiming samples to Django.

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
