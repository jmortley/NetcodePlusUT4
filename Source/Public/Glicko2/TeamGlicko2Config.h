// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Cross-file
// includes resolved via Plugins/NetcodePlus/Source/Public/Glicko2 added to
// NetcodePlus.Build.cs PublicIncludePaths. Update: re-pull from upstream and
// re-vendor, then re-apply the LOCAL MODs below.
//
// LOCAL MODS (ElimPlus tuning — 2026-04-30):
//   - kDefaultRD:       350.0 -> 200.0   (fresh-player ramp control)
//   - kBeta:            0.2   -> 0.15    (less perf-z-score amplification)
//   - kMaxRatingChange: 1.73  -> 0.5     (per-match swing cap: ~300 ELO -> ~87 ELO)
// Each LOCAL MOD has a "// LOCAL MOD" comment block with reasoning at the
// constant's definition site below.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMGLICKO2CONFIG_H_
#define GLICKO2_INCLUDE_TEAMGLICKO2CONFIG_H_

namespace TeamGlicko2 {
    // ========== Glicko-2 Base Parameters ==========

    /// Default initial rating (R_0)
    static const double kDefaultRating = 1400.0;

    /// Default initial rating deviation (RD_0)
    // LOCAL MOD: upstream 350.0 -> 200.0.
    // Why: 350 makes brand-new players converge so fast that round-1 of their
    // first match almost always hits kMaxRatingChange. 200 still allows quick
    // convergence (fresh players settle in 3-4 matches) but prevents the
    // "first match jumps you 300 ELO" feeling that's surprising in a casual
    // 4v4 context. Mid-experience steady-state RD lands around 80-150
    // regardless of the starting value, so this only affects the first ~3
    // matches per player.
    static const double kDefaultRD = 200.0;

    /// Default initial volatility (sigma_0)
    static const double kDefaultVolatility = 0.06;

    /// Lambda: Team uncertainty balance weight
    /// Objective = |avg(S_A) - avg(S_B)| + lambda * |avg(U_A) - avg(U_B)|
    /// Uses averages for fair comparison in uneven teams
    /// Range: [0.0, ∞)
    /// - 0.0 = ignore team uncertainty balance
    /// - 0.1 = small preference for balanced uncertainty (recommended)
    /// - 1.0 = strong preference for balanced uncertainty
    static const double kLambda = 0.8;

    /// Glicko-1 to Glicko-2 scale factor (173.7178)
    static const double kScale = 173.7178;

    /// System constant (tau) - controls volatility change rate
    static const double kTau = 0.5;

    /// Convergence tolerance for volatility update (epsilon)
    static const double kConvergence = 0.000001;


    // ========== Performance Weighting Parameters ==========

    /// Beta: controls how strongly performance influences rating changes
    /// Uses sign-aware formula: f_i = 1 + β·sign(Δμ)·z_i
    /// This ensures good performers gain more in wins and lose less in losses
    /// Recommended range: [0.15, 0.30]
    // LOCAL MOD: upstream 0.2 -> 0.15.
    // Why: kBeta multiplied by a teammate-relative z-score adds up to 30%
    // (clamped by kScaleMin/kScaleMax) on top of the base rating change. In
    // a 4-player team where one player has all the damage stat (because bots
    // don't track damage), their z-score is artificially high and the bonus
    // exaggerates rating gain. 0.15 sits at the bottom of the recommended
    // range — still rewards top fraggers, just less aggressively.
    static const double kBeta = 0.15;

    /// Minimum performance scaling factor
    static const double kScaleMin = 0.5;

    /// Maximum performance scaling factor
    static const double kScaleMax = 1.5;

    /// Small constant to avoid division by zero in standard deviation
    static const double kEpsilon = 1e-6;


    // ========== Optional Rating Change Clamping ==========

    /// Enable clamping of rating changes (set to false to disable)
    static const bool kEnableRatingClamp = true;

    /// Maximum rating change per match (in Glicko-2 scale)
    /// This corresponds to approximately 300 points in Glicko-1 scale
    // LOCAL MOD: upstream 1.73 (~300 ELO) -> 0.5 (~87 ELO).
    // Why: ProcessMatch is called once per ROUND in ElimPlus, so the cap
    // applies per-round. With the upstream value, a fresh player (high RD)
    // sweeping a 3-round match could gain ~+300 ELO in round 1 alone. 0.5
    // means no single round can swing more than ~87 ELO — first matches
    // accumulate sensibly across rounds (~+30-60 typical, ~+150 worst-case
    // multi-round), and steady-state competitive 4v4 lands at ±20-30 per
    // round. Bot-only matches are already clamped to ±5 in
    // FlushAtMatchEnd::HumansWithHumanOpposition logic, so this cap mainly
    // affects human-vs-human play.
    static const double kMaxRatingChange = 0.5;


    // ========== Match Outcome Scores ==========

    /// Score for winning team
    static const double kWinScore = 1.0;

    /// Score for losing team
    static const double kLossScore = 0.0;

    /// Score for draw (if supported)
    static const double kDrawScore = 0.5;


    // ========== Performance Score Calculation ==========
    // These weights compute: (Kills*250) + (Deaths*kDeathWeight) + (Damage*0.5) + (Score*0.1)
    // Adjust based on your game's balance

    /// Weight for kills in performance score
    static const double kKillWeight = 1;

    /// Weight for deaths in performance score (negative penalty)
    /// -100 means 1 death ≈ 2.5 kills penalty
    /// -50 would make deaths less punishing
    static const double kDeathWeight = -1;

    /// Weight for damage in performance score
    static const double kDamageWeight = 1.0/220.0;

    /// Weight for objective score in performance score
    static const double kObjectiveWeight = 0;


    // ========== Inactivity Decay Parameters ==========

    /// Minimum RD value (rating deviation floor)
    static const double kMinRD = 30.0;

    /// Maximum RD value (rating deviation ceiling, same as default)
    static const double kMaxRD = kDefaultRD;

    /// Days to consider as one "rating period" for inactivity decay
    /// After this many inactive days, RD increases once
    static const double kDaysPerRatingPeriod = 7.0;

    /// Minimum number of rounds in past D days to be considered "active"
    /// If player has fewer rounds than this, they're considered inactive
    static const int kMinRoundsForActivity = 3;


    // ========== Recent Performance Tracking Parameters ==========

    /// Target window for recent performance EMA (number of games)
    /// Recent performance behaves like a moving average over this many games
    static const double kPerfTargetWindow = 10.0;

    /// Rating points per 1σ of performance index
    /// Controls how much recent performance affects effective rating
    /// Higher = performance swings rating more; Lower = more conservative
    static const double kPerfToRating = 80.0;

    /// RD scale constant for blending long-term vs recent rating
    /// Controls sensitivity to RD when computing effective rating
    static const double kRDScaleConstant = 80.0;

    /// Maximum z-score clipping for performance index
    /// Prevents extreme outliers from dominating the calculation
    static const double kMaxPerfZScore = 3.0;
}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_TEAMGLICKO2CONFIG_H_
