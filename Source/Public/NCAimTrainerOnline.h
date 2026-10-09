#pragma once

#include "CoreMinimal.h"
#include "NCAimTrainerTypes.h"

class UWorld;

/** Bounded UT4Stats calls: authority submissions and public client leaderboard reads. */
class NETCODEPLUS_API FNCAimTrainerOnline
{
public:
	// UT3 animation boards stay at 14; reward-only airborne scoring starts at 15.
	enum { PresetRevision = 14, AirbornePresetRevision = 15 };
	static int32 PresetRevisionForScenario(int32 Scenario);
	static const TCHAR* ScenarioSlug(int32 Scenario);
	static void Submit(UWorld* World, const FNCAimTrainerResult& Result,
		TFunction<void(bool, const FString&)> Completion);
	static void Fetch(UWorld* World, int32 Scenario,
		TFunction<void(bool, const TArray<FNCAimTrainerLeaderboardRow>&)> Completion, bool bLocal = false, bool bMovementPractice = false);
};
