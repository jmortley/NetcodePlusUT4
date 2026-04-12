#include "ShockDomGameMode.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTTeamInfo.h"
#include "UTTeamPlayerStart.h"
#include "UTPickup.h"
#include "UTPickupWeapon.h"
#include "UTPickupHealth.h"
#include "UTPickupAmmo.h"
#include "UTPickupInventory.h"
#include "UTGenericObjectivePoint.h"
#include "TeamArenaCharacter.h"
#include "UTDamageType.h"
#include "ShockDomControlPoint.h"
#include "ShockDomReplicator.h"
#include "ShockDomHUD.h"
#include "ShockDomMessage.h"
#include "UTPlusShockRifle.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"


// ============================================================================
// CONSTRUCTOR
// ============================================================================

AShockDomGameMode::AShockDomGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("UTGameMode", "ShockDOM", "Shock Domination");
	bTeamGame = true;
	HUDClass = AShockDomHUD::StaticClass();

	// Team defaults
	NumTeams = 2;
	bBalanceTeams = true;
	bUseTeamStarts = false;
	bAnnounceTeam = true;

	// Respawn: instant with short delay
	bForceRespawn = true;
	RespawnWaitTime = 3.0f;
	ForceRespawnTime = 3.0f;

	// Match rules
	TimeLimit = 10;    // 10 minutes
	GoalScore = 200;   // DOM score limit

	// Weapon: shock rifle only, clear character default inventory (enforcer)
	bClearPlayerInventory = true;
	DefaultInventory.Empty();
	DefaultInventory.Add(AUTPlusShockRifle::StaticClass());

	// DOM scoring
	ScorePerPointPerTick = 1;
	ScoringTickInterval = 1.0f;

	// Control points
	ControlPointCaptureRadius = 200.f;
	MaxControlPoints = 3;
	ControlPointClass = AShockDomControlPoint::StaticClass();

	// Spawn penalties (adapted from NCPlusCTF)
	ControlPointSpawnPenaltyRadius = 2500.f;
	ControlPointSpawnPenalty = 8.0f;
	EnemyBlockRange = 2500.f;
	EnemyBlockPenalty = 10.0f;
	EnemyLOSBlockRange = 3000.f;
	EnemyLOSPenalty = 8.0f;

	DomReplicator = nullptr;
}


// ============================================================================
// INITIALIZATION
// ============================================================================

void AShockDomGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Parse URL options
	int32 URLGoalScore = UGameplayStatics::GetIntOption(Options, TEXT("GoalScore"), -1);
	if (URLGoalScore >= 0)
	{
		GoalScore = URLGoalScore;
	}

	int32 URLMaxPoints = UGameplayStatics::GetIntOption(Options, TEXT("MaxPoints"), -1);
	if (URLMaxPoints > 0)
	{
		MaxControlPoints = URLMaxPoints;
	}
}


void AShockDomGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		SpawnControlPoints();
	}
}


void AShockDomGameMode::HandleMatchHasStarted()
{
	Super::HandleMatchHasStarted();

	// Spawn DOM replicator for scoreboard stats (captures, damage, score goal)
	if (HasAuthority() && !DomReplicator)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		DomReplicator = GetWorld()->SpawnActor<AShockDomReplicator>(SpawnParams);
		if (DomReplicator)
		{
			DomReplicator->DomScoreGoal = GoalScore;
		}
	}

	// Start scoring timer
	if (HasAuthority())
	{
		GetWorldTimerManager().SetTimer(
			ScoringTimerHandle, this, &AShockDomGameMode::TickDomScoring,
			ScoringTickInterval, true);
	}
}


// ============================================================================
// CONTROL POINT SPAWNING
// ============================================================================

void AShockDomGameMode::SpawnControlPoints()
{
	TArray<AUTGenericObjectivePoint*> ObjectivePoints;
	for (TActorIterator<AUTGenericObjectivePoint> It(GetWorld()); It; ++It)
	{
		ObjectivePoints.Add(*It);
	}

	if (ObjectivePoints.Num() == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("ShockDOM: No AUTGenericObjectivePoint found — falling back to PlayerStart locations."));
		SpawnControlPointsFromPlayerStarts();
		return;
	}

	int32 NumToSpawn = FMath::Min(ObjectivePoints.Num(), MaxControlPoints);
	for (int32 i = 0; i < NumToSpawn; i++)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AShockDomControlPoint* CP = GetWorld()->SpawnActor<AShockDomControlPoint>(
			ControlPointClass,
			ObjectivePoints[i]->GetActorLocation(),
			ObjectivePoints[i]->GetActorRotation(),
			SpawnParams);

		if (CP)
		{
			CP->PointIndex = i;
			CP->PointLabel = FString::Printf(TEXT("%c"), 'A' + i);
			CP->OwningTeamIndex = 255;
			CP->CaptureVolume->SetSphereRadius(ControlPointCaptureRadius);
			ControlPoints.Add(CP);
		}
	}

	// Control points are bAlwaysRelevant — clients find them via TActorIterator

	UE_LOG(LogGameMode, Log, TEXT("ShockDOM: Spawned %d control points from map objective markers."), ControlPoints.Num());
}


