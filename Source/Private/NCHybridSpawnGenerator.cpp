#include "NCHybridSpawnGenerator.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"

FNCHybridSpawnSettings::FNCHybridSpawnSettings()
	: MaxTeamSpawnRadius(3100.f)
	, MaxInitialSeedRadius(1300.f)
	, PlayerMinRadius(300.f)
	, GeneratedSpacingMultiplier(1.01f)
	, TraceStartOffsetZ(90.f)
	, TraceEndOffsetZ(-2000.f)
	, TraceRadius(40.f)
	// Must match AUTCharacter's STANDING capsule (40x108, UTCharacter.cpp InitCapsuleSize),
	// not APlayerStart's 40x92: the floor-sweep hit is this capsule's center, so the half-
	// height is both the clearance the validation proves AND the Z the pawn is placed at.
	// At 92 every generated spawn sat 16uu low and 186-216uu ceilings validated but
	// wedged the spawned pawn.
	, TraceHalfHeight(108.f)
	, PitTestDistanceHorizontal(60.f)
	, PitTestDistanceVertical(600.f)
	, MinDistanceFromKillZ(200.f)
	, MinimumCrossTeamDistance(4000.f)
	, ExtraSpawnPadding(20)
	, MaxExpansionTiers(30)
{
}

FNCHybridSpawnBuildStats::FNCHybridSpawnBuildStats()
	: SeedCount(0)
	, GeneratedCount(0)
	, RejectedRadius(0)
	, RejectedFloor(0)
	, RejectedSlope(0)
	, RejectedClearance(0)
	, RejectedSpacing(0)
	, RejectedKillZ(0)
	, RejectedPit(0)
	, RejectedPainVolume(0)
{
}

FNCHybridSpawnResult::FNCHybridSpawnResult()
	: Team0Anchor(FVector::ZeroVector)
	, Team1Anchor(FVector::ZeroVector)
	, Team0AnchorName(NAME_None)
	, Team1AnchorName(NAME_None)
	, AnchorDistance2D(0.f)
	, TeamRadius(0.f)
	, bAnchorsValid(false)
	, bTeam0Complete(false)
	, bTeam1Complete(false)
{
}

namespace NCHybridSpawn
{
	struct FAnchorPair
	{
		APlayerStart* Team0;
		APlayerStart* Team1;
		float Distance2D;
	};

	static void ShuffleTier(TArray<FTransform>& Tier)
	{
		for (int32 Index = Tier.Num() - 1; Index > 0; --Index)
		{
			Tier.Swap(Index, FMath::RandRange(0, Index));
		}
	}

	static bool PickAnchorPair(
		const TArray<APlayerStart*>& Team0Candidates,
		const TArray<APlayerStart*>& Team1Candidates,
		int32 LargestTeam,
		const FNCHybridSpawnSettings& Settings,
		FAnchorPair& OutPair)
	{
		TArray<FAnchorPair> Pairs;
		float BestDistance = -1.f;

		for (APlayerStart* Team0Start : Team0Candidates)
		{
			if (!Team0Start) continue;
			for (APlayerStart* Team1Start : Team1Candidates)
			{
				if (!Team1Start || Team1Start == Team0Start) continue;

				FAnchorPair Pair;
				Pair.Team0 = Team0Start;
				Pair.Team1 = Team1Start;
				Pair.Distance2D = (Team0Start->GetActorLocation() - Team1Start->GetActorLocation()).Size2D();
				Pairs.Add(Pair);
				BestDistance = FMath::Max(BestDistance, Pair.Distance2D);
			}
		}

		if (Pairs.Num() == 0 || BestDistance < Settings.MinimumCrossTeamDistance)
		{
			return false;
		}

		// Randomly choose among the well-separated pairs from the quality-ranked
		// layout. This keeps the recovered generator unpredictable without using
		// Absolute's learnable 137-degree orbit.
		const float ExpansionStep = Settings.PlayerMinRadius * Settings.GeneratedSpacingMultiplier;
		const float DesiredRoom = (LargestTeam > 1) ? (2.f * ExpansionStep) : 0.f;
		const float QualityThreshold = FMath::Min(
			BestDistance,
			FMath::Max(BestDistance * 0.85f, Settings.MinimumCrossTeamDistance + DesiredRoom));

		TArray<int32> QualifiedIndices;
		for (int32 Index = 0; Index < Pairs.Num(); ++Index)
		{
			if (Pairs[Index].Distance2D >= QualityThreshold)
			{
				QualifiedIndices.Add(Index);
			}
		}

		if (QualifiedIndices.Num() == 0)
		{
			return false;
		}

		OutPair = Pairs[QualifiedIndices[FMath::RandRange(0, QualifiedIndices.Num() - 1)]];
		return true;
	}

