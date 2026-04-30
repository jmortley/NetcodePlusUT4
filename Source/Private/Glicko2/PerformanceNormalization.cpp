// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "PerformanceNormalization.h"
#include <numeric>

namespace TeamGlicko2 {
    TeamPerformanceStats PerformanceNormalization::ComputeTeamStats(const std::vector<double>& scores) {
        TeamPerformanceStats stats;
        stats.teamSize = static_cast<int>(scores.size());

        if (stats.teamSize == 0) {
            return stats;
        }

        // Compute mean
        stats.mean = std::accumulate(scores.begin(), scores.end(), 0.0) / stats.teamSize;

        // Compute variance
        double variance = 0.0;
        for (double score : scores) {
            double diff = score - stats.mean;
            variance += diff * diff;
        }
        variance /= stats.teamSize;

        // Compute standard deviation with minimum threshold to avoid division by zero
        stats.stddev = (variance < 1e-6) ? 1.0 : std::sqrt(variance);

        return stats;
    }

    std::vector<PlayerPerformance> PerformanceNormalization::NormalizeTeamPerformance(
        const std::vector<double>& scores,
        double maxZScore) {
        std::vector<PlayerPerformance> results;
        results.reserve(scores.size());

        // Compute team statistics
        TeamPerformanceStats stats = ComputeTeamStats(scores);

        // Normalize each player's score
        for (size_t i = 0; i < scores.size(); ++i) {
            PlayerPerformance perf;
            perf.playerIndex = static_cast<int>(i);
            perf.rawScore = scores[i];
            perf.zScore = ComputeZScore(scores[i], stats);
            perf.clippedZScore = ClipZScore(perf.zScore, maxZScore);

            results.push_back(perf);
        }

        return results;
    }

    double PerformanceNormalization::ComputeZScore(double score, const TeamPerformanceStats& stats) {
        // z = (score - mean) / stddev
        return (score - stats.mean) / stats.stddev;
    }

    double PerformanceNormalization::ClipZScore(double zScore, double maxZScore) {
        // Clip to [-maxZScore, +maxZScore] range
        return std::max(-maxZScore, std::min(zScore, maxZScore));
    }

}  // namespace TeamGlicko2