void AShockDomGameMode::SpawnControlPointsFromPlayerStarts()
{
	// Collect all player starts
	TArray<APlayerStart*> AllStarts;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		AllStarts.Add(*It);
	}

	if (AllStarts.Num() < MaxControlPoints)
	{
		UE_LOG(LogGameMode, Error, TEXT("ShockDOM: Not enough player starts to place control points."));
		return;
	}

	// Pick the most spread-out starts: greedy farthest-point algorithm
	TArray<int32> SelectedIndices;
	SelectedIndices.Add(0); // Start with first

	while (SelectedIndices.Num() < MaxControlPoints)
	{
		float BestMinDist = -1.f;
		int32 BestIdx = -1;

		for (int32 i = 0; i < AllStarts.Num(); i++)
		{
			if (SelectedIndices.Contains(i)) continue;

			float MinDistToSelected = MAX_FLT;
			for (int32 Sel : SelectedIndices)
			{
				float Dist = (AllStarts[i]->GetActorLocation() - AllStarts[Sel]->GetActorLocation()).Size();
				MinDistToSelected = FMath::Min(MinDistToSelected, Dist);
			}

			if (MinDistToSelected > BestMinDist)
			{
				BestMinDist = MinDistToSelected;
				BestIdx = i;
			}
		}

		if (BestIdx >= 0)
		{
			SelectedIndices.Add(BestIdx);
		}
		else
		{
			break;
		}
	}

	// Spawn at selected locations
	for (int32 i = 0; i < SelectedIndices.Num(); i++)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AShockDomControlPoint* CP = GetWorld()->SpawnActor<AShockDomControlPoint>(
			ControlPointClass,
			AllStarts[SelectedIndices[i]]->GetActorLocation(),
			FRotator::ZeroRotator,
			SpawnParams);

		if (CP)
		{
			CP->PointIndex = i;
			CP->PointLabel = FString::Printf(TEXT("%c"), 'A' + i);
			CP->OwningTeamIndex = 255;
			CP->CaptureVolume->SetSphereRadius(ControlPointCaptureRadius);
			ControlPoints.Add(CP);
		}
	}

	// Control points are bAlwaysRelevant — clients find them via TActorIterator

	UE_LOG(LogGameMode, Log, TEXT("ShockDOM: Spawned %d control points from player start locations (fallback)."), ControlPoints.Num());
}


// ============================================================================
// SCORING
// ============================================================================

void AShockDomGameMode::TickDomScoring()
{
	if (!HasAuthority() || !IsMatchInProgress()) return;

	for (AShockDomControlPoint* CP : ControlPoints)
	{
		if (!CP || CP->OwningTeamIndex >= (uint8)NumTeams) continue;

		if (Teams.IsValidIndex(CP->OwningTeamIndex) && Teams[CP->OwningTeamIndex])
		{
			Teams[CP->OwningTeamIndex]->Score += ScorePerPointPerTick;
		}
	}

	// Check win condition
	for (int32 i = 0; i < NumTeams; i++)
	{
		if (Teams.IsValidIndex(i) && Teams[i] && GoalScore > 0 && Teams[i]->Score >= GoalScore)
		{
			AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(i);
			EndGame(BestPlayer, FName(TEXT("scorelimit")));
			return;
		}
	}
}


void AShockDomGameMode::OnPointCaptured(AShockDomControlPoint* Point, uint8 NewTeam, AUTCharacter* Capturer)
{
	if (!Point || !Capturer) return;

	// Track capture count (replicated via DOM replicator)
	AUTPlayerState* PS = Cast<AUTPlayerState>(Capturer->PlayerState);
	if (PS)
	{
		FString UniqueId = PS->UniqueId.ToString();
		if (DomReplicator)
		{
			DomReplicator->IncrementCaptures(UniqueId);
		}

		// Broadcast capture message
		BroadcastLocalized(this, UShockDomMessage::StaticClass(), 0, PS, nullptr, Point);

		// Check triple cap
		bool bTripleCap = true;
		for (AShockDomControlPoint* CP : ControlPoints)
		{
			if (CP && CP->OwningTeamIndex != NewTeam)
			{
				bTripleCap = false;
				break;
			}
		}
		if (bTripleCap && ControlPoints.Num() > 0)
		{
			BroadcastLocalized(this, UShockDomMessage::StaticClass(), 1, PS, nullptr, Point);
		}
	}
}


