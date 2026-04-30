// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "TeamRatingAggregator.h"
#include <cmath>

namespace TeamGlicko2 {
    TeamRatingStats TeamRatingAggregator::ComputeTeamStats(const std::vector<PlayerRating>& team) {
        TeamRatingStats stats;
        stats.teamSize = team.size();

        if (stats.teamSize == 0) {
            return stats;
        }

        stats.mu = ComputeTeamMu(team);
        stats.phi = ComputeTeamPhi(team);

        return stats;
    }

    double TeamRatingAggregator::ComputeTeamMu(const std::vector<PlayerRating>& team) {
        if (team.empty()) {
            return 0.0;
        }

        // mu_T = (1/|T|) * sum(mu_i for i in T)
        double sum = 0.0;
        for (const auto& player : team) {
            sum += player.GetMu();
        }

        return sum / team.size();
    }

    double TeamRatingAggregator::ComputeTeamPhi(const std::vector<PlayerRating>& team) {
        if (team.empty()) {
            return 0.0;
        }

        // phi_T = sqrt((1/|T|^2) * sum(phi_i^2 for i in T))
        double sumSquares = 0.0;
        for (const auto& player : team) {
            double phi = player.GetPhi();
            sumSquares += phi * phi;
        }

        int teamSize = team.size();
        return std::sqrt(sumSquares / (teamSize * teamSize));
    }

}  // namespace TeamGlicko2