	static bool IsFarEnoughFromAccepted(
		const FVector& Location,
		const TArray<FTransform>& Accepted,
		float MinimumDistance2D)
	{
		for (const FTransform& Transform : Accepted)
		{
			if ((Transform.GetLocation() - Location).Size2D() < MinimumDistance2D)
			{
				return false;
			}
		}
		return true;
	}

	static bool IsInsidePainVolume(UWorld* World, const FVector& Location, float Radius)
	{
		for (TActorIterator<APainCausingVolume> It(World); It; ++It)
		{
			APainCausingVolume* Volume = *It;
			if (Volume && !Volume->IsPendingKill() && Volume->EncompassesPoint(Location, Radius))
			{
				return true;
			}
		}
		return false;
	}

	static bool HasPitSupport(
		UWorld* World,
		const FVector& Location,
		const FNCHybridSpawnSettings& Settings,
		const FCollisionObjectQueryParams& StaticObjects,
		const FCollisionQueryParams& QueryParams)
	{
		const FVector2D Diagonals[] =
		{
			FVector2D( 0.7071067f,  0.7071067f),
			FVector2D( 0.7071067f, -0.7071067f),
			FVector2D(-0.7071067f,  0.7071067f),
			FVector2D(-0.7071067f, -0.7071067f)
		};

		for (const FVector2D& Diagonal : Diagonals)
		{
			const FVector End = Location
				+ FVector(Diagonal.X * Settings.PitTestDistanceHorizontal,
					Diagonal.Y * Settings.PitTestDistanceHorizontal,
					-Settings.PitTestDistanceVertical);
			FHitResult Hit;
			if (!World->LineTraceSingleByObjectType(Hit, Location, End, StaticObjects, QueryParams))
			{
				return false;
			}
		}
		return true;
	}

	static bool IsFacingSafe(
		UWorld* World,
		const FVector& Location,
		float Yaw,
		const FNCHybridSpawnSettings& Settings,
		const FCollisionObjectQueryParams& StaticObjects,
		const FCollisionQueryParams& QueryParams)
	{
		const FVector Forward = FRotator(0.f, Yaw, 0.f).Vector();
		const FVector Ahead = Location + Forward * Settings.PitTestDistanceHorizontal;

		FHitResult WallHit;
		const FCollisionShape ShoulderShape = FCollisionShape::MakeSphere(Settings.TraceRadius);
		if (World->SweepSingleByObjectType(
			WallHit, Location, Ahead, FQuat::Identity, StaticObjects, ShoulderShape, QueryParams))
		{
			return false;
		}

		FHitResult FloorAhead;
		const FVector FloorTraceStart = Ahead + FVector(0.f, 0.f, Settings.TraceStartOffsetZ);
		const FVector FloorTraceEnd = Ahead - FVector(0.f, 0.f, Settings.PitTestDistanceVertical);
		return World->LineTraceSingleByObjectType(
			FloorAhead, FloorTraceStart, FloorTraceEnd, StaticObjects, QueryParams)
			&& FloorAhead.ImpactNormal.Z > 0.7058f;
	}