// ============================================================================
// SPAWN RATING (adapted from NCPlusCTF)
// ============================================================================

float AShockDomGameMode::RatePlayerStart(APlayerStart* P, AController* Player)
{
	float Result = Super::RatePlayerStart(P, Player);

	if (Result <= 0.f || !Player) return Result;

	AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
	if (!PS || !PS->Team) return Result;

	const uint8 PlayerTeamNum = PS->Team->TeamIndex;
	const FVector StartLoc = P->GetActorLocation();
	static FName NAME_DOMSpawnCheck = FName(TEXT("DOMSpawnCheck"));

	// ── Control point penalties ──
	// Don't spawn near enemy-controlled points
	for (AShockDomControlPoint* CP : ControlPoints)
	{
		if (!CP) continue;
		// Only penalize enemy-controlled points (not neutral or friendly)
		if (CP->OwningTeamIndex != PlayerTeamNum && CP->OwningTeamIndex != 255)
		{
			float DistToPoint = (StartLoc - CP->GetActorLocation()).Size();
			if (DistToPoint < ControlPointSpawnPenaltyRadius)
			{
				float DistFactor = 1.f - (DistToPoint / ControlPointSpawnPenaltyRadius);
				Result -= ControlPointSpawnPenalty * DistFactor;
			}
		}
	}

	// ── Enemy proximity & LOS penalties ──
	if (EnemyBlockRange > 0.f || EnemyLOSBlockRange > 0.f)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* C = It->Get();
			if (!C || !C->GetPawn()) continue;

			AUTPlayerState* EnemyPS = Cast<AUTPlayerState>(C->PlayerState);
			if (!EnemyPS || !EnemyPS->Team || EnemyPS->Team->TeamIndex == PlayerTeamNum)
				continue;

			AUTCharacter* EnemyChar = Cast<AUTCharacter>(C->GetPawn());
			if (!EnemyChar || EnemyChar->IsDead()) continue;

			const FVector EnemyLoc = EnemyChar->GetActorLocation();
			const float DistToEnemy = (StartLoc - EnemyLoc).Size();

			// Distance-based penalty
			if (EnemyBlockRange > 0.f && DistToEnemy < EnemyBlockRange)
			{
				float DistFactor = 1.f - (DistToEnemy / EnemyBlockRange);
				Result -= EnemyBlockPenalty * DistFactor;
			}

			// LOS penalty
			if (EnemyLOSBlockRange > 0.f && DistToEnemy < EnemyLOSBlockRange)
			{
				FVector SpawnEye = StartLoc + FVector(0.f, 0.f, 64.f);
				FVector EnemyEye = EnemyLoc + FVector(0.f, 0.f, EnemyChar->BaseEyeHeight);

				if (!GetWorld()->LineTraceTestByChannel(
					SpawnEye, EnemyEye,
					COLLISION_TRACE_WEAPONNOCHARACTER,
					FCollisionQueryParams(NAME_DOMSpawnCheck, false)))
				{
					Result -= EnemyLOSPenalty;
				}
			}
		}
	}

	return FMath::Max(Result, 0.2f);
}


// ============================================================================
// PING-COMPENSATED SPAWN
// ============================================================================

void AShockDomGameMode::RestartPlayer(AController* NewPlayer)
{
	Super::RestartPlayer(NewPlayer);

	// Hide pawn until client confirms control. Skip bots.
	ATeamArenaCharacter* SpawnedChar = (NewPlayer && NewPlayer->GetPawn()) ? Cast<ATeamArenaCharacter>(NewPlayer->GetPawn()) : nullptr;
	if (bEnablePingCompensatedSpawn && SpawnedChar && NewPlayer->GetPawn()->GetRemoteRole() == ROLE_AutonomousProxy)
	{
		SpawnedChar->bPingCompensatedSpawnPending = true;
		SpawnedChar->SetActorHiddenInGame(true);
		SpawnedChar->SetActorEnableCollision(false);
		SpawnedChar->SpawnHiddenTimestamp = GetWorld()->GetTimeSeconds();
	}
}


// ============================================================================
// WEAPON / ITEM FILTERING
// ============================================================================

void AShockDomGameMode::GiveDefaultInventory(APawn* PlayerPawn)
{
	AUTCharacter* UTChar = Cast<AUTCharacter>(PlayerPawn);
	if (UTChar)
	{
		// Temporarily clear character inventory so only game mode inventory (shock) is given
		TArray<TSubclassOf<AUTInventory>> SavedInv = UTChar->DefaultCharacterInventory;
		UTChar->DefaultCharacterInventory.Empty();
		Super::GiveDefaultInventory(PlayerPawn);
		UTChar->DefaultCharacterInventory = SavedInv;
	}
	else
	{
		Super::GiveDefaultInventory(PlayerPawn);
	}
}


