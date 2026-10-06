#pragma once

#include "CoreMinimal.h"
#include "NCAimTrainerTypes.generated.h"

/** Small public scoreboard row. Server credentials never enter replicated data. */
USTRUCT()
struct NETCODEPLUS_API FNCAimTrainerLeaderboardRow
{
	GENERATED_BODY()

	UPROPERTY() int32 Rank = 0;
	UPROPERTY() FString DisplayName;
	UPROPERTY() int32 Score = 0;
	UPROPERTY() float AccuracyPercent = 0.0f;
};

/** Constructed only by the trainer authority after a complete fixed-preset run. */
struct FNCAimTrainerResult
{
	int32 Scenario = 0;
	FString RunId;
	FString PlayerId;
	FString DisplayName;
	int32 Score = 0;
	int32 Shots = 0;
	int32 Hits = 0;
	int32 Headshots = 0;
	int32 TargetsExpired = 0;
	int32 TrackedMilliseconds = 0;
	int32 DurationMilliseconds = 60000;
};
