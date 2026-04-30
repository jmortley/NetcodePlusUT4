// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "TeamBalancer.h"
#include <set>
#include <iostream>

namespace TeamGlicko2 {
    TeamAssignment TeamBalancer::BalanceTeams(
        const std::vector<PlayerInfo>& players,
        const BalancerConfig& config) {
        if (players.size() < 2) {
            // Not enough players to balance
            TeamAssignment empty;
            empty.objectiveValue = 0.0;
            return empty;
        }

        // Calculate team sizes (handles both even and odd player counts)
        // For odd numbers: one team gets the extra player (e.g., 7 players -> 3v4)
        int teamSize = static_cast<int>(players.size()) / 2;
        bool isUnevenTeams = (players.size() % 2 != 0);

        // Calculate effective ratings for all players
        std::vector<PlayerInfo> sortedPlayers = players;
        for (auto& player : sortedPlayers) {
            player.effectiveRating = player.rating.ComputeEffectiveRating();
        }

        // Sort by effective rating (descending) to identify top-2 players
        std::sort(sortedPlayers.begin(), sortedPlayers.end(),
            [&config](const PlayerInfo& a, const PlayerInfo& b) {
                double ratingA = a.rating.ComputeEffectiveRating();
                double ratingB = b.rating.ComputeEffectiveRating();
                return ratingA > ratingB;
            });

        // Initialize best assignment
        TeamAssignment bestAssignment;
        bestAssignment.objectiveValue = std::numeric_limits<double>::max();

        // Try all valid team combinations
        std::vector<int> currentTeam0;
        currentTeam0.reserve(teamSize);
        int combinationsTried = 0;

        // For uneven teams with putTopPlayerInSmallerTeam enabled:
        // Ensure player 0 (top player) starts in the smaller team (team 0)
        if (isUnevenTeams && config.putTopPlayerInSmallerTeam) {
            currentTeam0.push_back(0);  // Force top player in smaller team
        }

        int startIndex = (isUnevenTeams && config.putTopPlayerInSmallerTeam) ? 1 : 0;

        GenerateCombinations(
            sortedPlayers,
            config,
            teamSize,
            startIndex,
            currentTeam0,
            bestAssignment,
            combinationsTried);

        std::cout << "Team balancing complete. Tried " << combinationsTried
                  << " combinations. Best objective value: "
                  << bestAssignment.objectiveValue
                  << " (strength diff: " << bestAssignment.strengthDifference
                  << ", uncertainty diff: " << bestAssignment.uncertaintyDifference << ")" << std::endl;

        return bestAssignment;
    }

    double TeamBalancer::CalculateTeamStrength(
        const std::vector<PlayerInfo>& players,
        const std::vector<int>& playerIndices) {
        // Sum of effective ratings for team strength
        double sum = 0.0;
        for (int idx : playerIndices) {
            sum += players[idx].effectiveRating;
        }
        return sum;
    }

    double TeamBalancer::CalculateTeamUncertainty(
        const std::vector<PlayerInfo>& players,
        const std::vector<int>& playerIndices) {
        // U = sqrt(sum(RD_i^2)) - root-sum-of-squares
        double sumSquares = 0.0;
        for (int idx : playerIndices) {
            double rd = players[idx].rating.GetRD();
            sumSquares += rd * rd;
        }
        return std::sqrt(sumSquares);
    }

    double TeamBalancer::CalculatePureRatingSum(
        const std::vector<PlayerInfo>& players,
        const std::vector<int>& playerIndices) {
        // Pure rating sum (no RD adjustment) for tie-breaking
        double sum = 0.0;
        for (int idx : playerIndices) {
            sum += players[idx].rating.GetRating();
        }
        return sum;
    }

