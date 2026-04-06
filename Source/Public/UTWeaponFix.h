// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTWeapon.h"
#include "UTWeaponFix.generated.h"

/**
 * Enhanced weapon base class combining three critical fixes:
 * 
 * 1. Transaction Validation: Fixes high-FPS desync with unique event indices
 * 2. Epic's Lag Compensation: Uses built-in GetRewindLocation() for hit validation
 * 3. Split Prediction: Separates visual (0ms) from hit validation (120ms) time
 * 
 * Key improvements:
 * - Transactional fire events with unique event indexing per fire mode
 * - Server-authoritative validation with client correction feedback loop
 * - Prevention of simultaneous fire mode activation
 * - Strict cooldown enforcement on server side
 * - Automatic desync recovery via ClientConfirmFireEvent RPC
 * - Forgiving hit detection using split prediction's hit validation time
 * 
 * Works with TeamArenaPredictionPC and TeamArenaCharacter for complete hybrid system.
 */

 // --- Forward Declarations ---
class AUTProjectile;
class AClientHitsounds;

// --- Struct Definition (MUST BE BEFORE THE CLASS) ---
USTRUCT(BlueprintType)
struct FNetcodeDelayedProjectile
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<AUTProjectile> ProjectileClass;

	UPROPERTY()
	FVector SpawnLocation;

	UPROPERTY()
	FRotator SpawnRotation;

	FNetcodeDelayedProjectile()
		: ProjectileClass(nullptr), SpawnLocation(FVector::ZeroVector), SpawnRotation(FRotator::ZeroRotator)
	{
	}
};

struct FPendingFireEventFix
{
    bool bIsStartFire;
    uint8 FireModeNum;
    int32 FireEventIndex;
    float ClientTimestamp;
    FRotator ClientViewRot;
    uint8 ZOffset;
    TWeakObjectPtr<AUTCharacter> HitChar; // <--- ADDED THIS

    FPendingFireEventFix(bool bStart, uint8 Mode, int32 EventIdx, float Timestamp, FRotator ViewRot, uint8 Z, AUTCharacter* InChar)
        : bIsStartFire(bStart), FireModeNum(Mode), FireEventIndex(EventIdx), ClientTimestamp(Timestamp), ClientViewRot(ViewRot), ZOffset(Z), HitChar(InChar)
    {
    }
};

USTRUCT()
struct FPendingFakeProjectile
{
    GENERATED_BODY()

    UPROPERTY()
    TWeakObjectPtr<AUTProjectile> Projectile;

    UPROPERTY()
    int32 EventIndex;

    UPROPERTY()
    uint8 FireMode;

    FPendingFakeProjectile()
        : EventIndex(-1)
        , FireMode(0)
    {
    }

    FPendingFakeProjectile(AUTProjectile* InProj, int32 InIndex, uint8 InMode)
        : Projectile(InProj)
        , EventIndex(InIndex)
        , FireMode(InMode)
    {
    }
};

/** Server-side tracking of authoritative projectiles for rewind validation */
USTRUCT()
struct FActiveServerProjectile
{
    GENERATED_BODY()

    UPROPERTY()
    TWeakObjectPtr<AUTProjectile> Projectile;

    UPROPERTY()
    int32 EventIndex;

    UPROPERTY()
    uint8 FireMode;

    FActiveServerProjectile()
        : EventIndex(-1)
        , FireMode(0)
    {
    }

    FActiveServerProjectile(AUTProjectile* InProj, int32 InIndex, uint8 InMode)
        : Projectile(InProj)
        , EventIndex(InIndex)
        , FireMode(InMode)
    {
    }
};


UCLASS(Abstract)
class NETCODEPLUS_API AUTWeaponFix : public AUTWeapon
{
    GENERATED_BODY()
public:
    AUTWeaponFix(const FObjectInitializer& ObjectInitializer);

    virtual void BeginPlay() override;
    static int32 GetTargetProjectileTickRate();

    /** Per-weapon hide state — keyed by class name.
     *  Set via "weaponskins" menu or "weaponhand hidden/show" console command.
     *  BringUp() checks this to hide 1P mesh on weapon switch. */
    static TMap<FName, bool> HiddenWeaponsByTag;

