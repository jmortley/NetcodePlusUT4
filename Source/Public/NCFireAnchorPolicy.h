#pragma once

// Engine-independent admission policy, shared with the native regression suite.
// A client-selected movement marker is bounded evidence, never proof of click time.
#include <cmath>
#include <limits>

namespace NCFireAnchorPolicy
{
    // ACK traffic is a conservative liveness prerequisite, not a timestamp for
    // the engine's last successful RTT measurement. Both arguments use net-driver
    // time, independently of paused/dilated gameplay time.
    inline bool AckIsRecent(double DriverNow, double LastAck)
    {
        if (!std::isfinite(DriverNow) || !std::isfinite(LastAck)) return false;
        // UE4.15 stores LastRecvAckTime as float but Driver->Time as double.
        // A just-received ACK can round slightly ahead of the driver's clock;
        // do not reject half of same-tick arrivals on a long-running server.
        const double Rounding = std::fabs(LastAck) * std::numeric_limits<float>::epsilon() + 0.000001;
        const double Age = DriverNow - LastAck;
        return Age >= -Rounding && Age <= 2.0 + Rounding;
    }

    struct Input
    {
        bool Match = false;
        bool Consumed = false;
        bool Obstructed = false;
        float Now = 0.f;
        float MoveProcessed = 0.f;
        float BaseRewind = 0.f;
        float Cap = 0.f;
        float OriginXY = 0.f;
        float OriginZ = 0.f;
        float AimDelta = 0.f;
    };

    inline const char* Admit(const Input& I)
    {
        if (!I.Match) return "no_matching_move";
        if (I.Consumed) return "move_already_used";
        if (!std::isfinite(I.Now) || !std::isfinite(I.MoveProcessed)
            || !std::isfinite(I.BaseRewind) || !std::isfinite(I.Cap)
            || !std::isfinite(I.OriginXY) || !std::isfinite(I.OriginZ)
            || !std::isfinite(I.AimDelta)) return "nonfinite";
        const float Age = I.Now - I.MoveProcessed;
        if (Age < 0.f || Age > 0.080001f) return "move_age";
        if (I.BaseRewind < 0.f || I.Cap <= 0.f || I.Cap > 0.125001f
            || I.BaseRewind + Age > I.Cap) return "rewind_budget";
        if (I.OriginXY < 0.f || I.OriginXY > 20.f || std::fabs(I.OriginZ) > 24.f)
            return "origin_delta";
        if (I.AimDelta < 0.f || I.AimDelta > 2.f) return "aim_delta";
        if (I.Obstructed) return "origin_obstructed";
        return nullptr;
    }

    inline bool DispatchAge(float Now, float AcceptedAt, float ExtraAtAccept,
        float BaseRewind, float Cap, float& Extra)
    {
        Extra = 0.f;
        if (!std::isfinite(Now) || !std::isfinite(AcceptedAt)
            || !std::isfinite(ExtraAtAccept) || !std::isfinite(BaseRewind)
            || !std::isfinite(Cap) || Now < AcceptedAt || ExtraAtAccept < 0.f
            || ExtraAtAccept > 0.080001f || BaseRewind < 0.f || Cap <= 0.f
            || Cap > 0.125001f) return false;
        Extra = ExtraAtAccept + (Now - AcceptedAt);
        return Extra >= 0.f && BaseRewind + Extra <= Cap;
    }
}
