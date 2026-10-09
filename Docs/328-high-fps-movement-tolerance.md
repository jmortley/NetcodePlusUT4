# High-FPS movement correction tolerance

The 328 release candidate raises the TeamArena movement position-error threshold
from 14 to 18 Unreal units (`MaxPositionErrorSquared`: 196 to 324). This is a
modest tuning change following reported jitter around 700 FPS, not a measured
threshold that establishes the underlying problem is fixed.

The comparison runs on the server. UT's `UTServerMoveHandleClientError` compares
its simulated position against the client-reported position. A position error
within the threshold receives a good-move ACK without copying the client's
position into the server pawn. Larger errors and incompatible movement modes
still use the existing correction path. Allowing a wider difference can leave
the client's view of its own position farther from the server's position.

This changes neither collision simulation, hit-validation tolerances, dodge
timing nor movement send frequency. It requires the updated NetcodePlus server
binary and a restarted instance/new pawns. A client-only DLL cannot change the
server's correction threshold. Modes must use TeamArenaCharacter or a subclass
with its movement component; stock movement components are unaffected.

## Prediction and the 700 FPS case

UT predicts the local pawn immediately, saves each move, and sends those moves
for server simulation. After a correction it replays unacknowledged moves.
The engine derives prediction delta time from the saved float timestamps so
the client and server use matching timestamp differences. Different render and
server tick rates alone do not prove a simulation mismatch.

The custom `UTCallServerMove` batches ordinary ground movement at roughly 40 Hz
and airborne/high-speed/important movement at roughly 90 Hz. However, its loop
sends every unsent saved frame as a Quick/Saved move, followed by the full latest
move. The inherited UT `ReplicateMoveToServer` creates a saved move per client
frame and does not combine them. Consequently a 700 FPS client can approach
700 movement RPCs per second before important-move resends. These are not 700
separate UDP datagrams: RPCs can share packets.

Do not drop saved moves or lower their flush rate merely to accommodate optional
anticheat traffic. Combining moves would need a separate simulation change with
tests for dodge/slide/shot boundaries, impulses, moving bases and replay. The
current change only adjusts correction tolerance.

## Verification and remaining runtime check

The change was checked against this fork's `UTCharMovementReplication.cpp`
correction/ACK and replay paths and `CharacterMovementComponent.cpp` timestamp
handling. It adds no RPCs, reflected fields or engine overrides. Source diff and
whitespace checks pass; no Unreal build or multiplayer test was run here.

Compare the same server at 700 FPS and a lower cap with continuous UT4AC behavior
capture disabled. Exercise ground movement, repeated dodges, jumppads, slopes
and knockback. If the issue remains, measure correction sizes and transport
loss/headroom before increasing the threshold again. The engine's
`p.NetShowCorrections` instrumentation is excluded from Shipping in this fork;
it is not an available Shipping diagnostic command.
