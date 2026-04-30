// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_PERFORMANCEWEIGHTING_H_
#define GLICKO2_INCLUDE_PERFORMANCEWEIGHTING_H_

#include <vector>
#include <cmath>
#include "TeamGlicko2Config.h"

namespace TeamGlicko2 {
    /// Represents performance-based scaling for a single player
    struct PlayerWeight {
        int playerIndex;        // Index in team array
        double performanceScore;  // Raw performance score p_i
        double zScore;          // Standardized z-score relative to teammates

        PlayerWeight()
            : playerIndex(0), performanceScore(0.0), zScore(0.0) {}
    };

    /// Computes performance-based scaling factors for team members
    /// Uses sign-aware formula: f_i = 1 + β·sign(Δμ)·z_i
    class PerformanceWeighting {
    public:
        /// Compute z-scores for all players in a team
        /// Z-scores measure performance relative to teammates
        /// @param performanceScores Raw performance scores for each player
        /// @return Vector of PlayerWeight structs with z-scores
        static std::vector<PlayerWeight> ComputeZScores(
            const std::vector<double>& performanceScores);

        /// Compute sign-aware scaling factor for rating adjustment
        /// f_i = 1 + β·sign(Δμ)·z_i, clamped to [fMin, fMax]
        /// @param zScore Performance z-score relative to teammates
        /// @param deltaMu Rating change from Glicko-2 (μ* - μ)
        /// @param beta Sensitivity parameter (default from config)
        /// @param fMin Minimum scaling factor (default from config)
        /// @param fMax Maximum scaling factor (default from config)
        /// @return Scaling factor to apply to rating change
        static double ComputeScalingFactor(
            double zScore,
            double deltaMu,
            double beta = TeamGlicko2::kBeta,
            double fMin = TeamGlicko2::kScaleMin,
            double fMax = TeamGlicko2::kScaleMax);

        /// Compute mean performance score for a team
        static double ComputeMean(const std::vector<double>& scores);

        /// Compute standard deviation of performance scores
        /// Includes epsilon to avoid division by zero
        static double ComputeStdDev(const std::vector<double>& scores, double mean);

        /// Compute z-score for a performance value
        /// z_i = (p_i - mean) / stddev
        static double ComputeZScore(double score, double mean, double stddev);
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_PERFORMANCEWEIGHTING_H_
