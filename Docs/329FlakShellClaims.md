# 329 Flak secondary hit claims

Requires matching updated 329 client and server builds. The added weapon RPC
and initial projectile properties are not a 328-compatible hotfix.

## Exact shell selection

Authority assigns each `AUTPlusProj_FlakShell` a nonzero, monotonically increasing
`ShotId` scoped to its firing weapon actor. The real shell initially replicates
that ID and the original weapon reference. The shooter's client reports contact
from that replicated shell. Cosmetic fakes do not submit claims.

The server records the ID and firing pawn before projectile catchup. A claim
must match that exact entry and current pawn, whether the entry is live or
resolved. A newer live shell cannot hide an older shell's grace entry. Missing
identity or an unmapped weapon reference does not fall back to class lookup or
fire-mode FIFO. Legacy claims cannot consume identified shells or loaded rockets.

Switching weapons preserves in-flight claims. Inventory removal clears Flak
entries, including when the same pawn later picks up that cannon. The counter
does not reset, and exhaustion refuses new identities instead of reusing one.
The RPC also requires the existing 329 protocol handshake.

## Damage and terminal state

Accepted live claims consume their entry before invoking the real shell's
`ProcessHit`. This preserves native direct damage, the configured single-shard
direct-impact bonus, explosion, splash and shard behavior. Repeated claims
cannot select another shell or reapply damage.

Terminal snapshots occur inside the actual authority `Explode` transition.
An overlap rejected by `ProcessHit` no longer records a false terminal impact.
Flak resolved grace is deliberately limited:

- Any previous pawn impact makes the shell ineligible.
- Any configured shard-producing explosion makes it ineligible. World-impact
  shards may damage a target later, outside the original splash radius. A
  subsequent direct-hit award would risk duplicate damage.
- Shardless world explosions can use the existing grace window, with captured
  adjusted damage and momentum. The explosion's possible splash targets cannot
  receive another award; the candidate query includes blocked and nonblocking
  overlaps conservatively.
- Missing or invalid snapshots fail closed. Expiry and the ten-entry bound stay
  in place.

Consequently standard shard-producing shells gain exact **live** hit validation,
not unrestricted recovery after explosion. Recovering those later claims would
also require accounting for child shards' past and future damage.

## Limits and release checks

This adds no per-frame stream. It adds initial identity replication and one ID
to each Flak claim. Existing ping, rewind, capsule contact, path and LOS checks
remain. Weapon Blueprint projectile-rewind opt-in is still required, as is a
shell Blueprint derived from `AUTPlusProj_FlakShell`.

It does not change the ballistic fake matcher, projectile trajectory, catchup
precheck or constant-gravity rewind approximation. Nor does it create a claim
for a close-range projectile that never reaches the shooter's client.
The shardless grace snapshot uses explosion-time `GetDamageParams(nullptr)`;
custom target-dependent damage overrides are outside that grace contract.

Native regression tests compile the production tracking and validation methods
against observable engine shims. Packaged multiplayer testing should cover two
balls in flight with different paths, reversed contact order, a switch while a
ball is airborne, drop/re-pick, close-range impacts and natural shell/shard
damage at 0/20/40 ms. Verify a successful live rescue retains normal direct-hit
damage and a resolved shell never adds damage after splash or child shards.
Use the existing `ut.RocketLagCompDebug` diagnostics; claim lines include
`flakShot` alongside the loaded-rocket identity.

Local validation (2026-10-06): all 29 projectile-claim tests pass, including
compilation of the actual capture, cleanup, lookup, client notification, RPC,
snapshot and common validation methods. Six related rocket-volley, fire-anchor
and client-fire-timing tests also pass. UE4.15 NetcodePlus module builds pass for
Win64 Development Editor, Win64 Shipping client and Linux Shipping server with
the aim-assist test fixture disabled. These are module builds, not a packaged
multiplayer playtest.
