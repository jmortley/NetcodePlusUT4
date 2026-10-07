#include "NCRocketVolley.h"
#include <cassert>
#include <cstdio>

using namespace NCRocketVolley;

static void TestIndependentOrdinalOutcomes()
{
    const uint8_t Orders[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    for (uint8_t SpawnedMask = 0; SpawnedMask < 8; ++SpawnedMask)
    {
        for (const auto& Order : Orders)
        {
            FProgress Volley;
            assert(Volley.Begin(1) && Volley.Release(1, 3));
            for (uint8_t Ordinal : Order)
            {
                const bool Spawned = (SpawnedMask & (1u << Ordinal)) != 0;
                assert(Volley.Resolve(1, Ordinal, Spawned));
                // A conflicting retry cannot reverse an individual outcome.
                assert(!Volley.Resolve(1, Ordinal, !Spawned));
            }
            assert(Volley.ResolvedMask == 7 && Volley.SpawnedMask == SpawnedMask);
            assert(Volley.Finish(1));
            for (uint8_t Ordinal = 0; Ordinal < 3; ++Ordinal)
            {
                const bool Survives = (SpawnedMask & (1u << Ordinal)) != 0;
                assert(CancelOrdinal(EResult::Completed, 3, SpawnedMask, Ordinal) == !Survives);
                assert(CancelOrdinal(EResult::Cancelled, 3, SpawnedMask, Ordinal) == !Survives);
            }
        }
    }
}

int main()
{
    TestIndependentOrdinalOutcomes();
    assert(Next(0) == 1 && Next(0xffffffffu) == 1);
    assert(!IsNewer(0, 0) && !IsNewer(42, 42));
    assert(IsNewer(1, 0xffffffffu) && !IsNewer(0xffffffffu, 1));
    assert(!IsNewer(0x80000001u, 1));

    FProgress State;
    assert(!State.Begin(0));
    assert(State.Begin(50));
    assert(!State.Begin(50) && !State.Begin(51)); // No duplicate/reset while loading.
    assert(!State.Release(49, 3));
    assert(!State.Resolve(50, 0, true)); // Nothing can spawn before release.
    assert(!State.Release(50, 4));
    assert(State.Release(50, 3));
    assert(!State.Release(50, 2)); // Later Stop cannot change mode/count snapshot.
    assert(!State.Resolve(50, 3, true));
    assert(State.Resolve(50, 0, true));
    assert(!State.Resolve(50, 0, true)); // Replayed ordinal cannot fire twice.
    assert(State.Resolve(50, 2, true)); // Completion order need not be ordinal order.
    assert(State.Resolve(50, 1, false));
    assert(State.ResolvedMask == 7 && State.SpawnedMask == 5);
    assert(State.Finish(50));
    assert(!State.Resolve(50, 1, true) && !State.Release(50, 3));
    assert(!State.Begin(49));
    assert(State.Begin(51));
    assert(!State.Finish(50) && !State.Resolve(50, 0, false)); // Stale result cannot cancel new load.
    assert(State.Id == 51 && !State.Terminal && !State.Released);
    assert(State.Release(51, 1));
    assert(State.Resolve(51, 0, true));
    assert(State.Finish(51));
    assert(State.Begin(0xffffffffu) == false); // Implausible backward jump.

    FProgress Wrap;
    assert(Wrap.Begin(0xffffffffu));
    assert(Wrap.Finish(0xffffffffu));
    assert(Wrap.Begin(1));
    assert(!Wrap.Finish(0xffffffffu));

    // An acceptance proves count, not spawn; no accepted visual is retired
    // merely because the server still owes its per-rocket actor outcome.
    for (uint8_t i = 0; i < 3; ++i) assert(!CancelOrdinal(EResult::Accepted, 3, 0, i));
    assert(CancelOrdinal(EResult::Accepted, 1, 0, 1));
    assert(!CancelOrdinal(EResult::Completed, 3, 5, 0));
    assert(CancelOrdinal(EResult::Completed, 3, 5, 1));
    assert(!CancelOrdinal(EResult::Completed, 3, 5, 2));
    assert(!CancelOrdinal(EResult::Cancelled, 3, 1, 0)); // An already-spawned rocket survives swap/death.
    assert(CancelOrdinal(EResult::Cancelled, 3, 1, 1));
    assert(CancelOrdinal(EResult::Rejected, 3, 7, 0));
    assert(CountMask(0) == 0 && CountMask(1) == 1 && CountMask(3) == 7 && CountMask(4) == 0);
    std::puts("rocket volley identity, replay, wrap, partial-outcome and stale-result cases passed");
}
