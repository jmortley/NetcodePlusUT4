# 329 protocol gate validation

The 329 shot RPC schema requires matching client and server packages. The gate
requires an exact `ServerReportVersion(329)` before a remote player can use the
new fire implementation. It does not make mixed 328/329 replication safe: UE4.15
can reject a mismatched class/RPC bunch before the implementation guard runs.
Do not promise a graceful version popup in every mixed-build path.

The existing gate's one reliable `ServerReportVersion(int32)` RPC and its lone
nonreplicated UPROPERTY are unchanged. Server-only bookkeeping now binds exact
confirmation to the weak controller, world and UNetConnection. A hub advisor
report is never match authorization. Movement, spectators and elimination no
longer bypass the match handshake; timeout is still configurable with a 100 s
default and a final 5 s grace. Exact reports arriving during that grace succeed.
Disconnect uses the existing non-banning GuaranteedKick path.

Unknown/replaced sessions fail closed and start a new match gate. Duplicate
SpawnFor calls do not reset a pending deadline or erase an established mismatch.
Seamless travel must confirm again; the next protected fire query creates a gate
if PostLogin did not run. This can cause initial predicted shots to be rejected
until confirmation, so test first-spawn and post-travel behavior explicitly.
There is no client-ready acknowledgement in this unchanged gate protocol.

Cleanup: weak identities prevent reused object addresses from inheriting state;
logout removes the session and destroys its pending gate; world cleanup removes
that world's entries; EndPlay clears gate timers; module shutdown unregisters
cleanup/logout hooks and destroys remaining gates. A stale gate cannot authorize
or disconnect a newer world/connection session. A destroyed pending gate may be
retried, while confirmation outlives normal gate destruction.

## Checks run

`python -B tools/tests/test_version329_gate.py`

Two tests pass, including 52 native checks compiling the actual production
identity predicates, version transition, registry cleanup, SpawnGate and
IsProtocolConfirmed against a small Unreal adapter. Scenarios cover exact match,
mismatch terminality, unknown/pending denial, same-controller travel, changed
connection, closed connections, stale reports, duplicate spawn calls, hub-to-match separation,
logout, cleanup, destroyed pending gates, object serial reuse, spawn failure,
local/bot exemptions and client-side denial. The second test checks the 329
constant, stable gate RPC/property declarations, absence of the former movement
and pawnless bypasses, and lifecycle hook/timer wiring.

This is not a UBT/UHT or network integration test. UE weak object serial behavior,
actor replication and timeout execution are represented by an adapter, not a
running Unreal process.

## Required before rollout

- Build matching Windows client and Linux server with UBT/UHT.
- Join a fresh 329 match, fire immediately and after confirmation, then respawn.
- Seamless travel, spectator/eliminated joins, reconnect and listen-host/bots.
- Mismatched 328/329 clients in both directions: ensure rejection and no gameplay
  from an unconfirmed remote player. Expect engine-level failure to be possible.
- Lost/delayed gate replication: pending fire remains denied; late matching
  report during grace succeeds; unknown timeout disconnects without instance ban.
- Hub advisor remains whisper-only and cannot authorize a match.

Do not install a global FNetworkVersion override merely to enforce this plugin
boundary. Stock UT already owns the process-global, cached version hook; a 329
checksum would also change connectivity to stock servers. A true pre-actor,
per-session plugin negotiation needs a separate design.
