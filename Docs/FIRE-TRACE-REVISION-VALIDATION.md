# Fire-trace review revisions — 2026-09-27

Revises 4.15 `f7d5893` on `328-release-candidate` and 4.27 `79bd45c`
on `codex/ncp-fire-diagnostics-427`.

## Scope

Disabled-by-default instrumentation and report corrections. No new RPC/reflected
field or change to input acceptance, fire cadence, equip/put-down timing, retry
policy or weapon-state selection. The stock shared byte-index behavior remains
unchanged; the trace exposes its actual validator result for investigation.

- Fixed requests on stock firing states now log acceptance and retain event/generation
  through their first synchronous dispatch. Runtime firing-state slots are recorded.
- Stock send bytes, validation results, sequence entry/results, Stop gates, and sync
  origin/decisions are separate records. Continuous paths have hold summaries;
  charged releases have guarded volley summaries, without fabricated pulse matching.
- A shared fresh nonce, exact wire-timestamp float bits, local connection/weapon
  lifetime checks, owner-retention markers and player filtering prevent unsafe joins.
- Delayed fake reservation outcomes, cancellations, server-only stream dispatches,
  cone execution, empty captures, aborts and capture-epoch scope invalidation are covered.
- Shipping instructions use authenticated rcon; the old server-only checker recognizes
  new cancellation reasons and explicitly reports them.

## Verification

Python: `python -B -m unittest discover -s tools/tests -p test_fire*.py`.
93 tests pass independently in both source trees. Includes stored synthetic C++
printf-format fixtures, stock-state fixed dispatch, exact wire equality, wrong/missing
session identity, connection/GUID reuse, player filtering, owner loss/trade grace,
stock validation/sync, byte reuse, charged releases, delayed fakes, abort/empty captures,
legacy cancellation handling and server-only text output.

C++: 28 distinct translation-unit/configuration syntax checks passed (`/Y- /Zs`):

| Configuration | Units |
| --- | ---: |
| 4.15 Win64 Shipping client | 11 |
| 4.15 Win64 Shipping server | 3 |
| 4.27 Win64 Development editor | 11 |
| 4.27 Win64 Shipping client | 3 |

The 11-unit set is NCFireDiagnostics, UTWeaponFix, UTPlusSniper,
UTPlusWeap_RocketLauncher, UTWeap_Minigun_Plus, UTWeap_LinkGun_NCP,
UTWeap_LinkGun_Plus, UTWeaponStateFiringLinkBeam_NCP,
UTWeaponStateFiringLinkBeam_Plus, UTWeaponStateFiringChargedRocket_Transactional,
and UTWeaponStateFiring_Transactional. The 3-unit set is NCFireDiagnostics,
UTWeaponFix and UTWeaponStateFiringChargedRocket_Transactional.

The checks use existing generated headers/target definitions and installed compiler
headers, not a full UBT build. The 4.15 server response needed explicit installed
MSVC/UCRT include directories. The new 4.27 cone observer uses GetLocalRole() in
place of 4.15 Role. Diagnostic source deltas otherwise match; helper, tests, tools
and capture instructions match between trees. Git whitespace checks pass.

## Not yet verified

No full compile/link, Linux compile, cook, package, deployment or Unreal launch was
performed for these revisions. No live paired capture was collected. Shipping rcon
reachability through the deployed private hub, runtime Blueprint state layouts,
logging overhead, mixed old/new binaries and actual packet/input behavior remain
runtime checks. See FIRE-TRACE-CAPTURE.md for the capture and negative-control matrix.
