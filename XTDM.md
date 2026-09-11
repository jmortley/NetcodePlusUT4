# NetcodePlus xTDM Instagib

Four-team instagib TDM, default **2v2v2v2**, supporting **3v3v3v3** and **4v4v4v4**.
The native mode is `NetcodePlus.NCPlusXTDMGameMode`; the content entry point is
`/Game/Blueprints/XTDM/NCP_XTDM.NCP_XTDM_C`.

## Rules and presentation

| Setting | Default / behavior |
| --- | --- |
| Teams | Red 0, Blue 1, Green 2, Yellow 3 |
| Team capacity | 2; `?XTDMTeamSize=3` or `4` changes all four teams together |
| Player capacity | Exactly four times team capacity; spectators are separate |
| Match | 15 minutes; no frag limit unless `?GoalScore=` is supplied |
| Overtime | Another 120 seconds if first place is tied; repeat while tied |
| Scoring | Stock TDM frag scoring; suicide/team-kill penalties remain |
| Friendly fire | Off; teammate hitscan blocking remains the rifle's behavior |
| Respawn | Forced after 1 second; no additional forced-respawn delay |
| Spawn protection | Off by default |
| Loadout | Existing IGCharacterFootsteps and N+InstagibRifle, granted once |
| Pickups | Removed; inventory drops and pickups disabled |
| Ready-up | Incomplete teams allowed; at least one human and every active human ready; F5 or `mutate nc_ready` |
| Require Full | Off by default |
| Bots | Allowed; stock `BotFill`/`Bots` options, capped at the four-team capacity |
| Ranked / VSAI | Stock two-team matchmaking and Humans-vs-AI sessions unsupported |
| Replays | Instant replay unsupported; automatic server recording disabled |

The gameplay HUD has one clock, four fixed team score cards, and the local player's
1–3 teammates with alive/respawn status. The scoreboard has four team panels and up
to 16 player rows: frags, deaths, instagib hit accuracy, and ping. Rosters and row
text refresh at 4 Hz while visible; scores, membership and player statistics use
existing UT replication. Alive/respawn/result metadata uses a separate `AInfo`.
There is no every-frame server roster rebuild or mesh multicast.

Spectators can click players in any team, or use Alt+1–4 to select a team and
Alt+Q/W/E/R to select its player slot. Scoreboard arrows/Enter retain player
selection. F5 includes movable xTDM score/clock and teammate elements.

Four-team body/arm colors use absolute red/blue/green/yellow even with ForceModels
disabled. Model choices remain available; personal team hue overrides and two-color
ForceModels outlines are bypassed in four-team games. The existing Malcolm material
parameters were checked in the editor. Arbitrary custom skins with baked colors or
no tint parameters are not guaranteed to display all four colors; validate those
skins before competitive use. Stock spectator X-ray outlines are still two-color.

## Team assignment and reconnects

Without a draft, arrivals fill the smallest available team. A requested team wins a
tie between equally small teams; it cannot overfill a team. An optional URL draft
uses `?PugTeams=authenticated-id:0,authenticated-id:1,...`, with indices 0–3.
Draft entries reserve seats, reject duplicates/over-capacity drafts, and make
unlisted arrivals spectators. Use the actual authenticated player IDs, not names.

The ready countdown locks existing players' team choices. A cancelled pre-match
countdown unlocks them. In the default incomplete-team mode without a draft, new
players may join available seats after start and can replace bots when necessary;
departed players do not reserve empty seats indefinitely. Reconnects recheck capacity.
With a full-roster policy or an explicit draft, authenticated reconnect reservations
remain protected and unreserved late arrivals spectate. Reconnect matching does not
use the stock Windows IP/name fallback. Bots never acquire authenticated reservations.

For a strict human-only pug, use
`?XTDMAllowIncompleteTeams=0?RequireFull=1?ForceNoBots=1`. Turning off Require Full
alone does not bypass xTDM's own full-roster check when incomplete teams are disabled.

