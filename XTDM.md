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
| Ready-up | Full four-team roster and every player ready; F5 or `mutate nc_ready` |
| Bots / ranked | Disabled for this mode |
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

The ready countdown locks teams. A cancelled pre-match countdown unlocks them.
Once play starts, reconnects retain their authenticated reserved team; unreserved
arrivals spectate. Reconnect matching does not use the stock Windows IP/name fallback.
For a manual smoke test with fewer players, add `?XTDMAllowIncompleteTeams=1` and
ready the connected player(s). Leave that option off for pugs.

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

The prepared GameState asset is tracked at
`ProjectContent/Blueprints/XTDM/BP_NCP_XTDMGameState.uasset`. Its destination is the
project's **Content/Blueprints/XTDM**, because the runtime path is `/Game/...`, not
the plugin's `/NetcodePlus/...` mount. The live editor project already contains it.
The final native-parent mode Blueprint must be created after the editor loads the
new DLL.

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

Example travel options after cooking the mode Blueprint:

```text
DM-DeckTest?Game=/Game/Blueprints/XTDM/NCP_XTDM.NCP_XTDM_C?XTDMTeamSize=2
DM-DeckTest?Game=/Game/Blueprints/XTDM/NCP_XTDM.NCP_XTDM_C?XTDMTeamSize=4
```

Substitute the installed map name. Native entry is also possible with
`?Game=NetcodePlus.NCPlusXTDMGameMode` when the required GameState and instagib
packages have been included in the cook separately.

## Validation status and first playtest

The prepared Blueprint GameState compiled and saved without messages in the live
UE4.15 editor; read-back verified its native parent and connected highlight event.
The setup script's read-only path verified that asset and correctly reported the
new native mode was not loaded. Native compilation, cooking and online gameplay
tests have **not** been run as part of this implementation.

Two native automation groups are provided: `NetcodePlus.XTDM.SeatPolicy` and
`NetcodePlus.XTDM.TeamWinner`. Run these after building, then test on a dedicated
server with matching clients:

1. Fill/readied starts at 8 and 16 players; full teams reject extra players.
2. Cancel a pre-match countdown by disconnecting; reconnect before and during play.
3. Confirm Green and Yellow colors, team announcements, scoreboard selection and wins.
4. Tie first place at the time limit, finish overtime, and test a disconnected winner.
5. Check one rifle per spawn, unlimited ammo, no pickups/drops, and crowded spawn behavior.
6. Check F5 layout, spectator mouse/keys, return to another mode, and absence of replay/line-up paths.

Native build, cook and dedicated-client validation remain pending at this implementation handoff.
