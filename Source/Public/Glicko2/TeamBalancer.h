// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMBALANCER_H_
#define GLICKO2_INCLUDE_TEAMBALANCER_H_

#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include "TeamGlickoRating.h"

namespace TeamGlicko2 {
    /// Represents a player with their rating information for balancing
    struct PlayerInfo {
        int playerId;              // Unique identifier for the player
        PlayerRating rating;       // Full rating state (R, RD, volatility, performance)
        double effectiveRating;    // R_eff = effective rating (accounts for uncertainty via RD-weighted blending)

        PlayerInfo() : playerId(0), effectiveRating(0.0) {}
        PlayerInfo(int id, const PlayerRating& r)
            : playerId(id), rating(r) {
            // Use effective rating (already accounts for uncertainty via RD-weighted blending)
            effectiveRating = rating.ComputeEffectiveRating();
        }
    };

    /// Represents a team assignment result
    struct TeamAssignment {
        std::vector<int> team0PlayerIds;  // Players assigned to team 0
        std::vector<int> team1PlayerIds;  // Players assigned to team 1
        double objectiveValue;            // J(A,B) = |avg(S_A) - avg(S_B)| + lambda * |avg(U_A) - avg(U_B)|
        double strengthDifference;        // |avg(S_A) - avg(S_B)| - average per player
        double uncertaintyDifference;     // |avg(U_A) - avg(U_B)| - normalized by sqrt(team_size)
        double team0Strength;             // sum(S_i) for team 0
        double team1Strength;             // sum(S_i) for team 1
        double team0Uncertainty;          // U_A = sqrt(sum(RD_i^2)) for team 0
        double team1Uncertainty;          // U_B = sqrt(sum(RD_i^2)) for team 1
        double pureRatingDifference;      // |avg(R_A) - avg(R_B)| - average per player for tie-breaking

        TeamAssignment()
            : objectiveValue(std::numeric_limits<double>::max())
            , strengthDifference(0.0)
            , uncertaintyDifference(0.0)
            , team0Strength(0.0)
            , team1Strength(0.0)
            , team0Uncertainty(0.0)
            , team1Uncertainty(0.0)
            , pureRatingDifference(0.0) {}
    };

    /// Configuration for team balancing algorithm
    struct BalancerConfig {
        /// Lambda: Team uncertainty balance weight
        /// Objective = |avg(S_A) - avg(S_B)| + lambda * |avg(U_A) - avg(U_B)|
        /// Uses averages for fair handling of uneven teams (4v3, 5v4)
        /// Range: [0.0, ∞)
        /// Default from kLambda
        double lambda;

        /// Whether to enforce top 2 players on different teams (hard constraint)
        bool separateTopPlayers;

        /// For uneven teams (e.g., 7 players -> 3v4), put the top player in the smaller team
        /// This helps compensate for the numerical disadvantage
        bool putTopPlayerInSmallerTeam;

        /// Maximum number of combinations to try for optimal balance
        /// Higher = more accurate but slower
        /// For N players with top-2 constraint: C(N-2, N/2-1) combinations
        /// 8 players = 20 combinations (fast)
        /// 10 players = 56 combinations (fast)
        /// 12 players = 126 combinations (acceptable)
        /// 14 players = 252 combinations (acceptable)
        int maxCombinationsToTry;

        BalancerConfig()
            : lambda(TeamGlicko2::kLambda)
            , separateTopPlayers(true)
            , putTopPlayerInSmallerTeam(true)
            , maxCombinationsToTry(10000) {}
    };

    /// Team balancing system for creating fair matches
    class TeamBalancer {
    public:
        /// Balance players into two teams with minimal rating difference
        /// Enforces constraints (e.g., top 2 players on different teams)
        /// @param players Vector of all players to balance
        /// @param config Balancing configuration parameters
        /// @return Optimal team assignment
        static TeamAssignment BalanceTeams(
            const std::vector<PlayerInfo>& players,
            const BalancerConfig& config = BalancerConfig());

        /// Calculate team strength (sum of effective ratings)
        /// Returns sum of R_eff for all players in the team
        static double CalculateTeamStrength(
            const std::vector<PlayerInfo>& players,
            const std::vector<int>& playerIndices);

        /// Calculate team uncertainty U = sqrt(sum(RD_i^2))
        static double CalculateTeamUncertainty(
            const std::vector<PlayerInfo>& players,
            const std::vector<int>& playerIndices);

        /// Calculate pure rating sum (for tie-breaking)
        static double CalculatePureRatingSum(
            const std::vector<PlayerInfo>& players,
            const std::vector<int>& playerIndices);

        /// Evaluate objective function J(A,B) for a team assignment
        /// J = |avg(R_eff_A) - avg(R_eff_B)| + lambda * |avg(U_A) - avg(U_B)|
        /// Uses averages for fair comparison in uneven teams (4v3, 5v4)
        /// For even teams, equivalent to sum-based comparison
        /// Returns the objective value and populates output parameters
        static double EvaluateAssignment(
            const std::vector<PlayerInfo>& players,
            const std::vector<int>& team0Indices,
            const std::vector<int>& team1Indices,
            double lambda,
            double& outStrength0,
            double& outStrength1,
            double& outUncertainty0,
            double& outUncertainty1,
            double& outPureRating0,
            double& outPureRating1);

        /// Check if an assignment violates the "top 2 separated" constraint
        static bool ViolatesTopPlayerConstraint(
            const std::vector<PlayerInfo>& sortedPlayers,
            const std::vector<int>& teamIndices);

    private:
        /// Recursive helper for trying all valid team combinations
        static void GenerateCombinations(
            const std::vector<PlayerInfo>& players,
            const BalancerConfig& config,
            int teamSize,
            int startIndex,
            std::vector<int>& currentTeam0,
            TeamAssignment& bestAssignment,
            int& combinationsTried);

        /// Create a team assignment from team 0 indices
        /// Team 1 is implicitly all remaining players
        static TeamAssignment CreateAssignment(
            const std::vector<PlayerInfo>& players,
            const std::vector<int>& team0Indices);
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_TEAMBALANCER_H_
