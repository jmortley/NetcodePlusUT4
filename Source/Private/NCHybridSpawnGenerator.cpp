#include "NCHybridSpawnGenerator.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR
static TAutoConsoleVariable<int32> CVarNCHybridSpawnPIEPreview(
	TEXT("nc.HybridSpawnPIEPreview"),
	0,
	TEXT("Draw the exhaustive accepted NetcodePlus hybrid spawn field in PIE.\n")
	TEXT(" 0: disabled\n")
	TEXT(" 1: draw the current round's full team fields"),
	ECVF_Cheat);
#endif

FNCHybridSpawnSettings::FNCHybridSpawnSettings()
	: MaxTeamSpawnRadius(3100.f)
	, MaxInitialSeedRadius(1300.f)
	, PlayerMinRadius(300.f)
	, GeneratedSpacingMultiplier(1.01f)
	, TraceStartOffsetZ(90.f)
	, TraceEndOffsetZ(-2000.f)
	// One hex step is 303uu and accepted floors are limited to roughly 45 degrees.
	// 400uu leaves margin for uneven terrain without allowing a probe to snap to
	// an unrelated floor one or two thousand units below the source tier.
	, MaxGeneratedStepDrop(400.f)
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
	, RejectedDrop(0)
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

	static bool ValidateAndBuildTransform(
		UWorld* World,
		const FVector& Location,
		float PreferredYaw,
		bool bFindSafeRotation,
		const FVector& Anchor,
		const FVector& TowardEnemyDirection,
		float TeamRadius,
		float MaxForwardExtent,
		const TArray<FTransform>& Accepted,
		const FNCHybridSpawnSettings& Settings,
		FNCHybridSpawnBuildStats& Stats,
		FTransform& OutTransform)
	{
		// Absolute uses a full 3D radius for its authored and generated fields.
		// Checking again after projection also rejects transforms that landed on a
		// vertically unrelated floor even though their XY stayed inside the field.
		if ((Location - Anchor).Size() > TeamRadius)
		{
			++Stats.RejectedRadius;
			return false;
		}
		if (FVector::DotProduct(Location - Anchor, TowardEnemyDirection) > MaxForwardExtent)
		{
			++Stats.RejectedRadius;
			return false;
		}

		const FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(TEXT("NCHybridSpawnFinal"), false);
		QueryParams.bFindInitialOverlaps = true;
		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(
			Settings.TraceRadius, Settings.TraceHalfHeight);

		const AWorldSettings* WorldSettings = World->GetWorldSettings();
		if (WorldSettings && Location.Z < WorldSettings->KillZ + Settings.MinDistanceFromKillZ)
		{
			++Stats.RejectedKillZ;
			return false;
		}

		const float MinimumSpacing = Settings.PlayerMinRadius - 2.f * Settings.TraceRadius;
		if (!IsFarEnoughFromAccepted(Location, Accepted, MinimumSpacing))
		{
			++Stats.RejectedSpacing;
			return false;
		}

		if (World->OverlapAnyTestByObjectType(
			Location, FQuat::Identity, StaticObjects, Capsule, QueryParams))
		{
			++Stats.RejectedClearance;
			return false;
		}

		if (!HasPitSupport(World, Location, Settings, StaticObjects, QueryParams))
		{
			++Stats.RejectedPit;
			return false;
		}

		if (IsInsidePainVolume(World, Location, Settings.TraceRadius))
		{
			++Stats.RejectedPainVolume;
			return false;
		}

		const float Yaw = bFindSafeRotation
			? FindSafeYaw(World, Location, PreferredYaw, Settings, StaticObjects, QueryParams)
			: FRotator::NormalizeAxis(PreferredYaw);
		OutTransform = FTransform(FRotator(0.f, Yaw, 0.f), Location);
		return true;
	}

	static bool BuildAuthoredSeed(
		UWorld* World,
		APlayerStart* Start,
		const FVector& Anchor,
		const FVector& TowardEnemyDirection,
		float TeamRadius,
		float MaxForwardExtent,
		const TArray<FTransform>& Accepted,
		const FNCHybridSpawnSettings& Settings,
		FNCHybridSpawnBuildStats& Stats,
		FTransform& OutTransform)
	{
		if (!Start)
		{
			return false;
		}

		// Match Absolute's authored-start treatment: trust the mapper's PlayerStart
		// and only correct a floor intersecting its local capsule. Never run an
		// authored seed through the generated candidate's 2000uu floor projection.
		const FVector AuthoredLocation = Start->GetActorLocation();
		const FVector LocalTraceStart = AuthoredLocation
			+ FVector(0.f, 0.f, Settings.TraceHalfHeight * 1.02f);
		const FVector LocalTraceEnd = AuthoredLocation
			- FVector(0.f, 0.f, Settings.TraceHalfHeight * 0.98f);
		const FCollisionObjectQueryParams StaticObjects(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(TEXT("NCHybridAuthoredSeed"), false);

		FVector SeedLocation = AuthoredLocation + FVector(0.f, 0.f, 10.f);
		FHitResult LocalFloorHit;
		if (World->LineTraceSingleByObjectType(
			LocalFloorHit, LocalTraceStart, LocalTraceEnd, StaticObjects, QueryParams))
		{
			// Absolute adds two units to the capsule-center correction and then
			// another ten to the final authored transform. Retain that clearance
			// while using the actual 108uu standing pawn capsule.
			SeedLocation.Z = LocalFloorHit.Location.Z + Settings.TraceHalfHeight + 12.f;
		}

		return ValidateAndBuildTransform(
			World, SeedLocation, Start->GetActorRotation().Yaw, false,
			Anchor, TowardEnemyDirection, TeamRadius, MaxForwardExtent,
			Accepted, Settings, Stats, OutTransform);
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
		if ((CandidateLocation - Anchor).Size() > TeamRadius)
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
		if (CandidateLocation.Z - ProjectedLocation.Z > Settings.MaxGeneratedStepDrop)
		{
			++Stats.RejectedDrop;
			return false;
		}

		return ValidateAndBuildTransform(
			World, ProjectedLocation, PreferredYaw, true,
			Anchor, TowardEnemyDirection, TeamRadius, MaxForwardExtent,
			Accepted, Settings, Stats, OutTransform);
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
			return (A.GetActorLocation() - Anchor).SizeSquared()
				< (B.GetActorLocation() - Anchor).SizeSquared();
		});

		TArray<FTransform> Accepted;
		TArray<FTransform> PreviousTier;
		for (APlayerStart* Start : OrderedStarts)
		{
			if ((Start->GetActorLocation() - Anchor).Size() > InitialRadius) break;

			FTransform SeedTransform;
			if (BuildAuthoredSeed(
				World, Start, Anchor, TowardEnemyDirection, TeamRadius, MaxForwardExtent,
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

void FNCHybridSpawnGenerator::DrawPIEPreview(
	UWorld* World,
	const TArray<APlayerStart*>& AllStarts,
	const FNCHybridSpawnSettings& Settings,
	const FNCHybridSpawnResult& RoundResult)
{
#if WITH_EDITOR
	if (!World || World->WorldType != EWorldType::PIE
		|| CVarNCHybridSpawnPIEPreview.GetValueOnGameThread() <= 0
		|| !RoundResult.bAnchorsValid)
	{
		return;
	}

	APlayerStart* Team0Anchor = nullptr;
	APlayerStart* Team1Anchor = nullptr;
	for (APlayerStart* Start : AllStarts)
	{
		if (!Start || Start->IsPendingKill()) continue;
		if (Start->GetFName() == RoundResult.Team0AnchorName) Team0Anchor = Start;
		if (Start->GetFName() == RoundResult.Team1AnchorName) Team1Anchor = Start;
	}

	if (!Team0Anchor || !Team1Anchor)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HybridSpawnPIE] Could not recover selected anchors %s/%s"),
			*RoundResult.Team0AnchorName.ToString(),
			*RoundResult.Team1AnchorName.ToString());
		return;
	}

	// The live queue stops at team size + 20. A deliberately unreachable goal
	// makes the preview continue until the field naturally exhausts or reaches
	// the same 30-tier safety cap as gameplay. With a 303uu grid inside a 3100uu
	// radius, 10,000 is safely above the geometrically possible count.
	FNCHybridSpawnSettings PreviewSettings = Settings;
	PreviewSettings.ExtraSpawnPadding = 10000;
	TArray<APlayerStart*> Team0Anchors;
	TArray<APlayerStart*> Team1Anchors;
	Team0Anchors.Add(Team0Anchor);
	Team1Anchors.Add(Team1Anchor);

	FNCHybridSpawnResult PreviewResult;
	if (!Generate(
		World, AllStarts, Team0Anchors, Team1Anchors,
		1, 1, PreviewSettings, PreviewResult))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HybridSpawnPIE] Full-field generation failed for anchors %s/%s"),
			*RoundResult.Team0AnchorName.ToString(),
			*RoundResult.Team1AnchorName.ToString());
		return;
	}

	// Preview lines intentionally persist until PIE stops or the built-in
	// FlushPersistentDebugLines command is issued.
	const bool bPersistent = true;
	const float LifeTime = -1.f;
	const uint8 DepthPriority = 0;
	const float CapsuleThickness = 1.5f;
	const float ArrowLength = 90.f;
	const float ArrowSize = 18.f;
	const FColor Team0Generated(255, 32, 32);
	const FColor Team0Seed(255, 160, 32);
	const FColor Team1Generated(32, 96, 255);
	const FColor Team1Seed(32, 240, 255);

	auto DrawTeam = [&](const TArray<FTransform>& Queue, int32 SeedCount,
		const FVector& AnchorLocation, const FColor& GeneratedColor,
		const FColor& SeedColor)
	{
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			const FTransform& Transform = Queue[Index];
			const FVector Location = Transform.GetLocation();
			const FColor Color = (Index < SeedCount) ? SeedColor : GeneratedColor;
			DrawDebugCapsule(
				World, Location, Settings.TraceHalfHeight, Settings.TraceRadius,
				FQuat::Identity, Color, bPersistent, LifeTime,
				DepthPriority, CapsuleThickness);

			const FVector FacingEnd = Location
				+ FRotator(0.f, Transform.Rotator().Yaw, 0.f).Vector() * ArrowLength;
			DrawDebugDirectionalArrow(
				World, Location, FacingEnd, ArrowSize, Color,
				bPersistent, LifeTime, DepthPriority, CapsuleThickness);

			if (FMath::Abs(Location.Z - AnchorLocation.Z) > 50.f)
			{
				const FVector AnchorZ(Location.X, Location.Y, AnchorLocation.Z);
				DrawDebugLine(
					World, Location, AnchorZ,
					FColor(Color.R / 2, Color.G / 2, Color.B / 2),
					bPersistent, LifeTime, DepthPriority, 0.75f);
			}
		}
	};

	DrawTeam(
		PreviewResult.Team0Queue, PreviewResult.Team0Stats.SeedCount,
		PreviewResult.Team0Anchor, Team0Generated, Team0Seed);
	DrawTeam(
		PreviewResult.Team1Queue, PreviewResult.Team1Stats.SeedCount,
		PreviewResult.Team1Anchor, Team1Generated, Team1Seed);

	DrawDebugSphere(
		World, PreviewResult.Team0Anchor, 70.f, 16, Team0Seed,
		bPersistent, LifeTime, DepthPriority, 4.f);
	DrawDebugSphere(
		World, PreviewResult.Team1Anchor, 70.f, 16, Team1Seed,
		bPersistent, LifeTime, DepthPriority, 4.f);
	DrawDebugLine(
		World, PreviewResult.Team0Anchor, PreviewResult.Team1Anchor,
		FColor::Yellow, bPersistent, LifeTime, DepthPriority, 2.f);

	UE_LOG(LogTemp, Warning,
		TEXT("[HybridSpawnPIE] Drew exhaustive fields: T0=%d (seeds=%d) T1=%d (seeds=%d), radius=%.0f, Z-drop rejects=%d/%d. Use FlushPersistentDebugLines to clear."),
		PreviewResult.Team0Queue.Num(), PreviewResult.Team0Stats.SeedCount,
		PreviewResult.Team1Queue.Num(), PreviewResult.Team1Stats.SeedCount,
		PreviewResult.TeamRadius,
		PreviewResult.Team0Stats.RejectedDrop,
		PreviewResult.Team1Stats.RejectedDrop);
#else
	(void)World;
	(void)AllStarts;
	(void)Settings;
	(void)RoundResult;
#endif
}
