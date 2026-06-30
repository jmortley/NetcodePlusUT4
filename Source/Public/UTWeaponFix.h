// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTWeapon.h"
#include "UTWeaponFix.generated.h"

class UUTWeaponSkin;

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

    // --- Grace buffer (populated when the projectile RESOLVES / explodes) ---
    // Retain a resolved projectile's final state briefly so a claim arriving after the server
    // projectile is gone (close-range timing race) can still rewind-rescue. ExpireTime < 0 means
    // the projectile is still live (not yet resolved).
    UPROPERTY()
    FVector FinalLoc = FVector::ZeroVector;
    UPROPERTY()
    FVector FinalVel = FVector::ZeroVector;
    UPROPERTY()
    float FinalGravityZ = 0.f;
    UPROPERTY()
    float HitRadius = 0.f;
    UPROPERTY()
    float BaseDamage = 0.f;
    UPROPERTY()
    float Momentum = 0.f;
    UPROPERTY()
    TSubclassOf<UDamageType> DamageType;
    UPROPERTY()
    float ExpireTime = -1.f;
    UPROPERTY()
    TWeakObjectPtr<class AUTCharacter> DamagedTarget;

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

    /** Master gate for weapon skin support. Disabled for now: stock skin allocation
     *  (LoadObject + per-instance MID creation) causes a ~1-3ms hitch every life in
     *  duel when you pick up a fresh weapon. Hide/show is independent of this and
     *  stays enabled. Flip to true to re-enable skins once the per-life MID cost
     *  is addressed (e.g., static shared MID pool keyed by WeaponClass+SkinPath). */
    static constexpr bool bSkinsEnabled = false;

    /** Per-weapon hide state — keyed by class name.
     *  Set via "weaponskins" menu or "weaponhand hidden/show" console command.
     *  BringUp() checks this to hide 1P mesh on weapon switch. */
    static TMap<FName, bool> HiddenWeaponsByTag;

    /** Tracer/beam origin offsets used when the firing weapon is hidden.
     *  Plumbed into GetImpactSpawnPosition (camera-relative). Set via the
     *  weaponskins menu; persisted in Mod.ini [NetcodePlus.WeaponSettings]
     *  HiddenBeamBack + HiddenBeamDown. Defaults match the original hardcoded
     *  values (10 back, 35 down = "stomach"). 0 back + 0 down = camera-origin
     *  (beam can render edge-on / invisible when stationary). */
    static float HiddenBeamBackOffset;
    static float HiddenBeamDownOffset;

    /** Saved skin asset paths — keyed by WeaponSkinCustomizationTag.
     *  Loaded from Mod.ini, applied in BringUp(). */
    static TMap<FName, FString> SavedSkinPaths;

    /** Pre-loaded skin assets — keyed by WeaponSkinCustomizationTag.
     *  Loaded eagerly in LoadWeaponSettings() so BringUp() avoids a blocking LoadObject. */
    static TMap<FName, UUTWeaponSkin*> CachedSkinAssets;

    /** Load weapon settings (skins + hide state) from Mod.ini. Called once on first BringUp. */
    static void LoadWeaponSettings();

    /** Save weapon settings to Mod.ini. */
    static void SaveWeaponSettings();

    /** Whether settings have been loaded from Mod.ini this session */
    static bool bWeaponSettingsLoaded;
    //~ Begin AUTWeapon Interface
    virtual void GetImpactSpawnPosition(const FVector& TargetLoc, FVector& SpawnLocation, FRotator& SpawnRotation) override;
    virtual void PlayFiringEffects() override;
    virtual void StartFire(uint8 FireModeNum) override;
    virtual void StopFire(uint8 FireModeNum) override;
    virtual void PostInitProperties() override;
    virtual void Tick(float DeltaTime) override;
    virtual void DetachFromOwner_Implementation() override;
    virtual bool PutDown() override;
    void PutDownDelayed();
    virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;

    /** Get the hit validation prediction time (ping-based rewind amount).
     *  Public so ServerShield can use the same rewind for radial offset analysis. */
    virtual float GetHitValidationPredictionTime() const;

    /** Half-width (seconds) of the server-side bidirectional time-search fallback in
     *  HitScanTrace, used when the client claimed a hit the primary rewind missed.
     *  Per-weapon overridable. Base 30ms; shock/instagib widen to 45ms. */
    virtual float GetHitscanTimeSearchWindow() const { return 0.030f; }
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

    /** Server-side: a tracked projectile (rocket/flak shell) calls this when it resolves (explodes) to
     *  snapshot its final state into ActiveServerProjectiles for the lag-comp grace buffer, so a claim
     *  arriving after the projectile is gone can still rewind-rescue. DamagedChar = the pawn it directly
     *  hit this frame, or null (geometry/whiff) — prevents double-damaging a target that already took the
     *  present-time hit. PUBLIC: called from the UTPlusProj_* classes, which are not AUTWeaponFix subclasses. */
    void OnTrackedProjectileResolved(class AUTProjectile* Proj, class AUTCharacter* DamagedChar);
    UPROPERTY()
    TArray<float> LastFireTime;

    /** Per-fire-mode timestamp of the most recent StopFire call. Used by the
     *  mouse-bounce debounce in StartFire to coalesce rapid release+press
     *  pairs (low-debounce mice, scroll-wheel binds) into a single fire
     *  intent rather than treating them as separate clicks. */
    UPROPERTY()
    TArray<float> LastReleaseTime;

    /** Mouse-bounce debounce window in seconds. A press event arriving
     *  within this many seconds of the prior release is treated as a bounce
     *  (or scroll-wheel rapid-fire) — PendingFire is kept true so any held
     *  intent is preserved, but no new fire event is triggered. Default
     *  30ms sits comfortably between hardware bounce ceiling (~20ms) and
     *  human double-click physiological floor (~40-80ms), so it cannot eat
     *  intentional rapid clicks. Tune higher per-weapon if low-debounce
     *  mice still produce rejected shots; set to 0 to disable entirely. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Weapon")
    float MouseDebounceWindow;

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

    // Ghost-rocket fix (ncp.GhostFix): the REAL fire-button-held state per mode,
    // tracked from genuine input on the locally-controlled client (set in StartFire
    // when !bHandlingRetry, cleared in StopFire on a non-switch release). Read at the
    // PutDown switch boundary so held intent carries across a weapon switch WITHOUT
    // graduating a stale cooldown-retry into a phantom rocket/shock fire.
    bool bFireHeldByPlayer[2];
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
    static constexpr float TradeKillGracePeriod = 0.20f;

    /** Cached weapon skin MIDs — created once in BringUp, reused in SetSkin to avoid per-call GPU allocations */
    UPROPERTY(Transient)
    TArray<UMaterialInstanceDynamic*> CachedSkinMIDs;

