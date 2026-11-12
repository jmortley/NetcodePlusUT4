#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "NCAimTrainerClock.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCAimTrainerClockTest,
    "NetcodePlus.AimTrainer.Clock.LongSessionPrecision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNCAimTrainerClockTest::RunTest(const FString& Parameters)
{
    // Exercise the actual accumulator used before the game tick. Starting at
    // an older map age catches reintroducing float storage, not merely a float
    // subtraction at the end of an otherwise double-precision calculation.
    for (int32 Fps : { 700, 960 })
    {
        const float DeltaSeconds = 1.f / float(Fps);
        const double Start = 5000.0;
        double Now = Start;
        int32 NextCheckpoint = 1;
        for (int32 Frame = 1; Frame <= Fps * 60 + 1; ++Frame)
        {
            Now = NCAimTrainerClock::Advance(Now, DeltaSeconds);
            const double Elapsed = Now - Start;
            TestTrue(TEXT("Accumulated elapsed stays within 1 ms of supplied frame deltas"),
                FMath::Abs(Elapsed - double(Frame) * double(DeltaSeconds)) < 0.001);
            if (NextCheckpoint <= 12 && Elapsed >= 5.0 * NextCheckpoint)
            {
                // Dispatch is quantized to one tick (1.43 ms at 700 fps).
                // The deadline remains exactly 5 seconds, rather than moving
                // with the rounded UWorld float clock.
                const double RealElapsed = double(Frame) / double(Fps);
                TestTrue(TEXT("Checkpoint reaches its real five-second deadline within one frame"),
                    RealElapsed >= 5.0 * NextCheckpoint - 0.000001
                    && RealElapsed <= 5.0 * NextCheckpoint + double(DeltaSeconds) + 0.000001);
                ++NextCheckpoint;
            }
        }
        TestEqual(TEXT("All twelve checkpoint deadlines reached"), NextCheckpoint, 13);
    }
    TestTrue(TEXT("Negative, zero and non-finite deltas cannot rewind or poison the clock"),
        NCAimTrainerClock::Advance(5000.0, -1.f) == 5000.0
        && NCAimTrainerClock::Advance(5000.0, 0.f) == 5000.0
        && NCAimTrainerClock::Advance(5000.0, std::numeric_limits<float>::infinity()) == 5000.0
        && NCAimTrainerClock::Advance(5000.0, std::numeric_limits<float>::quiet_NaN()) == 5000.0);
    return true;
}

#endif
