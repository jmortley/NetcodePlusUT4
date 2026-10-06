#pragma once

#include "CoreMinimal.h"
#include "NCAimTrainerTypes.h"

class UWorld;

/** Bounded asynchronous UT4Stats calls. Invoked by the trainer game mode only. */
class NETCODEPLUS_API FNCAimTrainerOnline
{
public:
	// Revised movement/head geometry and popup timing must not share v1 ranks.
	enum { PresetRevision = 2 };
	static const TCHAR* ScenarioSlug(int32 Scenario);
	static void Submit(UWorld* World, const FNCAimTrainerResult& Result,
		TFunction<void(bool, const FString&)> Completion);
	static void Fetch(UWorld* World, int32 Scenario,
		TFunction<void(bool, const TArray<FNCAimTrainerLeaderboardRow>&)> Completion);
};
