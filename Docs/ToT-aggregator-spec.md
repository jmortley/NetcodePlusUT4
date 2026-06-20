# ToT Aggregator Spec — peer-relative trigger detection (drop FirstFramePct)

Status: design, parked aggregator. Engine emitter (`NCToTCollector`) already ships descriptive
output; this spec defines the pooled, peer-relative flag logic that consumes it (StatSQL/Django
or a standalone script over pooled CSVs — home unresolved).

## What ToT measures

Per shot, the **dwell** between when the crosshair acquired an enemy (per-frame on-target trace)
and when the player fired. A human reacting/tracking produces a **broad** distribution with a fat
**upper tail** (reaction shots ~150 ms+, tracking and held shots into the hundreds of ms). A
trigger-bot fires within its engine reaction floor of acquisition (single-digit to low-tens of ms)
and **cannot hold** → a tight low cluster with **no upper tail**.

The engine is an EMITTER only — per match it writes per-player descriptive stats + server-time-
stamped raw shots, NO verdict. This aggregator pools a player's shots across matches and produces a
peer-relative suspicion for HUMAN REVIEW (demo). No auto-ban.

## Empirical basis (dogfood, CTF-Duku, 2026-06-20, ~500 fps)

| metric          | clean human (phantaci) | trigger-bot (same player) |
|-----------------|------------------------|---------------------------|
| n               | 14                     | 19                        |
| median dwell    | 41 ms                  | 13 ms                     |
| max dwell       | 288 ms                 | 116 ms                    |
| FastFrac @16ms  | 14%                    | **68%**                   |
| HeldFrac @150ms | ~7%                    | **0%**                    |
| FirstFrame%     | 7%                     | 0% (INVERTED)             |
| CV              | 1.17                   | 1.26 (INVERTED)           |

ServerShield (accuracy/aim/hit-distribution) did **not** flag the trigger-bot (composite 8.5) —
server-side behavioral AC is structurally blind to a fire-*timing* cheat; the human is doing the
aiming, so accuracy and hit-placement look human. ToT is the only signal that separates them.

## Metric changes

### DROP — FirstFramePct
`dwell <= frame_ms`. It pegs the "impossibly fast" threshold to **frame time**, but human reaction
(~150 ms) is an absolute physiological constant independent of fps. At 500 fps the bar is 2 ms, so a
real trigger-bot (~10 ms) reads ~0% and looks clean; at 60 fps the bar collapses onto the
measurement floor. Empirically INVERTED here (clean 7% > bot 0%). Unsalvageable as a frame-relative
metric — "fixing" it means pinning the threshold to absolute time, which makes it FastFrac below.

### DROP from the flag (keep descriptive) — CV
Trigger-bot CV (1.26) was HIGHER than clean (1.17): two held-ish outliers inflate variance, so a
"low CV = bot" rule CLEARS the bot. Too outlier-sensitive at these sample sizes. Keep CV in the
descriptive output; do not use it as a flag input.

### PRIMARY — HeldFraction
`HeldFraction = count(dwell > HELD_MS) / N`, default `HELD_MS = 150`.
The "track-and-hold absence." Humans have a fat upper tail (reactions + tracking + holds); a
trigger-bot cannot hold without surrendering its edge → ~0. It lives at the **top** of the
distribution, so it is immune to the frame quantization that corrupts the low end (acquire is
detected once per frame). 150 ms = 5–75 frames across 60–500 fps → measurable and meaningful at any
fps. **Lower HeldFraction = more suspicious.** This is the fake-resistant discriminator: a bot can't
reproduce the human upper tail without giving up the reason it exists.

### SECONDARY — FastFrac@Nms
`FastFrac = count(dwell <= FAST_MS) / N`, default `FAST_MS = 30` (covers a 1–2 frame bot reaction
from 60–500 fps; well under the 150 ms human floor; 16 ms worked at 500 fps but 30 generalizes).
Corroborates HeldFraction. **Higher = more suspicious.** High-fps humans have finer quantization →
slightly higher FastFrac → compare peer-relative, never as an absolute cut.

## Aggregation (pooled, peer-relative)

1. Pool each player's qualifying on-target shots across matches (off-target spam already excluded by
   the emitter gate).
2. Require `MIN_SHOTS` per player (default 90, matching the instagib accuracy floor; ToT-specific
   floor >= ~60 acceptable) — else abstain for that player.
3. Within a peer set, compute peer **median** and **MAD** of HeldFraction and FastFrac.
4. Robust-z per metric: `z = (x - median) / (1.4826 * max(MAD, MAD_FLOOR))`.
   `1.4826` converts MAD->sigma. `MAD_FLOOR` stops clustered bounded fractions from inflating z into
   a silent false positive.
5. Flag candidate iff `HeldFraction z <= -Zcrit` AND `FastFrac z >= +Zcrit` (default `Zcrit ~3`).
6. **Abstain if fewer than `MIN_PEERS` qualifying players (default 8)** — n=3 can't carry a MAD.
7. The two metrics are ~ONE latent axis ("no human upper tail"). Treat the AND as corroboration, NOT
   two independent votes — do not multiply confidences; set `Zcrit` accordingly.
8. Output a ranked suspicion list, persisted over time. The strongest signal is the SAME player
   surfacing across many pooled windows. Conviction is human demo review, never the score alone.

## fps rationale (why absolute, and why the upper tail)

- `dwell = fire_time - acquire_time`; acquire is sampled by a once-per-frame on-target trace, so the
  LOW end of dwell is frame-quantized — a 60 fps player physically cannot produce a sub-16 ms dwell,
  cheating or not. Any metric anchored at the low end fights the measurement resolution and drifts
  with fps.
- Human reaction (~150 ms) is an absolute floor, unrelated to fps. So discriminating thresholds must
  be ABSOLUTE time, and the most robust live at the UPPER tail (HeldFraction), which frame
  quantization cannot reach.
- Ship `frame_ms` per shot for context; never put it inside a threshold.

## Engine emitter (NCToTCollector) — descriptive only, per player per match

`N, mean, sd, CV (descriptive), p10/p50/p90, min/max, HeldFraction@150, FastFrac@30,
median frame_ms, on-target hit/miss split`, plus server-time-stamped raw shots to
`ToT_<match>.csv`. No flag/verdict in the engine — the aggregator owns the decision.

## Open questions

- Aggregator home: StatSQL/Django table vs standalone script over pooled CSVs.
- Peer set: per-lobby pooled vs per-season global baseline (or both, with different `Zcrit`).
- Final `HELD_MS` / `FAST_MS` / `Zcrit` calibration from a clean-league corpus + known-cheat samples.
- Evasion: a cheat that injects a randomized human-like reaction delay AND occasional fake held
  shots would erode both metrics — bound what this catches (naive/greedy triggers) vs what still
  needs demo review.
