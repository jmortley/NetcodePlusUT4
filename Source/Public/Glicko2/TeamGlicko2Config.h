// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMGLICKO2CONFIG_H_
#define GLICKO2_INCLUDE_TEAMGLICKO2CONFIG_H_

namespace TeamGlicko2 {
    // ========== Glicko-2 Base Parameters ==========

    /// Default initial rating (R_0)
    static const double kDefaultRating = 1400.0;

    /// Default initial rating deviation (RD_0)
    static const double kDefaultRD = 350.0;

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
    static const double kBeta = 0.2;

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
    static const double kMaxRatingChange = 1.73;


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
