#pragma once

#include <cmath>

namespace NCAimTrainerClock
{
    // World TimeSeconds is an accumulated float in UE4.15. Retain each frame's
    // delta in double precision so an older practice map cannot change run pace.
    // Tick supplies the same game-time delta used by movement and timer managers.
    inline double Advance(double Now, float DeltaSeconds)
    {
        return std::isfinite(DeltaSeconds) && DeltaSeconds > 0.f
            ? Now + double(DeltaSeconds) : Now;
    }
}
