# 329 bounded movement/fire anchor validation

`python -B tools/tests/test_fire_anchor329.py` compiles the complete production
NCFireAnchor.cpp and NCFireAnchorPolicy.h with a small Unreal adapter. All 62
native checks pass. This checks source logic, not an Unreal/UHT build, real
collision queries, replicated actor ordering, or an actual delayed network.

Production behavior:

- Mode defaults to 1 (shadow). Mode 0 disables admission; mode 2 applies only an
  eligible snapshot selected by the caller's precision-hitscan scope.
- Only the newest server-observed shot-marked movement timestamp is eligible.
  Match is exact, not a scan of historical client-selected positions. It is still
  bounded client-originated evidence, not a trusted physical-click timestamp.
- A matched marker is consumed once even if origin, aim, RTT, age, or geometry
  rejects it. A wrong timestamp does not select or consume another sample.
- Marker age is at most 80 ms; total base rewind plus measured movement/fire gap
  and server queue residence cannot exceed the existing one-way cap, with a hard
  upper limit of 125 ms. RTT is frozen by the caller on acceptance. The helper
  does not grant backdated fire-rate credit or catch-up firing.
- Client origin stays within 20 uu horizontally and 24 uu vertically of the
  server movement eye; aim differs by at most 2 degrees. A world trace rejects an
  intervening surface. Nonfinite/out-of-bounds data never reaches that trace.
- The frozen anchor binds weak pawn, weapon, world, player controller and network
  connection identities, plus a monotonic generation. Death, owner changes,
  world changes, connection changes, timestamp reset, teleport markers and
  weapon invalidation prevent reuse. Module/world cleanup releases the registry.
- Extra compensation requires an open connection and ACK traffic within 2 s,
  measured in net-driver time, with tolerance for its float/double storage. This
  is only an ACK-liveness check: LastRecvAckTime
  is not the time of the last successful AvgLag sample, so stale RTT measurement
  freshness is not claimed to be solved here.

Native coverage: finite-before-physics, failed-claim single consumption, exact
stamp matching, origin/aim/wall limits, unchanged cap and queue age, shadow/off,
controller/connection/world/death/teleport/weapon/object-serial changes, failed
component lifetime, world cleanup/module shutdown and ACK liveness.

Runtime acceptance remains pending. Measure shadow admission under ordinary
shots, timer shots, buffered clicks, fast flicks, crouching/sliding and teleports.
A successful FlushPendingMoveForShot binds the exact CurrentTimeStamp sample;
an already-sent unmarked move produces no eligible marker and retains the legacy
path. Test separately delayed fire and jointly delayed movement/fire: only the
server-observed gap between their processing is measured by this design.
