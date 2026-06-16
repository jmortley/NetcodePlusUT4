// TeamArenaCharacter.h
#pragma once
#include "NetcodePlus.h"
#include "UTCharacter.h"
#include "UTCarriedObject.h"
#include "UTCharacterMovement.h"
#include "UTRecastNavMesh.h"
#include "UTHat.h"
#include "UTHatLeader.h"
#include "UTEyewear.h"
#include "TeamArenaCharacter.generated.h"


class UTeamArenaCharacterMovement;

/**
 * Enhanced character that uses split prediction for movement.
 * 
 * Key change: OnRep_ReplicatedMovement() uses GetVisualPredictionTime()
 * instead of GetPredictionTime() for client-side extrapolation.
 * 
 * This eliminates the "dual hitbox" bug where enemies appear to have
 * different positions visually vs where the server validates hits.
 * 
 * Visual position (predict 0) ≈ Server position (now)
 * Hit validation (predict 120) ≈ What shooter saw when they fired
 */
UCLASS()
class NETCODEPLUS_API ATeamArenaCharacter : public AUTCharacter
{
    GENERATED_BODY()

public:
    ATeamArenaCharacter(const FObjectInitializer& ObjectInitializer);

    virtual void BecomeViewTarget(APlayerController* PC) override;
	// The material to use for the overlay (Assign M_ShieldBelt_Overlay here in BP)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection")
	UMaterialInterface* SpawnProtectionMaterial;

	// The color and opacity to force on the overlay (R=1, G=0.9, B=0, A=0.7 for Gold/70%)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection")
	FLinearColor SpawnProtectionColor = FLinearColor(1.0f, 0.9f, 0.0f, 0.7f);

	// Add this property to control Opacity explicitly in BP
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spawn Protection", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SpawnProtectionOpacity = 0.7f;

	virtual bool IsHeadShot(FVector HitLocation, FVector ShotDirection, float WeaponHeadScaling,
		AUTCharacter* ShotInstigator, float PredictionTime) override;

	virtual void Tick(float DeltaTime) override;

    /**
     * Override replication callback to use visual prediction time.
     * This is THE critical change for split prediction.
     */
    //virtual void OnRep_ReplicatedMovement() override;
    // Add override declaration
    virtual void UTUpdateSimulatedPosition(const FVector& NewLocation, const FRotator& NewRotation, const FVector& NewVelocity) override;

    UPROPERTY()
    float SmoothedStabilityFactor = 1.0f;
    // Override to decouple visual effects from standard UTPlayerController prediction
    virtual void FiringInfoUpdated() override;

    // Override to use custom prediction time for position rewinding
    virtual FVector GetRewindLocation(float PredictionTime, AUTPlayerController* DebugViewer = NULL) override;

    //virtual void PositionUpdated(bool bShotSpawned) override;
	virtual void BeginPlay() override;

	virtual FVector GetHeadLocation(float PredictionTime = 0.f)  override;

	// Allow Blueprints to read the version number from your header file
	UFUNCTION(BlueprintPure, Category = "NetcodePlus")
	static int32 GetNetcodeVersion();
    /** Rate at which to save positions for lag compensation (Hz). Default 120. */
    //UPROPERTY(EditAnywhere, Category = "Team Arena|Optimization")
    //float PositionSaveRate;

    // Add to protected section:
    /** Last time we saved a position (for throttling) */
    //float LastPositionSaveTime;

    /** Calculated interval between position saves */
    //float PositionSaveInterval;


protected:
    /**
     * Get the client's visual prediction time from the viewing controller.
     * Returns 0ms if using TeamArenaPredictionPC (no extrapolation).
     * 
     * @return Prediction time in seconds for visual movement extrapolation
     */
    float GetClientVisualPredictionTime() const;

    /**
     * Cached reference to viewing controller (for performance).
     * Updated when controller changes.
     */
    UPROPERTY()
    class ATeamArenaPredictionPC* CachedPredictionPC;

    /**
     * Whether we've already tried to cache the prediction controller.
     * Prevents repeated casts every frame.
     */
    bool bHasCachedPC;

	bool bHasSpawnOverlay = false;

	// --- Performance: Spawn protection dirty flag ---
	// Tracks last bShowGlowToViewer state to skip redundant material updates
	// Initialized to 0xFF (invalid) to force first-frame apply
	uint8 bLastShowGlowState = 0xFF;

	// --- Performance: OverlayMesh throttle ---
	// Timestamp of last OverlayMesh->MarkRenderStateDirty() allowed through Super::Tick.
	// Time-based throttle at 60Hz — independent of render frame rate (480/720/etc).
	float LastOverlayDirtyTime = 0.f;

	// --- Per-weapon hide tracking ---
	// Tracks last equipped weapon to detect weapon switches and apply hide state
	UPROPERTY()
	AUTWeapon* LastEquippedWeapon = nullptr;

	// Whether the 1P mesh transform is currently offset for hidden weapon beam origin
	bool bHiddenWeaponTransformApplied = false;

	// Saved original transform to restore when weapon is shown
	FTransform SavedFirstPersonMeshTransform;

