#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h" // Inherit from fixed class
#include "UTProj_Rocket.h"
#include "NCRocketVolley.h"
#include "UTPlusWeap_RocketLauncher.generated.h"



// Forward declarations
class UUTWeaponStateFiringChargedRocket_Transactional;
class AUTProj_RocketSpiral;

struct FNCLoadedRocketPrediction
{
    uint32 OwnershipEpoch = 0;
    uint32 VolleyId = 0;
    uint32 ProjectileNetGUID = 0;
    uint8 Ordinal = 0;
    uint8 Outcome = 255;
    float CreatedAt = 0.f;
    TWeakObjectPtr<AUTProjectile> Fake;
    TWeakObjectPtr<AUTProjectile> Real;
};

struct FNCLoadedVolleyReceipt
{
    uint32 VolleyId = 0;
    uint8 Result = 0;
    uint8 Count = 0;
    uint8 SpawnedMask = 0;
};

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rocket Mode")
    FText DisplayString;

    FPlusRocketFireMode()
        : ProjClass(nullptr)
        , bCauseMuzzleFlash(true)
        , Spread(0.0f)
        , FireSound(nullptr)
        , FPFireSound(nullptr)
        , DisplayString(FText::GetEmpty())
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
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Removed() override;
    virtual void GivenTo(AUTCharacter* NewOwner, bool bAutoActivate) override;
    virtual void ClientGivenTo_Internal(bool bAutoActivate) override;
    virtual bool PutDown() override;
    virtual void DetachFromOwner_Implementation() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // === ROCKET LOADING ===
    /** Font used to draw the firemode text (Grenades/Spiral) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD")
    UFont* RocketModeFont;

    /** Texture used for the lock-on reticle */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUD")
    UTexture2D* LockCrosshairTexture;

    // This definition fixes the "inherited member is not allowed" error
    virtual void DrawWeaponCrosshair_Implementation(UUTHUDWidget* WeaponHudWidget, float RenderDelta) override;
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
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CurrentRocketFireMode, Category = "Rocket Launcher")
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

    /** Hard-disable fire mode 1 (rocket loading) for single-rocket-only loadouts
     *  (Clutch defenders). Gated in BeginFiringSequence — the one funnel both the
     *  local StartFire path and ServerStartFireFixed pass through — so the server
     *  rejects modified clients too. Non-replicated config: no schema change, no
     *  new RPC. Also suppresses the lock-on timer (only loaded rockets can seek,
     *  so an acquired lock would be unconsumable) and the AI's alt-fire pick. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    bool bDisableAltLoading;

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

    /** How long the lock reticle keeps drawing on the last target after the lock clears
     *  (e.g. when the load is released). Display-only linger — does NOT keep the lock state
     *  alive, so seekers won't acquire during it. 0 = clear instantly (old behaviour). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    float LockDisplayLingerTime;

    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    bool bLockedOnTarget;

    UPROPERTY(BlueprintReadOnly, Category = "Rocket Launcher|Lock")
    bool bTargetLockingActive;

    UPROPERTY()
    float LastLockedOnTime;

    /** Client draw-linger state: the target the lock was last on, and when it cleared.
     *  Consumed only by DrawWeaponCrosshair for the reticle linger. */
    TWeakObjectPtr<AActor> LingerLockedTarget;
    UPROPERTY()
    float LockClearedTime;

    UPROPERTY()
    float PendingLockedTargetTime;

    UPROPERTY()
    float LastValidTargetTime;

    UPROPERTY()
    float LastTargetLockCheckTime;

    UPROPERTY()
    TArray<AUTProj_Rocket*> TrackingRockets;

    FTimerHandle UpdateLockHandle;

  

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> LoadingAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> LoadingAnimationHands;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> EmptyLoadingAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> EmptyLoadingAnimationHands;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> FiringAnimation;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher")
    TArray<UAnimMontage*> FiringAnimationHands;
    
    // === SOUNDS ===

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
    USoundBase* RocketLoadedSound;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
    USoundBase* AltFireModeChangeSound;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
	USoundBase* LockAcquiredSound;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|Sound")
	USoundBase* LockLostSound;

    // === HUD/VISUAL ===

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|HUD")
    float CrosshairRotationTime;

    /**The textures used for drawing the HUD*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = RocketLauncher)
    TArray<UTexture2D*> LoadCrosshairTextures;


    /**The texture for locking on a target*/
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = RocketLauncher)
    UTexture2D* PendingLockCrosshairTexture;


    UPROPERTY()
    float CurrentRotation;

    //UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rocket Launcher|HUD")
    //FVector2D HUDViewKickback;

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
    void ClientAbortLoad(uint32 OwnershipEpoch, uint32 VolleyId);

    // 329 charged-fire protocol. The reliable transport is deliberately separate
    // from both the stock byte-event and fixed-fire watermark RPC families.
    virtual void StartFire(uint8 FireModeNum) override;
    virtual void StopFire(uint8 FireModeNum) override;
    virtual AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile> ProjectileClass,
        FVector SpawnLocation, FRotator SpawnRotation) override;

    UFUNCTION(Server, Reliable, WithValidation)
    void ServerBeginLoadedVolley(uint32 OwnershipEpoch, uint32 VolleyId, AUTCharacter* ExpectedPawn);
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerReleaseLoadedVolley(uint32 OwnershipEpoch, uint32 VolleyId, AUTCharacter* ExpectedPawn, uint8 SelectedMode, uint8 RequestedCount);
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerSetLoadedRocketMode(uint32 OwnershipEpoch, uint32 VolleyId, AUTCharacter* ExpectedPawn, uint8 SelectedMode);
    UFUNCTION(Client, Reliable)
    void ClientLoadedVolleyResult(uint32 OwnershipEpoch, uint32 VolleyId, AUTCharacter* ExpectedPawn, uint8 Result, uint8 Count, uint8 SpawnedMask);
    UFUNCTION(Client, Reliable)
    void ClientLoadedRocketResult(uint32 OwnershipEpoch, uint32 VolleyId, AUTCharacter* ExpectedPawn, uint8 Ordinal, uint8 Result, AUTProjectile* Projectile, uint32 ProjectileNetGUID);

    UPROPERTY(ReplicatedUsing=OnRep_LoadedOwnershipEpoch)
    uint32 LoadedOwnershipEpoch = 0;
    UFUNCTION()
    void OnRep_LoadedOwnershipEpoch();

    uint32 GetLoadedVolleyId() const { return LoadedVolley.Id; }
    uint32 GetLoadedVolleyEpoch() const { return LoadedVolleyEpoch; }
    bool HasLoadedVolley() const { return LoadedVolley.Id != 0 && !LoadedVolley.Terminal; }
    bool IsLoadedVolleyCommitted() const { return LoadedVolley.Released; }
    bool IsLoadedVolleyReleasePending() const { return bLoadedVolleyReleaseReceived || bLoadedVolleyReleaseSent; }
    bool BeginLoadedVolleyState();
    void NotifyLoadedVolleyRelease();
    bool CommitLoadedVolley();
    void CompleteLoadedVolley(bool bCancelled);
    void ContinueLoadedVolley();
    void TryBeginLoadedVolley();
    void CaptureLoadedRocketSpawn(AUTProjectile* Projectile);
    bool ObserveLoadedRocketActor(uint32 OwnershipEpoch, uint32 VolleyId, uint8 Ordinal, AUTProjectile* Projectile);
    void NoteLoadedRocketAmmoSpent(int32 Amount);

    // Firing
    virtual bool BeginFiringSequence(uint8 FireModeNum, bool bClientFired) override;
    virtual bool AllowServerFireMode(uint8 FireModeNum) const override;
    virtual void FireShot() override;
    virtual AUTProjectile* FireProjectile() override;
    virtual AUTProjectile* FireRocketProjectile();
    virtual void PlayFiringEffects() override;
    virtual void PlayDelayedFireSound();
    void FireShotDirect();

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

    /** Defends against silent server CRFM replication overwrites mid-burst.
     *  See implementation comment for race details. */
    UFUNCTION()
    void OnRep_CurrentRocketFireMode(int32 OldValue);

    // AI
    virtual float GetAISelectRating_Implementation() override;
    virtual float SuggestAttackStyle_Implementation() override;
    virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;
    virtual bool IsPreparingAttack_Implementation() override;

