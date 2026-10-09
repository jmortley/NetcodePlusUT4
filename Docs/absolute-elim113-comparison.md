# ElimPlus and Absolute Elimination 113 comparison

ElimPlus with NCStockWeapons is not a drop-in reproduction of Absolute Elimination
1.13. The stock arsenal is selected by `?WeaponSet=Stock`, and ElimPlus still owns
round rules, damage accounting, ratings, spawns, and the HUD. The Absolute artwork
previously displayed only a subset of ElimPlus's scoreboard data.

This audit compares the supplied Elimination package with native
`329-release-candidate` at `3a50951a58780ea37ae8be5fe3ccd488b6e5e14a` and the editable
UE4.15 Blueprint assets on 8 October 2026. Values below are defaults, not a claim
about a running hub. Cooked asset precedence, the game URL, mutator order, and
applicable Mod.ini settings determine the effective configuration.

## Scoreboard columns and meanings

| Field | Absolute 1.13 | ElimPlus |
| --- | --- | --- |
| Player | Name, flag, rank/XP decoration | Name and flag; standard board also has host and teammate status presentation |
| ELO | Original saved rating, with match-end updates | TeamGlicko-based rating, global rank and match delta |
| DPR or PPR | Historical damage per round, displayed beneath the player name beside ELO | Current-match points per participated round: kills plus damage divided by 100; completed rounds only |
| Ping | Before the damage and kill columns; also visible before start | Same position on the Absolute skin, last column on the standard skin; bots and standalone players display a dash, with no Skill column |
| Damage | Effective credited damage with lethal overkill removed | Overkill-inclusive match damage, capped at 255 per credited hit |
| Kills | Kills only | Kills plus kill assists in the displayed K column |
| Deaths | Deaths | Deaths |
| LG accuracy | No corresponding column in the recovered scoreboard | Standard skin only: Sniper/Lightning accuracy, or instagib accuracy when those shot stats exist; not Link Gun accuracy |
| Row sorting | Damage descending, then kills descending, then deaths ascending | Damage descending, then kills plus assists descending, then score descending |

The original active Blueprint path is `ALTSHUD::DrawScoreboard2` and
`Draw Scoreboard Team 2`, not the older `DrawScoreboardTeam` function still present
in the package. Its 2560-wide authored coordinates put ping at 400, damage at 549,
kills at 640, and deaths at 740 relative to the row. ELO and `(DPR)` occupy the
player-name area around offset 260; rank decoration begins at 428. The previous
C++ skin instead drew `PLAYER | K | D | DMG | ELO | PING`, omitting ElimPlus's PPR,
LG accuracy, global rank, and ELO delta.

The presentation patch restores the original arrangement:
`PLAYER | ELO (PPR) | PING | DAMAGE | KILLS | DEATHS`. The rating group is
centered under its header and vertically within each player/total row, rather
than resting against the bottom edge. Names fit to the available space before
the measured rating group so long names cannot overlap the values.
The banners use large scores facing the center and mirrored Red/Blue names.
It optionally loads the original stock scoreboard score font, falling back to
the existing HugeFont if that asset is absent from the installed cook. Headers,
player rows, and totals share the original column coordinates and top alignment.
Global rank appears beside the flag; a nonzero match rating delta is included
in the centered rating group. The obsolete Epic XP/rating badge is not synthesized from
ElimPlus's different rating data.

At the user's request the Absolute skin excludes LG accuracy, which remains on
the standard skin. Neither skin displays bot Skill. Ping remains visible during
ready-up, and pending team switches use the existing ready-state display. Players
waiting to start are not dimmed as dead. The normal board's team K total includes
the same assists its player rows already show; team ELO remains an average.

Opening Escape or a menu no longer automatically pauses standalone NCP games.
Manual console pause with menus closed still uses the existing permissions, and
standalone resumes without the host countdown. Networked pause behavior is
unchanged. These native changes do not require a Blueprint/pak recook.