	// --- Spectator rotation smoothing ---
	// Smoothed rotation for spectators viewing this character (prevents jitter at 480fps)
	mutable FRotator SmoothedViewRotation;
	mutable bool bSmoothedViewRotationInitialized = false;

public:
	// Override to smooth rotation for spectators at high FPS
	virtual FRotator GetViewRotation() const override;

	// Override to clear all ambient sounds on death (prevents link gun overheat loop)
	virtual bool Died(AController* EventInstigator, const FDamageEvent& DamageEvent, AActor* DamageCauser = nullptr) override;

	// ArmorPlus: override damage absorption
	// Belt armor always absorbs at 100%, non-belt absorbs at 66.67%
	virtual bool ModifyDamageTaken_Implementation(
		int32& AppliedDamage, int32& Damage, FVector& Momentum,
		AUTInventory*& HitArmor, const FHitResult& HitInfo,
		AController* EventInstigator, AActor* DamageCauser,
		TSubclassOf<UDamageType> DamageType) override;

	// ── Ping-Compensated Spawn ──────────────────────────────────────
	// Adapted from UT99 InstaGibPlus: spawn pawn hidden + no collision,
	// reveal only after the client confirms it has control. Prevents the
	// "frozen at spawn" window where enemies can see/shoot the player
	// before the player can move, and the camera crash-zoom on respawn.

	/** Server sets true on spawn; client clears + calls ServerConfirmSpawnReady on first controlled tick */
	UPROPERTY(Replicated)
	bool bPingCompensatedSpawnPending = false;

	/** Server timestamp when bPingCompensatedSpawnPending was set (for timeout) */
	float SpawnHiddenTimestamp = 0.f;

	/** Client has confirmed possession — reveal the pawn */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerConfirmSpawnReady();

	// ── Force Models (MutForceModels port, phase 1) ─────────────────
	// Client-side render override: force every OTHER player to a chosen AUTCharacterContent
	// + team-recolour, driven by the local NCPlusForceModels config. Fires on spawn /
	// team-change (both route through NotifyTeamChanged) and is a no-op on a dedicated server.
	virtual void NotifyTeamChanged() override;

	// Force Models: redirect the stock yellow armour overlay to our match/complimentary armour colour.
	virtual void UpdateArmorOverlay() override;

	// Force Models cosmetic strip (the "Cosmetics" flag): when a reskinned pawn should have its hat/
	// eyewear/leader-hat removed, suppress their (re)creation via these stock setter overrides — needed
	// because OnRep_PlayerState calls SetCosmeticsFromPlayerState AFTER NotifyTeamChanged, so destroying
	// in NotifyTeamChanged alone would be undone. StripCosmetics() clears any already-spawned.
	virtual void SetHatClass(TSubclassOf<AUTHat> HatClass) override;
	virtual void SetEyewearClass(TSubclassOf<AUTEyewear> EyewearClass) override;
	virtual void LeaderHatStatusChanged_Implementation() override;

	// Force Models "DarkenBodies": on a non-gib death, dissolve the corpse into its skeleton mesh.
	// Client-side (PlayDying runs per-client), gated by bEnabled + bDarkenBodies. Spawns dc's cooked
	// ModelDissolveEffect with the forced (or real) character's SkeletonMesh.
	virtual void PlayDying() override;

protected:
	// ArmorPlus: tracks how much of the current armor pool is belt (100% absorb).
	// Server-only; synced when ArmorType is belt, decremented on damage.
	int32 BeltArmorRemaining = 0;

	// ── Force Models state ──
	/** Re-evaluate this pawn and apply (or clear) the forced model + team-recolour. Client-only.
	 *  bForceReapply=true (the NotifyTeamChanged path) always re-asserts, because the base
	 *  NotifyTeamChanged just reverted us to the real model; false (the cross-pawn refresh path)
	 *  skips a no-op when the desired model+colour is unchanged. */
	void ApplyForcedModel(bool bForceReapply = true);
	/** When THIS is the local player's pawn and its team changed, every OTHER pawn's friend/enemy
	 *  bucket can flip without their own NotifyTeamChanged firing — re-evaluate them. */
	void RefreshOtherForcedModels();
	/** Re-entrancy guard — ApplyCharacterData / base NotifyTeamChanged can re-enter. */
	bool bApplyingForcedModel = false;
	/** Last applied forced state, so the refresh path can skip no-op re-applies. */
	UPROPERTY()
	UClass* LastForcedContent = nullptr;
	FLinearColor LastForcedColour = FLinearColor::Transparent;
	bool bForcedModelApplied = false;

	/** True while this reskinned pawn should have its cosmetics stripped — gates the setter overrides. */
	bool bForceModelStripCosmetics = false;
	/** Destroy any spawned Hat/Eyewear/LeaderHat on this pawn (Force Models cosmetic strip). */
	void StripCosmetics();
	/** Apply the desired strip state: strip + suppress when true, or restore (SetCosmeticsFromPlayerState)
	 *  when transitioning back to false. Called from ApplyForcedModel. */
	void UpdateCosmeticStrip(bool bShouldStrip);

	/** DarkenBodies: spawn dc's ModelDissolveEffect with this pawn's (forced or real) skeleton mesh, if
	 *  enabled and the death isn't a gib. Called from PlayDying (client-side). */
	void SpawnSkeletonDissolve();
};
