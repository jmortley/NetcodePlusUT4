#pragma once

// Pure server scoring rules. All supported preset revisions are checked by UT4Stats.
namespace NCAimTrainerScoring
{
    inline int PrecisionScore(int Hits, int Shots, int Expired)
    {
        if (Hits < 0 || Shots < Hits || Expired < 0 || Shots > 200 || Expired > 200)
        {
            return 0;
        }
        const int Value = 100 * Hits - 25 * (Shots - Hits) - 25 * Expired;
        return Value > 0 ? Value : 0;
    }

    // One delayed frame cannot fabricate a long interval of target contact.
    // Requiring both endpoints also avoids crediting the approach interval.
    inline double TrackingCredit(double Delta, bool PreviousContact, bool Contact)
    {
        return PreviousContact && Contact && Delta > 0.0 && Delta <= 0.1 ? Delta : 0.0;
    }

    inline int TrackingMilliseconds(double Seconds)
    {
        if (!(Seconds > 0.0)) { return 0; }
        if (Seconds >= 60.0) { return 60000; }
        return static_cast<int>(Seconds * 1000.0);
    }

    inline float TrackingAccuracy(int TrackedMilliseconds, int FiredMilliseconds)
    {
        if (FiredMilliseconds <= 0 || FiredMilliseconds > 60000
            || TrackedMilliseconds < 0 || TrackedMilliseconds > FiredMilliseconds) { return 0.f; }
        return 100.f * float(TrackedMilliseconds) / float(FiredMilliseconds);
    }
}
