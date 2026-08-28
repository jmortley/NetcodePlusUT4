// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTWeapon.h"
#include "NCFriendlyTargetProbeCache.h"
#include "UTWeaponFix.generated.h"

class UUTWeaponSkin;
class UMaterialInstanceDynamic;

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
class AUTCharacter;
class AUTPlayerController;
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

/** What produced a delayed Flak prediction request. Shell-impact fragments are deliberately
 * absent: they are spawned authoritatively by AUTProj_FlakShell::Explode() and never enter
 * the weapon prediction path. */
enum class ENetcodeDelayedFlakKind : uint8
{
    PrimaryShard,
    SecondaryShell
};

/** One immutable delayed Flak fake request. Primary fire owns nine independent records even
 * though all shards share one fire-event index; secondary owns one shell record. */
struct FNetcodeDelayedFlakProjectile
{
    TSubclassOf<AUTProjectile> ProjectileClass;
    FVector SpawnLocation;
    FRotator SpawnRotation;
    FTimerHandle TimerHandle;
    uint8 FireMode;
    int32 EventIndex;
    uint32 ReservationId;
    int32 ProjectileOrdinal;
    float RequestTime;
    ENetcodeDelayedFlakKind Kind;

    FNetcodeDelayedFlakProjectile()
        : ProjectileClass(nullptr)
        , SpawnLocation(FVector::ZeroVector)
        , SpawnRotation(FRotator::ZeroRotator)
        , FireMode(0)
        , EventIndex(INDEX_NONE)
        , ReservationId(0)
        , ProjectileOrdinal(0)
        , RequestTime(0.f)
        , Kind(ENetcodeDelayedFlakKind::PrimaryShard)
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
    TWeakObjectPtr<AUTCharacter> HitChar;
    FVector ClientHeadOffset;

    FPendingFireEventFix(uint8 Mode, int32 EventIdx)
        : bIsStartFire(false)
        , FireModeNum(Mode)
        , FireEventIndex(EventIdx)
        , ClientTimestamp(0.0f)
        , ClientViewRot(FRotator::ZeroRotator)
        , ZOffset(0)
        , HitChar(nullptr)
        , ClientHeadOffset(FVector::ZeroVector)
    {
    }

    FPendingFireEventFix(uint8 Mode, int32 EventIdx, float Timestamp, FRotator ViewRot,
        AUTCharacter* InChar, uint8 Z, FVector HeadOffset)
        : bIsStartFire(true)
        , FireModeNum(Mode)
        , FireEventIndex(EventIdx)
        , ClientTimestamp(Timestamp)
        , ClientViewRot(ViewRot)
        , ZOffset(Z)
        , HitChar(InChar)
        , ClientHeadOffset(HeadOffset)
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
        : FireMode(0)
    {
    }

    FActiveServerProjectile(AUTProjectile* InProj, uint8 InMode)
        : Projectile(InProj)
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

    /** Master gate for the NetcodePlus weapon-skin path. */
    static constexpr bool bSkinsEnabled = true;

    /** Per-weapon hide state — keyed by class name.
     *  Set via "weaponskins" menu or "weaponhand hidden/show" console command.
     *  BringUp() checks this to hide 1P mesh on weapon switch. */
    static TMap<FName, bool> HiddenWeaponsByTag;

    /** Apply or restore the hidden-weapon state. Two selectable styles:
     *  DEFAULT (bClassicWeaponHide=false) = BP-parity, rendering-only —
     *  SetVisibility(propagate) on the gun mesh + arm-bone hiding on the shared
     *  FirstPersonMesh; bHiddenInGame never touched (stock owns that flag:
     *  zoom/overlay/spectate), so the stock muzzle-socket beam origin keeps
     *  working while hidden. CLASSIC (true) = the pre-BP behavior: SetHiddenInGame
     *  on gun + FirstPersonMesh with the camera-relative beam origin
     *  (GetImpactSpawnPosition below). The SHOW path heals both styles' residue —
     *  but ONLY state this code set itself (ComponentTags on the touched meshes),
     *  so it never fights the old UT+ BP content hiding guns/arms with the same
     *  primitives — and re-seats via stock UpdateWeaponHand(), which re-applies the viewer's
     *  weapon-position preference (Lowered/Very Low = LowMeshOffset/
     *  VeryLowMeshOffset) — a bare archetype reset here is what broke Very Low
     *  for visible weapons in the 2026-07-19 roll. Works on any AUTWeapon (stock
     *  included). Callers: BringUp, TeamArenaCharacter's weapon-swap detector,
     *  the "weaponhand hidden/show" console command, and the weaponskins menu. */
    static void ApplyWeaponHideState(AUTWeapon* Weapon, AUTCharacter* Char, bool bHide);

