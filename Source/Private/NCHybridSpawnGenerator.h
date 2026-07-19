#pragma once

#include "CoreMinimal.h"

class APlayerStart;
class UWorld;

/**
 * Native reconstruction of the useful part of Absolute Elimination's cooked
 * spawn queue: authored PlayerStarts seed a breadth-first field of validated
 * transforms. Anchor selection is intentionally supplied by NetcodePlus so it
 * is not tied to Absolute's predictable 137-degree round rotation.
 */
struct FNCHybridSpawnSettings
{
	float MaxTeamSpawnRadius;
	float MaxInitialSeedRadius;
	float PlayerMinRadius;
	float GeneratedSpacingMultiplier;
	float TraceStartOffsetZ;
	float TraceEndOffsetZ;
	float TraceRadius;
	float TraceHalfHeight;
	float PitTestDistanceHorizontal;
	float PitTestDistanceVertical;
	float MinDistanceFromKillZ;
	float MinimumCrossTeamDistance;
	int32 ExtraSpawnPadding;
	int32 MaxExpansionTiers;

	FNCHybridSpawnSettings();
};

struct FNCHybridSpawnBuildStats
{
	int32 SeedCount;
	int32 GeneratedCount;
	int32 RejectedRadius;
	int32 RejectedFloor;
	int32 RejectedSlope;
	int32 RejectedClearance;
	int32 RejectedSpacing;
	int32 RejectedKillZ;
	int32 RejectedPit;
	int32 RejectedPainVolume;

	FNCHybridSpawnBuildStats();
};

struct FNCHybridSpawnResult
{
	TArray<FTransform> Team0Queue;
	TArray<FTransform> Team1Queue;
	FNCHybridSpawnBuildStats Team0Stats;
	FNCHybridSpawnBuildStats Team1Stats;
	FVector Team0Anchor;
	FVector Team1Anchor;
	FName Team0AnchorName;
	FName Team1AnchorName;
	float AnchorDistance2D;
	float TeamRadius;
	bool bAnchorsValid;
	bool bTeam0Complete;
	bool bTeam1Complete;

	FNCHybridSpawnResult();
};

class FNCHybridSpawnGenerator
{
public:
	/**
	 * Builds randomized, breadth-first transform queues around one high-quality
	 * anchor pair selected from the mode's current per-team layout.
	 *
	 * A true return means a safe anchor pair was found. Individual queues may be
	 * shorter than requested; callers deliberately consume the partial queue and
	 * then fall back to their existing PlayerStart selection.
	 */
	static bool Generate(
		UWorld* World,
		const TArray<APlayerStart*>& AllStarts,
		const TArray<APlayerStart*>& Team0AnchorCandidates,
		const TArray<APlayerStart*>& Team1AnchorCandidates,
		int32 Team0PlayerCount,
		int32 Team1PlayerCount,
		const FNCHybridSpawnSettings& Settings,
		FNCHybridSpawnResult& OutResult);

	/** A real PlayerStart retained only for RestartPlayer engine bookkeeping. */
	static APlayerStart* FindNearestPlayerStart(
		const TArray<APlayerStart*>& Starts,
		const FVector& Location);
};