    /** Saved skin asset paths — keyed by WeaponSkinCustomizationTag.
     *  Loaded from Mod.ini, applied in BringUp(). */
    static TMap<FName, FString> SavedSkinPaths;

    /** Load weapon settings (skins + hide state) from Mod.ini. Called once on first BringUp. */
    static void LoadWeaponSettings();

    /** Save weapon settings to Mod.ini. */
    static void SaveWeaponSettings();

    /** Whether settings have been loaded from Mod.ini this session */
    static bool bWeaponSettingsLoaded;
    //~ Begin AUTWeapon Interface
    virtual void GetImpactSpawnPosition(const FVector& TargetLoc, FVector& SpawnLocation, FRotator& SpawnRotation) override;
    virtual void StartFire(uint8 FireModeNum) override;
    virtual void StopFire(uint8 FireModeNum) override;
    virtual void PostInitProperties() override;
    virtual void Tick(float DeltaTime) override;
    virtual void DetachFromOwner_Implementation() override;
    virtual bool PutDown() override;
    void PutDownDelayed();
    virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;
    virtual void FireShot() override;

    // Guard against race condition: replicated fire RPC arrives after owner dies
    // and weapon is being destroyed. The base class dereferences owner without null check.
    virtual void ServerUpdateFiringStates_Implementation(uint8 FireSettings) override;
    virtual FRotator GetAdjustedAim_Implementation(FVector StartFireLoc) override;
    virtual void HitScanTrace(const FVector& StartLocation, const FVector& EndTrace,
        float TraceRadius, FHitResult& Hit, float PredictionTime) override;
    virtual AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile> ProjectileClass, FVector SpawnLocation, FRotator SpawnRotation) override;
    virtual void FireCone() override;
    virtual FVector GetFireStartLoc(uint8 FireMode = 255) override;
    virtual FRotator GetBaseFireRotation() override;
    virtual void BringUp(float OverflowTime) override;
    virtual void SetSkin(UMaterialInterface* NewSkin) override;
    void ClearPendingFakeProjectiles();
    void DeferredGotoActiveState(uint8 FireModeNum);
    virtual void Removed() override;
    //~ End AUTWeapon Interface

    // =========================================================================
    // PROJECTILE REWIND SYSTEM
    // Called by UTPlusProj_Rocket / UTPlusProj_FlakShell when fake hits a pawn.
    // Sends ServerProjectileHitClaim RPC if bEnableProjectileRewind is true.
    // =========================================================================
    void NotifyFakeProjectileHit(AUTCharacter* HitTarget, const FVector& HitLocation, uint8 FireModeNum);
    UPROPERTY()
    TArray<float> LastFireTime;
     /**
     * Checks if a fire mode is currently on cooldown.
     *
     * @param FireModeNum - Fire mode to check
     * @param CurrentTime - Current world time
     * @return true if cooldown is still active (cannot fire yet)
     */
    bool IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime);
    void OnRetryTimer(uint8 FireModeNum);
    bool bIsTransactionalFire;
    float LastMultiPressTime;
    UPROPERTY()
    float LastShockCoreSpawnTime;
    UPROPERTY()
    float LastFlakShellSpawnTime;
    //UPROPERTY()
    //TWeakObjectPtr<AUTProjectile> PendingFakeProjectile;

    //int32 PendingFakeProjectileEventIndex;

    void ResetFiringModeTracker() { CurrentlyFiringMode = 255; }
    void ForceResetTransactionalState(uint8 ModeToClear)
    {
        CurrentlyFiringMode = 255;
        if (FireModeActiveState.IsValidIndex(ModeToClear))
        {
            FireModeActiveState[ModeToClear] = 0;
        }
    }