	static float FindSafeYaw(
		UWorld* World,
		const FVector& Location,
		float PreferredYaw,
		const FNCHybridSpawnSettings& Settings,
		const FCollisionObjectQueryParams& StaticObjects,
		const FCollisionQueryParams& QueryParams)
	{
		const float YawOffsets[] = { 0.f, 45.f, -45.f, 90.f, -90.f, 135.f, -135.f, 180.f };
		for (float Offset : YawOffsets)
		{
			const float TestYaw = FRotator::NormalizeAxis(PreferredYaw + Offset);
			if (IsFacingSafe(World, Location, TestYaw, Settings, StaticObjects, QueryParams))
			{
				return TestYaw;
			}
		}
		return FRotator::NormalizeAxis(PreferredYaw);
	}

	static bool ProjectAndValidateCandidate(
		UWorld* World,
		const FVector& CandidateLocation,
		float PreferredYaw,
		const FVector& Anchor,
		const FVector& TowardEnemyDirection,
		float TeamRadius,
		float MaxForwardExtent,
		const TArray<FTransform>& Accepted,
		const FNCHybridSpawnSettings& Settings,
		FNCHybridSpawnBuildStats& Stats,
		FTransform& OutTransform)
	{
		if ((CandidateLocation - Anchor).Size2D() > TeamRadius)
		{
			++Stats.RejectedRadius;
			return false;
		}

		// Keep both queues behind symmetric planes around the map midpoint.
		// This preserves the mode's hard cross-team distance while still allowing
		// expansion sideways and away from the enemy on tighter maps.
		if (FVector::DotProduct(CandidateLocation - Anchor, TowardEnemyDirection) > MaxForwardExtent)
		{
			++Stats.RejectedRadius;
			return false;
		}

		const FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(TEXT("NCHybridSpawn"), false);
		QueryParams.bFindInitialOverlaps = true;

		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(
			Settings.TraceRadius, Settings.TraceHalfHeight);
		const FVector TraceStart = CandidateLocation + FVector(0.f, 0.f, Settings.TraceStartOffsetZ);
		const FVector TraceEnd = CandidateLocation + FVector(0.f, 0.f, Settings.TraceEndOffsetZ);

		FHitResult FloorHit;
		if (!World->SweepSingleByObjectType(
			FloorHit, TraceStart, TraceEnd, FQuat::Identity, StaticObjects, Capsule, QueryParams))
		{
			++Stats.RejectedFloor;
			return false;
		}

		if (FloorHit.ImpactNormal.Z <= 0.7058f)
		{
			++Stats.RejectedSlope;
			return false;
		}

		// Sweep hit location is the capsule center at contact. Lift it very slightly
		// so the subsequent occupancy test does not count the supporting floor.
		const FVector ProjectedLocation = FloorHit.Location + FVector(0.f, 0.f, 2.f);
		if ((ProjectedLocation - Anchor).Size2D() > TeamRadius)
		{
			++Stats.RejectedRadius;
			return false;
		}
		if (FVector::DotProduct(ProjectedLocation - Anchor, TowardEnemyDirection) > MaxForwardExtent)
		{
			++Stats.RejectedRadius;
			return false;
		}

		const AWorldSettings* WorldSettings = World->GetWorldSettings();
		if (WorldSettings && ProjectedLocation.Z < WorldSettings->KillZ + Settings.MinDistanceFromKillZ)
		{
			++Stats.RejectedKillZ;
			return false;
		}

		const float MinimumSpacing = Settings.PlayerMinRadius - 2.f * Settings.TraceRadius;
		if (!IsFarEnoughFromAccepted(ProjectedLocation, Accepted, MinimumSpacing))
		{
			++Stats.RejectedSpacing;
			return false;
		}

		if (World->OverlapAnyTestByObjectType(
			ProjectedLocation, FQuat::Identity, StaticObjects, Capsule, QueryParams))
		{
			++Stats.RejectedClearance;
			return false;
		}

		if (!HasPitSupport(World, ProjectedLocation, Settings, StaticObjects, QueryParams))
		{
			++Stats.RejectedPit;
			return false;
		}

		if (IsInsidePainVolume(World, ProjectedLocation, Settings.TraceRadius))
		{
			++Stats.RejectedPainVolume;
			return false;
		}

		const float SafeYaw = FindSafeYaw(
			World, ProjectedLocation, PreferredYaw, Settings, StaticObjects, QueryParams);
		OutTransform = FTransform(FRotator(0.f, SafeYaw, 0.f), ProjectedLocation);
		return true;
	}