This exposes ElimPlus statistics in the Absolute artwork. It does not recreate
Absolute's historical DPR. Renaming PPR to DPR would be incorrect: Absolute sums
recorded round damage and divides by recorded rounds. For existing valid history
it blends DPR toward the match result with weight 0.55; other branches retain the
previous DPR for insufficient valid rounds or initialize a new record from the
match result. ElimPlus's PPR includes kills and uses a different damage definition.
Restoring that historical metric needs an explicit data and persistence decision,
not just a label change. This patch adds no replicated fields or rating changes.

Source: `Source/Private/ElimPlusScoreboard.cpp`,
`Source/Private/ElimPlusStatsReplicator.cpp::UpdateFromPlayerStates`,
`Source/Private/ElimPlusGame.cpp::EndRoundForTeam`; original `ALTSHUD` defaults and
`Draw Scoreboard Team 2` bytecode, plus `EliminationSavedData::Update Stats`.

## Selecting stock weapons

`ElimPlus` already includes `ElimPlusMutator` as a built-in mutator. That
Blueprint's `Init` reads the case-insensitive `WeaponSet` URL value. With
`?WeaponSet=Stock` it assigns the stock-flavor loadout and
`TeamArenaCharacterStock`. Without it, the default inventory remains the
competitive NCP arsenal.

NCStockWeapons does exact-class lookup against stock weapons. It does not
recognize and convert the NCP classes already in the default ElimPlus inventory.
It can still set `DefaultPawnClass` and apply warmup and inventory policies.
Adding NCStockWeapons without selecting the stock weapon set can therefore leave
a mixed setup. The final pawn policy also depends on mutator execution order.

The stock selection contains chest armor, stock Enforcer/Link/Minigun/Bio/Grenade
Launcher, `BP_FlakCannonNC`, `BP_SniperNC`, `ShockRifleNC`,
`UTNPRocketLauncherStock`, and `UT+ImpactHammerElim`. The default competitive set
instead includes `NPFlakCannon`, `UTNPSniper`, `UTNPRocketLauncher`,
`UTNPShockRifle`, `NCPLinkGun`, and `NCPMinigun2`.

Only Shock, Flak, Rocket, and Sniper in NCStock's replacement table become NCP
weapon classes. Do not describe its retained stock Link or Minigun as using the
NCPLinkGun or NCPMinigun weapon implementation.

Do not combine NCStockWeapons and NCWepMut. They both replace stock classes and
can both apply other policies after one has already replaced the inventory.
The existing `SERVER-ADMINS.md` likewise specifies choosing one. Exact duplicate
mutator classes are rejected by the engine, which does not resolve conflicts
between different mutator flavors.

## Weapon and character differences

The original mode computes starting ammunition as a fraction of each weapon's
maximum ammunition. It does not simply retain the stock weapon's initial ammo.
With the packaged Absolute options and the inspected stock weapon defaults:

| Starting ammunition | Absolute 1.13 | ElimPlus stock selection |
| --- | --- | --- |
| Shock | 29 | 16 |
| Link | 90 | 80 |
| Minigun | 150 | 80 |
| Flak | 15 | 16 |
| Rocket | 12 | 9 |
| Sniper | 13 | 10 |

Absolute's `ALTSDefaultCharacter2::Reset Weapons` first sets maximum ammo to
`trunc(originalMax * MaxAmmoPct + .01)`, then sets starting ammo to
`trunc(newMax * StartingAmmoPct + .01)`. Its packaged defaults enable dual
Enforcers and explicitly call `BecomeDual`. The inspected ElimPlusMutator and
NCStock scripts have no corresponding call; verify the effective Enforcer in
game before claiming dual-Enforcer parity. Absolute's default inventory references
Lightning Gun, but its packaged start-enabled flag is false.

Death pickups differ too. Absolute awards 20 health and
`ceil(MaxAmmo * .10)` ammo per weapon. Our `CandyPlaceholder` awards 20 health
with superheal enabled and calls `RestoreAmmoPct(.15, true)`, which truncates the
positive ammo product. With the stock profile, that changes Link ammo gained
from 12 to 18 and Minigun from 20 to 30; Shock remains 4, Flak 4, Rocket 3, and
Sniper 2. Matching the health amount does not establish identical health caps
or queued pickup behavior.