    /** Hidden-weapon style toggle (weaponskins menu; Mod.ini
     *  [NetcodePlus.WeaponSettings] ClassicWeaponHide). False (default) = BP-parity
     *  visibility-only hide, beam from the live muzzle socket. True = classic
     *  camera-relative beam (SetHiddenInGame + GetImpactSpawnPosition override
     *  reading the HiddenBeam offsets below) — kill-switch back to the pre-2026-07-19
     *  behavior. Client-local render state only. */
    static bool bClassicWeaponHide;

    /** Tracer/beam origin offsets for CLASSIC hide (bClassicWeaponHide) — inert in
     *  the default BP-parity style, which keeps the stock muzzle-socket origin.
     *  Set via the weaponskins menu sliders; persisted in Mod.ini
     *  [NetcodePlus.WeaponSettings] HiddenBeamBack + HiddenBeamDown. Defaults match
     *  the original hardcoded values (10 back, 35 down = "stomach"). */
    static float HiddenBeamBackOffset;
    static float HiddenBeamDownOffset;

    /** Saved skin asset paths — keyed by WeaponSkinCustomizationTag.
     *  Loaded from Mod.ini, applied in BringUp(). */
    static TMap<FName, FString> SavedSkinPaths;

    /** Locally selected skin assets — keyed by WeaponSkinCustomizationTag.
     *  Borrowed from the session catalog loaded by LoadWeaponSettings(). */
    static TMap<FName, UUTWeaponSkin*> CachedSkinAssets;

    /** Load weapon settings and retain selected skins before gameplay. */
    static void LoadWeaponSettings();

    /** Retry skin assets that were unavailable until the map packages mounted. */
    static void RetryPendingWeaponSkins();

    /** Release weapon-skin assets retained by LoadWeaponSettings(). */
    static void CleanupWeaponSettings();

    /** Return a skin only when it belongs to the preloaded, server-approved catalog. */
    static UUTWeaponSkin* FindPreloadedWeaponSkin(const FString& SkinPath);

    /** Snapshot the preloaded catalog for the F5 selector without loading assets. */
    static void GetPreloadedWeaponSkins(TArray<UUTWeaponSkin*>& OutSkins);

    /** True when Skin targets WeaponClass or one of its native/Blueprint parents. */
    static bool IsWeaponSkinCompatible(UUTWeaponSkin* Skin, UClass* WeaponClass);

    /** Find the authoritative entry for WeaponClass in a replicated stock skin array. */
    static UUTWeaponSkin* FindWeaponSkinForClass(
        const TArray<UUTWeaponSkin*>& WeaponSkins, UClass* WeaponClass);

    /** Resolve the local F5 selection for this weapon from the preloaded catalog. */
    static UUTWeaponSkin* GetConfiguredWeaponSkin(const AUTWeapon* Weapon);

    /** Resolve the configured object path; empty means the explicit stock default. */
    static FString GetConfiguredWeaponSkinPath(const AUTWeapon* Weapon);

    /** Save weapon settings to Mod.ini. */
    static void SaveWeaponSettings();

    /** Whether settings have been loaded from Mod.ini this session */
    static bool bWeaponSettingsLoaded;

    /** uint32-backed material-slot masks support slots 0 through 31. */
    static constexpr int32 MaxWeaponSkinTargetSlots = 32;

