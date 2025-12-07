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
};