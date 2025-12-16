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
    //~ End UUTCharacterMovement Interface

    /** How often to update team collision ignores (seconds). Default 0.25s */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float TeamCollisionUpdateInterval;

    /** Tolerance added to dodge cooldown checks to prevent server rejection from timing jitter */
    UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    float DodgeCooldownTolerance;

protected:
    /** Last time we updated team collision ignores */
    float LastTeamCollisionUpdateTime;

    /** Performs the team collision ignore update (throttled) */
    void UpdateTeamCollisionIgnores();
};