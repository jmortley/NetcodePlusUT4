#pragma once

#include <stdint.h>

// 329 wire identities are scoped to a weapon actor. Zero is never a volley.
// This core deliberately has no engine dependency so replay/ordering rules can
// be exercised with the same implementation in the native regression tests.
namespace NCRocketVolley
{
    enum class EResult : uint8_t { Accepted, Completed, Rejected, Cancelled };
    enum class ERocketResult : uint8_t { Spawned, Resolved, Rejected, Cancelled };
    static const uint8_t MaxRockets = 3;
    // Local input may wait briefly for the initial server-owned epoch. This
    // is not charge time and never changes the authoritative load schedule.
    constexpr double OwnershipInputWindowSeconds = 0.25;

    inline bool IsNewer(uint32_t Candidate, uint32_t Previous)
    {
        return Candidate != 0 && (Previous == 0 ||
            (Candidate != Previous && uint32_t(Candidate - Previous) < 0x80000000u));
    }

    inline uint32_t Next(uint32_t Previous)
    {
        const uint32_t Result = Previous + 1;
        return Result == 0 ? 1 : Result;
    }

    inline uint8_t CountMask(uint8_t Count)
    {
        return Count <= MaxRockets ? uint8_t((1u << Count) - 1u) : 0;
    }

    inline bool CancelOrdinal(EResult Result, uint8_t Count, uint8_t SpawnedMask, uint8_t Ordinal)
    {
        return Ordinal >= MaxRockets || Result == EResult::Rejected || Ordinal >= Count ||
            (Result != EResult::Accepted && !(SpawnedMask & (1u << Ordinal)));
    }

    struct FProgress
    {
        uint32_t Id;
        uint8_t Count;
        uint8_t SpawnedMask;
        uint8_t ResolvedMask;
        bool Released;
        bool Terminal;

        FProgress() : Id(0), Count(0), SpawnedMask(0), ResolvedMask(0), Released(false), Terminal(false) {}

        bool Begin(uint32_t NewId)
        {
            if (!IsNewer(NewId, Id) || (Id != 0 && !Terminal)) return false;
            Id = NewId;
            Count = SpawnedMask = ResolvedMask = 0;
            Released = Terminal = false;
            return true;
        }

        bool Release(uint32_t InId, uint8_t InCount)
        {
            if (InId != Id || Id == 0 || Released || Terminal || InCount > MaxRockets) return false;
            Released = true;
            Count = InCount;
            return true;
        }

        bool Resolve(uint32_t InId, uint8_t Ordinal, bool Spawned)
        {
            if (InId != Id || !Released || Terminal || Ordinal >= Count) return false;
            const uint8_t Bit = uint8_t(1u << Ordinal);
            if (ResolvedMask & Bit) return false;
            ResolvedMask |= Bit;
            if (Spawned) SpawnedMask |= Bit;
            return true;
        }

        bool Finish(uint32_t InId)
        {
            if (Id == 0 || InId != Id || Terminal) return false;
            Terminal = true;
            return true;
        }
    };
}
