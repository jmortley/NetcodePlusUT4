// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_PERFORMANCENORMALIZATION_H_
#define GLICKO2_INCLUDE_PERFORMANCENORMALIZATION_H_

#include <vector>
#include <cmath>
#include <algorithm>
#include "TeamGlicko2Config.h"

namespace TeamGlicko2 {
    /// Statistics for team performance
    struct TeamPerformanceStats {
        double mean;        // Mean performance score
        double stddev;      // Standard deviation
        int teamSize;       // Number of players

        TeamPerformanceStats() : mean(0.0), stddev(1.0), teamSize(0) {}
    };

    /// Individual player's normalized performance for one match
    struct PlayerPerformance {
        int playerIndex;           // Index in team array
        double rawScore;           // Raw performance score (kills, deaths, damage)
        double zScore;             // Normalized z-score relative to team
        double clippedZScore;      // z-score clipped to [-3, +3] range

        PlayerPerformance()
            : playerIndex(0), rawScore(0.0), zScore(0.0), clippedZScore(0.0) {}
    };

    /// Utilities for normalizing performance within a team
    /// Implements "how much above/below my team did I play?" calculation
    class PerformanceNormalization {
    public:
        /// Compute team performance statistics (mean, stddev)
        /// @param scores Raw performance scores for each player
        /// @return Team statistics
        static TeamPerformanceStats ComputeTeamStats(const std::vector<double>& scores);

        /// Normalize performance scores within a team
        /// Converts raw scores to z-scores (standard deviations from team mean)
        /// @param scores Raw performance scores for each player
        /// @param maxZScore Maximum z-score clipping (default from config)
        /// @return Vector of normalized performance data
        static std::vector<PlayerPerformance> NormalizeTeamPerformance(
            const std::vector<double>& scores,
            double maxZScore = TeamGlicko2::kMaxPerfZScore);

        /// Compute z-score for a single value given team stats
        /// @param score Raw performance score
        /// @param stats Team performance statistics
        /// @return z-score (standard deviations from mean)
        static double ComputeZScore(double score, const TeamPerformanceStats& stats);

        /// Clip z-score to prevent extreme outliers
        /// @param zScore Original z-score
        /// @param maxZScore Maximum absolute value (default from config)
        /// @return Clipped z-score in range [-maxZScore, +maxZScore]
        static double ClipZScore(double zScore, double maxZScore = TeamGlicko2::kMaxPerfZScore);
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_PERFORMANCENORMALIZATION_H_
