#pragma once
#include "NetcodePlus.h"
#include "UTWeaponStateFiring.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.generated.h"

// Forward declarations
class AUTPlusWeap_RocketLauncher;
class AUTWeaponFix;
class AUTBot;
class AUTGameState;

/**
 * Transactional Charged Rocket State
 *
 * This state handles the alt-fire "hold to load multiple rockets" behavior
 * using your transactional networking model:
 * - Client holds button -> loads rockets locally with animations
 * - Client releases (or grace timer fires) -> sends ONE RPC with all loaded rockets
 * - Server validates and fires
 *
 * Key differences from stock:
 * - NO auto-release due to desyncing (only grace timer after full load)
 * - NO dependency on Epic's AUTWeap_RocketLauncher (uses your AUTPlusWeap_RocketLauncher)
 * - NO inheritance from UUTWeaponStateFiringCharged
 * - Transactional RPC flow instead of continuous server simulation
 */
UCLASS()
class UUTWeaponStateFiringChargedRocket_Transactional : public UUTWeaponStateFiring
{
    GENERATED_BODY()

public:
    UUTWeaponStateFiringChargedRocket_Transactional(const FObjectInitializer& ObjectInitializer);


    // === STATE TRACKING ===

    /** Are we currently in the charging/loading phase? */
    UPROPERTY()
    bool bCharging;

    /** Accumulated charge time (for compatibility/future use) */
    UPROPERTY()
    float ChargeTime;

    /** Cached pointer to our rocket launcher (avoids repeated casts) */
    UPROPERTY()
    AUTPlusWeap_RocketLauncher* RocketLauncher;

    // === TIMER HANDLES ===

    /** Timer for loading each rocket */
    FTimerHandle LoadTimerHandle;

    /** Grace period timer - fires rockets if player holds too long after full load */
    FTimerHandle GraceTimerHandle;

    /** Timer for burst-firing loaded rockets (if BurstInterval > 0) */
    FTimerHandle FireLoadedRocketHandle;

    // === STATE OVERRIDES ===

    virtual void BeginState(const UUTWeaponState* PrevState) override;
    virtual void EndState() override;
    virtual void Tick(float DeltaTime) override;
    virtual void PutDown() override;

    // === FIRING SEQUENCE ===

    /** Called when player releases fire button */
    virtual void EndFiringSequence(uint8 FireModeNum) override;

    /** Called by RefireCheckTimer to handle continued firing after burst completes */
    virtual void RefireCheckTimer() override;

    /** Override FireShot to route through transactional system */
    virtual void FireShot() override;

    /** Updates timing when fire rate changes (powerups, etc) */
    virtual void UpdateTiming() override;

    // === ROCKET LOADING ===

    /** Timer callback - called when a rocket finishes loading */
    UFUNCTION()
    void LoadTimer();

    /** Timer callback - grace period expired, force fire */
    UFUNCTION()
    void GraceTimer();

    /** Fire all loaded rockets (handles burst interval if set) */
    UFUNCTION()
    void FireLoadedRocket();

protected:
    /** Helper to get the weapon as UTWeaponFix for transactional calls */
    AUTWeaponFix* GetWeaponFix() const;

    /** Clean up all timers */
    void ClearAllTimers();
};
