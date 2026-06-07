// ============================================================================
// Vendored from https://github.com/tronunator/Glicko2 @ a2db253b
// © Tron (tronunator). No upstream LICENSE file at vendor time; included here
// with author authorization (relayed via NetcodePlus author). Local
// modifications: NONE — vendored verbatim. Cross-file includes resolved via
// Plugins/NetcodePlus/Source/Public/Glicko2 added to NetcodePlus.Build.cs
// PublicIncludePaths. Update: re-pull from upstream and re-vendor.
// ============================================================================
#include "TeamGlicko2System.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TeamGlicko2 {

    // LOCAL MOD (carry-rework): map a lobby-wide perf z-score to a (0,1) impact
    // via a logistic, clamped so a large kPerfSlope can't overflow exp().
    static double LobbyImpact(double zScore) {
        double x = kPerfSlope * zScore;
        if (x > 60.0) x = 60.0;
        else if (x < -60.0) x = -60.0;
        return 1.0 / (1.0 + std::exp(-x));
    }

    void TeamGlicko2System::ProcessMatch(MatchResult& match, bool bLobbyImpactBlend) {
        // Step 1: Extract player ratings for each team
        std::vector<PlayerRating> teamARatings;
        std::vector<PlayerRating> teamBRatings;
        std::vector<double> teamAPerformance;
        std::vector<double> teamBPerformance;

        for (const auto& player : match.teamA) {
            teamARatings.push_back(player.rating);
            teamAPerformance.push_back(player.performanceScore);
        }

        for (const auto& player : match.teamB) {
            teamBRatings.push_back(player.rating);
            teamBPerformance.push_back(player.performanceScore);
        }

        // Step 2: Compute team aggregated ratings (each team = one opponent)
        TeamRatingStats statsA = TeamRatingAggregator::ComputeTeamStats(teamARatings);
        TeamRatingStats statsB = TeamRatingAggregator::ComputeTeamStats(teamBRatings);

        // LOCAL MOD (ElimPlus/Wipeout carry-rework): carry-aware lobby-impact
        // blend (see TeamGlicko2Config.h). z-score perf across the WHOLE LOBBY so
        // a stud who tops the lobby scores high regardless of teammates, then
        // blend that impact into the Glicko SCORE:
        //   eff = kCarryWeight*impact + (1-kCarryWeight)*outcome
        // Passing zScore=0 to UpdatePlayerRating makes its within-team scaler a
        // no-op (factor 1.0), so the rating moves purely on the blended score.
        if (bLobbyImpactBlend) {
            std::vector<double> lobbyPerf;
            lobbyPerf.reserve(teamAPerformance.size() + teamBPerformance.size());
            for (double p : teamAPerformance) lobbyPerf.push_back(p);
            for (double p : teamBPerformance) lobbyPerf.push_back(p);
            std::vector<PlayerWeight> lobbyW = PerformanceWeighting::ComputeZScores(lobbyPerf);

            const size_t nA = match.teamA.size();
            for (size_t i = 0; i < match.teamA.size(); ++i) {
                double eff = kCarryWeight * LobbyImpact(lobbyW[i].zScore)
                           + (1.0 - kCarryWeight) * match.scoreA;
                match.teamA[i].rating = UpdatePlayerRating(
                    match.teamA[i].rating, statsB.mu, statsB.phi, eff, 0.0);
            }
            for (size_t i = 0; i < match.teamB.size(); ++i) {
                double eff = kCarryWeight * LobbyImpact(lobbyW[nA + i].zScore)
                           + (1.0 - kCarryWeight) * match.scoreB;
                match.teamB[i].rating = UpdatePlayerRating(
                    match.teamB[i].rating, statsA.mu, statsA.phi, eff, 0.0);
            }
            return;
        }

        // ---- Original within-team multiplicative path (1v1 Duel/ShaftArena, CTF) ----
        // Step 3: Compute performance z-scores for each team
        std::vector<PlayerWeight> weightsA = PerformanceWeighting::ComputeZScores(teamAPerformance);
        std::vector<PlayerWeight> weightsB = PerformanceWeighting::ComputeZScores(teamBPerformance);

        // Step 4: Update ratings for Team A players
        for (size_t i = 0; i < match.teamA.size(); ++i) {
            match.teamA[i].rating = UpdatePlayerRating(
                match.teamA[i].rating,
                statsB.mu,
                statsB.phi,
                match.scoreA,
                weightsA[i].zScore);
        }

        // Step 5: Update ratings for Team B players
        for (size_t i = 0; i < match.teamB.size(); ++i) {
            match.teamB[i].rating = UpdatePlayerRating(
                match.teamB[i].rating,
                statsA.mu,
                statsA.phi,
                match.scoreB,
                weightsB[i].zScore);
        }
    }

    PlayerRating TeamGlicko2System::UpdatePlayerRating(
        const PlayerRating& player,
        double opponentMu,
        double opponentPhi,
        double score,
        double zScore) {
        // Get current rating parameters
        double mu = player.GetMu();
        double phi = player.GetPhi();
        double sigma = player.GetSigma();

        // Compute g(phi_opp)
        double phiOppSquared = opponentPhi * opponentPhi;
        double g = 1.0 / std::sqrt(1.0 + 3.0 * phiOppSquared / (M_PI * M_PI));

        // Compute expected score E
        double expectedScore = player.ComputeExpectedScore(opponentMu, g);

        // Compute variance v
        double v = ComputeVariance(g, expectedScore);

        // Compute delta
        double delta = ComputeDelta(v, g, score, expectedScore);

        // Update volatility
        double sigmaPrime = UpdateVolatility(sigma, phi, delta, v);

        // Update rating deviation
        double phiPrime = UpdateRatingDeviation(phi, sigmaPrime, v);

        // Update rating mean (standard Glicko-2)
        double muStar = UpdateRatingMean(mu, phiPrime, g, score, expectedScore);

        // Compute rating change
        double deltaMu = muStar - mu;

        // Apply sign-aware performance scaling
        double scalingFactor = PerformanceWeighting::ComputeScalingFactor(zScore, deltaMu);
        double muPrime = mu + scalingFactor * deltaMu;

        // Optional: clamp rating change
        if (TeamGlicko2::kEnableRatingClamp) {
            muPrime = ClampRatingChange(mu, muPrime);
        }

        // Create and return updated rating
        PlayerRating updatedRating;
        updatedRating.SetMu(muPrime);
        updatedRating.SetPhi(phiPrime);
        updatedRating.SetSigma(sigmaPrime);

        return updatedRating;
    }

    double TeamGlicko2System::ComputeVariance(double g, double expectedScore) {
        // v = [g^2 * E * (1 - E)]^(-1)
        double denominator = g * g * expectedScore * (1.0 - expectedScore);
        return 1.0 / denominator;
    }

    double TeamGlicko2System::ComputeDelta(double v, double g, double score, double expectedScore) {
        // Delta = v * g * (s - E)
        return v * g * (score - expectedScore);
    }

    double TeamGlicko2System::UpdateVolatility(
        double sigma,
        double phi,
        double delta,
        double v) {
        // Implementation of the Illinois algorithm for volatility convergence
        // Based on Step 5 of the Glicko-2 paper

        double deltaSquared = delta * delta;
        double phiSquared = phi * phi;
        double tauSquared = TeamGlicko2::kTau * TeamGlicko2::kTau;
        double a = std::log(sigma * sigma);

        // Determine initial values for A and B
        double A = a;
        double B;

        if (deltaSquared > phiSquared + v) {
            B = std::log(deltaSquared - phiSquared - v);
        } else {
            // Find B by iterating downward
            B = a - TeamGlicko2::kTau;
            while (VolatilityFunction(B, deltaSquared, phiSquared, v, a, tauSquared) < 0.0) {
                B -= TeamGlicko2::kTau;
            }
        }

        // Illinois algorithm iteration
        double fA = VolatilityFunction(A, deltaSquared, phiSquared, v, a, tauSquared);
        double fB = VolatilityFunction(B, deltaSquared, phiSquared, v, a, tauSquared);

        while (std::abs(B - A) > TeamGlicko2::kConvergence) {
            double C = A + (A - B) * fA / (fB - fA);
            double fC = VolatilityFunction(C, deltaSquared, phiSquared, v, a, tauSquared);

            if (fC * fB < 0.0) {
                A = B;
                fA = fB;
            } else {
                fA /= 2.0;
            }

            B = C;
            fB = fC;
        }

        // Return new volatility
        return std::exp(A / 2.0);
    }

    double TeamGlicko2System::VolatilityFunction(
        double x,
        double deltaSquared,
        double phiSquared,
        double v,
        double a,
        double tauSquared) {
        // f(x) = [e^x * (Delta^2 - phi^2 - v - e^x)] / [2(phi^2 + v + e^x)^2]
        //        - (x - a) / tau^2

        double eX = std::exp(x);
        double numerator = eX * (deltaSquared - phiSquared - v - eX);
        double denominator = 2.0 * (phiSquared + v + eX) * (phiSquared + v + eX);

        return (numerator / denominator) - ((x - a) / tauSquared);
    }

    double TeamGlicko2System::UpdateRatingDeviation(double phi, double sigmaPrime, double v) {
        // Step 1: Compute intermediate phi*
        // phi* = sqrt(phi^2 + sigma'^2)
        double phiStar = std::sqrt(phi * phi + sigmaPrime * sigmaPrime);

        // Step 2: Compute new phi'
        // phi' = [1/phi*^2 + 1/v]^(-1/2)
        double phiStarSquared = phiStar * phiStar;
        double phiPrime = 1.0 / std::sqrt(1.0 / phiStarSquared + 1.0 / v);

        return phiPrime;
    }

    double TeamGlicko2System::UpdateRatingMean(
        double mu,
        double phiPrime,
        double g,
        double score,
        double expectedScore) {
        // mu* = mu + phi'^2 * g * (s - E)
        return mu + phiPrime * phiPrime * g * (score - expectedScore);
    }

    double TeamGlicko2System::ClampRatingChange(double mu, double muPrime) {
        // Limit |mu' - mu| to kMaxRatingChange
        double deltaMu = muPrime - mu;

        if (std::abs(deltaMu) > TeamGlicko2::kMaxRatingChange) {
            if (deltaMu > 0.0) {
                return mu + TeamGlicko2::kMaxRatingChange;
            } else {
                return mu - TeamGlicko2::kMaxRatingChange;
            }
        }

        return muPrime;
    }

}  // namespace TeamGlicko2