    double TeamBalancer::EvaluateAssignment(
        const std::vector<PlayerInfo>& players,
        const std::vector<int>& team0Indices,
        const std::vector<int>& team1Indices,
        double lambda,
        double& outStrength0,
        double& outStrength1,
        double& outUncertainty0,
        double& outUncertainty1,
        double& outPureRating0,
        double& outPureRating1) {
        // Calculate team strengths (sum of effective ratings)
        double totalStrength0 = CalculateTeamStrength(players, team0Indices);
        double totalStrength1 = CalculateTeamStrength(players, team1Indices);

        // Calculate team uncertainties (root-sum-of-squares)
        double totalUncertainty0 = CalculateTeamUncertainty(players, team0Indices);
        double totalUncertainty1 = CalculateTeamUncertainty(players, team1Indices);

        // Calculate pure rating sums (for tie-breaking)
        outPureRating0 = CalculatePureRatingSum(players, team0Indices);
        outPureRating1 = CalculatePureRatingSum(players, team1Indices);

        // Store total values for output
        outStrength0 = totalStrength0;
        outStrength1 = totalStrength1;
        outUncertainty0 = totalUncertainty0;
        outUncertainty1 = totalUncertainty1;

        // Calculate averages for objective function (handles uneven teams fairly)
        int size0 = static_cast<int>(team0Indices.size());
        int size1 = static_cast<int>(team1Indices.size());

        double avgStrength0 = (size0 > 0) ? totalStrength0 / size0 : 0.0;
        double avgStrength1 = (size1 > 0) ? totalStrength1 / size1 : 0.0;
        double avgUncertainty0 = (size0 > 0) ? totalUncertainty0 / std::sqrt(size0) : 0.0;
        double avgUncertainty1 = (size1 > 0) ? totalUncertainty1 / std::sqrt(size1) : 0.0;

        // Objective function: J = |avg(R_eff_A) - avg(R_eff_B)| + lambda * |avg(U_A) - avg(U_B)|
        // For even teams, this is equivalent to sum-based comparison
        // For odd teams (4v3), this provides fair comparison
        double strengthDiff = std::abs(avgStrength0 - avgStrength1);
        double uncertaintyDiff = std::abs(avgUncertainty0 - avgUncertainty1);

        return strengthDiff + lambda * uncertaintyDiff;
    }

    bool TeamBalancer::ViolatesTopPlayerConstraint(
        const std::vector<PlayerInfo>& sortedPlayers,
        const std::vector<int>& teamIndices) {
        // Check if both index 0 and index 1 (top 2 players) are in this team
        if (sortedPlayers.size() < 2)
            return false;

        bool hasTop1 = false;
        bool hasTop2 = false;

        for (int idx : teamIndices) {
            if (idx == 0) hasTop1 = true;
            if (idx == 1) hasTop2 = true;
        }

        // Violation if both top players are on same team
        return (hasTop1 && hasTop2);
    }