    /** Bitmask of mesh material slots a weapon family renders its skin on, for one
     *  view. Bit N = slot N. Verified in-editor against the shipped skin assets'
     *  own texture sets:
     *    Flak          — 1P and 3P both {0,1}: FlakVoid's M_Flak_Skin_Void01_P /
     *                    M_Flak_Skin_Void01 replace M_Flak_Gun_Inst /
     *                    M_Flak_Gun_3P_Inst, and BOTH Flak meshes carry that body
     *                    material on slot 0 AND slot 1.
     *    Lightning Gun — single-material fallback is asymmetric: 1P {0}, 3P {1}.
     *                    PinkLG is expanded by GetResolvedWeaponSkinTargetSlotMask()
     *                    to {0,1} in both views, with a different E0/E1 material on
     *                    each slot. Its 1P
     *                    MAT_INS_LG_Pink_E0_1p carries the PartTWO texture set
     *                    (T_LightingGunTwo_*), which Lightning_Gun_1p has on slot 0;
     *                    its 3P MAT_INS_LG_Pink_E1_3p carries the PartONE set
     *                    (T_LightingGun_one_*), which Lightning_Gun_3p has on slot 1.
     *                    The authored E0/E1 names ARE the element indices. Writing the
     *                    other slot paints a section with the wrong part's textures.
     *    everything else — {0}, i.e. unchanged stock behaviour.
     *  Keyed on the replicated WeaponSkinCustomizationTag — loads no asset. Slot names
     *  on these meshes are unreliable, so these are verified explicit indices and every
     *  caller bounds-checks each slot against the live GetNumMaterials(). Slots outside
     *  the resolved mask (Shock screen, ammo counters, decals, glass) stay owned by
     *  the mesh / SetupSpecialMaterials(). */
    static uint32 GetWeaponSkinTargetSlotMask(FName WeaponSkinCustomizationTag,
        bool bFirstPersonMesh);

    /** Resolve the final slot mask for one skin/view. Authentic invisibility
     *  materials replace every live mesh slot; the five Ghost skins replace all
     *  1P slots but explicitly target zero 3P slots; PinkLG uses both E0/E1 slots;
     *  other authored skins retain the verified per-family mask above. */
    static uint32 GetResolvedWeaponSkinTargetSlotMask(const UUTWeaponSkin* Skin,
        FName WeaponSkinCustomizationTag, bool bFirstPersonMesh,
        int32 MaterialSlotCount);

    /** Material for one targeted slot. Most skins return their one per-view material;
     *  Ghost skins resolve the stock pickup hologram only in 1P and restore stock in
     *  3P; PinkLG loads its MutAnnouncers-cooked E1_1p / E0_3p supplement so both
     *  multipart LG sections receive the matching texture set. */
    UMaterialInterface* GetResolvedWeaponSkinMaterialForSlot(
        const UUTWeaponSkin* Skin, bool bFirstPersonMesh, int32 MaterialSlot);

    /** Apply an already-resolved selection to the correct material slots for that
     *  skin/view; authority also keeps pickup identity. */
    void ApplyResolvedWeaponSkin(UUTWeaponSkin* Skin);

    //~ Begin AUTWeapon Interface
    /** CLASSIC hide only (bClassicWeaponHide + weapon hidden): camera-relative beam
     *  origin using the HiddenBeam offsets. In the default BP-parity style this is
     *  a pure pass-through to Super (live muzzle socket). */
    virtual void GetImpactSpawnPosition(const FVector& TargetLoc, FVector& SpawnLocation, FRotator& SpawnRotation) override;
    /** CLASSIC hide only: suppress the muzzle flash for the firing mode — the PSC
     *  sits on the hidden weapon's muzzle socket while the beam spawns from the
     *  camera-adjusted origin. BP-parity style relies on SetVisibility(propagate)
     *  silencing the PSC children instead, so this passes through untouched. */
    virtual void PlayFiringEffects() override;
    virtual void StartFire(uint8 FireModeNum) override;
    virtual void StopFire(uint8 FireModeNum) override;
    virtual bool ShouldDrawFFIndicator(APlayerController* Viewer,
        AUTPlayerState*& HitPlayerState) const override;

    /** Server-authoritative fire policy hook. Return false to hard-reject a fire mode
     *  at every server fire entry (ServerStartFireFixed and the resend funnel) BEFORE any
     *  trade-kill spawn, SetPendingFire latch, or state entry. A subclass restriction that
     *  lives only in BeginFiringSequence is bypassable: ServerStartFireFixed latches
     *  PendingFire before that gate, and stock UUTWeaponStateActive::BeginState then
     *  auto-enters the firing state from the latched flag without re-consulting the gate.
     *  Vetoing the mode here closes that path. Base allows every mode. */
    virtual bool AllowServerFireMode(uint8 FireModeNum) const { return true; }

    virtual void PostInitProperties() override;
    virtual void Tick(float DeltaTime) override;
    virtual void DetachFromOwner_Implementation() override;
    virtual bool PutDown() override;
    void PutDownDelayed();
    virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;

