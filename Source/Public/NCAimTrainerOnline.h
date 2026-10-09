#pragma once

#include "CoreMinimal.h"
#include "NCAimTrainerTypes.h"

class UWorld;

/** Bounded UT4Stats calls: authority submissions and public client leaderboard reads. */
class NETCODEPLUS_API FNCAimTrainerOnline
{
public:
	// Preset revisions isolate scoring, movement and weapon-specific leaderboards.
	enum { PresetRevision = 11 };
	static int32 PresetRevisionForScenario(int32 Scenario);
	static const TCHAR* ScenarioSlug(int32 Scenario);
	static void Submit(UWorld* World, const FNCAimTrainerResult& Result,
		TFunction<void(bool, const FString&)> Completion);
	static void Fetch(UWorld* World, int32 Scenario,
		TFunction<void(bool, const TArray<FNCAimTrainerLeaderboardRow>&)> Completion, bool bLocal = false, bool bMovementPractice = false);
};
