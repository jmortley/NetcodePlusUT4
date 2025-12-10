#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h" // Inherit from fixed class
#include "UTProj_Rocket.h"
#include "UTPlusWeap_RocketLauncher.generated.h"



// Forward declarations
class UUTWeaponStateFiringChargedRocket_Transactional;
class AUTProj_RocketSpiral;

/**
 * Rocket Fire Mode Configuration
 * Supports: Standard Spread (0), Grenades (1), Spiral (2)
 */
USTRUCT(BlueprintType)
struct FPlusRocketFireMode
{
    GENERATED_BODY()

    /** Projectile class to spawn for this mode */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    TSubclassOf<AUTProjectile> ProjClass;

    /** Whether this mode causes muzzle flash */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    bool bCauseMuzzleFlash;

    /** Spread amount for this mode */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    float Spread;

    /** Fire sound for this mode */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    USoundBase* FireSound;

    /** First person fire sound (optional) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    USoundBase* FPFireSound;

    /** Display name for HUD */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Mode")
    FText ModeName;

    FPlusRocketFireMode()
        : ProjClass(nullptr)
        , bCauseMuzzleFlash(true)
        , Spread(0.0f)
        , FireSound(nullptr)
        , FPFireSound(nullptr)
        , ModeName(FText::GetEmpty())
    {
    }
};

UCLASS(Abstract, Config = Game)
class AUTPlusWeap_RocketLauncher : public AUTWeaponFix
{
    GENERATED_BODY()

public:
    AUTPlusWeap_RocketLauncher(const FObjectInitializer& ObjectInitializer);

    virtual void PostInitProperties() override;
    virtual void Destroyed() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // === ROCKET LOADING ===

    /** Number of rockets currently loaded and ready to fire */
    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher")
    int32 NumLoadedRockets;

    /** Number of barrel positions used (for animation sync) */
    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher")
    int32 NumLoadedBarrels;

    /** Maximum rockets that can be loaded */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    int32 MaxLoadedRockets;

    /** Time to load subsequent rockets */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float RocketLoadTime;

    /** Time to load the first rocket (usually faster) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float FirstRocketLoadTime;

    /** Grace period after full load before auto-fire */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float GracePeriod;

    /** Interval between rockets in a burst (0 = instant) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float BurstInterval;

    /** Interval between grenades in a burst */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float GrenadeBurstInterval;

    /** Timestamp of last rocket load (for animation timing) */
    UPROPERTY()
    float LastLoadTime;

    // === FIRE MODES ===

    /** Current rocket fire mode: 0=Spread, 1=Grenades, 2=Spiral */
    UPROPERTY(BlueprintReadOnly, Replicated, Category = "Rocket Launcher")
    int32 CurrentRocketFireMode;

    /** Whether to show the mode string on HUD */
    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher")
    bool bDrawRocketModeString;

    /** Available rocket fire modes */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<FPlusRocketFireMode> RocketFireModes;

    /** Enable alternate fire modes (grenades, spiral) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    bool bAllowAltModes;

    // Legacy compatibility
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    bool bAllowGrenades;

    // === SPREAD SETTINGS ===

    /** Spread amount for loaded rockets (non-seeking) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float FullLoadSpread;

    /** Spread amount for seeking rockets */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float SeekingLoadSpread;

    /** Radius of rocket barrels (for spawn offset) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    float BarrelRadius;

    // === SPIRAL ROCKET SETTINGS ===

    /** Projectile class for spiral rockets (if not set in RocketFireModes[2]) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Spiral")
    TSubclassOf<AUTProjectile> SpiralRocketClass;

    /** Burst interval for spiral rockets (0 = all fire instantly) */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Spiral")
    float SpiralBurstInterval;

    // === TARGET LOCKING ===

    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_LockedTarget, Category = "Rocket Launcher")
    AActor* LockedTarget;

    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PendingLockedTarget, Category = "Rocket Launcher")
    AActor* PendingLockedTarget;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    TSubclassOf<AUTProjectile> SeekingRocketClass;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockCheckTime;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockRange;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockAcquireTime;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockTolerance;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockAim;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockOffset;

    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    bool bLockedOnTarget;

    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    bool bTargetLockingActive;

    UPROPERTY()
    float LastLockedOnTime;

    UPROPERTY()
    float PendingLockedTargetTime;

    UPROPERTY()
    float LastValidTargetTime;

    UPROPERTY()
    float LastTargetLockCheckTime;

    UPROPERTY()
    TArray<AUTProj_Rocket*> TrackingRockets;

    FTimerHandle UpdateLockHandle;

    // === ANIMATIONS ===

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> LoadingAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> LoadingAnimationHands;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> EmptyLoadingAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> EmptyLoadingAnimationHands;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> FiringAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Animation")
    TArray<UAnimMontage*> FiringAnimationHands;

    // === SOUNDS ===

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
    USoundBase* RocketLoadedSound;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
    USoundBase* AltFireModeChangeSound;

    // === HUD/VISUAL ===

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|HUD")
    float CrosshairRotationTime;

    UPROPERTY()
    float CurrentRotation;

    //UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|HUD")
   // FVector2D HUDViewKickback;

    // === TIMER HANDLES ===

    FTimerHandle SpawnDelayedFakeProjHandle;
    FTimerHandle PlayLowAmmoSoundHandle;

    // === FUNCTIONS ===

    // Loading
    virtual void BeginLoadRocket();
    virtual void EndLoadRocket();
    virtual void ClearLoadedRockets();
    virtual float GetLoadTime(int32 InNumLoadedRockets);

    UFUNCTION(Client, Reliable)
    void ClientAbortLoad();

    // Firing
    virtual void FireShot() override;
    virtual AUTProjectile* FireProjectile() override;
    virtual AUTProjectile* FireRocketProjectile();
    virtual void PlayFiringEffects() override;
    virtual void PlayDelayedFireSound();

    /** Check if we should dump all rockets immediately (death, ragdoll, etc) */
    virtual bool ShouldFireLoad();

    // Fire Mode
    virtual void OnMultiPress_Implementation(uint8 OtherFireMode) override;
    virtual void SetRocketFlashExtra(uint8 InFireMode, int32 InNumLoadedRockets, int32 InCurrentRocketFireMode, bool bInDrawRocketModeString);
    virtual void GetRocketFlashExtra(uint8 InFlashExtra, uint8 InFireMode, int32& OutNumLoadedRockets, int32& OutCurrentRocketFireMode, bool& bOutDrawRocketModeString);
    virtual void FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode) override;
    virtual void FiringInfoUpdated_Implementation(uint8 InFireMode, uint8 FlashCount, FVector InFlashLocation) override;

    // Spread Helper
    virtual float GetSpread(int32 ModeIndex);

    // Target Locking
    virtual void StateChanged() override;
    virtual bool CanLockTarget(AActor* Target);
    virtual bool WithinLockAim(AActor* Target);
    virtual void SetLockTarget(AActor* NewTarget);
    virtual void UpdateLock();
    virtual bool HasLockedTarget() const { return LockedTarget != nullptr && bLockedOnTarget; }

    UFUNCTION()
    void OnRep_LockedTarget();

    UFUNCTION()
    void OnRep_PendingLockedTarget();

    // AI
    virtual float GetAISelectRating_Implementation() override;
    virtual float SuggestAttackStyle_Implementation() override;
    virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;
    virtual bool IsPreparingAttack_Implementation() override;

protected:
    // AI helpers
    UPROPERTY()
    FVector PredicitiveTargetLoc;

    UPROPERTY()
    float LastAttackSkillCheckTime;

    UPROPERTY()
    bool bAttackSkillCheckResult;
};