    void TeamBalancer::GenerateCombinations(
        const std::vector<PlayerInfo>& players,
        const BalancerConfig& config,
        int teamSize,
        int startIndex,
        std::vector<int>& currentTeam0,
        TeamAssignment& bestAssignment,
        int& combinationsTried) {
        // Check if we've hit the combination limit
        if (combinationsTried >= config.maxCombinationsToTry)
            return;

        // Base case: we've selected enough players for team 0
        if (static_cast<int>(currentTeam0.size()) == teamSize) {
            combinationsTried++;

            // Check top player constraint
            if (config.separateTopPlayers && ViolatesTopPlayerConstraint(players, currentTeam0)) {
                return;  // Skip this combination
            }

            // Create team 1 from remaining players
            std::set<int> team0Set(currentTeam0.begin(), currentTeam0.end());
            std::vector<int> team1Indices;
            for (int i = 0; i < static_cast<int>(players.size()); i++) {
                if (team0Set.find(i) == team0Set.end()) {
                    team1Indices.push_back(i);
                }
            }

            // Check top player constraint for team 1
            if (config.separateTopPlayers && ViolatesTopPlayerConstraint(players, team1Indices)) {
                return;  // Skip this combination
            }

            // Evaluate this assignment using the objective function
            double strength0, strength1, uncertainty0, uncertainty1, pureRating0, pureRating1;
            double objectiveValue = EvaluateAssignment(
                players, currentTeam0, team1Indices, config.lambda,
                strength0, strength1, uncertainty0, uncertainty1,
                pureRating0, pureRating1);

            // Update best if this is better (with tie-breaking)
            bool isBetter = false;
            if (objectiveValue < bestAssignment.objectiveValue) {
                isBetter = true;
            } else if (objectiveValue == bestAssignment.objectiveValue) {
                // Tie-breaker 1: Prefer smaller pure rating gap
                double pureRatingDiff = std::abs(pureRating0 - pureRating1);
                if (pureRatingDiff < bestAssignment.pureRatingDifference) {
                    isBetter = true;
                } else if (pureRatingDiff == bestAssignment.pureRatingDifference) {
                    // Tie-breaker 2: Prefer smaller uncertainty difference
                    double uncertaintyDiff = std::abs(uncertainty0 - uncertainty1);
                    if (uncertaintyDiff < bestAssignment.uncertaintyDifference) {
                        isBetter = true;
                    }
                }
            }

            if (isBetter) {
                TeamAssignment newAssignment = CreateAssignment(players, currentTeam0);
                bestAssignment = newAssignment;
            }

            return;
        }

        // Recursive case: try adding each remaining player to team 0
        int needed = teamSize - static_cast<int>(currentTeam0.size());
        int remaining = static_cast<int>(players.size()) - startIndex;

        // Prune: not enough players left to fill team
        if (remaining < needed)
            return;

        for (int i = startIndex; i < static_cast<int>(players.size()); i++) {
            // Early pruning: if top 2 constraint is enabled and we already have
            // player 0, skip player 1 (and vice versa)
            if (config.separateTopPlayers && currentTeam0.size() > 0) {
                bool hasPlayer0 = false;
                bool hasPlayer1 = false;
                for (int idx : currentTeam0) {
                    if (idx == 0) hasPlayer0 = true;
                    if (idx == 1) hasPlayer1 = true;
                }

                if ((hasPlayer0 && i == 1) || (hasPlayer1 && i == 0)) {
                    continue;  // Skip this player
                }
            }

            currentTeam0.push_back(i);
            GenerateCombinations(players, config, teamSize, i + 1, currentTeam0, bestAssignment, combinationsTried);
            currentTeam0.pop_back();

            // Check limit after each recursion
            if (combinationsTried >= config.maxCombinationsToTry)
                return;
        }
    }

    TeamAssignment TeamBalancer::CreateAssignment(
        const std::vector<PlayerInfo>& players,
        const std::vector<int>& team0Indices) {
        TeamAssignment assignment;

        // Build team 0
        std::set<int> team0Set(team0Indices.begin(), team0Indices.end());
        for (int idx : team0Indices) {
            assignment.team0PlayerIds.push_back(players[idx].playerId);
        }

        // Build team 1 (all remaining players)
        std::vector<int> team1Indices;
        for (int i = 0; i < static_cast<int>(players.size()); i++) {
            if (team0Set.find(i) == team0Set.end()) {
                assignment.team1PlayerIds.push_back(players[i].playerId);
                team1Indices.push_back(i);
            }
        }

        // Evaluate assignment
        BalancerConfig defaultConfig;
        double pureRating0, pureRating1;
        assignment.objectiveValue = EvaluateAssignment(
            players, team0Indices, team1Indices, defaultConfig.lambda,
            assignment.team0Strength, assignment.team1Strength,
            assignment.team0Uncertainty, assignment.team1Uncertainty,
            pureRating0, pureRating1);

        // Calculate average-based differences for fair comparison (handles uneven teams)
        int size0 = static_cast<int>(team0Indices.size());
        int size1 = static_cast<int>(team1Indices.size());

        double avgStrength0 = (size0 > 0) ? assignment.team0Strength / size0 : 0.0;
        double avgStrength1 = (size1 > 0) ? assignment.team1Strength / size1 : 0.0;
        double avgUncertainty0 = (size0 > 0) ? assignment.team0Uncertainty / std::sqrt(size0) : 0.0;
        double avgUncertainty1 = (size1 > 0) ? assignment.team1Uncertainty / std::sqrt(size1) : 0.0;
        double avgRating0 = (size0 > 0) ? pureRating0 / size0 : 0.0;
        double avgRating1 = (size1 > 0) ? pureRating1 / size1 : 0.0;

        assignment.strengthDifference = std::abs(avgStrength0 - avgStrength1);
        assignment.uncertaintyDifference = std::abs(avgUncertainty0 - avgUncertainty1);
        assignment.pureRatingDifference = std::abs(avgRating0 - avgRating1);

        return assignment;
    }

}  // namespace TeamGlicko2