protected:
    NCRocketVolley::FProgress LoadedVolley;
    uint32 LoadedVolleyEpoch = 0;
    uint32 LastClientLoadedVolleyId = 0;
    uint32 LastServerLoadedVolleyId = 0;
    uint8 LoadedVolleyRequestedCount = 0;
    uint8 LoadedVolleySelectedMode = 0;
    uint8 LoadedVolleyNextOrdinal = 0;
    int32 LoadedVolleyAmmoSpent = 0;
    bool bLoadedVolleyEnteredState = false;
    float LoadedVolleyBeginRequestedAt = 0.f;
    bool bLoadedVolleyReleaseSent = false;
    bool bLoadedVolleyReleaseReceived = false;
    bool bLoadedVolleySpawnInProgress = false;
    bool bLoadedVolleySpawnSucceeded = false;
    bool bLoadedVolleyApplyingResult = false;
    TWeakObjectPtr<AUTCharacter> LoadedVolleyPawn;
    TWeakObjectPtr<AUTProjectile> LoadedVolleySpawnedProjectile;
    TArray<FNCLoadedRocketPrediction> LoadedRocketPredictions;
    TArray<FNCLoadedVolleyReceipt> LoadedVolleyReceipts;
    FTimerHandle LoadedRocketReconcileHandle;
    FTimerHandle LoadedVolleyBeginHandle;
    // Local-only first press/release, before the initial ownership epoch maps.
    // Separate from PendingFire and the numbered volley so it cannot load early.
    bool bPendingLoadedVolleyInput = false;
    bool bPendingLoadedVolleyRelease = false;
    double PendingLoadedVolleyInputAt = 0.0;
    TWeakObjectPtr<AUTCharacter> PendingLoadedVolleyPawn;
    TWeakObjectPtr<UWorld> PendingLoadedVolleyWorld;
    TWeakObjectPtr<AController> PendingLoadedVolleyController;
    FTimerHandle PendingLoadedVolleyInputHandle;
    bool CanBeginLoadedVolleyInput();
    void BufferLoadedVolleyInput();
    void TryDrainLoadedVolleyInput();
    void ClearLoadedVolleyInput();
    bool CanBeginLoadedVolley();
    bool IsLoadedVolleyModeValid(uint8 Mode) const;
    void ResetLoadedVolley(uint32 Id);
    void SendLoadedVolleyReceipt(NCRocketVolley::EResult Result);
    void ReconcileLoadedRockets();
    FNCLoadedRocketPrediction& FindOrAddLoadedRocket(uint32 OwnershipEpoch, uint32 Id, uint8 Ordinal);
    void ResetLoadedOwnershipState(bool bPreservePendingInput = false);

    // AI helpers
    UPROPERTY()
    FVector PredicitiveTargetLoc;

    UPROPERTY()
    float LastAttackSkillCheckTime;

    UPROPERTY()
    bool bAttackSkillCheckResult;
};