    /** Get the hit validation prediction time (ping-based rewind amount).
     *  Public so ServerShield can use the same rewind for radial offset analysis. */
    virtual float GetHitValidationPredictionTime() const;

    /** Read ACK-derived full RTT from the server's connection. False until a
     *  usable measurement exists; callers must reject or use a zero base epoch
     *  rather than fall back to the client's writable ExactPing. */
    static bool GetServerObservedRTTMs(const AUTPlayerController* ShooterPC,
        float& OutRTTMs);

    /** Server-live full-RTT buffer used by hitscan target rewind. Kept separate
     *  from FudgeFactorMs so hitscan tuning cannot change projectile catch-up
     *  or delayed-fake timing. */
    static float GetConfiguredHitscanFudgeMs();

    /** Half-width (seconds) of the server-side bidirectional time-search fallback in
     *  HitScanTrace, used when the client claimed a hit the primary rewind missed.
     *  Per-weapon overridable. Standard 45ms for ALL hitscan (2026-07-07: sniper/LG
     *  raised from 30ms to match the shock family — the search probes fixed 15ms
     *  rungs {15,30,45}, so 45 is the last rung before the ±60 defender tradeoff). */
    virtual float GetHitscanTimeSearchWindow() const { return 0.045f; }

    /** Opt-in for render-authoritative target sampling in HitScanTrace (legacy
     *  cvar name ncp.RenderCredit). For a remote human using claim-incapable
     *  fire, the estimated render-time capsule replaces raw validation history
     *  as the sole target-selection sample. Ray, world clipping, spread, and
     *  timing estimate remain server-owned — no client hit result is trusted.
     *  Off per weapon by default; continuous/spread modes opt in. */
    virtual bool SupportsRenderCredit() const { return false; }

    /** True only while a character is live, visible, and has capsule geometry.
     *  Collision state is deliberately ignored because live feigning players
     *  have a NoCollision capsule but manual hitscan must still hit them. Every
     *  selector and final damage/stat gate uses this predicate so a retained
     *  corpse cannot be rewound into a valid hit. */
    static bool IsLiveHitscanTarget(const AUTCharacter* Target);

    /** Slide-posture selection for hitscan capsule tests. A floor slide shrinks the
     *  authoritative capsule the same frame it starts, but a remote shooter keeps
     *  rendering a mostly-standing body for one replication interp plus the animBP
     *  blend-in — shots aimed at that rendered torso were unhittable air on the
     *  server. Within ncp.SlideGraceMs of the target's slide start (rewind-adjusted
     *  via RewindTime; slide age is reconstructible because PerformFloorSlide
     *  re-stamps FloorSlideTapTime at true slide start), substitutes the
     *  bottom-aligned STANDING capsule envelope, which strictly contains the slide
     *  capsule; afterwards applies the classic SlideTargetHeight shrink. Mutates
     *  the test location/half-height in place; no-op for non-sliding targets.
     *  Shared by HitScanTrace, the claim time-search fallback, FireCone's pawn
     *  sweep, the Enforcer trace, AND all three projectile rewind tests (the
     *  catchup spawn sweep, the post-fast-forward overlap check, and
     *  ServerProjectileHitClaim's per-sample anchor search + contact test) so
     *  every validation path — hitscan and projectile — judges one posture. */
    static void ApplySlidePostureForValidation(const AUTCharacter* Target,
        float RewindTime, FVector& InOutTargetLocation, float& InOutCollisionHeight);

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
    virtual void UpdateOutline() override;
    virtual TArray<UMeshComponent*> Get1PMeshes_Implementation() const override;
    void ClearPendingFakeProjectiles();
    void DeferredGotoActiveState(uint8 FireModeNum);
    virtual void Removed() override;
    virtual void Destroyed() override;
    //~ End AUTWeapon Interface

    // =========================================================================
    // PROJECTILE REWIND SYSTEM
    // Called by UTPlusProj_Rocket / UTPlusProj_FlakShell when fake hits a pawn.
    // Sends ServerProjectileHitClaim RPC if bEnableProjectileRewind is true.
    // =========================================================================
    /** @param SourceProj  The projectile reporting the hit. Callers resolve `this` weapon from
     *                     UTCharacter::GetWeapon() at IMPACT time, which is the weapon currently
     *                     HELD — not necessarily the one that fired. Fire a rocket, switch to flak,
     *                     rocket lands: `this` is the flak cannon. Passing the projectile lets the
     *                     hitsound prediction read damage off the instance that actually hit,
     *                     instead of ProjClass[FireModeNum] on the wrong weapon. Optional: a null
     *                     SourceProj keeps the legacy CDO lookup. */
    void NotifyFakeProjectileHit(AUTCharacter* HitTarget, const FVector& HitLocation, uint8 FireModeNum,
        AUTProjectile* SourceProj = nullptr);