protected:

    FTimerHandle DelayedPutDownHandle;
    bool bHandlingRetry;
    FTimerHandle RetryFireHandle[2];
    UPROPERTY(Transient)
    FRotator CachedTransactionalRotation;

    // --- Trade-kill grace period: cache owner state before Removed() nulls UTOwner ---
    /** World time when UTOwner was lost (weapon removed from dying player) */
    float OwnerLostTime = 0.f;
    /** Last known fire start location when owner was alive */
    FVector CachedFireStartLoc = FVector::ZeroVector;
    /** Last known fire rotation when owner was alive */
    FRotator CachedFireRotation = FRotator::ZeroRotator;
    /** Max time after death to still allow pending fire RPCs (seconds) */
    static constexpr float TradeKillGracePeriod = 0.10f;

    /** Cached weapon skin MIDs — created once in BringUp, reused in SetSkin to avoid per-call GPU allocations */
    UPROPERTY(Transient)
    TArray<UMaterialInstanceDynamic*> CachedSkinMIDs;

    FTimerHandle DeferredActiveStateHandle;
    /**
 * Server-side authoritative fire event index for each fire mode.
 * This is the ground truth that clients must sync to.
 * Replicated to clients for verification.
 */
    UPROPERTY(Replicated)
    TArray<int32> AuthoritativeFireEventIndex;

    /**
     * Client-side fire event counter for each fire mode.
     * Incremented locally on each fire attempt, then validated by server.
     */
    UPROPERTY()
    TArray<int32> ClientFireEventIndex;

    /**
     * Timestamp of last successful fire for each mode (server time).
     * Used for refire rate validation on server.
     */


    /**
     * Replicated active state for each fire mode (0 = inactive, 1 = active).
     * Used to prevent simultaneous fire modes and sync state to non-owning clients.
     */
    UPROPERTY(ReplicatedUsing = OnRep_FireModeState)
    TArray<uint8> FireModeActiveState;

    /**
     * Currently active fire mode (255 = none active).
     * Prevents race conditions from rapid mode switching.
     */
    UPROPERTY()
    uint8 CurrentlyFiringMode;

    /**
     * Server RPC to request firing with full validation.
     *
     * @param FireModeNum - Which fire mode to activate
     * @param InFireEventIndex - Unique sequence number for this fire event
     * @param ClientTimestamp - Client's GetWorld()->GetTimeSeconds() when fire was initiated
     * @param bClientPredicted - Whether client has already predicted this shot
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset);

    /**
     * Server RPC to stop firing.
     *
     * @param FireModeNum - Which fire mode to deactivate
     * @param InFireEventIndex - Final event index from client
     * @param ClientTimestamp - Client's timestamp when fire was stopped
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot); // Added ClientViewRot

    /**
     * Client RPC to confirm a fire event or correct client's event index.
     * This is the critical desync recovery mechanism - server pushes corrections.
     *
     * @param FireModeNum - Which fire mode this applies to
     * @param InAuthorizedEventIndex - Server's authoritative event index (what client should sync to)
     */
    UFUNCTION(Client, Reliable)
    void ClientConfirmFireEvent(uint8 FireModeNum, int32 InAuthorizedEventIndex);

    /**
     * Validates a fire request from the client.
     * Performs multi-layer checks:
     * - Event sequence validity (no duplicate or out-of-order events)
     * - Timestamp sanity (reject if >1s desync)
     * - Refire rate compliance (server-authoritative cooldown)
     *
     * @return true if request is valid and should be processed
     */
    bool ValidateFireRequest(uint8 FireModeNum, int32 InEventIndex, float ClientTime);

    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation")
    float SmoothingMs = 20.0f;

    /** Maximum rewind time allowed in milliseconds */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation")
    float MaxRewindMs = 250.0f;

	/** * Maximum forward-prediction time for projectiles in milliseconds.
		 * Caps how far the server will fast-forward the rocket.
		 * Default: 120.0ms (Epic Standard).
		 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectiles")
	float ProjectilePredictionCapMs = 120.0f;

	/** * Safety buffer subtracted from Ping before calculating prediction.
	 * Absorbs jitter to prevent overshooting the client's view.
	 * Default: 20.0ms.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectiles")
	float FudgeFactorMs = 20.0f;


    /** * Extra radius added to the target capsule during hit validation.
     * Applied ONLY if:
     * 1. The Client claimed a hit on this specific target.
     * 2. The Target is moving (Velocity > 1.0).
     * * Default UT4 value is 40.0f. 
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation")
    float HitScanPadding = 45.0f;
    /**
     * Generates next event index for client-side fire prediction.
     * Uses int32 to prevent overflow issues (stock code used uint8).
     *
     * @return Next sequential event index
     */
    int32 GetNextClientFireEventIndex(uint8 FireModeNum);


    /** * Radius added to STATIONARY targets if client claimed a hit.
     * Small value (e.g. 10.0) to cover idle anims/jitter without allowing "magic hits".
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation")
    float HitScanPaddingStationary = 10.0f;

    /**
     * Validates that a fire event index is in valid sequence.
     * Allows small lookahead (10 events) to handle network reordering,
     * but rejects old/duplicate events.
     *
     * @return true if event index is valid in sequence
     */
    bool IsFireEventSequenceValid(uint8 FireModeNum, int32 InEventIndex);

    /**
     * Replication notify for fire mode state changes.
     * Updates CurrentlyFiringMode on non-owning clients.
     */
    UFUNCTION()
    void OnRep_FireModeState();

    /**
     * Get the hit validation time from the player controller.
     * Uses split prediction's hit validation time if available (120ms default).
     * Falls back to standard GetPredictionTime() if not using split prediction.
     *
     * @return Time in seconds to rewind pawns for hit validation
     */
    virtual float GetHitValidationPredictionTime() const;

    /** Setup replication */
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** Impressive Add On */
    virtual void OnServerHitScanResult(const FHitResult& Hit, float PredictionTime);

	// Helper function for the timer
	void SpawnDelayedFakeProjectile();

	// Timer handle
	FTimerHandle SpawnDelayedFakeProjHandle;

	// RENAMED TO AVOID SHADOWING PARENT CLASS VARIABLE
	UPROPERTY()
	FNetcodeDelayedProjectile NetcodeDelayedProjectile;

	// Guard Rail Cap (120ms)
	const float MaxCatchupTime = 0.10f;

    TArray<FPendingFireEventFix> ResendFireEvents;
    FTimerHandle ResendFireHandle;

    void QueueResendFireFixed(bool bIsStartFire, uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot, uint8 ZOffset, AUTCharacter* ClientHitChar);
    void ResendNextFireEventFixed();
    void ClearFireEventsFixed();

    UFUNCTION(Server, Unreliable, WithValidation)
    void ResendServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot, uint8 ZOffset, AUTCharacter* ClientHitChar);

    UFUNCTION(Server, Unreliable, WithValidation)
    void ResendServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot); // Added ClientViewRot

    UPROPERTY()
    TArray<FPendingFakeProjectile> PendingFakeProjectiles;

    // =========================================================================
    // PROJECTILE REWIND LAG COMPENSATION
    //
    // When a client's fake projectile hits an enemy, the client sends an RPC.
    // The server validates by rewinding the target and checking proximity to
    // the real (authoritative) projectile. Gated by bEnableProjectileRewind.
    // =========================================================================

    /** Master toggle. Set to true in BP defaults to activate projectile rewind. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    bool bEnableProjectileRewind = false;

    /** Max rewind scale at low ping (0.85 = 85% of half-RTT). Never full rewind. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMaxScale = 0.85f;

    /** Ping (ms) below which max scale is applied */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindFullPingMs = 80.0f;

    /** Ping (ms) above which NO rewind is applied. Linear falloff between Full and Max. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMaxPingMs = 120.0f;

    /** Minimum rewind scale at the falloff boundary */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMinScale = 0.15f;

    /** Server RPC: Client's fake projectile hit a target, validate with rewind */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerProjectileHitClaim(AUTCharacter* ClaimedTarget, FVector ClaimedHitLocation,
        int32 ClaimedEventIndex, uint8 ClaimedFireMode);

    /** Server-side tracking of authoritative projectiles by EventIndex */
    UPROPERTY()
    TArray<FActiveServerProjectile> ActiveServerProjectiles;


    // =========================================================================
    // CLIENT-SIDE HITSOUND PREDICTION HELPER
    // =========================================================================

    /** Find and cache the ClientHitsounds mutator from the game state mutator chain */
    AClientHitsounds* FindClientHitsoundsMutator();

    /** Cached pointer to the ClientHitsounds mutator */
    UPROPERTY()
    TWeakObjectPtr<AClientHitsounds> CachedClientHitsounds;
};
