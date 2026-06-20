# ToT Aggregator Spec v2 — distribution-shape trigger detection

Status: design, parked aggregator. v2 supersedes v1 after a Gemini adversarial review + domain
corrections. The engine emitter (`NCToTCollector`) ships descriptive output; this spec defines the
pooled detector that consumes it.

## Changelog v1 -> v2
- **DROP the fast/held bins + per-metric robust-z.** Gemini (verified): robust-z on a fraction
  bounded near the human median is structurally unable to flag — a perfect 0% bot reaches only
  z = (0-median)/(1.4826*MAD) ~= -1.6 at median 0.07 / MAD 0.03, never -3. Hard bins are also
  trivially evaded (a [32,45]ms delay zeroes FastFrac while staying superhuman; 10% padded held
  shots passes HeldFraction). Both removed.
- **Replace with whole-distribution testing** (Anderson-Darling + Hartigan dip) on the dwell ECDF.
- **Pipeline correction:** NetcodePlus POSTs ToT straight to a ut4stats Django endpoint. It already
  has HTTP (the collector's existing report POST). StatSQL is NOT a courier here — that pattern is
  ServerShield-only, because SS has no HTTP of its own.
- **Instagib playstyle correction (domain):** top instagib is crosshair-placement + pre-aiming +
  predictive fire, so legit low dwell and low held-fraction are NORMAL. Absolute thresholds on
  "fastness" are invalid for this mode. Only distribution SHAPE (spike vs broad/multimodal)
  discriminates, and the reference MUST be top-tier instagib players, not generic dogfood.

## What ToT measures
Per shot, the dwell (ms) between crosshair acquiring an enemy (once-per-frame on-target trace) and
firing. Dwell is CLIENT-measured (client stamps acquire time, computes dwell at fire, ships via RPC)
-> immune to server-tick packing, but spoofable -> this is a SCREEN that triggers demo review, never
an auto-ban or sole conviction.

## Step 0 (GATING) — is instagib even separable?
Before building anything, pull the dwell distributions of 5-10 known-clean top-tier instagib players
over many matches. Confirm each is BROAD / multimodal: a low-dwell predictive/pre-fire mode PLUS a
reaction mode (~120-200ms) PLUS a tracking/held tail. If a top pre-aimer's distribution is itself a
tight low spike (no reaction/tracking spread), a trigger-bot is indistinguishable and ToT must be
limited to a soft review-trigger or dropped for instagib. Everything below assumes step 0 passes.
The detector's whole job is "is this player's dwell distribution a SPIKE or a human SPREAD."

## Detector
For each player, pool dwell over a rolling window (see Pooling) and build the ECDF. Compute three
shape features; combine into ONE score (NOT an AND of independent thresholds):

1. **Shape divergence** — two-sample **Anderson-Darling** of the player's dwell ECDF vs a pooled,
   skill-matched human REFERENCE ECDF. A-D over KS: more sensitive to the tails (the reaction /
   tracking modes a trigger lacks). Output = the A-D statistic.
2. **Spikiness / unimodal collapse** — **Hartigan dip** + a concentration measure (mass within the
   modal +/-1-frame bin). A trigger spikes at its reaction floor; a padded cheat (90% spike + 10%
   fake held) goes bimodal -> the dip catches what a single divergence misses when the spike
   dominates.
3. **Reaction-mode deficit** — mass in the human reaction band (~100-220ms) RELATIVE to the
   reference. Pre-aimers still have reaction shots; a trigger has ~none. Use the relative deficit,
   not an absolute count.

Combine via **Mahalanobis distance** of [A-D, dip, reaction-deficit] against the skill-stratum's
feature covariance (or a logistic calibrated on labelled clean + known-cheat samples). One score ->
no rectangular AND-region, no bounded-fraction z, correlated features handled by the covariance.

## fps normalization
Quantize every dwell to a common temporal grid (one 60fps frame = 16.67ms) BEFORE building the ECDF,
so mixed-fps peer groups aren't biased by a high-fps player's finer low-end resolution. The shape
tests live mostly in the mid/upper distribution, which quantization barely touches.

## Pooling & baseline
- **MIN_SHOTS >= 300** pooled per player (a mechanical baseline, not a hot streak). ~2-4 instagib
  matches.
- **Reference = global, skill-stratified, rolling 30-day** clean/general-population ECDF, bucketed by
  ELO so a top player is compared to other top players (kills the smurf/skill confound and answers
  "who is the peer when I pool one player across many lobbies"). NOT a single-match peer set.
- Abstain if the player's stratum reference has too few contributors to be stable.

## Data pipeline (NetcodePlus -> Django -> Postgres)
- **Emitter:** at match end, NCToTCollector POSTs per player `{stats_id, match_id, mode, fps_median,
  N, dwell_histogram[] (5ms bins 0-500ms + overflow), reaction_band_count, hit/miss split}`. A binned
  HISTOGRAM, not per-shot rows -> compact, but enough to rebuild ECDFs and run A-D + dip on pooled
  data. Neutral endpoint + field names (no "trigger"/"bot" in shipped strings).
- **Django:** new `utstats` model `ToTProfile` (FK player, FK match, mode, fps_median, N, histogram
  int array, reaction_band_count, created_at) + migration + an authenticated ingest view. Justified
  WRITE endpoint (the "no new endpoints" rule was about read endpoints).
- **Aggregator:** a management command pools each player's histograms over the rolling window, builds
  the ECDF, computes the per-ELO-stratum reference ECDF, runs the detector, writes a ranked suspicion
  table. Persist per-run rows: the strong signal is the SAME player surfacing across many windows.

## Evasion bounds
- Catches: naive/greedy triggers (tight spike), padded triggers (bimodal via dip), delay-shifted
  triggers (A-D still sees the missing reaction/tracking modes and the wrong shape).
- Evades: a cheat that SAMPLES its delay from a real recorded human dwell distribution (matches the
  shape) -> demo review. Spoofing (client-measured dwell) -> a tampered client can lie; cross-check
  with server-coarse reaction where possible, else treat as a review trigger only.

## Open
- Aggregator: Django management command vs a service.
- Reference seeding (chicken-and-egg): bootstrap from the general population, then refine by removing
  demo-confirmed cheats.
- Final A-D / dip / Mahalanobis thresholds from the step-0 corpus + a few known-cheat captures.
