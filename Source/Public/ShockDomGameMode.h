// ShockDomGameMode — Domination game mode for NetcodePlus
#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTTeamGameMode.h"
#include "TimerManager.h"
#include "ShockDomGameMode.generated.h"

class AShockDomControlPoint;
class AShockDomReplicator;
class AUTCharacter;

UCLASS(Config = Game)
class NETCODEPLUS_API AShockDomGameMode : public AUTTeamGameMode
{
	GENERATED_BODY()

public:
	AShockDomGameMode(const FObjectInitializer& ObjectInitializer);


	// =======================================================================
	// DOMINATION CONFIGURATION
	// =======================================================================

	/** Points awarded per controlled point per scoring tick */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Scoring")
	int32 ScorePerPointPerTick;

	/** Seconds between scoring ticks */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Scoring")
	float ScoringTickInterval;

	/** Capture volume radius for control points */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Points")
	float ControlPointCaptureRadius;

	/** Maximum number of control points to spawn */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Points")
	int32 MaxControlPoints;

	/** Control point class to spawn — set to your BP subclass to customize mesh/effects */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Points")
	TSubclassOf<AShockDomControlPoint> ControlPointClass;


	// =======================================================================
	// SPAWN PENALTIES (adapted from NCPlusCTF)
	// =======================================================================

	/** Penalize spawns near enemy-controlled points within this range */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float ControlPointSpawnPenaltyRadius;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float ControlPointSpawnPenalty;

	/** Penalize spawns near living enemies within this range */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float EnemyBlockRange;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float EnemyBlockPenalty;

	/** Penalize spawns with clear LOS to enemies within this range */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float EnemyLOSBlockRange;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DOM|Spawning")
	float EnemyLOSPenalty;


	// =======================================================================
	// RUNTIME STATE
	// =======================================================================

	UPROPERTY(Transient)
	TArray<AShockDomControlPoint*> ControlPoints;

	/** Replicated stats broadcaster (captures, damage, score goal) */
	UPROPERTY(Transient)
	AShockDomReplicator* DomReplicator;


	// =======================================================================
	// PUBLIC API
	// =======================================================================

	/** Called by AShockDomControlPoint when a player captures it */
	void OnPointCaptured(AShockDomControlPoint* Point, uint8 NewTeam, AUTCharacter* Capturer);


	// =======================================================================
	// UT OVERRIDES
	// =======================================================================

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void HandleMatchHasStarted() override;
	virtual void DefaultTimer() override;

	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual float RatePlayerStart(APlayerStart* P, AController* Player) override;
	virtual void GiveDefaultInventory(APawn* PlayerPawn) override;
	virtual bool CheckRelevance_Implementation(AActor* Other) override;
	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
		AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser,
		TSubclassOf<UDamageType> DamageType) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;
	virtual void EndGame(AUTPlayerState* Winner, FName Reason) override;

protected:

	/** Scoring timer handle and callback */
	FTimerHandle ScoringTimerHandle;
	void TickDomScoring();

	/** Spawn control points from map-placed AUTGenericObjectivePoint markers */
	void SpawnControlPoints();

	/** Fallback: pick spread-out locations from player starts */
	void SpawnControlPointsFromPlayerStarts();

	/** Find the best player on a team (most captures, then most kills) */
	virtual AUTPlayerState* FindBestPlayerOnTeam(int32 TeamIndex) override;
};
