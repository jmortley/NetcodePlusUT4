#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "../NCFriendlyTargetProbeCacheInternal.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNCFriendlyTargetProbeDiscontinuityTest,
	"NetcodePlus.Performance.FriendlyTargetProbeDiscontinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCFriendlyTargetProbeDiscontinuityTest::RunTest(const FString& Parameters)
{
	using namespace NCFriendlyTargetProbeCachePrivate;

	const FVector Origin = FVector::ZeroVector;
	const FRotator Forward = FRotator::ZeroRotator;

	TestFalse(TEXT("Normal movement inside one cache window is retained"),
		IsMeaningfulCameraDiscontinuity(Origin, Forward,
			FVector(16.0f, 8.0f, 4.0f), FRotator(3.0f, 12.0f, 1.0f)));
	TestFalse(TEXT("Threshold values remain inside the bounded cache"),
		IsMeaningfulCameraDiscontinuity(Origin, Forward,
			FVector(MeaningfulCameraTranslationUnits, 0.0f, 0.0f),
			FRotator(0.0f, MeaningfulCameraRotationDegrees, 0.0f)));
	TestTrue(TEXT("Large camera translation forces a refresh"),
		IsMeaningfulCameraDiscontinuity(Origin, Forward,
			FVector(MeaningfulCameraTranslationUnits + 1.0f, 0.0f, 0.0f), Forward));
	TestTrue(TEXT("Large camera rotation forces a refresh"),
		IsMeaningfulCameraDiscontinuity(Origin, Forward, Origin,
			FRotator(0.0f, MeaningfulCameraRotationDegrees + 1.0f, 0.0f)));
	TestFalse(TEXT("Yaw wrap is measured by its short angular distance"),
		IsMeaningfulCameraDiscontinuity(Origin, FRotator(0.0f, 179.0f, 0.0f),
			Origin, FRotator(0.0f, -179.0f, 0.0f)));

	const double Interval240Hz = 1.0 / 240.0;
	TestEqual(TEXT("First probe starts one configured interval"),
		AdvanceProbeDeadline(10.0, 0.0, Interval240Hz),
		10.0 + Interval240Hz);
	TestEqual(TEXT("An early forced refresh restarts the bounded interval"),
		AdvanceProbeDeadline(10.001, 10.0 + Interval240Hz, Interval240Hz),
		10.001 + Interval240Hz);
	TestEqual(TEXT("A hitch skips missed periods instead of scheduling catch-up probes"),
		AdvanceProbeDeadline(10.030, 10.0 + Interval240Hz, Interval240Hz),
		10.030 + Interval240Hz);
	TestTrue(TEXT("An unchanged configured interval can reuse the cache"),
		IsProbeIntervalReusable(float(Interval240Hz), float(Interval240Hz)));
	TestFalse(TEXT("A runtime rate increase cannot inherit the old longer deadline"),
		IsProbeIntervalReusable(float(Interval240Hz), float(1.0 / 30.0)));
	TestFalse(TEXT("Stock every-frame mode cannot reuse the cache"),
		IsProbeIntervalReusable(0.0f, float(Interval240Hz)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