	static void BuildTeamQueue(
		UWorld* World,
		const TArray<APlayerStart*>& AllStarts,
		const FVector& Anchor,
		const FVector& OpposingAnchor,
		int32 PlayerCount,
		float TeamRadius,
		const FNCHybridSpawnSettings& Settings,
		TArray<FTransform>& OutQueue,
		FNCHybridSpawnBuildStats& OutStats)
	{
		OutQueue.Empty();
		OutStats = FNCHybridSpawnBuildStats();
		if (PlayerCount <= 0 || !World || AllStarts.Num() == 0 || TeamRadius <= 0.f)
		{
			return;
		}

		FVector TowardEnemyDirection = OpposingAnchor - Anchor;
		TowardEnemyDirection.Z = 0.f;
		const float AnchorDistance2D = TowardEnemyDirection.Size();
		TowardEnemyDirection = TowardEnemyDirection.GetSafeNormal();
		const float MaxForwardExtent = FMath::Max(0.f, (AnchorDistance2D - Settings.MinimumCrossTeamDistance) * 0.5f);

		const int32 GoalCount = PlayerCount + FMath::Max(0, Settings.ExtraSpawnPadding);
		const float InitialRadius = FMath::Min(TeamRadius, Settings.MaxInitialSeedRadius);

		TArray<APlayerStart*> OrderedStarts;
		for (APlayerStart* Start : AllStarts)
		{
			if (Start && !Start->IsPendingKill()) OrderedStarts.Add(Start);
		}
		OrderedStarts.Sort([&Anchor](const APlayerStart& A, const APlayerStart& B)
		{
			return (A.GetActorLocation() - Anchor).SizeSquared2D()
				< (B.GetActorLocation() - Anchor).SizeSquared2D();
		});

		TArray<FTransform> Accepted;
		TArray<FTransform> PreviousTier;
		for (APlayerStart* Start : OrderedStarts)
		{
			if ((Start->GetActorLocation() - Anchor).Size2D() > InitialRadius) break;

			FTransform SeedTransform;
			if (ProjectAndValidateCandidate(
				World, Start->GetActorLocation(), Start->GetActorRotation().Yaw,
				Anchor, TowardEnemyDirection, TeamRadius, MaxForwardExtent,
				Accepted, Settings, OutStats, SeedTransform))
			{
				Accepted.Add(SeedTransform);
				PreviousTier.Add(SeedTransform);
				++OutStats.SeedCount;
			}
		}

		if (PreviousTier.Num() == 0)
		{
			// The selected anchor is an authored PlayerStart, but malformed maps can
			// still fail geometric validation. Leave the queue empty so the mode's
			// established PlayerStart fallback owns the spawn.
			return;
		}

		TArray<FTransform> ShuffledSeeds = PreviousTier;
		ShuffleTier(ShuffledSeeds);
		OutQueue.Append(ShuffledSeeds);

		const float Step = Settings.PlayerMinRadius * Settings.GeneratedSpacingMultiplier;
		const float HexYawOffsets[] = { 0.f, 60.f, -60.f, 180.f, 120.f, -120.f };

		for (int32 TierIndex = 0;
			TierIndex < Settings.MaxExpansionTiers && Accepted.Num() < GoalCount && PreviousTier.Num() > 0;
			++TierIndex)
		{
			TArray<FTransform> NewTier;
			for (const FTransform& Source : PreviousTier)
			{
				const float SourceYaw = Source.Rotator().Yaw;
				for (float YawOffset : HexYawOffsets)
				{
					const FVector Direction = FRotator(0.f, SourceYaw + YawOffset, 0.f).Vector();
					const FVector Candidate = Source.GetLocation() + Direction * Step;
					FTransform GeneratedTransform;
					if (ProjectAndValidateCandidate(
						World, Candidate, SourceYaw, Anchor, TowardEnemyDirection,
						TeamRadius, MaxForwardExtent, Accepted, Settings, OutStats,
						GeneratedTransform))
					{
						Accepted.Add(GeneratedTransform);
						NewTier.Add(GeneratedTransform);
						++OutStats.GeneratedCount;
						if (Accepted.Num() >= GoalCount) break;
					}
				}
				if (Accepted.Num() >= GoalCount) break;
			}

			if (NewTier.Num() == 0) break;
			ShuffleTier(NewTier);
			OutQueue.Append(NewTier);
			PreviousTier = NewTier;
		}
	}
}

