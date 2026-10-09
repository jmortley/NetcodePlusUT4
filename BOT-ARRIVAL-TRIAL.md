# MutBotEvents arrival warning trial

This is a server-only NetcodePlus source change for UT4IGBot's warning trial.
Keep `NETCODE_PLUGIN_VERSION` at **328**. No replicated properties, RPCs,
client assets, or weapon interfaces are changed. No additional plugin is needed.
Native builds and deployment are left to the maintainer.

## Identity and observations

The existing `/state_change` player rows gain a string `StatsID`, using the same
`AUTPlayerState::StatsID` that StatSQL sends to Django. `Id` and `Index` remain
the original numeric engine `PlayerId`; their meaning and type do not change.
Old bot versions can ignore the additional field.

The warning trial uses authenticated `/arrival` posts from this same mutator.
It is enabled only when launch options contain a valid `ArrivalId` from the
updated bot. These messages retain first-login times across disconnects and
include an instance ID, sequence, monotonic elapsed time, match state, and
bounded cumulative account evidence. Attendance identities come from the
server connection's `UniqueId`, not the client-reported `StatsID`, names,
shared passwords, teams, or ready state. This is the account identity normally
used by StatSQL/Django and linked to Discord. Unresolved identities make
absence unknown; they are never substituted with a numeric slot or a name.

Human spectators and brief visits count as arrivals. Recording spectators and
bots do not. The bot evaluates a six-minute warmup window with a 15-second
timing allowance. Lost/incomplete observations and restarts produce staff
diagnostics only. This trial cannot ban players or cancel PUGs.

## One-server rollout

1. Deploy the updated UT4IGBot source with `ARRIVAL_MODE=warn`, its existing
   nonempty `API_SERVER_TOKEN`, and the existing public PUG/staff channels.
   `ARRIVAL_ENFORCEMENT_NOTICE_DATE=2026-09-11` is notice text only; there is no
   automatic switch to bans. Use `ARRIVAL_MODE=off` to disable the trial.
2. Build NetcodePlus for the dedicated server's actual platform/configuration
   and deploy that server module to the trial box. Retain the shipped client
   and UT4AC binaries. Keep the existing `MutBotEvents` launch entry and
   `[BOT_EVENTS]` URL/token configuration; the bot supplies `ArrivalId`.
3. Start a new controlled PUG. Existing running PUGs are not enrolled. Other
   boxes with older modules continue normal reporting; their arrival trials
   time out as unknown after 20 minutes and can only produce staff diagnostics.
4. Join using the unchanged version-328 client and shipped UT4AC. Verify join,
   travel, normal scoring/StatSQL, UT4AC capture generation and Django import.
   Check `/arrival status pug_id:<id>` and staff evidence for correct account
   matches. Test an unready/spectating player, a brief visit, and a late player.
5. Interrupt telemetry or restart a trial server. Confirm staff-only unknown
   results. Public warnings belong only to a complete observation window with
   at least one confirmed on-time roster arrival. No trial path issues bans.

## Release provenance

The source baseline is `f95ff1a7733750fc9ade4c86a3ab8a1d33f9f08d`; its changes
since UT4AC dogfood18's NCP target `4328a0836dbe68e4dea9e324b1377cacbc4c6d59`
are documentation only. The compatibility claims and server-only rollout here
apply only to the arrival changes in `MutBotEvents`, `NCBotArrivalTests`, and this
document. This branch also contains xTDM native/client and content changes; a
build of the complete branch is not an arrival-only hotfix. For an arrival-only
deployment, isolate those arrival changes against the stated baseline. For the
combined branch, also follow the build and client validation instructions in
`XTDM.md`.

UT4AC remains built against its original target and will still record that
target in its manifest. It does not discover this hotfix's running server
revision. Record the new server source revision/patch and module checksum in
the deployment record; do not label the old target pin as the actual hotfix
binary identity or rewrite historical pins. This patch does not change
UT4AC's evidence schema or require a Django importer update.

Compatibility is supported by source review, not a completed deployment test.
The maintainer's native build and unchanged-client checks above remain to be
performed. The native test group is `NetcodePlus.BotArrival`.