Do not infer active damage-based healing from Absolute's
`HealthPerDamageIncrement=50`: its reward path also requires a nonzero reward
amount. The original Blueprint reward amount and per-kill award default to zero;
server options can enable them.

These values distinguish our two weapon selections. They are not universal
Absolute server settings; Absolute itself has per-weapon inventory/ammo options.

| Setting | ElimPlus stock selection | ElimPlus competitive selection |
| --- | --- | --- |
| Sniper body damage | 65 | 70 |
| Sniper refire put-down fraction | 0.55 | 0.45 |
| Shock initial/max ammo | 16 / 32 | 14 / 28 |
| Shock beam momentum | 90,000 | 72,900 |
| Shock core speed | 2,300 | 2,415 |
| Flak initial/max ammo | 16 / 32 | 11 / 22 |
| Flak primary/secondary interval | 0.8 / 1.0 seconds | 1.0 / 1.0 seconds |
| Rocket initial/max ammo | 9 / 22 | 11 / 22 |

The stock flavor still has exceptions. NCStock's `SpeedSwap` rewrites selected
stock inventory actors to 0.3-second bring-up, 0.3-second put-down, and a 0.65
refire put-down fraction. Its stock-flavor Sniper and Flak also raise faster than
their stock defaults. Rockets retain NCP grenade/spiral modes and have a maximum
of 22 ammunition rather than the stock 21. The stock selection deliberately
retains the Elim hammer with 0.5/0.95-second fire intervals rather than the stock
hammer's 1.1/1.33 seconds.

Both current TeamArenaCharacter assets derive from `ATeamArenaCharacter` and set
HealthMax to 125. The competitive movement component overrides friction to 14,
acceleration to 5000, walking braking to 2000, and dodge air control to 0.55.
The stock-character asset lacks those competitive overrides and inherits its
movement values. Merely changing the weapon set also selects a different pawn.

NCStock additionally applies a warmup policy with unlimited ammunition, no
pickups, a warmup inventory, and 199 health. Its composition with ElimPlus's own
warmup/restart path needs a runtime check using the deployed ruleset.

Source: current `NCStockWeapons`, `ElimPlus`, `ElimPlusMutator`, character and
weapon Blueprint defaults and bytecode; `UTPlusFlakCannon.cpp`,
`UTPlusWeap_RocketLauncher.cpp`, and stock `UTWeap_Sniper.cpp` defaults.

## Round rules and scoring

| Area | Absolute 1.13 defaults or behavior | Current ElimPlus defaults or behavior |
| --- | --- | --- |
| Goal | 10 rounds | 10 rounds |
| Round duration | 90 seconds | Native 90, overridden to 80 in the inspected editable ElimPlus Blueprint |
| Starting health | 120 | 125 through current TeamArenaCharacter assets |
| Starting armor | 100 | Chest armor in the loadout |
| Uneven teams | Ratio-based adjustment to both starting health and armor | Public-only health adjustment, 5% per missing player by default, capped at 50%; armor unchanged |
| Overtime | 1.5-second ticks, base ramps from 1 to 11 over 35 seconds, with alive-team and starting-resource scaling | 5-second delay and 5-second waves, starting at 5 damage and multiplying by 1.5 each wave; no default cap |
| Damage credit | Removes lethal overkill using the character's effective remaining resources | Reconstructs overkill-inclusive credit, capped at 255 per hit; intentional existing policy |
| Ratings | Original saved ELO/DPR system | TeamGlicko ratings with performance blend, persisted in Mods.db; normally frozen HUD ELO during the match |
| Mid-match public balance | Original shuffle mechanisms | At most one PPR-based intervention after a qualifying 6–0 start |
| Round spawns | Blueprint transform-generation pipeline | Adapted native transform queues, changed anchor selection and PlayerStart fallbacks |

