#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "../NCRemoteAnimationPolicy.h"
#include "../NCRemoteAnimationURO.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCRemoteAnimationPriorityTest,
	"NetcodePlus.Performance.RemoteAnimation.Priority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCRemoteAnimationPriorityTest::RunTest(const FString& Parameters)
{
	using namespace NCRemoteAnimationPolicy;
	const FSettings Settings;
	FView View;
	TestTrue(TEXT("A peripheral sphere touching the near-distance boundary is protected"),
		IsHighPriorityForView(FVector(1560.f, 2080.f, 0.f), 100.f, View, Settings));
	TestFalse(TEXT("A small peripheral sphere outside the near boundary may be reduced"),
		IsHighPriorityForView(FVector(1620.f, 2160.f, 0.f), 100.f, View, Settings));
	TestFalse(TEXT("A distant peripheral sphere may be reduced"),
		IsHighPriorityForView(FVector(10000.f, 10000.f, 0.f), 100.f, View, Settings));
	TestTrue(TEXT("Central aim remains protected even at long range"),
		IsHighPriorityForView(FVector(30000.f, 0.f, 0.f), 60.f, View, Settings));
	TestTrue(TEXT("Projected radius at the screen threshold is protected"),
		IsHighPriorityForView(FVector(5000.f, 5000.f, 0.f), 500.f, View, Settings));
	TestFalse(TEXT("Projected radius below the screen threshold can be reduced"),
		IsHighPriorityForView(FVector(5000.f, 5000.f, 0.f), 490.f, View, Settings));

	const float CenterAngle = FMath::DegreesToRadians(20.f);
	const FVector ConeEdgeCenter(10000.f * FMath::Cos(CenterAngle),
		10000.f * FMath::Sin(CenterAngle), 0.f);
	FSettings ConeSettings = Settings;
	ConeSettings.MinScreenFraction = 1.f; // Isolate sphere/cone intersection from projected size.
	TestTrue(TEXT("A sphere intersecting the aim cone is protected even with its center outside"),
		IsHighPriorityForView(ConeEdgeCenter, 873.f, View, ConeSettings));
	TestFalse(TEXT("A sphere just clear of the aim cone may be reduced"),
		IsHighPriorityForView(ConeEdgeCenter, 870.f, View, ConeSettings));
	TestFalse(TEXT("A distant sphere wholly behind the camera is not protected by projection"),
		IsHighPriorityForView(FVector(-5000.f, 0.f, 0.f), 100.f, View, Settings));
	TestTrue(TEXT("A near sphere remains protected behind the camera"),
		IsHighPriorityForView(FVector(-1000.f, 0.f, 0.f), 100.f, View, Settings));
	TestTrue(TEXT("A distant sphere crossing the camera plane conservatively stays full rate"),
		IsHighPriorityForView(FVector(-100.f, 10000.f, 0.f), 200.f, View, Settings));

	View.Forward = FVector(10.f, 0.f, 0.f);
	TestTrue(TEXT("Non-unit forward vectors retain central aim protection"),
		IsHighPriorityForView(FVector(30000.f, 0.f, 0.f), 60.f, View, Settings));
	TestFalse(TEXT("Non-unit forward vectors do not enlarge the aim cone"),
		IsHighPriorityForView(FVector(10000.f, 10000.f, 0.f), 100.f, View, Settings));
	FView SecondView;
	SecondView.Forward = FVector(1.f, 1.f, 0.f);
	TestTrue(TEXT("Another local view can protect a pawn peripheral to the first view"),
		IsHighPriorityForView(FVector(10000.f, 10000.f, 0.f), 100.f, SecondView, Settings));
	View.Location = FVector(1000.f, 500.f, 200.f);
	TestTrue(TEXT("Priority uses the supplied view location"),
		IsHighPriorityForView(View.Location + FVector(30000.f, 0.f, 0.f), 60.f, View, Settings));
	View.bProtectAll = true;
	TestTrue(TEXT("Zoom protection keeps even distant behind-camera actors full rate"),
		IsHighPriorityForView(FVector(-30000.f, 0.f, 0.f), 60.f, View, Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCRemoteAnimationInvalidInputTest,
	"NetcodePlus.Performance.RemoteAnimation.InvalidInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCRemoteAnimationInvalidInputTest::RunTest(const FString& Parameters)
{
	using namespace NCRemoteAnimationPolicy;
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	const FVector Peripheral(10000.f, 10000.f, 0.f);
	FView View;
	FSettings Settings;
	TestTrue(TEXT("Nonfinite target coordinates fail to full rate"),
		IsHighPriorityForView(FVector(NaN, 0.f, 0.f), 100.f, View, Settings));
	TestTrue(TEXT("Overflowing finite geometry fails to full rate"),
		IsHighPriorityForView(FVector(TNumericLimits<float>::Max(), 0.f, 0.f), 100.f, View, Settings));
	TestTrue(TEXT("Negative radius fails to full rate"),
		IsHighPriorityForView(Peripheral, -1.f, View, Settings));
	TestTrue(TEXT("Nonfinite radius fails to full rate"),
		IsHighPriorityForView(Peripheral, Infinity, View, Settings));
	View.Location.X = Infinity;
	TestTrue(TEXT("Nonfinite view location fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	View = FView();
	View.Forward = FVector::ZeroVector;
	TestTrue(TEXT("Zero view direction fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	View.Forward.X = NaN;
	TestTrue(TEXT("Nonfinite view direction fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	View = FView();
	View.TanHalfVerticalFOV = 0.f;
	TestTrue(TEXT("Invalid FOV fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	View = FView();
	Settings.NearDistance = NaN;
	TestTrue(TEXT("Invalid near threshold fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	Settings = FSettings();
	Settings.AimHalfAngleDegrees = 90.f;
	TestTrue(TEXT("Invalid aim cone fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	Settings = FSettings();
	Settings.MinScreenFraction = -1.f;
	TestTrue(TEXT("Invalid screen threshold fails to full rate"),
		IsHighPriorityForView(Peripheral, 100.f, View, Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCRemoteAnimationHysteresisTest,
	"NetcodePlus.Performance.RemoteAnimation.Hysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCRemoteAnimationHysteresisTest::RunTest(const FString& Parameters)
{
	using namespace NCRemoteAnimationPolicy;
	FState State;
	TestFalse(TEXT("Initial eligibility starts a delay"), State.Update(true, 10.0, 0.25f));
	TestFalse(TEXT("Before the delay full rate is retained"), State.Update(true, 10.24, 0.25f));
	TestTrue(TEXT("At the delay reduced updates are allowed"), State.Update(true, 10.25, 0.25f));
	TestTrue(TEXT("Continuous eligibility remains reduced"), State.Update(true, 11.0, 0.25f));
	TestFalse(TEXT("High priority promotes immediately"), State.Update(false, 11.001, 0.25f));
	TestFalse(TEXT("New eligibility must wait again"), State.Update(true, 11.1, 0.25f));
	TestFalse(TEXT("A short eligibility flicker never demotes"), State.Update(false, 11.2, 0.25f));
	TestFalse(TEXT("A fresh candidate starts its own delay"), State.Update(true, 12.0, 0.25f));
	TestTrue(TEXT("The fresh candidate eventually demotes"), State.Update(true, 12.25, 0.25f));
	TestFalse(TEXT("Backward time immediately restores full rate"), State.Update(true, 5.0, 0.25f));
	TestFalse(TEXT("The rewind restarts the full delay"), State.Update(true, 5.24, 0.25f));
	TestTrue(TEXT("Eligibility can recover after a rewind"), State.Update(true, 5.25, 0.25f));
	TestFalse(TEXT("Nonfinite time resets the candidate"),
		State.Update(true, std::numeric_limits<double>::quiet_NaN(), 0.25f));
	TestFalse(TEXT("Finite time after a reset starts a fresh delay"), State.Update(true, 20.0, 0.25f));
	TestFalse(TEXT("Infinite time also resets the candidate"),
		State.Update(true, std::numeric_limits<double>::infinity(), 0.25f));
	TestFalse(TEXT("Negative delay fails to full rate"), State.Update(true, 21.0, -1.f));
	TestFalse(TEXT("Nonfinite delay fails to full rate"),
		State.Update(true, 22.0, std::numeric_limits<float>::quiet_NaN()));
	TestTrue(TEXT("Explicit zero delay allows immediate demotion"), State.Update(true, 23.0, 0.f));
	State.Reset();
	TestFalse(TEXT("Explicit reset discards eligibility history"), State.Update(true, 24.0, 0.25f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCRemoteAnimationPendingTimeTest,
	"NetcodePlus.Performance.RemoteAnimation.PendingTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCRemoteAnimationPendingTimeTest::RunTest(const FString& Parameters)
{
	using namespace NCRemoteAnimationURO;
	TestEqual(TEXT("A skipped pose owes its unconsumed interval"),
		GetPendingAnimationTime(-0.004f), 0.004f);
	TestEqual(TEXT("Several skipped poses retain the whole owed interval"),
		GetPendingAnimationTime(-0.012f), 0.012f);
	TestEqual(TEXT("A completed catch-up has no pending interval"),
		GetPendingAnimationTime(0.f), 0.f);
	TestEqual(TEXT("Look-ahead credit must not be replayed as elapsed time"),
		GetPendingAnimationTime(0.012f), 0.f);
	TestEqual(TEXT("Nonfinite offset cannot become animation delta"),
		GetPendingAnimationTime(std::numeric_limits<float>::quiet_NaN()), 0.f);
	TestEqual(TEXT("Negative infinity cannot become owed animation time"),
		GetPendingAnimationTime(-std::numeric_limits<float>::infinity()), 0.f);
	TestEqual(TEXT("Positive infinity cannot become animation credit"),
		GetPendingAnimationTime(std::numeric_limits<float>::infinity()), 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
