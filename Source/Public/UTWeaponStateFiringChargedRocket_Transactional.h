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
 * - Client releases (or grace timer fires) -> commits the completed load once
 * - The 328 client intentionally retains both the stock and fixed Stop RPC families;
 *   this state makes their duplicate release notifications idempotent
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

    /**
     * Set by the first release notification for the current charge cycle.
     * This is local state-machine bookkeeping, not replicated authority data.
     */
    bool bReleaseRequested;

    /**
     * Set before release performs any firing or state-transition side effects.
     * It prevents the stock and fixed Stop RPC families from committing twice.
     */
    bool bReleaseCommitted;

    /**
     * True only while EndLoadRocket() is executing from LoadTimer(). A Stop can
     * arrive synchronously through ammo/bot callbacks during that call; it records
     * release intent but defers the commit until EndLoadRocket() has returned.
     */
    bool bCompletingLoadTimer;

    /** Suppresses repeated diagnostic lines from identical Stop retries. */
    bool bDuplicateReleaseLogged;

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

    /**
     * Resume a release only after the weapon watchdog has proved that this state
     * owns no load, grace, burst, or refire timer. This is not a normal Stop path.
     */
    void RecoverWedgedRelease();

    /** Override FireShot to route through transactional system */
    //virtual void FireShot() override;

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

    /**
     * Resolve a previously latched release. A release with no completed rocket
     * waits for the already-armed first load callback; later incomplete loads
     * are never promoted and are cancelled before the completed volley fires.
     */
    void CommitRelease();

    /** Return idle without ActiveState auto-firing, then retry a still-held primary safely. */
    void ExitToActiveAndAttemptBufferedPrimary();

    /** Clean up all timers */
    void ClearAllTimers();
};