    /** Resolve the weapon that FIRED Proj, rather than the one OwnerChar happens to be holding.
     *  AUTCharacter::GetWeapon() is evaluated at IMPACT: fire a rocket, switch to flak, and the
     *  rocket's claim routes to the flak cannon, whose ActiveServerProjectiles never held it, so
     *  the server drops the claim and that shot silently loses lag compensation. Matching on the
     *  projectile's exact class is unambiguous — each claim-capable class comes from exactly one
     *  weapon. Falls back to the held weapon when nothing was recorded, so the worst case is the
     *  behaviour that shipped. Call this instead of GetWeapon() from projectile impact handlers. */
    static AUTWeaponFix* FindFiringWeaponForProjectile(AUTCharacter* OwnerChar, AUTProjectile* Proj);

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
     *  intent is preserved, but no new fire event is triggered.
     *  ⚠ 2026-07-17: the runtime CAPS this at ncp.MouseDebounceCap (default
     *  0.01) — the old 30ms rationale ("bounce ceiling ~20ms vs double-click
     *  floor ~40-80ms") doesn't survive modern mice: optical switches can't
     *  bounce, mechanicals debounce in firmware, and fast tap-fire
     *  release->press gaps dip under 30ms (eaten clicks on Viper V3-class
     *  mice, worst at high fps). Effective window = min(this, cap). This BP
     *  value now mostly matters as a lower-than-cap override; set 0 to
     *  disable for a weapon. Raising it above the cap has no effect unless
     *  the cap cvar is raised/-1 too. */
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
    void OnBufferedClickRetryTimer(uint8 FireModeNum, FRotator ReleaseAim, float ReleaseTime);
    /** State/game-driven stops must not masquerade as a physical mouse release. */
    void StopFireInternal(uint8 FireModeNum);
    /** Same guard, but route through AUTCharacter::StopFire for owner-side cleanup. */
    void StopOwnerFireInternal(uint8 FireModeNum);
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

    /** Client-only crosshair presentation state; see FNCFriendlyTargetProbeCache. */
    mutable FNCFriendlyTargetProbeCache FriendlyTargetProbeCache;

    /** Common server RTT-to-rewind conversion. Hitscan passes the live cvar;
     *  the legacy projectile-origin path passes its per-weapon field. */
    float GetPredictionTimeWithFudgeMs(float InFudgeMs) const;

    FTimerHandle DelayedPutDownHandle;
    bool bHandlingRetry;
    FTimerHandle RetryFireHandle[2];

    /** True when the charged-rocket state can no longer self-transition: none of its four
     *  timers (load / grace / burst / post-burst refire) are active. A
     *  legitimately active charged state ALWAYS has one of those in flight, so this is the
     *  wedge signature (the state that silently swallows primaries — see the Tick watchdog
     *  and the ServerStartFireFixed fast recovery). Callers must branch on logical
     *  NumLoadedRockets only; NumLoadedBarrels tracks loading/visual state and can remain
     *  stale after a completed spread volley, so it is not a count of unfired projectiles. */
    bool IsChargedRocketStateWedged(class UUTWeaponStateFiringChargedRocket_Transactional* Chg);

    /** First watchdog tick a wedge was observed; -1 = not currently observed.
     *  Debounces the fast recovery (~0.25s) so a transient no-timer instant between state
     *  callbacks can't false-trigger it. Reset on recovery, on busy, and on leaving the
     *  charged state. */
    float ChargedWedgeFirstSeenTime = -1.f;

    // Ghost-rocket fix (ncp.GhostFix): the REAL fire-button-held state per mode,
    // tracked from genuine input on the locally-controlled client (set in StartFire
    // when !bHandlingRetry, cleared in StopFire on a non-switch release). Read at the
    // PutDown switch boundary so held intent carries across a weapon switch WITHOUT
    // graduating a stale cooldown-retry into a phantom rocket/shock fire.
    bool bFireHeldByPlayer[2];

    // True while RetryFireHandle[mode] is armed BY THE CROSS-MODE stall-fix block
    // (ncp.CrossModeRetry) rather than the same-mode cooldown paths. The legacy
    // (ncp.GhostFix=0) PutDown retry-graduation must NOT graduate these — a tapped
    // cross-mode press followed by a fast weapon switch would become a ghost shot
    // on the next weapon. Set only at the cross-mode arm site; every other arm site
    // overwrites it false (the graduation's IsTimerActive guard covers cleared timers).
    bool bCrossModeRetryArmed[2];

    // True while RetryFireHandle[mode] holds a BUFFERED CLICK. In 328 dogfood
    // this is restricted to Shock-derived primary: a cooldown-blocked press
    // RELEASED within ncp.ClickBufferMs keeps release-time aim in the timer
    // payload. OnBufferedClickRetryTimer executes once at legal fire time.
    // Cleared by replacement input and weapon lifecycle; never graduated to
    // pawn PendingFire because it is spent click intent, not a held button.
    bool bBufferedClickPending[2];

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

    /** Per-slot originals captured before NetcodePlus applies a configured skin. */
    UPROPERTY(Transient)
    TArray<UMaterialInterface*> OriginalFPSMaterials;

    /** Immutable selected body-material parent per slot, or nullptr for Default. */
    UPROPERTY(Transient)
    TArray<UMaterialInterface*> AppliedFPSMaterialParents;

    /** One actor-local selected-material MID per targeted slot. Separate MIDs keep
     *  SetupSpecialMaterials() changes isolated to the slot they configure. */
    UPROPERTY(Transient)
    TArray<UMaterialInstanceDynamic*> AppliedFPSMaterialInstances;

    uint32 AppliedFPSMaterialSlotMask;
    bool bCapturedOriginalFPSMaterials;

    /** Custom-depth silhouette paired with a stock pickup-hologram FPS material.
     *  The regular Mesh remains the visible, animated 1P weapon; this owner-only
     *  master-pose slave supplies the depth sampled by M_HoloEffect. */
    UPROPERTY(Transient)
    USkeletalMeshComponent* FirstPersonHologramDepthMesh;

    bool bFirstPersonHologramSkinActive;

    void PrepareConfiguredWeaponSkin();
    void ApplyFirstPersonHologramProjectionParams();
    void UpdateFirstPersonHologramDepthMesh(bool bEnable);
    void DestroyFirstPersonHologramDepthMesh();

public:
    /** Server-side only: impact point from the last FireInstantHit trace.
     *  Written just before TakeDamage so ServerShield can read it in ModifyDamage
     *  for accurate hitbox radial offset computation. Not replicated. */
    FVector LastHitscanImpactPoint = FVector::ZeroVector;

    /** Server-side only: total padded radius used for the hitscan validation
     *  (CollisionRadius + TraceRadius + ExtraHitPadding). For hitplot normalization. */
    float LastHitscanPaddedRadius = 0.0f;

    /** Server-side only: set when THIS trace's pawn result was rejected by the
     *  unclaimed-hit render gate, or render-authoritative claimless targeting
     *  selected no pawn. Head-sphere fallbacks (base FireInstantHit and sniper
     *  subclasses) must not resurrect a raw-history target from the same ray.
     *  Reset at every HitScanTrace entry. */
    bool bLastUnclaimedRenderDemoted = false;

    /** Server-side only (327 client-informed headshot): WHERE the client rendered the claimed target's
     *  head — the offset of its rendered mesh head bone from the target's body. Lets the server place a
     *  NORMAL-size head sphere at the head the player actually saw (forced models render their own head
     *  here), instead of a fixed capsule point. Set in ServerStartFireFixed alongside the stock
     *  ReceivedHitScanHitChar; the headshot gate CLAMPS it into the plausible head band of the rewound
     *  capsule (so a chest/feet claim is impossible) and uses a normal radius (no inflation -> a torso hit
     *  can't be upgraded). Zero = no claim. Not replicated. */
    FVector ReceivedHeadOffset = FVector::ZeroVector;

protected:

    FTimerHandle DeferredActiveStateHandle;
    /** Server-side ground truth for the last accepted fire event in each mode.
     *  Owning clients are corrected explicitly through ClientConfirmFireEvent. */
    UPROPERTY(Transient)
    TArray<int32> AuthoritativeFireEventIndex;

    /** Server-side deduplication for initial/retried stop events. */
    UPROPERTY(Transient)
    TArray<int32> LastProcessedStopEventIndex;

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
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp,
        FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset);