bool FNCHybridSpawnGenerator::Generate(
	UWorld* World,
	const TArray<APlayerStart*>& AllStarts,
	const TArray<APlayerStart*>& Team0AnchorCandidates,
	const TArray<APlayerStart*>& Team1AnchorCandidates,
	int32 Team0PlayerCount,
	int32 Team1PlayerCount,
	const FNCHybridSpawnSettings& Settings,
	FNCHybridSpawnResult& OutResult)
{
	OutResult = FNCHybridSpawnResult();
	if (!World || AllStarts.Num() < 2
		|| Team0AnchorCandidates.Num() == 0 || Team1AnchorCandidates.Num() == 0)
	{
		return false;
	}

	NCHybridSpawn::FAnchorPair Pair;
	if (!NCHybridSpawn::PickAnchorPair(
		Team0AnchorCandidates, Team1AnchorCandidates,
		FMath::Max(Team0PlayerCount, Team1PlayerCount), Settings, Pair))
	{
		return false;
	}

	OutResult.bAnchorsValid = true;
	OutResult.Team0Anchor = Pair.Team0->GetActorLocation();
	OutResult.Team1Anchor = Pair.Team1->GetActorLocation();
	OutResult.Team0AnchorName = Pair.Team0->GetFName();
	OutResult.Team1AnchorName = Pair.Team1->GetFName();
	OutResult.AnchorDistance2D = Pair.Distance2D;

	// Preserve Absolute's 49% radial region. BuildTeamQueue adds a symmetric
	// midpoint plane constraint so opposing transforms retain the mode's hard
	// enemy-distance floor without over-constraining sideways/away expansion.
	const float AbsoluteRadius = Pair.Distance2D * 0.49f;
	OutResult.TeamRadius = FMath::Min(Settings.MaxTeamSpawnRadius, AbsoluteRadius);

	NCHybridSpawn::BuildTeamQueue(
		World, AllStarts, OutResult.Team0Anchor, OutResult.Team1Anchor,
		Team0PlayerCount, OutResult.TeamRadius, Settings,
		OutResult.Team0Queue, OutResult.Team0Stats);
	NCHybridSpawn::BuildTeamQueue(
		World, AllStarts, OutResult.Team1Anchor, OutResult.Team0Anchor,
		Team1PlayerCount, OutResult.TeamRadius, Settings,
		OutResult.Team1Queue, OutResult.Team1Stats);

	OutResult.bTeam0Complete = (Team0PlayerCount <= 0 || OutResult.Team0Queue.Num() >= Team0PlayerCount);
	OutResult.bTeam1Complete = (Team1PlayerCount <= 0 || OutResult.Team1Queue.Num() >= Team1PlayerCount);
	return true;
}

APlayerStart* FNCHybridSpawnGenerator::FindNearestPlayerStart(
	const TArray<APlayerStart*>& Starts,
	const FVector& Location)
{
	APlayerStart* BestStart = nullptr;
	float BestDistanceSq = FLT_MAX;
	for (APlayerStart* Start : Starts)
	{
		if (!Start || Start->IsPendingKill()) continue;
		const float DistanceSq = (Start->GetActorLocation() - Location).SizeSquared();
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			BestStart = Start;
		}
	}
	return BestStart;
}
