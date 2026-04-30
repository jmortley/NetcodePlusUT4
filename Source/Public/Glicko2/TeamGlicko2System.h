// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMGLICKO2SYSTEM_H_
#define GLICKO2_INCLUDE_TEAMGLICKO2SYSTEM_H_

#include <vector>
#include <cmath>
#include "TeamGlickoRating.h"
#include "TeamRatingAggregator.h"
#include "PerformanceWeighting.h"
#include "TeamGlicko2Config.h"

namespace TeamGlicko2 {
    /// Represents a player in a match with their performance data
    struct MatchPlayer {
        PlayerRating rating;         // Current rating state
        double performanceScore;     // Performance score for this match

        MatchPlayer() : performanceScore(0.0) {}
        MatchPlayer(const PlayerRating& r, double perf)
            : rating(r), performanceScore(perf) {}
    };

    /// Represents the outcome of a match between two teams
    struct MatchResult {
        std::vector<MatchPlayer> teamA;  // Team A players and performance
        std::vector<MatchPlayer> teamB;  // Team B players and performance
        double scoreA;                    // Match score for team A (1.0 = win, 0.0 = loss, 0.5 = draw)
        double scoreB;                    // Match score for team B

        MatchResult() : scoreA(0.0), scoreB(0.0) {}
    };

    /// Main system for processing team-based Glicko-2 rating updates
    /// Implements the full algorithm from Section 8 of the specification
    class TeamGlicko2System {
    public:
        /// Process a match and update all player ratings
        /// @param match Match result with player ratings and performance scores
        /// This function modifies the rating member of each MatchPlayer in place
        static void ProcessMatch(MatchResult& match);

        /// Update a single player's rating based on team outcome
        /// This implements the single-opponent Glicko-2 update with sign-aware performance scaling
        /// @param player Current player rating
        /// @param opponentMu Opposing team's aggregated mu
        /// @param opponentPhi Opposing team's aggregated phi
        /// @param score Match outcome (1.0 = win, 0.0 = loss, 0.5 = draw)
        /// @param zScore Performance z-score relative to teammates
        /// @return Updated player rating
        static PlayerRating UpdatePlayerRating(
            const PlayerRating& player,
            double opponentMu,
            double opponentPhi,
            double score,
            double zScore);

    private:
        /// Compute the v (variance) term for Glicko-2 update
        /// v = [g(phi_opp)^2 * E * (1 - E)]^(-1)
        static double ComputeVariance(double g, double expectedScore);

        /// Compute the Delta term for Glicko-2 update
        /// Delta = v * g(phi_opp) * (s - E)
        static double ComputeDelta(double v, double g, double score, double expectedScore);

        /// Update volatility (sigma) using Illinois algorithm
        /// This solves the volatility convergence equation from Glicko-2
        /// Returns new sigma'
        static double UpdateVolatility(
            double sigma,
            double phi,
            double delta,
            double v);

        /// Helper function f(x) for volatility update
        static double VolatilityFunction(
            double x,
            double deltaSquared,
            double phiSquared,
            double v,
            double a,
            double tauSquared);

        /// Update rating deviation (phi)
        /// First compute intermediate phi* = sqrt(phi^2 + sigma'^2)
        /// Then compute phi' = [1/phi*^2 + 1/v]^(-1/2)
        static double UpdateRatingDeviation(double phi, double sigmaPrime, double v);

        /// Update rating mean (mu) without performance weighting
        /// mu* = mu + phi'^2 * g(phi_opp) * (s - E)
        static double UpdateRatingMean(
            double mu,
            double phiPrime,
            double g,
            double score,
            double expectedScore);

        /// Optionally clamp the final rating change
        /// Limits |mu' - mu| to kMaxRatingChange
        static double ClampRatingChange(double mu, double muPrime);
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_TEAMGLICKO2SYSTEM_H_