bool AShockDomGameMode::CheckRelevance_Implementation(AActor* Other)
{
	if (!Other)
	{
		return Super::CheckRelevance_Implementation(Other);
	}

	// Remove ALL weapon pickups (shock-only mode)
	if (Other->IsA(AUTPickupWeapon::StaticClass()))
	{
		return false;
	}

	// Remove all ammo pickups
	if (Other->IsA(AUTPickupAmmo::StaticClass()))
	{
		return false;
	}

	// Health pickups: vials heal 7, health packs restore to 100
	AUTPickupHealth* HealthPickup = Cast<AUTPickupHealth>(Other);
	if (HealthPickup)
	{
		if (HealthPickup->HealAmount <= 5)
		{
			// Vials: 7hp (3 vials = survive an extra shock primary shot)
			HealthPickup->HealAmount = 7;
		}
		else
		{
			// Health packs: always restore to 100 (capped at HealthMax)
			HealthPickup->HealAmount = 100;
		}
		return Super::CheckRelevance_Implementation(Other);
	}

	// Inventory pickups: selective
	AUTPickupInventory* InvPickup = Cast<AUTPickupInventory>(Other);
	if (InvPickup && InvPickup->GetInventoryType())
	{
		FString InvName = InvPickup->GetInventoryType()->GetName();

		// Keep: ShieldBelt (200 armor, full absorb, 60s)
		if (InvName.Contains(TEXT("ShieldBelt")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Keep: UDamage / Amp (persists until death)
		if (InvName.Contains(TEXT("UDamage")) || InvName.Contains(TEXT("Amp")) || InvName.Contains(TEXT("Berserk")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Keep: Armor (50 armor, 100 armor) — standard pickups
		if (InvName.Contains(TEXT("Armor")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Remove everything else (Jumpboots, Invisibility, etc.)
		return false;
	}

	return Super::CheckRelevance_Implementation(Other);
}


bool AShockDomGameMode::ModifyDamage_Implementation(int32& Damage, FVector& Momentum,
	APawn* Injured, AController* InstigatedBy, const FHitResult& HitInfo,
	AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	// Block shock ball and shock combo damage (primary only mode)
	if (DamageType)
	{
		FString DmgName = DamageType->GetName();
		if (DmgName.Contains(TEXT("ShockBall")) || DmgName.Contains(TEXT("ShockCombo")))
		{
			Damage = 0;
			Momentum = FVector::ZeroVector;
			return true;
		}
	}

	return Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
}


// ============================================================================
// WIN CONDITIONS
// ============================================================================

bool AShockDomGameMode::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	// Score limit already checked in TickDomScoring — this handles time limit
	return false;
}


void AShockDomGameMode::DefaultTimer()
{
	Super::DefaultTimer();

	// Time limit check: when time expires, higher score wins
	if (HasAuthority() && IsMatchInProgress() && TimeLimit > 0)
	{
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS && GS->GetRemainingTime() <= 0)
		{
			// Check for tie — enter overtime
			if (Teams.IsValidIndex(0) && Teams.IsValidIndex(1) &&
				Teams[0] && Teams[1] && Teams[0]->Score == Teams[1]->Score)
			{
				// Overtime: keep playing, first team to gain lead wins
				// Don't end the game — scoring timer keeps running
				return;
			}

			// Find winning team
			int32 WinnerTeamIdx = 0;
			int32 HighestScore = 0;
			for (int32 i = 0; i < NumTeams; i++)
			{
				if (Teams.IsValidIndex(i) && Teams[i] && Teams[i]->Score > HighestScore)
				{
					HighestScore = Teams[i]->Score;
					WinnerTeamIdx = i;
				}
			}

			AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIdx);
			EndGame(BestPlayer, FName(TEXT("timelimit")));
		}
	}
}


void AShockDomGameMode::EndGame(AUTPlayerState* Winner, FName Reason)
{
	// Stop scoring timer
	GetWorldTimerManager().ClearTimer(ScoringTimerHandle);
	Super::EndGame(Winner, Reason);
}


AUTPlayerState* AShockDomGameMode::FindBestPlayerOnTeam(int32 TeamIndex)
{
	AUTPlayerState* Best = nullptr;
	int32 BestCaptures = -1;
	int32 BestKills = -1;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>((*It)->PlayerState);
		if (!PS || !PS->Team || PS->Team->TeamIndex != TeamIndex) continue;

		int32 Captures = 0;
		if (DomReplicator && PS->UniqueId.IsValid())
		{
			Captures = DomReplicator->GetCapturesForPlayer(PS->UniqueId.ToString());
		}

		if (Captures > BestCaptures || (Captures == BestCaptures && PS->Kills > BestKills))
		{
			Best = PS;
			BestCaptures = Captures;
			BestKills = PS->Kills;
		}
	}

	return Best;
}
