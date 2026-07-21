#include "NCHybridSpawnGenerator.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR
static void DrawHybridSpawnPIEPreviewCommand(const TArray<FString>& Args, UWorld* World);

static FAutoConsoleCommandWithWorldAndArgs CmdNCHybridSpawnPIEPreview(
	TEXT("nc.HybridSpawnPIEPreview"),
	TEXT("Draw the map-wide NetcodePlus hybrid spawn potential field in the local PIE world.\n")
	TEXT(" Usage: nc.HybridSpawnPIEPreview [1|page N|0]\n")
	TEXT(" 1 (or no argument): capped whole-map overview\n")
	TEXT(" page N: draw 500 exact candidates; 0: clear persistent debug lines"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DrawHybridSpawnPIEPreviewCommand),
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
	, RejectedPath(0)
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

	static int32 RandRange(
		int32 MinValue,
		int32 MaxValue,
		FRandomStream* RandomStream)
	{
		return RandomStream
			? RandomStream->RandRange(MinValue, MaxValue)
			: FMath::RandRange(MinValue, MaxValue);
	}

	static void ShuffleTier(TArray<FTransform>& Tier, FRandomStream* RandomStream)
	{
		for (int32 Index = Tier.Num() - 1; Index > 0; --Index)
		{
			Tier.Swap(Index, RandRange(0, Index, RandomStream));
		}
	}

	static bool PickAnchorPair(
		const TArray<APlayerStart*>& Team0Candidates,
		const TArray<APlayerStart*>& Team1Candidates,
		int32 LargestTeam,
		const FNCHybridSpawnSettings& Settings,
		FAnchorPair& OutPair,
		FRandomStream* RandomStream)
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

		OutPair = Pairs[QualifiedIndices[
			RandRange(0, QualifiedIndices.Num() - 1, RandomStream)]];
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

	// All generator geometry queries run on the PAWN channel, not an
	// ObjectType(WorldStatic) mask. A channel query resolves what actually BLOCKS
	// a pawn — doors, lifts, movers, physics props included — the same semantics
	// the engine's own spawn-encroachment test uses. The old WorldStatic-only mask
	// was blind to BlockAllDynamic/InvisibleWallDynamic geometry: the clearance
	// capsule passed inside closed movers ("spawned half in a wall") and the floor
	// sweep tunnelled through lift platforms to the static shaft below ("spawned
	// under the map"). The original Absolute BP traced [Pawn, WorldStatic] objects;
	// the channel form is a strict superset of that. Pawn-typed bodies are ignored
	// outright: both modes build their queues after destroying all pawns, and a
	// straggler must not poison a whole field.
	static FCollisionResponseParams PawnBlockingResponse()
	{
		FCollisionResponseParams Response(ECR_Block);
		Response.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return Response;
	}

	static bool HasPitSupport(
		UWorld* World,
		const FVector& Location,
		const FNCHybridSpawnSettings& Settings,
		const FCollisionResponseParams& PawnResponse,
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
			if (!World->LineTraceSingleByChannel(Hit, Location, End, ECC_Pawn, QueryParams, PawnResponse))
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
		const FCollisionResponseParams& PawnResponse,
		const FCollisionQueryParams& QueryParams)
	{
		const FVector Forward = FRotator(0.f, Yaw, 0.f).Vector();
		const FVector Ahead = Location + Forward * Settings.PitTestDistanceHorizontal;

		FHitResult WallHit;
		const FCollisionShape ShoulderShape = FCollisionShape::MakeSphere(Settings.TraceRadius);
		if (World->SweepSingleByChannel(
			WallHit, Location, Ahead, FQuat::Identity, ECC_Pawn, ShoulderShape, QueryParams, PawnResponse))
		{
			return false;
		}

		FHitResult FloorAhead;
		const FVector FloorTraceStart = Ahead + FVector(0.f, 0.f, Settings.TraceStartOffsetZ);
		const FVector FloorTraceEnd = Ahead - FVector(0.f, 0.f, Settings.PitTestDistanceVertical);
		return World->LineTraceSingleByChannel(
			FloorAhead, FloorTraceStart, FloorTraceEnd, ECC_Pawn, QueryParams, PawnResponse)
			&& FloorAhead.ImpactNormal.Z > 0.7058f;
	}

	static float FindSafeYaw(
		UWorld* World,
		const FVector& Location,
		float PreferredYaw,
		const FNCHybridSpawnSettings& Settings,
		const FCollisionResponseParams& PawnResponse,
		const FCollisionQueryParams& QueryParams)
	{
		const float YawOffsets[] = { 0.f, 45.f, -45.f, 90.f, -90.f, 135.f, -135.f, 180.f };
		for (float Offset : YawOffsets)
		{
			const float TestYaw = FRotator::NormalizeAxis(PreferredYaw + Offset);
			if (IsFacingSafe(World, Location, TestYaw, Settings, PawnResponse, QueryParams))
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
		float MinimumAcceptedSpacing,
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

		const FCollisionResponseParams PawnResponse = PawnBlockingResponse();
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

		if (!IsFarEnoughFromAccepted(Location, Accepted, MinimumAcceptedSpacing))
		{
			++Stats.RejectedSpacing;
			return false;
		}

		// Blocking variant, NOT OverlapAnyTest: overlap-only volumes (triggers,
		// pickup spheres) must not reject a perfectly good spawn spot.
		if (World->OverlapBlockingTestByChannel(
			Location, FQuat::Identity, ECC_Pawn, Capsule, QueryParams, PawnResponse))
		{
			++Stats.RejectedClearance;
			return false;
		}

		if (!HasPitSupport(World, Location, Settings, PawnResponse, QueryParams))
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
			? FindSafeYaw(World, Location, PreferredYaw, Settings, PawnResponse, QueryParams)
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
		float MinimumAcceptedSpacing,
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
		const FCollisionResponseParams PawnResponse = PawnBlockingResponse();
		FCollisionQueryParams QueryParams(TEXT("NCHybridAuthoredSeed"), false);

		FVector SeedLocation = AuthoredLocation + FVector(0.f, 0.f, 10.f);
		FHitResult LocalFloorHit;
		if (World->LineTraceSingleByChannel(
			LocalFloorHit, LocalTraceStart, LocalTraceEnd, ECC_Pawn, QueryParams, PawnResponse))
		{
			// Absolute adds two units to the capsule-center correction and then
			// another ten to the final authored transform. Retain that clearance
			// while using the actual 108uu standing pawn capsule.
			SeedLocation.Z = LocalFloorHit.Location.Z + Settings.TraceHalfHeight + 12.f;
		}

		return ValidateAndBuildTransform(
			World, SeedLocation, Start->GetActorRotation().Yaw, false,
			Anchor, TowardEnemyDirection, TeamRadius, MaxForwardExtent,
			Accepted, MinimumAcceptedSpacing, Settings, Stats, OutTransform);
	}

	static bool ProjectAndValidateCandidate(
		UWorld* World,
		const FVector& SourceLocation,
		const FVector& CandidateLocation,
		float PreferredYaw,
		const FVector& Anchor,
		const FVector& TowardEnemyDirection,
		float TeamRadius,
		float MaxForwardExtent,
		const TArray<FTransform>& Accepted,
		float MinimumAcceptedSpacing,
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

		const FCollisionResponseParams PawnResponse = PawnBlockingResponse();
		FCollisionQueryParams QueryParams(TEXT("NCHybridSpawn"), false);
		QueryParams.bFindInitialOverlaps = true;

		const FCollisionShape Capsule = FCollisionShape::MakeCapsule(
			Settings.TraceRadius, Settings.TraceHalfHeight);

		// Connectivity gate, restored from the original Absolute BP: its
		// LineTraceForBlocking(source+10Z → candidate+90Z, offsets relative to the
		// capsule-center-seated transforms) ran BEFORE any floor projection —
		// dropped in the port, which is how expansion tunnelled through walls onto
		// wall cavities and out-of-map shells within the step-drop clamp. Kept as
		// the BP's exact LINE geometry, deliberately NOT a capsule: the chest-high
		// rising line (~floor+120 → +200) clears stair risers and walkable ramps
		// to ~33° uphill over a 303uu step while still hitting any wall, whereas a
		// full-height capsule grazes the source floor on inclines ≳2° and
		// collapses the field to flat-and-downhill. Pawn FIT at the destination is
		// already proven by the clearance overlap below; this gate only proves the
		// step doesn't cross a pawn-blocker.
		FHitResult PathHit;
		if (World->LineTraceSingleByChannel(
			PathHit,
			SourceLocation + FVector(0.f, 0.f, 10.f),
			CandidateLocation + FVector(0.f, 0.f, 90.f),
			ECC_Pawn, QueryParams, PawnResponse))
		{
			++Stats.RejectedPath;
			return false;
		}

		const FVector TraceStart = CandidateLocation + FVector(0.f, 0.f, Settings.TraceStartOffsetZ);
		const FVector TraceEnd = CandidateLocation + FVector(0.f, 0.f, Settings.TraceEndOffsetZ);

		FHitResult FloorHit;
		if (!World->SweepSingleByChannel(
			FloorHit, TraceStart, TraceEnd, FQuat::Identity, ECC_Pawn, Capsule, QueryParams, PawnResponse))
		{
			++Stats.RejectedFloor;
			return false;
		}

		// Absolute's sweep-time gate: a floor hit that begins embedded (Time ≈ 0 /
		// start-penetrating) is a wall or ceiling the sweep started inside, not a
		// floor — its depenetration normal can masquerade as walkable.
		if (FloorHit.bStartPenetrating || FloorHit.Time <= 0.01f)
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
			Accepted, MinimumAcceptedSpacing, Settings, Stats, OutTransform);
	}

	static void BuildTeamQueue(
		UWorld* World,
		const TArray<APlayerStart*>& AllStarts,
		const FVector& Anchor,
		const FVector& OpposingAnchor,
		int32 PlayerCount,
		float TeamRadius,
		float MinimumAcceptedSpacing,
		const FNCHybridSpawnSettings& Settings,
		TArray<FTransform>& OutQueue,
		FNCHybridSpawnBuildStats& OutStats,
		FRandomStream* RandomStream)
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
				Accepted, MinimumAcceptedSpacing, Settings, OutStats, SeedTransform))
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
		ShuffleTier(ShuffledSeeds, RandomStream);
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
						World, Source.GetLocation(), Candidate, SourceYaw, Anchor, TowardEnemyDirection,
						TeamRadius, MaxForwardExtent, Accepted, MinimumAcceptedSpacing,
						Settings, OutStats,
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
			ShuffleTier(NewTier, RandomStream);
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
		FMath::Max(Team0PlayerCount, Team1PlayerCount), Settings, Pair, nullptr))
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
		Team0PlayerCount, OutResult.TeamRadius,
		Settings.PlayerMinRadius - 2.f * Settings.TraceRadius, Settings,
		OutResult.Team0Queue, OutResult.Team0Stats, nullptr);
	NCHybridSpawn::BuildTeamQueue(
		World, AllStarts, OutResult.Team1Anchor, OutResult.Team0Anchor,
		Team1PlayerCount, OutResult.TeamRadius,
		Settings.PlayerMinRadius - 2.f * Settings.TraceRadius, Settings,
		OutResult.Team1Queue, OutResult.Team1Stats, nullptr);

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

