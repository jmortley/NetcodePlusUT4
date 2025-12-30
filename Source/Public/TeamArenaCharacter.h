// TeamArenaCharacter.h
#pragma once
#include "NetcodePlus.h"
#include "UTCharacter.h"
#include "UTCarriedObject.h"
#include "UTCharacterMovement.h"
#include "UTRecastNavMesh.h"
#include "UTHat.h"
#include "UTHatLeader.h"
#include "UTEyewear.h"
#include "TeamArenaCharacter.generated.h"


class UTeamArenaCharacterMovement;

/**
 * Enhanced character that uses split prediction for movement.
 * 
 * Key change: OnRep_ReplicatedMovement() uses GetVisualPredictionTime()
 * instead of GetPredictionTime() for client-side extrapolation.
 * 
 * This eliminates the "dual hitbox" bug where enemies appear to have
 * different positions visually vs where the server validates hits.
 * 
 * Visual position (predict 0) ≈ Server position (now)
 * Hit validation (predict 120) ≈ What shooter saw when they fired
 */
UCLASS()
class NETCODEPLUS_API ATeamArenaCharacter : public AUTCharacter
{
    GENERATED_BODY()

public:
    ATeamArenaCharacter(const FObjectInitializer& ObjectInitializer);


	// The material to use for the overlay (Assign M_ShieldBelt_Overlay here in BP)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection")
	UMaterialInterface* SpawnProtectionMaterial;

	// The color and opacity to force on the overlay (R=1, G=0.9, B=0, A=0.7 for Gold/70%)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection")
	FLinearColor SpawnProtectionColor = FLinearColor(1.0f, 0.9f, 0.0f, 0.7f);

	// Add this property to control Opacity explicitly in BP
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SpawnProtectionOpacity = 0.7f;

	virtual bool IsHeadShot(FVector HitLocation, FVector ShotDirection, float WeaponHeadScaling,
		AUTCharacter* ShotInstigator, float PredictionTime) override;

	virtual void Tick(float DeltaTime) override;

    /**
     * Override replication callback to use visual prediction time.
     * This is THE critical change for split prediction.
     */
    //virtual void OnRep_ReplicatedMovement() override;
    // Add override declaration
    virtual void UTUpdateSimulatedPosition(const FVector& NewLocation, const FRotator& NewRotation, const FVector& NewVelocity) override;


    // Override to decouple visual effects from standard UTPlayerController prediction
    virtual void FiringInfoUpdated() override;

    // Override to use custom prediction time for position rewinding
    virtual FVector GetRewindLocation(float PredictionTime, AUTPlayerController* DebugViewer = NULL) override;

    //virtual void PositionUpdated(bool bShotSpawned) override;
	virtual void BeginPlay() override;

	virtual FVector GetHeadLocation(float PredictionTime = 0.f)  override;

	// Allow Blueprints to read the version number from your header file
	UFUNCTION(BlueprintPure, Category = "NetcodePlus")
	static int32 GetNetcodeVersion();
    /** Rate at which to save positions for lag compensation (Hz). Default 120. */
    //UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    //float PositionSaveRate;

    // Add to protected section:
    /** Last time we saved a position (for throttling) */
    //float LastPositionSaveTime;

    /** Calculated interval between position saves */
    //float PositionSaveInterval;


protected:
    /**
     * Get the client's visual prediction time from the viewing controller.
     * Returns 0ms if using TeamArenaPredictionPC (no extrapolation).
     * 
     * @return Prediction time in seconds for visual movement extrapolation
     */
    float GetClientVisualPredictionTime() const;

    /**
     * Cached reference to viewing controller (for performance).
     * Updated when controller changes.
     */
    UPROPERTY()
    class ATeamArenaPredictionPC* CachedPredictionPC;

    /**
     * Whether we've already tried to cache the prediction controller.
     * Prevents repeated casts every frame.
     */
    bool bHasCachedPC;

	bool bHasSpawnOverlay = false;
};
