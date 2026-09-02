// TeamArenaCharacterMovement.h
// High-FPS optimized movement component for UT4
// Fixes: Team collision spam, position error tolerance, dodge timing tolerance

#pragma once
#include "NetcodePlus.h"
#include "UTCharacterMovement.h"
#include "TeamArenaCharacterMovement.generated.h"

class AUTCharacter;
class UPrimitiveComponent;
class USceneComponent;

UCLASS()
class NETCODEPLUS_API UTeamArenaCharacterMovement : public UUTCharacterMovement
{
    GENERATED_BODY()

public:
    UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer);

    //~ Begin UActorComponent Interface
    virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void OnUnregister() override;
    //~ End UActorComponent Interface

    //~ Begin UMovementComponent Interface
    virtual void SetUpdatedComponent(USceneComponent* NewUpdatedComponent) override;
    //~ End UMovementComponent Interface

    //~ Begin UUTCharacterMovement Interface
    virtual bool CanDodge() override;
    virtual void UTCallServerMove() override;
    virtual void SmoothClientPosition(float DeltaTime) override;
    /** BSP slope-edge stick fix: stock reduces only Result.Z in the slope-dodge-boost
     *  branch, tilting the slide vector into the surface it just left; we apply the same
     *  Z limit but cancel the into-plane component via the slope's horizontal normal so
     *  the slide stays plane-parallel (uniform-rescale fallback for near-vertical
     *  normals). See .cpp notes. */
    virtual FVector ComputeSlideVectorUT(const float DeltaTime, const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) override;
    //~ End UUTCharacterMovement Interface

    /** How often to detect team collision ignore changes (seconds). Default ~90 Hz. */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float TeamCollisionUpdateInterval;

    /** Tolerance added to dodge cooldown checks to prevent server rejection from timing jitter */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float DodgeCooldownTolerance;

    virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;


protected:
    /** Last time we updated team collision ignores */
    double LastTeamCollisionUpdateTime;

    /** Actors currently written into the movement component's ignore set. The 90 Hz
     *  detection pass diffs against this set instead of repeating unchanged writes. */
    TSet<TWeakObjectPtr<AUTCharacter>> TeamCollisionIgnoredActors;

    /** Component that owns TeamCollisionIgnoredActors; UpdatedComponent can change. */
    TWeakObjectPtr<UPrimitiveComponent> TeamCollisionIgnoreComponent;

    /** Remove every valid cached ignore from Component, then reset the cache. */
    void ClearTeamCollisionIgnores(UPrimitiveComponent* Component);

    /** Performs the team collision ignore detection/update (throttled) */
    void UpdateTeamCollisionIgnores();
};
