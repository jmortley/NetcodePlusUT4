#pragma once

// Authority-owned attempt state. No weapon damage, impulse, or aim assistance
// is changed globally; these rules apply only to the two single-target drills.
namespace NCAimTrainerDrillPolicy
{
    constexpr float FlakKillDamage = 100.f;
    constexpr float FlakResolveSeconds = 0.35f;
    constexpr float CaptureX = -1400.f;
    constexpr float RunnerStartX = 1800.f;
    constexpr float LaneHalfWidth = 550.f;

    struct FAttempt
    {
        int Shot = 0;
        int Streak = 0;
        int PreviousStreak = 0;
        int Stops = 0;
        bool ShotHit = false;
        float ShotAt = -1.f;
        float FlakDamage = 0.f;

        void ObserveShot(int Count, float Now)
        {
            if (Count <= Shot) { return; }
            PreviousStreak = Count == Shot + 1 ? Streak : 0;
            Streak = 0;
            ShotHit = false;
            Shot = Count;
            ShotAt = Now;
        }

        bool ShockHit()
        {
            if (Shot <= 0 || ShotHit) { return false; }
            ShotHit = true;
            Streak = PreviousStreak + 1;
            if (Streak == 5) { ++Stops; Streak = 0; return true; }
            return false;
        }

        void NextTarget()
        {
            Streak = PreviousStreak = 0;
            FlakDamage = 0.f;
            // A later appearance cannot reuse the previous primary shot.
            ShotHit = true;
        }
    };

    inline int FlakScore(int Kills) { return Kills >= 0 && Kills <= 200 ? 100 * Kills : 0; }
    inline int ShockScore(int Hits, int Shots, int Captures)
    {
        if (Hits < 0 || Hits > Shots || Shots > 200 || Captures < 0 || Captures > 200) { return 0; }
        const int Points = 100 * Hits - 25 * (Shots - Hits) - 100 * Captures;
        return Points > 0 ? Points : 0;
    }
}