#if WITH_EDITOR
static void DrawHybridSpawnPIEPreviewCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World || World->WorldType != EWorldType::PIE)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HybridSpawnPIE] Run nc.HybridSpawnPIEPreview from a PIE viewport"));
		return;
	}

	if (Args.Num() > 0
		&& (Args[0].Equals(TEXT("0")) || Args[0].Equals(TEXT("clear"), ESearchCase::IgnoreCase)))
	{
		FlushPersistentDebugLines(World);
		UE_LOG(LogTemp, Warning, TEXT("[HybridSpawnPIE] Cleared persistent debug lines"));
		return;
	}

	// Never stack one preview on another. The original implementation could add
	// tens of thousands of persistent capsules and arrows to the line batcher;
	// clearing up front bounds renderer memory even when the command is repeated.
	FlushPersistentDebugLines(World);

	const bool bExactPage = Args.Num() > 0
		&& Args[0].Equals(TEXT("page"), ESearchCase::IgnoreCase);
	const int32 RequestedPage = bExactPage && Args.Num() > 1
		? FMath::Max(1, FCString::Atoi(*Args[1]))
		: 1;

	TArray<APlayerStart*> AllStarts;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		APlayerStart* Start = *It;
		if (Start && !Start->IsPendingKill())
		{
			AllStarts.Add(Start);
		}
	}
	if (AllStarts.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[HybridSpawnPIE] This PIE world has no PlayerStarts"));
		return;
	}

	// This is deliberately independent of a live round. Build a conservative
	// map-wide potential field around every authored PlayerStart, with the live
	// maximum radius and no opposing-team midpoint plane. A real round can only
	// remove points from this superset by choosing a smaller pair radius or by
	// applying its cross-team plane.
	FNCHybridSpawnSettings Settings;
	Settings.ExtraSpawnPadding = 10000;
	const float PreviewAcceptedSpacing = bExactPage
		? 10.f
		: Settings.PlayerMinRadius - 2.f * Settings.TraceRadius;
	TArray<FTransform> MapField;
	TSet<FIntVector> SeenLocations;
	int32 TotalDropRejects = 0;

	for (int32 AnchorIndex = 0; AnchorIndex < AllStarts.Num(); ++AnchorIndex)
	{
		APlayerStart* Anchor = AllStarts[AnchorIndex];
		TArray<FTransform> AnchorField;
		FNCHybridSpawnBuildStats AnchorStats;
		FRandomStream PreviewRandom(0x4E435000 + AnchorIndex);

		// Equal anchor/opposing locations make TowardEnemyDirection zero, which
		// disables the round-specific forward plane. Exact pages use only 10uu
		// duplicate suppression so random live acceptance order cannot hide a
		// geometrically reachable location. The default overview uses live 220uu
		// spacing to keep its collision-query and allocation cost bounded.
		NCHybridSpawn::BuildTeamQueue(
			World, AllStarts,
			Anchor->GetActorLocation(), Anchor->GetActorLocation(),
			1, Settings.MaxTeamSpawnRadius, PreviewAcceptedSpacing, Settings,
			AnchorField, AnchorStats, &PreviewRandom);
		TotalDropRejects += AnchorStats.RejectedDrop;

		for (const FTransform& Transform : AnchorField)
		{
			const FVector Location = Transform.GetLocation();
			const FIntVector Key(
				FMath::RoundToInt(Location.X / 10.f),
				FMath::RoundToInt(Location.Y / 10.f),
				FMath::RoundToInt(Location.Z / 10.f));
			if (!SeenLocations.Contains(Key))
			{
				SeenLocations.Add(Key);
				MapField.Add(Transform);
			}
		}
	}

	const int32 ExactPageSize = 500;
	const int32 ExactPageCount = FMath::Max(
		1, (MapField.Num() + ExactPageSize - 1) / ExactPageSize);
	TArray<int32> DrawIndices;

	if (bExactPage)
	{
		const int32 Page = FMath::Clamp(RequestedPage, 1, ExactPageCount);
		const int32 FirstIndex = (Page - 1) * ExactPageSize;
		const int32 EndIndex = FMath::Min(FirstIndex + ExactPageSize, MapField.Num());
		for (int32 Index = FirstIndex; Index < EndIndex; ++Index)
		{
			DrawIndices.Add(Index);
		}
	}
	else
	{
		// One representative per 250uu 3D cell gives a useful whole-map safety
		// picture without asking the renderer to retain every near-duplicate
		// transform. If a very large map still exceeds the hard limit, sample the
		// representatives evenly instead of truncating one end of the map.
		const float OverviewCellSize = 250.f;
		const int32 MaxOverviewPoints = 1000;
		TArray<int32> Representatives;
		TSet<FIntVector> SeenOverviewCells;
		for (int32 Index = 0; Index < MapField.Num(); ++Index)
		{
			const FVector Location = MapField[Index].GetLocation();
			const FIntVector Cell(
				FMath::FloorToInt(Location.X / OverviewCellSize),
				FMath::FloorToInt(Location.Y / OverviewCellSize),
				FMath::FloorToInt(Location.Z / OverviewCellSize));
			if (!SeenOverviewCells.Contains(Cell))
			{
				SeenOverviewCells.Add(Cell);
				Representatives.Add(Index);
			}
		}

		const int32 DrawCount = FMath::Min(MaxOverviewPoints, Representatives.Num());
		for (int32 DrawIndex = 0; DrawIndex < DrawCount; ++DrawIndex)
		{
			const int32 RepresentativeIndex = Representatives.Num() <= MaxOverviewPoints
				? DrawIndex
				: FMath::FloorToInt(
					(float)DrawIndex * (float)Representatives.Num() / (float)DrawCount);
			DrawIndices.Add(Representatives[RepresentativeIndex]);
		}
	}

	const bool bPersistent = true;
	const float LifeTime = -1.f;
	const uint8 DepthPriority = 0;
	const FColor PotentialColor(64, 255, 128);
	const FColor AuthoredColor(255, 190, 32);

	// Points are intentional: one capsule plus arrow expands to many persistent
	// line primitives. At 35k candidates that was enough to nearly hang PIE.
	for (int32 Index : DrawIndices)
	{
		DrawDebugPoint(
			World, MapField[Index].GetLocation(), 12.f, PotentialColor,
			bPersistent, LifeTime, DepthPriority);
	}

	for (APlayerStart* Start : AllStarts)
	{
		DrawDebugSphere(
			World, Start->GetActorLocation(), 70.f, 16, AuthoredColor,
			bPersistent, LifeTime, DepthPriority, 4.f);
	}

	if (bExactPage)
	{
		const int32 Page = FMath::Clamp(RequestedPage, 1, ExactPageCount);
		UE_LOG(LogTemp, Warning,
			TEXT("[HybridSpawnPIE] Drew exact page %d/%d: %d green points from %d exact transforms (%d authored PlayerStarts, radius %.0f, Z-drop rejects %d). Use page N or 0 to clear."),
			Page, ExactPageCount, DrawIndices.Num(), MapField.Num(), AllStarts.Num(),
			Settings.MaxTeamSpawnRadius, TotalDropRejects);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[HybridSpawnPIE] Drew capped whole-map overview: %d green points from %d live-spaced transforms (%d authored PlayerStarts, radius %.0f, Z-drop rejects %d). Use 'nc.HybridSpawnPIEPreview page 1' for paged exhaustive detail or 0 to clear."),
			DrawIndices.Num(), MapField.Num(), AllStarts.Num(),
			Settings.MaxTeamSpawnRadius, TotalDropRejects);
	}
}
#endif