public:
    /** Server-side only: impact point from the last FireInstantHit trace.
     *  Written just before TakeDamage so ServerShield can read it in ModifyDamage
     *  for accurate hitbox radial offset computation. Not replicated. */
    FVector LastHitscanImpactPoint = FVector::ZeroVector;

    /** Server-side only: total padded radius used for the hitscan validation
     *  (CollisionRadius + TraceRadius + ExtraHitPadding). For hitplot normalization. */
    float LastHitscanPaddedRadius = 0.0f;

    /** Server-side only (327 client-informed headshot): WHERE the client rendered the claimed target's
     *  head — the offset of its rendered mesh head bone from the target's body. Lets the server place a
     *  NORMAL-size head sphere at the head the player actually saw (forced models render their own head
     *  here), instead of a fixed capsule point. Set in ServerStartFireFixed alongside the stock
     *  ReceivedHitScanHitChar; the headshot gate CLAMPS it into the plausible head band of the rewound
     *  capsule (so a chest/feet claim is impossible) and uses a normal radius (no inflation -> a torso hit
     *  can't be upgraded). Zero = no claim. Not replicated. */
    FVector ReceivedHeadOffset = FVector::ZeroVector;

    // ============================================================
    // Fire-validation sample telemetry. Cheap client-only per-frame tracker + an
    // owner-only gate. The actual per-player accumulation + reporting is server-
    // authoritative and lives in FNCFireValCollector. Active ONLY in Elim /
    // instagib-CTF; dormant (zero cost) everywhere else. Pure telemetry — never
    // affects gameplay/hit-reg/scoring.
    // ============================================================

    /** Owner-only gate, set server-side in BeginPlay and replicated to the owning
     *  client. True only when (Elim or instagib-CTF) AND this weapon is a
     *  UTPlusSniper or UTPlusShockRifle (or child) — i.e. instagib / shock /
     *  sniper / LG. When false the client tracker and the report RPC are skipped
     *  entirely. */
    UPROPERTY(Replicated)
    bool bFireValActive = false;

    /** Client-side: local-clock time (World->GetTimeSeconds()) at which the
     *  crosshair FIRST landed on a visible (occlusion-checked) enemy in the current
     *  continuous run; -1 = not currently on a visible enemy. The interval is read
     *  at fire as (now - FireValAcquireTime), which avoids the per-frame DeltaTime
     *  accumulation and the tick/fire ordering jitter the old counter had. Shipped
     *  full-range in milliseconds (int32, no 255 ms cap), so heavy values no
     *  longer saturate the server-side mean. */
    float FireValAcquireTime = -1.0f;

    /** Client-side: the enemy the crosshair is currently resting on. When the
     *  traced enemy changes (target-to-target snap), the acquire instant resets so
     *  the new target starts from zero. Weak so a destroyed/reused actor address
     *  can't masquerade as the same target. */
    TWeakObjectPtr<class AUTCharacter> FireValLastTarget;

    /** Client-side: smoothed (EMA) frame time in seconds, sent alongside each
     *  sample as fps context. The low band is still frame-quantized near zero (a
     *  60 fps client cannot produce a sub-16 ms value), so review must be able to
     *  see each player's frame rate to discount that bias. */
    float FireValFrameTimeEMA = 0.0f;

    /** Per-frame client tracker: one occlusion-aware crosshair line trace. */
    void UpdateFireValTracker(float DeltaTime);

    /** Finds the bot-events mutator (the telemetry sink). Null on non-bot servers. */
    class AMutBotEvents* FindBotEventsMutator() const;

