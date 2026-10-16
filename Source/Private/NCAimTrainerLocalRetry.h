#pragma once

#include "CoreMinimal.h"
#include "Misc/DateTime.h"

namespace NCAimTrainerLocalRetry
{
	// Respect the service's current policy. This is not a client-side restart
	// quota: a cooldown is imposed only after an explicit server rejection.
	inline double DelaySeconds(int32 Code, const FString& RetryAfter, const FString& ResponseDate)
	{
		FString Value = RetryAfter;
		Value = Value.Trim().TrimTrailing();
		double Delay = 0;
		bool bDigits = !Value.IsEmpty();
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] < '0' || Value[Index] > '9') { bDigits = false; break; }
			Delay = Delay * 10.0 + double(Value[Index] - '0');
		}
		if (bDigits && FMath::IsFinite(Delay)) return Delay;
		FDateTime RetryDate;
		if (!bDigits && FDateTime::ParseHttpDate(Value, RetryDate))
		{
			FDateTime Date;
			if (!FDateTime::ParseHttpDate(ResponseDate, Date)) Date = FDateTime::UtcNow();
			Delay = (RetryDate - Date).GetTotalSeconds();
			return FMath::Max(0.0, Delay);
		}
		// A bare/malformed throttle response still must not cause a retry burst.
		return Code == 429 ? 60.0 : 0.0;
	}

	struct FStartCooldown
	{
		double Until = 0;
		void Defer(double Now, double Delay) { Until = FMath::Max(Until, Now + Delay); }
		double Remaining(double Now) const { return FMath::Max(0.0, Until - Now); }
	};
}
