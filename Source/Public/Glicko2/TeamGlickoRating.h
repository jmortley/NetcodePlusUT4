// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author).
// LOCAL MOD: removed `#include <iostream>` and the `friend operator<<` decl
// at the bottom of PlayerRating. <iostream> transitively pulls Windows.h on
// MSVC which corrupts the UE4 4.15 unity-build PCH chain (Engine UT headers
// like UTWeaponSkin.h/UTLocalMessage.h fail downstream). The operator<< was
// only diagnostic; UE_LOG covers the same need on the UE4 side.
// ============================================================================
#ifndef GLICKO2_INCLUDE_TEAMGLICKORATING_H_
#define GLICKO2_INCLUDE_TEAMGLICKORATING_H_

#include <cmath>
#include "TeamGlicko2Config.h"

namespace TeamGlicko2 {
    /// Represents a player's Glicko-2 rating state
    /// Maintains both Glicko-1 scale (R, RD) and Glicko-2 scale (mu, phi)
    class PlayerRating {
    public:
        /// Default constructor with initial rating values
        PlayerRating(double rating = TeamGlicko2::kDefaultRating,
                    double ratingDeviation = TeamGlicko2::kDefaultRD,
                    double volatility = TeamGlicko2::kDefaultVolatility);

        /// Copy constructor
        PlayerRating(const PlayerRating& other);

        /// Assignment operator
        PlayerRating& operator=(const PlayerRating& other);

        // ========== Glicko-1 Scale Getters (R, RD) ==========

        /// Get rating in Glicko-1 scale
        double GetRating() const { return mu * TeamGlicko2::kScale + TeamGlicko2::kDefaultRating; }

        /// Get rating deviation in Glicko-1 scale
        double GetRD() const { return phi * TeamGlicko2::kScale; }

        /// Get volatility (same in both scales)
        double GetVolatility() const { return sigma; }

        // ========== Glicko-2 Scale Getters (mu, phi) ==========

        /// Get rating in Glicko-2 scale (mu)
        double GetMu() const { return mu; }

        /// Get rating deviation in Glicko-2 scale (phi)
        double GetPhi() const { return phi; }

        /// Get volatility in Glicko-2 scale (sigma)
        double GetSigma() const { return sigma; }

        // ========== Setters ==========

        /// Set rating from Glicko-1 scale
        void SetRating(double rating);

        /// Set rating deviation from Glicko-1 scale
        void SetRD(double rd);

        /// Set volatility
        void SetVolatility(double vol) { sigma = vol; }

        /// Set rating from Glicko-2 scale (mu)
        void SetMu(double muValue) { mu = muValue; }

        /// Set rating deviation from Glicko-2 scale (phi)
        void SetPhi(double phiValue) { phi = phiValue; }

        /// Set volatility from Glicko-2 scale (sigma)
        void SetSigma(double sigmaValue) { sigma = sigmaValue; }

        // ========== Recent Performance Tracking ==========

        /// Get recent performance index (EMA of z-scores)
        double GetPerfIndexEMA() const { return perfIndexEMA; }

        /// Get number of games contributing to performance index
        int GetPerfGames() const { return perfGames; }

        /// Set recent performance index
        void SetPerfIndexEMA(double value) { perfIndexEMA = value; }

        /// Set performance games count
        void SetPerfGames(int count) { perfGames = count; }

        /// Update recent performance index with new match performance
        /// @param matchPerfIndex z-score from latest match (relative to teammates)
        /// @param targetWindow number of games for EMA window (default from config)
        void UpdateRecentPerformance(double matchPerfIndex,
                                    double targetWindow = TeamGlicko2::kPerfTargetWindow);

        /// Compute recent rating: base rating + performance boost
        /// @param perfToRating rating points per 1σ of performance (default from config)
        /// @return Recent rating based on short-term form
        double ComputeRecentRating(double perfToRating = TeamGlicko2::kPerfToRating) const;

        /// Compute effective rating: blend of long-term Glicko and recent performance
        /// Uses RD as trust factor - high RD allows more performance influence
        /// @param perfToRating rating points per 1σ of performance (default from config)
        /// @param rdScale RD scale constant for blending (default from config)
        /// @return Effective rating for matchmaking/balancing
        double ComputeEffectiveRating(double perfToRating = TeamGlicko2::kPerfToRating,
                                      double rdScale = TeamGlicko2::kRDScaleConstant) const;

        // ========== Utility Functions ==========

        /// Computes g(phi) function used in Glicko-2 calculations
        /// g(phi) = 1 / sqrt(1 + 3*phi^2 / pi^2)
        double ComputeG() const;

        /// Computes expected score E against another player
        /// E = 1 / (1 + exp(-g(phi_opp) * (mu - mu_opp)))
        double ComputeExpectedScore(double muOpp, double gOpp) const;

        /// Apply inactivity decay to rating deviation
        /// RD increases when player is inactive, representing growing uncertainty
        /// @param roundsInPastDays Number of rounds played in past D days
        /// @param deltaDays Days since last match
        /// @param minRoundsForActivity Minimum rounds to be considered active (default from config)
        /// @param daysPerPeriod Days per rating period for decay calculation (default from config)
        void DecayForInactivity(int roundsInPastDays, double deltaDays,
                               int minRoundsForActivity = TeamGlicko2::kMinRoundsForActivity,
                               double daysPerPeriod = TeamGlicko2::kDaysPerRatingPeriod);

        // LOCAL MOD: friend std::ostream& operator<< removed (see header banner).

    private:
        /// Internal rating (Glicko-2 scale)
        double mu;

        /// Internal rating deviation (Glicko-2 scale)
        double phi;

        /// Rating volatility
        double sigma;

        /// Recent performance index (EMA of z-scores relative to teammates)
        double perfIndexEMA;

        /// Number of games contributing to performance index
        int perfGames;
    };

}  // namespace TeamGlicko2

#endif  // GLICKO2_INCLUDE_TEAMGLICKORATING_H_
