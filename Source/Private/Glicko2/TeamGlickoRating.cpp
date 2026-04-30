// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "TeamGlickoRating.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TeamGlicko2 {
    PlayerRating::PlayerRating(double rating, double ratingDeviation, double volatility) {
        // Convert from Glicko-1 scale to Glicko-2 scale
        mu = (rating - TeamGlicko2::kDefaultRating) / TeamGlicko2::kScale;
        phi = ratingDeviation / TeamGlicko2::kScale;
        sigma = volatility;

        // Initialize recent performance tracking
        perfIndexEMA = 0.0;
        perfGames = 0;
    }

    PlayerRating::PlayerRating(const PlayerRating& other)
        : mu(other.mu), phi(other.phi), sigma(other.sigma),
          perfIndexEMA(other.perfIndexEMA), perfGames(other.perfGames){
    }

    PlayerRating& PlayerRating::operator=(const PlayerRating& other) {
        if (this != &other) {
            mu = other.mu;
            phi = other.phi;
            sigma = other.sigma;
            perfIndexEMA = other.perfIndexEMA;
            perfGames = other.perfGames;
        }
        return *this;
    }

    void PlayerRating::SetRating(double rating) {
        mu = (rating - TeamGlicko2::kDefaultRating) / TeamGlicko2::kScale;
    }

    void PlayerRating::SetRD(double rd) {
        phi = rd / TeamGlicko2::kScale;
    }

    double PlayerRating::ComputeG() const {
        // g(phi) = 1 / sqrt(1 + 3*phi^2 / pi^2)
        double phiSquared = phi * phi;
        return 1.0 / std::sqrt(1.0 + 3.0 * phiSquared / (M_PI * M_PI));
    }

    double PlayerRating::ComputeExpectedScore(
            double muOpp,
            double gOpp)
        const {
        // E = 1 / (1 + exp(-g(phi_opp) * (mu - mu_opp)))
        double exponent = -gOpp * (mu - muOpp);
        return 1.0 / (1.0 + std::exp(exponent));
    }

    void PlayerRating::DecayForInactivity(
            int roundsInPastDays,
            double deltaDays,
            int minRoundsForActivity,
            double daysPerPeriod) {
        // If player has been active recently, no decay
        if (roundsInPastDays >= minRoundsForActivity) {
            return;
        }

        // Calculate number of inactive rating periods
        // Each rating period is daysPerPeriod days (default: 7 days)
        double inactivePeriods = deltaDays / daysPerPeriod;

        // If less than one period has passed, no decay
        if (inactivePeriods < 1.0) {
            return;
        }

        // Apply decay for each inactive period
        // Standard Glicko-2 decay: phi' = sqrt(phi^2 + sigma^2)
        // We apply this formula once per rating period
        for (int i = 0; i < static_cast<int>(inactivePeriods); ++i) {
            double phiSquared = phi * phi;
            double sigmaSquared = sigma * sigma;
            phi = std::sqrt(phiSquared + sigmaSquared);

            // Cap at maximum RD (in Glicko-2 scale)
            double maxPhi = TeamGlicko2::kMaxRD / TeamGlicko2::kScale;
            if (phi > maxPhi) {
                phi = maxPhi;
                break;  // Already at maximum, no need to continue
            }
        }
    }

    void PlayerRating::UpdateRecentPerformance(double matchPerfIndex, double targetWindow) {
        // Clip extreme outliers to prevent domination
        matchPerfIndex = std::max(-TeamGlicko2::kMaxPerfZScore,
                                  std::min(matchPerfIndex, TeamGlicko2::kMaxPerfZScore));

        double alpha;
        if (perfGames <= 0) {
            // First game: jump directly to current value
            alpha = 1.0;
        } else if (perfGames < static_cast<int>(targetWindow)) {
            // Bootstrap phase: simple moving average
            alpha = 1.0 / (perfGames + 1.0);
        } else {
            // Steady state: exponential moving average
            alpha = 2.0 / (targetWindow + 1.0);
        }

        perfIndexEMA = (1.0 - alpha) * perfIndexEMA + alpha * matchPerfIndex;
        perfGames++;
    }

    double PlayerRating::ComputeRecentRating(double perfToRating) const {
        // Convert performance index to rating boost
        double boost = perfIndexEMA * perfToRating;

        // Cap boost based on RD: uncertain players can be adjusted more
        // Use at most 2*RD or 200 points
        double rd = GetRD();
        double maxBoost = std::min(2.0 * rd, 200.0);
        boost = std::max(-maxBoost, std::min(boost, maxBoost));

        return GetRating() + boost;
    }

    double PlayerRating::ComputeEffectiveRating(double perfToRating, double rdScale) const {
        double r_g = GetRating();           // Long-term Glicko rating
        double r_rec = ComputeRecentRating(perfToRating);  // Recent performance rating
        double rd = GetRD();

        // RD-based weight: higher RD = more influence from recent performance
        // w_RD = RD^2 / (RD^2 + C^2)
        double rdSquared = rd * rd;
        double cSquared = rdScale * rdScale;
        double w_RD = rdSquared / (rdSquared + cSquared);

        // Halve the weight so performance never fully dominates
        double w = 0.5 * w_RD;

        // Blend: r_true = r_g + w * (r_rec - r_g)
        double r_true = r_g + w * (r_rec - r_g);

        return r_true;
    }

    // LOCAL MOD: operator<< definition removed (matches header). See header banner.

}  // namespace TeamGlicko2