    /**
     * Server RPC to stop firing.
     *
     * @param FireModeNum - Which fire mode to deactivate
     * @param InFireEventIndex - Final event index from client
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex);

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

    /** Shared RPC-edge validation so initial and retry start payloads use identical checks. */
    bool ValidateStartFireFixedPayload(uint8 FireModeNum, int32 InFireEventIndex,
        float ClientTimestamp, FRotator ClientViewRot, FVector ClientHeadOffset);

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

	/** Legacy projectile catch-up / delayed-fake safety buffer. Server hitscan
	 * target rewind uses ncp.HitscanFudgeMs instead.
	 * Default: 20.0ms.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Projectiles")
	float FudgeFactorMs = 20.0f;


    /** Legacy asset field retained for serialized Blueprint compatibility.
     * Moving claimed-target primary padding is controlled live by
     * ncp.HitscanPrimaryPadding instead.
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
	virtual void SpawnDelayedFakeProjectile() override;
	void SpawnDelayedFlakFakeProjectile(uint32 ReservationId);
	void ClearDelayedFlakFakeProjectiles();
	AUTProjectile* SpawnNetPredictedProjectileInternal(
		TSubclassOf<AUTProjectile> ProjectileClass,
		FVector SpawnLocation,
		FRotator SpawnRotation,
		uint8 CapturedFireMode,
		int32 CapturedEventIndex,
		bool bAllowDelay);

	// Timer handle
	FTimerHandle SpawnDelayedFakeProjHandle;

	// RENAMED TO AVOID SHADOWING PARENT CLASS VARIABLE
	UPROPERTY()
	FNetcodeDelayedProjectile NetcodeDelayedProjectile;

	/** Flak-only delayed predictions. Kept separate so a nine-shard volley cannot collapse
	 * into the legacy single payload and so rocket behavior remains unchanged while its
	 * independent M1 cadence problem is being diagnosed. */
	TArray<FNetcodeDelayedFlakProjectile> DelayedFlakProjectiles;
	uint32 NextDelayedFlakReservationId;