Relevant native sources: `ElimPlusGame.cpp` constructor, `EndRoundForTeam`,
`CheckRoundWinConditions`, `ExecuteOvertimeWave`, `ModifyDamage_Implementation`,
`SpawnDefaultPawnFor_Implementation`, `MidGameShufflePPR`, and
`NCHybridSpawnGenerator.h`. The current mode also implements ready-up,
concede/version handling, clutch tracking and presentation, match-winning replay,
and optimized HUD/replication paths. These paths belong to the gamemode and
plugin; selecting the Absolute artwork does not select Absolute's round logic.

## Defects found outside the scoreboard patch

1. **The winner can be fixed before a trade finishes.**
   `ElimPlusGame.cpp:3331` refuses another win check while the 0.2-second end-round
   timer is active. At lines 3357–3375 it binds a winner into the timer delegate;
   `DelayedEndRound` at 1558 trusts that value. If the last survivor dies during
   the delay, a previously selected win can survive instead of becoming a draw.
   Recheck authoritative alive counts when the delay expires.

2. **Reconnect loses displayed match damage and PPR history.**
   `Logout` at `ElimPlusGame.cpp:4027–4033` removes PlayerState-keyed damage and
   match-PPR accumulators. Completed rating history uses stable IDs and survives,
   so ratings and the post-reconnect scoreboard can disagree. Move match history
   to stable account IDs while retaining pointer cleanup for live actors.

3. **Leaving during a round drops that player's round from rating collection.**
   `EndRoundForTeam` at `ElimPlusGame.cpp:1228–1299` collects current PlayerArray members;
   logout has removed the departure-round damage. Leaver recovery in
   `ElimPlusRatingSystem.cpp:968–977` only recovers rounds already captured.
   If a team is empty, `ProcessRound` at 449–452 skips the round entirely. Capture
   participation and round stats by stable ID, including players who disconnect.

4. **Some final global ranks can be stale.**
   `ElimPlusRatingSystem.cpp:723–728` queries ranks inside the same loop that is
   still writing other players' ratings. Refresh ranks after all writes finish.

5. **A partial rating read can discard lifetime damage.**
   `ElimPlusRatingSystem.cpp:306–330` accepts a successful core-row read when the
   separate damage/name query fails, caching zero lifetime damage. A later
   successful v3 replacement at 675–688 can overwrite the prior history with a
   zero-based total. Retry/fail closed on extras reads when the v3 schema is known.

Two smaller discrepancies remain: PostLogin can publish a mutable rating on
reconnect despite the intended frozen-match display, and rostered versus recovered
leaver uploads use different damage sources. None of these gameplay/persistence
paths is changed by the scoreboard presentation patch.

## Evidence and validation

- Supplied package: `Elimination_113-WindowsNoEditor.pak`, 2,465,944 bytes.
  SHA256: `0f997096770fadb4641e4bd5934db7a1ac04f7468124600a68d7081c908bf160`.
- Extracted 136 files using the bounded PAK reader. The recovered ALTSHUD matches
  the older local extraction byte-for-byte. Cooked bytecode/default inspection
  establishes the stored implementation, not every runtime configuration.
- Original evidence: `Plugins/.codex_tmp/elim113-audit-20261008/`, including
  `ALTSHUD.bytecode.txt`, `ALTSHUD.defaults.txt`, and the original extracted assets.
- Current Blueprint evidence: `Plugins/.codex_tmp/ncstock-elim113-audit/`, including
  `current-weapon-comparison.json`, `editor-NCStockWeapons-script.txt`, and
  `editor-ElimPlusMutator-script.txt`.
- Current editable assets:
  `C:/GDriveUT4/LAEditorUT4/UnrealTournamentEditor/UnrealTournament/Content`.
  Older inspected NCStock paks still had the UTMutator parent; the editable
  NCStock now has NCPickupBaseMutator. Those old paks are not proof of the current
  build's behavior.
- Scoreboard validation: source review and whitespace checks. No UE4.15 build,
  cook, live server configuration check, or rendered gameplay verification.
  After rebuilding, compare both skins in ready-up, live rounds, death/spectating,
  match end, and standalone, including long names and ranks at 1080p.