For a bot game, use `?BotFill=8` to fill to eight total participants including humans,
or `?Bots=7` for the stock seven-bots-plus-one-player alias. Targets are capped at
four times TeamSize and used during warmup too. Bots occupy team seats but never
need to ready up. At least one active human is still required to start. An explicit
`PugTeams` draft disables bot additions so its reserved human slots stay protected.

In single-process PIE, use `mutate nc_ready` in each client's console (or F5 Ready).
The September 11 source fix makes the `ready` and `ncpready` aliases use the invoking
client's world; an older editor DLL can silently select the dedicated-server world.
This alias fix needs a native rebuild. Reaching READY and having a full roster are
separate gates. Remove WipeoutMutator from the xTDM PIE mutator list.

Spawn scoring runs on spawn requests. It considers proximity to all three enemy
teams, recent spawn use and teammates, then checks line of sight for up to 16
candidates. Blocked capsules are excluded. It does not continuously tick every
map start. Maps with only team starts use those locations as neutral starts.

## GameState and stock two-team assumptions

**There is no new C++ GameState, PlayerState or PlayerController subclass.**
The existing native `ANPPlayerController` is assigned directly to correct legacy
two-team message dispatch. Do not create a Blueprint child of that controller.

`BP_NCP_XTDMGameState` derives **directly from stock UTGameState**. Its only active
event is `UpdateHighlights -> ClearHighlights(Self)`, with no parent call. This
avoids stock highlight code indexing a two-entry array with Green/Yellow indices.
Stock highlight badges and two-team intro/postmatch line-ups are omitted. The
four-team scoreboard and winner message provide match results. The mode refuses
to start if its required Blueprint GameState/override cannot be loaded.

## Content setup after the native build

The source implementation requires a matching updated NetcodePlus binary on the
server and clients. A content pak alone cannot add these native classes to an older
client. This is not a server-only change for existing 328 clients.

The completed GameState and mode assets are retained at
`ProjectContent/Blueprints/XTDM/BP_NCP_XTDMGameState.uasset` and
`ProjectContent/Blueprints/XTDM/NCP_XTDM.uasset`. Their destination is the project's
**Content/Blueprints/XTDM**, because the runtime path is `/Game/...`, not the plugin's
`/NetcodePlus/...` mount. Both are saved in the connected editor project. The editor
must load the updated native DLL before creating or opening the mode Blueprint.

With that editor open and MapForge connected, run from the NetcodePlus repository
using Python 3.9+:

```powershell
python tools/XTDM/setup_xtdm_editor.py
python tools/XTDM/setup_xtdm_editor.py --apply
```

The first command only checks. The second creates missing xTDM Blueprints, applies
the documented defaults, compiles and saves. It verifies existing parents and the
highlight graph, and refuses unexpected assets instead of reparenting them. It
does not build C++, cook, or alter the instagib character/rifle. It targets the
project reported by the connected editor and prints that directory.

The mode Blueprint pins these existing content dependencies for cooking:

- `/Game/Blueprints/Netcode/IGCharacterFootsteps`
- `/Game/Blueprints/Netcode/N+InstagibRifle`
- `/Game/Blueprints/XTDM/BP_NCP_XTDMGameState`

Use the current cleaned instagib assets. No original xTDM pak, WipeoutMutator, or
new duplicate weapon/material assets are required by this implementation. Shared
instagib assets still need one deliberate pak owner; inspect the final cooked
dependency list for stale Wipeout/UTNP references before distributing it.

From the editor installation, cook the saved mode without rebuilding native code:

```powershell
& ./Engine/Build/BatchFiles/RunUAT.bat makeUTDLC -DLCName=NCP_XTDM -platform=Win64 -version=3525360 -ReleaseVersion=UTVersion0 -nocompile
```

The staged result is
`UnrealTournament/Saved/StagedBuilds/NCP_XTDM/WindowsNoEditor/UnrealTournament/Content/Paks/NCP_XTDM-WindowsNoEditor.pak`.
UAT does not automatically deploy it into the user's MyContent directory.

Example travel options after cooking the mode Blueprint:

