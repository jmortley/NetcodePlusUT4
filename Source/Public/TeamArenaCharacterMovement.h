// TeamArenaCharacterMovement.h
// High-FPS optimized movement component for UT4
// Fixes: Team collision spam, position error tolerance, dodge timing tolerance

#pragma once
#include "NetcodePlus.h"
#include "UTCharacterMovement.h"
#include "TeamArenaCharacterMovement.generated.h"

UCLASS()
class NETCODEPLUS_API UTeamArenaCharacterMovement : public UUTCharacterMovement
{
    GENERATED_BODY()

public:
    UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer);

    //~ Begin UActorComponent Interface
    virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    //~ End UActorComponent Interface

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

    /** How often to update team collision ignores (seconds). Default 0.25s */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float TeamCollisionUpdateInterval;

    /** Tolerance added to dodge cooldown checks to prevent server rejection from timing jitter */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float DodgeCooldownTolerance;

    virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;


protected:
    /** Last time we updated team collision ignores */
    float LastTeamCollisionUpdateTime;

    /** Performs the team collision ignore update (throttled) */
    void UpdateTeamCollisionIgnores();
};
