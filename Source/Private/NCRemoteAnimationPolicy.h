#pragma once

#include "CoreMinimal.h"

namespace NCRemoteAnimationPolicy
{
	struct FView
	{
		FVector Location = FVector::ZeroVector;
		FVector Forward = FVector::ForwardVector;
		float TanHalfVerticalFOV = 1.f;
		bool bProtectAll = false;
	};

	struct FSettings
	{
		float NearDistance = 2500.f;
		float AimHalfAngleDegrees = 15.f;
		float MinScreenFraction = 0.10f;
		float DemotionDelay = 0.25f;
		float MinFPS = 240.f;
	};

	// True keeps full-rate animation. Callers must preserve the highest priority
	// across local views; this helper performs no visibility or world queries.
	inline bool IsHighPriorityForView(const FVector& Center, float Radius,
		const FView& View, const FSettings& Settings)
	{
		if (View.bProtectAll || Center.ContainsNaN() || View.Location.ContainsNaN()
			|| View.Forward.ContainsNaN() || !FMath::IsFinite(Radius) || Radius < 0.f
			|| !FMath::IsFinite(View.TanHalfVerticalFOV) || View.TanHalfVerticalFOV <= 0.f
			|| !FMath::IsFinite(Settings.NearDistance) || Settings.NearDistance < 0.f
			|| !FMath::IsFinite(Settings.AimHalfAngleDegrees)
			|| Settings.AimHalfAngleDegrees < 0.f || Settings.AimHalfAngleDegrees >= 90.f
			|| !FMath::IsFinite(Settings.MinScreenFraction) || Settings.MinScreenFraction <= 0.f)
		{
			return true;
		}

		const FVector Offset = Center - View.Location;
		const float DistanceSquared = Offset.SizeSquared();
		const float ForwardSquared = View.Forward.SizeSquared();
		if (!FMath::IsFinite(DistanceSquared) || !FMath::IsFinite(ForwardSquared)
			|| ForwardSquared <= SMALL_NUMBER)
		{
			return true;
		}
		if (FMath::Sqrt(DistanceSquared) - Radius <= Settings.NearDistance)
		{
			return true;
		}

		const FVector Forward = View.Forward / FMath::Sqrt(ForwardSquared);
		const float Depth = FVector::DotProduct(Offset, Forward);
		if (Depth + Radius <= 0.f)
		{
			return false; // The entire sphere is behind this view.
		}
		if (Depth <= Radius)
		{
			return true; // A sphere crossing the camera plane has no stable projected size.
		}
		const float ProjectionDenominator = Depth * View.TanHalfVerticalFOV;
		if (!FMath::IsFinite(ProjectionDenominator) || ProjectionDenominator <= 0.f
			|| Radius / ProjectionDenominator >= Settings.MinScreenFraction)
		{
			return true;
		}

		// Perpendicular distance to the cone's side includes spheres whose centers
		// lie outside the aim angle but whose surface intersects the protected cone.
		const float Lateral = FMath::Sqrt(FMath::Max(0.f, DistanceSquared - Depth * Depth));
		const float Angle = FMath::DegreesToRadians(Settings.AimHalfAngleDegrees);
		return Lateral * FMath::Cos(Angle) - Depth * FMath::Sin(Angle) <= Radius;
	}

	inline bool IsFiniteTime(double Value)
	{
		// This engine's FMath::IsFinite accepts float, so do not narrow timestamps.
		return Value >= -TNumericLimits<double>::Max() && Value <= TNumericLimits<double>::Max();
	}

	struct FState
	{
		void Reset()
		{
			bHasCandidate = false;
			EligibleSince = 0.0;
			LastUpdate = 0.0;
		}

		// True permits reduced updates after continuous eligibility for Delay.
		// Losing eligibility promotes immediately; a clock rewind restarts the wait.
		bool Update(bool bEligible, double Now, float Delay)
		{
			if (!bEligible || !IsFiniteTime(Now) || !FMath::IsFinite(Delay) || Delay < 0.f)
			{
				Reset();
				return false;
			}
			const bool bTimeRewound = bHasCandidate && Now < LastUpdate;
			if (!bHasCandidate || bTimeRewound)
			{
				bHasCandidate = true;
				EligibleSince = Now;
			}
			LastUpdate = Now;
			const double Elapsed = Now - EligibleSince;
			if (!IsFiniteTime(Elapsed))
			{
				Reset();
				return false;
			}
			return !bTimeRewound && Elapsed >= double(Delay);
		}

	private:
		bool bHasCandidate = false;
		double EligibleSince = 0.0;
		double LastUpdate = 0.0;
	};
}
