// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "PerformanceWeighting.h"
#include <cmath>
#include <algorithm>

namespace TeamGlicko2 {
    std::vector<PlayerWeight> PerformanceWeighting::ComputeZScores(
        const std::vector<double>& performanceScores) {
        std::vector<PlayerWeight> weights;
        int teamSize = performanceScores.size();

        if (teamSize == 0) {
            return weights;
        }

        // Initialize weights array
        weights.resize(teamSize);
        for (int i = 0; i < teamSize; ++i) {
            weights[i].playerIndex = i;
            weights[i].performanceScore = performanceScores[i];
        }

        // Compute team statistics
        double mean = ComputeMean(performanceScores);
        double stddev = ComputeStdDev(performanceScores, mean);

        // Compute z-scores for each player
        for (int i = 0; i < teamSize; ++i) {
            // z_i = (p_i - mean) / stddev
            weights[i].zScore = ComputeZScore(performanceScores[i], mean, stddev);
        }

        return weights;
    }

    double PerformanceWeighting::ComputeScalingFactor(
        double zScore,
        double deltaMu,
        double beta,
        double fMin,
        double fMax) {
        // Sign-aware formula: f_i = 1 + β·sign(Δμ)·z_i
        // This ensures:
        // - In wins (Δμ > 0): good performers (z > 0) get f > 1 (more gain)
        // - In losses (Δμ < 0): good performers (z > 0) get f < 1 (less loss)
        double signDeltaMu = (deltaMu >= 0.0) ? 1.0 : -1.0;
        double f = 1.0 + beta * signDeltaMu * zScore;

        // Clamp to valid range
        return std::min(fMax, std::max(fMin, f));
    }

    double PerformanceWeighting::ComputeMean(const std::vector<double>& scores) {
        if (scores.empty()) {
            return 0.0;
        }

        double sum = 0.0;
        for (double score : scores) {
            sum += score;
        }

        return sum / scores.size();
    }

    double PerformanceWeighting::ComputeStdDev(const std::vector<double>& scores, double mean) {
        if (scores.empty()) {
            return TeamGlicko2::kEpsilon;
        }

        // s_T = sqrt((1/|T|) * sum((p_i - mean)^2)) + epsilon
        double sumSquares = 0.0;
        for (double score : scores) {
            double diff = score - mean;
            sumSquares += diff * diff;
        }

        double variance = sumSquares / scores.size();
        return std::sqrt(variance) + TeamGlicko2::kEpsilon;
    }

    double PerformanceWeighting::ComputeZScore(double score, double mean, double stddev) {
        // z_i = (p_i - mean) / stddev
        return (score - mean) / stddev;
    }

}  // namespace TeamGlicko2
