// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMRATINGAGGREGATOR_H_
#define GLICKO2_INCLUDE_TEAMRATINGAGGREGATOR_H_

#include <vector>
#include <cmath>
#include "TeamGlickoRating.h"

namespace TeamGlicko2 {
    /// Represents aggregated rating statistics for a team
    struct TeamRatingStats {
        double mu;      // Mean rating (Glicko-2 scale)
        double phi;     // Team rating deviation (Glicko-2 scale)
        int teamSize;   // Number of players in team

        TeamRatingStats() : mu(0.0), phi(0.0), teamSize(0) {}
    };

    /// Computes team-level aggregated rating statistics
    /// Used to represent a team as a single "opponent" in Glicko-2 calculations
    class TeamRatingAggregator {
    public:
        /// Compute aggregated rating statistics for a team
        /// @param team Vector of player ratings
        /// @return TeamRatingStats containing mu_T and phi_T
        static TeamRatingStats ComputeTeamStats(const std::vector<PlayerRating>& team);

        /// Compute team mean rating (mu_T)
        /// mu_T = (1/|T|) * sum(mu_i for i in T)
        static double ComputeTeamMu(const std::vector<PlayerRating>& team);

        /// Compute team rating deviation (phi_T)
        /// phi_T = sqrt((1/|T|^2) * sum(phi_i^2 for i in T))
        static double ComputeTeamPhi(const std::vector<PlayerRating>& team);
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_TEAMRATINGAGGREGATOR_H_