protected:

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
    void ServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset);

    /**
     * Server RPC to stop firing.
     *
     * @param FireModeNum - Which fire mode to deactivate
     * @param InFireEventIndex - Final event index from client
     * @param ClientTimestamp - Client's timestamp when fire was stopped
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, FRotator ClientViewRot); // Added ClientViewRot

    /** Telemetry sidecar — reports a client-side fire-validation sample at the
     *  moment of a hitscan fire the client believes connected. UNRELIABLE on
     *  purpose: a dropped sample only thins the distribution, and this must never
     *  compete with the reliable fire RPCs for bandwidth. Server-side it is clamped
     *  and routed to FNCFireValCollector; it has ZERO effect on gameplay, hit
     *  validation, or scoring. Separate from ServerStartFireFixed by design — the
     *  hit-reg path is never touched. */
    UFUNCTION(Server, Unreliable, WithValidation)
    void ServerReportFireValidation(int32 SampleMs, uint8 FrameMs, bool bClaimedHit);

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

    /** Max rewind scale at low ping (1.0 = full half-RTT compensation). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMaxScale = 1.0f;

    /** Ping (ms) below which max scale is applied. <=100ms = full half-RTT rewind. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindFullPingMs = 100.0f;

    /** Ping (ms) above which NO rewind is applied (hard cliff to zero).
     *  Linear falloff between FullPingMs and this value, from MaxScale → MinScale. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMaxPingMs = 150.0f;

    /** Minimum rewind scale at the falloff boundary. 0.5 = half rewind at 150ms ping. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectile Rewind")
    float ProjectileRewindMinScale = 0.5f;

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
