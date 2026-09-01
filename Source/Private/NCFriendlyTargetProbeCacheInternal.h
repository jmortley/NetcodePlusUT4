#pragma once

#include "CoreMinimal.h"

namespace NCFriendlyTargetProbeCachePrivate
{
	// These are deliberately far above the camera motion possible during a normal
	// 4.2 ms cache window. They catch missed camera cuts/teleports without turning
	// fast mouse aiming back into one complex trace per rendered frame.
	static const float MeaningfulCameraTranslationUnits = 128.0f;
	static const float MeaningfulCameraRotationDegrees = 45.0f;

	FORCEINLINE bool IsProbeIntervalReusable(float CurrentInterval, float CachedInterval)
	{
		// Exact comparison is intentional: both values come from the same clamped
		// cvar calculation, and any runtime rate change must start a fresh bound.
		return CurrentInterval > 0.0f && CurrentInterval == CachedInterval;
	}

	FORCEINLINE double AdvanceProbeDeadline(double Now, double Deadline, double Interval)
	{
		if (Interval <= 0.0)
		{
			return Now;
		}
		if (Deadline <= 0.0 || Deadline > Now || Now - Deadline > 4.0 * Interval)
		{
			return Now + Interval;
		}

		// Preserve the configured average rate at 500+ FPS without issuing a run
		// of catch-up probes after a hitch.
		const int32 PeriodsToSkip = FMath::FloorToInt(
			float((Now - Deadline) / Interval)) + 1;
		return Deadline + double(PeriodsToSkip) * Interval;
	}

	FORCEINLINE bool IsMeaningfulCameraDiscontinuity(
		const FVector& PreviousLocation, const FRotator& PreviousRotation,
		const FVector& CurrentLocation, const FRotator& CurrentRotation)
	{
		if (PreviousLocation.ContainsNaN() || PreviousRotation.ContainsNaN() ||
			CurrentLocation.ContainsNaN() || CurrentRotation.ContainsNaN())
		{
			return true;
		}

		if (FVector::DistSquared(PreviousLocation, CurrentLocation) >
			MeaningfulCameraTranslationUnits * MeaningfulCameraTranslationUnits)
		{
			return true;
		}

		return FMath::Abs(FRotator::NormalizeAxis(
			CurrentRotation.Pitch - PreviousRotation.Pitch)) > MeaningfulCameraRotationDegrees ||
			FMath::Abs(FRotator::NormalizeAxis(
			CurrentRotation.Yaw - PreviousRotation.Yaw)) > MeaningfulCameraRotationDegrees ||
			FMath::Abs(FRotator::NormalizeAxis(
			CurrentRotation.Roll - PreviousRotation.Roll)) > MeaningfulCameraRotationDegrees;
	}
}