```text
DM-DeckTest?Game=/Game/Blueprints/XTDM/NCP_XTDM.NCP_XTDM_C?XTDMTeamSize=2
DM-DeckTest?Game=/Game/Blueprints/XTDM/NCP_XTDM.NCP_XTDM_C?XTDMTeamSize=4
```

Substitute the installed map name. Native entry is also possible with
`?Game=NetcodePlus.NCPlusXTDMGameMode` when the required GameState and instagib
packages have been included in the cook separately.

## Validation status and first playtest

On September 11, 2026, the live UE4.15 editor had the new native classes loaded.
The setup script created `NCP_XTDM`, applied the documented defaults, and compiled
and saved both Blueprints without compiler messages. Read-back verified their
native parents, exact GameState/character/rifle references, and the defaults.
The GameState's only active graph path is `UpdateHighlights -> ClearHighlights(Self)`;
the default BeginPlay/Tick nodes are disabled and disconnected, with no parent call.
The setup script's read-only check passed again after saving.

The original `NetcodePlus.XTDM.SeatPolicy` and `NetcodePlus.XTDM.TeamWinner`
automation groups passed in that editor on September 11. The bot/incomplete-team
update adds `HumanSeatPolicy`, `ReadyRoster`, and `BotPolicy`. All five groups
(52 assertions) passed through `python -B -m unittest tools.tests.test_xtdm_rules -v`,
which compiles and runs the actual rule header and automation cases with a small
standalone adapter. This does not compile the Unreal game mode or exercise actor
lifecycles. Next, test on a dedicated server with matching clients:

1. Start with one ready human, with and without bots; fill/readied starts at 8 and
   16 participants, with capacity enforced. Explicit full-roster games still wait
   for every configured seat and every human ready.
2. Cancel a pre-match countdown by disconnecting; reconnect before and during play.
3. Confirm Green and Yellow colors, team announcements, scoreboard selection and wins.
4. Tie first place at the time limit, finish overtime, and test a disconnected winner.
5. Check one rifle per spawn, unlimited ammo, no pickups/drops, and crowded spawn behavior.
6. Check F5 layout, spectator mouse/keys, return to another mode, and absence of replay/line-up paths.
7. Join a bot-filled match, replace a bot without overfilling or changing scores, disconnect/reconnect,
   and add/remove bots after start. Confirm bot creation failure cannot stall the server.

The user subsequently reported working PIE gameplay. That four-client test had
no incomplete-team URL option, and its two `ready` attempts selected the wrong
PIE world with the older DLL. The console-alias source fix above is not yet built
or runtime-tested. Dedicated-client validation remains pending.

The September 11 WindowsNoEditor content cook completed through `makeUTDLC` with
exit code 0, zero errors and 299 warnings. UnrealPak's integrity test passed all
477 files; both xTDM Blueprints, the existing instagib character/rifle, registry
and build-version marker are present. The pak is 123,191,158 bytes; SHA-256:
`B761495CB9592D46038340EFD4624EC38D19A8834240C0C4070E44CAA80D5113`.
Extraction of the final pak verified explicit cooked overrides:
`bRequireFull=False`, `bAllowIncompleteTeams=True`, and `bForceNoBots=False`.
The serialized cooked CDO title was verified as `NetcodePlus xTDM`. Its distinct
title avoids UTGameMode replacing a title equal to the parent's with the generated
Blueprint class name during initialization.

This standard UT cook also captures other loaded packages, including shared
NetcodePlus, Elim HUD and UTNP weapon content. Those extra packages are not proof
of xTDM runtime dependencies. It is not a two-Blueprint-only pak; review shared
package ownership when combining it with other custom paks. Successful cooking
does not establish runtime compatibility for those overlapping packages.

The current source and saved Blueprint defaults are `bRequireFull=False`,
`bAllowIncompleteTeams=True`, and `bForceNoBots=False`. No native build was launched.
The bot lifecycle changes require a rebuilt server/editor DLL: the earlier native
implementation forcibly disabled bots during InitGame, regardless of Blueprint
settings. Full bot gameplay and the updated console aliases need runtime validation
after that rebuild. Incomplete human-only games can use the updated pak immediately
with an already xTDM-capable binary, and ready up with `mutate nc_ready`.