	// Guard Rail Cap (120ms)
	const float MaxCatchupTime = 0.10f;

    TArray<FPendingFireEventFix> ResendFireEvents;
    FTimerHandle ResendFireHandle;

    void QueueResendStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp,
        FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset);
    void QueueResendStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex);
    void QueueResendFireEventFixed(const FPendingFireEventFix& Event);
    void ResendNextFireEventFixed();
    void ClearFireEventsFixed();

    UFUNCTION(Server, Unreliable, WithValidation)
    void ResendServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp,
        FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset, FVector ClientHeadOffset);

    UFUNCTION(Server, Unreliable, WithValidation)
    void ResendServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex);

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
        uint8 ClaimedFireMode);

    /** Server-side tracking of authoritative projectiles, matched oldest-first by fire mode. */
    UPROPERTY()
    TArray<FActiveServerProjectile> ActiveServerProjectiles;

    /** Every projectile class this weapon has spawned, recorded on BOTH server and the firing
     *  client (the client records it when it spawns the fake). Not replicated and never needs to
     *  be: each side populates its own copy from its own spawn. Bounded by the number of distinct
     *  projectile classes a weapon can fire (1-3), so it is add-unique and never cleared. */
    UPROPERTY()
    TArray<TSubclassOf<AUTProjectile>> NCPFiredProjClasses;


    // =========================================================================
    // CLIENT-SIDE HITSOUND PREDICTION HELPER
    // =========================================================================

    /** Find and cache the ClientHitsounds mutator from the game state mutator chain */
    AClientHitsounds* FindClientHitsoundsMutator();

    /** Damage the hitsound prediction should assume for a hit in this fire mode.
     *  bHeadshotClaimed is true when the shot is sending a head claim
     *  (ClientHeadOffset non-zero). Base weapons ignore the claim and return the
     *  fire mode's InstantHitInfo damage; headshot-capable weapons override and
     *  return their headshot damage so the predicted cue matches what the server
     *  will report if the claim validates. Estimation only — never used for
     *  actual damage. */
    virtual int32 GetPredictedHitsoundDamage(uint8 FireModeNum, bool bHeadshotClaimed);

    /** Cached pointer to the ClientHitsounds mutator */
    UPROPERTY()
    TWeakObjectPtr<AClientHitsounds> CachedClientHitsounds;
};
