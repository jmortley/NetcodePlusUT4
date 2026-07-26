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
#include "TimerManager.h"   // FTimerHandle SpawnRevealHandle (ping-comp spawn reveal timer)
#include "TeamArenaCharacter.generated.h"


class UTeamArenaCharacterMovement;
class ACTFStatsReplicator;
class AClutchRoundState;
class AUTWeaponFix;
class UUTWeaponSkin;
class UMaterialInstanceDynamic;

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
	virtual void PossessedBy(AController* NewController) override;
	virtual bool AddInventory(AUTInventory* InvToAdd, bool bAutoActivate) override;
	virtual void SetSkinForWeapon(UUTWeaponSkin* WeaponSkin) override;
	virtual void UpdateWeaponSkinPrefFromProfile(AUTWeapon* Weapon) override;
	virtual void UpdateWeaponSkin() override;
	virtual void UpdateSkin() override;
	void SubmitConfiguredWeaponSkin(AUTWeaponFix* Weapon, bool bForce);

	/** Submit the local F5 choice for an owned weapon. Empty SkinPath is Default. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSetNCPWeaponSkin(AUTWeapon* InWeapon, const FString& SkinPath);

	// Reject ambient loops emitted by inactive stock weapons before they can become
	// replicated, persistent character audio (notably the Link Gun overheat loop).
	virtual void SetAmbientSound(USoundBase* NewAmbientSound, bool bClear = false) override;

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

	// --- Replicated weapon skins ---
	/** Server-side burst limiter for the reliable cosmetic selection RPC. */
	float WeaponSkinRequestWindowStart = 0.f;
	uint8 WeaponSkinRequestsInWindow = 0;

	/** Per-attachment body-slot cache. Rebuilt only when the attachment or parent changes. */
	TWeakObjectPtr<AUTWeaponAttachment> SkinnedWeaponAttachment;
	UPROPERTY(Transient)
	UMaterialInterface* OriginalWeaponAttachmentMaterial = nullptr;
	/** Slot-1 3P material, captured only when the attachment's target mask includes
	 *  slot 1 — Flak (slots 0+1) and the Lightning Gun (slot 1 ONLY; its slot 0 keeps
	 *  the original). nullptr for slot-0-only weapons. */
	UPROPERTY(Transient)
	UMaterialInterface* OriginalWeaponAttachmentMaterialSecondary = nullptr;
	UPROPERTY(Transient)
	UMaterialInterface* AppliedWeaponAttachmentMaterial = nullptr;
	UPROPERTY(Transient)
	UMaterialInstanceDynamic* WeaponAttachmentSkinMID = nullptr;
	bool bCapturedWeaponAttachmentMaterial = false;

	UUTWeaponSkin* ResolveWeaponSkinForClass(UClass* WeaponClassToMatch) const;
	void ApplyWeaponAttachmentSkin(UUTWeaponSkin* Skin);
	void ApplyServerWeaponSkinSelection(AUTWeapon* InWeapon, UUTWeaponSkin* Skin);

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
	virtual void GiveArmor(class AUTArmor* InArmorType) override;
	virtual void SetArmorAmount(class AUTArmor* InArmorType, int32 Amount) override;
	virtual void RemoveArmor(int32 Amount) override;
	virtual void ServerDropArmor_Implementation() override;

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

	/** Server-side reveal timer: fires at ~RevealRttPct% of the spawner's RTT (an
	 *  estimate of when their client has control), instead of waiting for the full
	 *  round-trip ServerConfirmSpawnReady ACK. */
	FTimerHandle SpawnRevealHandle;

	/** Server-side: reveal the pawn (unhide + re-enable collision + clear the flag &
	 *  timer). Shared by the RevealRttPct timer, the client ACK, and the 0.5s safety
	 *  timeout; guarded against double-fire. */
	void RevealAfterPingComp();

	/** Client has confirmed possession — reveal the pawn */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerConfirmSpawnReady();

	/** Server-side: begin the ping-compensated spawn hide (set pending + hide +
	 *  disable collision + stamp), BUT only if the owning client's ping is at/above
	 *  the floor (Mod.ini [NetcodePlus] PingCompSpawnMinPingMs, default 60ms).
	 *  Low-ping spawners regain control almost instantly (no real spawn-kill risk),
	 *  and their brief hide is exactly what makes them look like they teleport-dodge
	 *  off the spawn on a higher-ping opponent's screen. (ExactPing is live every
	 *  spawn, so there's no unknown case.) Call from the gamemode's RestartPlayer after Super. */
	void BeginPingCompensatedSpawnHide();

	// ── Force Models (MutForceModels port, phase 1) ─────────────────
	// Client-side render override: force every OTHER player to a chosen AUTCharacterContent
	// + team-recolour, driven by the local NCPlusForceModels config. Fires on spawn /
	// team-change (both route through NotifyTeamChanged) and is a no-op on a dedicated server.
	virtual void NotifyTeamChanged() override;
	/** Keep stock outline recreation out of ApplyCharacterData's body-mesh reregister window. */
	virtual void ApplyCharacterData(TSubclassOf<AUTCharacterContent> Data) override;

	// Force Models: redirect the stock yellow armour overlay to our match/complimentary armour colour.
	virtual void UpdateArmorOverlay() override;

	// Force Models cosmetic strip (the "Cosmetics" flag): when a reskinned pawn should have its hat/
	// eyewear/leader-hat removed, suppress their (re)creation via these stock setter overrides — needed
	// because OnRep_PlayerState calls SetCosmeticsFromPlayerState AFTER NotifyTeamChanged, so destroying
	// in NotifyTeamChanged alone would be undone. StripCosmetics() clears any already-spawned.
	virtual void SetHatClass(TSubclassOf<AUTHat> HatClass) override;
	virtual void SetEyewearClass(TSubclassOf<AUTEyewear> EyewearClass) override;
	virtual void LeaderHatStatusChanged_Implementation() override;

	// Force Models "DarkenBodies": on death, hide the corpse after a short delay (so the death/ragdoll
	// effects are still visible briefly). Client-side (PlayDying runs per-client), gated by bEnabled +
	// bDarkenBodies.
	virtual void PlayDying() override;
	/** Clear client-local outline duplicates before stock teardown destroys the weapon attachment.
	 *  Prematch lineup pawns are destroyed alive, so they never pass through PlayDying(). */
	virtual void Destroyed() override;

	/** Keep TacCom/X-ray from rendering the separate CustomDepth duplicate after this pawn's body mesh
	 *  has been explicitly hidden by corpse cleanup. Stock TacCom re-calls SetOutlineLocal(true) every
	 *  spectator tick, so clearing the outline only once in PlayDying is not sufficient. */
	virtual void SetOutlineLocal(bool bNowOutlined, bool bWhenUnoccluded = false) override;

	// Clutch defenders play at 10% on every client. Outside Clutch, the iCTF-only F5
	// "Own Footstep Volume" setting still affects only this local player's own pawn.
	virtual void PlayFootstep(uint8 FootNum, bool bFirstPerson = false) override;

	/** True iff this pawn is controlled by a LOCAL HUMAN player — i.e. it's "my own" pawn (incl. split-screen).
	 *  PUBLIC so DrawHeadDebug can use it too. Use this, NOT IsLocallyControlled(), to skip the local player's
	 *  pawn: in NM_Standalone (offline) IsLocallyControlled() is true for EVERY controller (bots' AIControllers
	 *  included), which made Force Models + the DebugHeads ring skip ALL pawns offline. Bots use AIController, so
	 *  the Cast excludes them; IsLocalController() keeps remote players (listen server) from matching. */
	bool IsLocalPlayerPawn() const;

protected:
	// ArmorPlus: tracks the 100%-absorb portion of the replicated total armor pool.
	// Server-authoritative and updated when armor is granted or removed.
	int32 BeltArmorRemaining = 0;

	// The most recently collected regular armor CDO. While a mixed stack still has
	// belt remaining, ArmorType stays on the belt for its shell/effects; this becomes
	// ArmorType when the belt portion reaches zero. Server-only; CDOs are always rooted.
	UPROPERTY(Transient)
	class AUTArmor* LastRegularArmorType = nullptr;

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

	/** Coalesce the forced-model apply: a single replication burst fires NotifyTeamChanged 2-4x
	 *  (PossessedBy / OnRep_PlayerState / the PlayerState's NotifyTeamChanged / UTTeamInfo). Rather than
	 *  rebuild the forced mesh per OnRep, NotifyTeamChanged marks this dirty and Tick re-asserts the model
	 *  exactly once. Set only off the dedicated server. */
	bool bForcedModelDirty = false;
	/** Local pawn only: its team change flips every OTHER pawn's friend/enemy bucket; coalesced refresh. */
	bool bRefreshOthersDirty = false;
	/** Flush the coalesced forced-model work (the dirty flags) — called from the top of Tick on clients. */
	void FlushForcedModelUpdate();

	/** True while this reskinned pawn should have its cosmetics stripped — gates the setter overrides. */
	bool bForceModelStripCosmetics = false;
	/** Destroy any spawned Hat/Eyewear/LeaderHat on this pawn (Force Models cosmetic strip). */
	void StripCosmetics();
	/** Apply the desired strip state: strip + suppress when true, or restore (SetCosmeticsFromPlayerState)
	 *  when transitioning back to false. Called from ApplyForcedModel. */
	void UpdateCosmeticStrip(bool bShouldStrip);

	/** DarkenBodies: on death, schedule the corpse to hide after a short delay (lets death/ragdoll effects
	 *  play first). Gated by bEnabled + bDarkenBodies. Called from PlayDying (client-side). */
	void SpawnSkeletonDissolve();
	/** Permanently retire this pawn's client-local body and weapon CustomDepth render state. */
	void ClearLocalOutlineRenderState();
	/** Timer callback for SpawnSkeletonDissolve — hides the corpse mesh once the delay elapses. */
	void HideDeadBody();

	// ── Own footstep volume (iCTF) ──
	/** Reimplemented own-footstep play honouring OwnFootstepVolumeScale (UTPlaySound has no volume arg). */
	void PlayOwnFootstepScaled(uint8 FootNum);
	/** Reimplemented footstep play with a per-source volume multiplier. */
	void PlayFootstepScaled(uint8 FootNum, bool bFirstPerson, float VolumeScale);
	/** Client-side Clutch role lookup; returns 0.1 for a live-round defender, otherwise 1. */
	float GetClutchFootstepVolumeScale();
	TWeakObjectPtr<AClutchRoundState> CachedClutchFootstepState;
	/** 0..1 multiplier on the local player's own footstep; 1 = stock (no override). Read once from config. */
	float OwnFootstepVolumeScale = 1.f;
	bool bOwnFootstepVolumeRead = false;
	/** Cached iCTF gate (ACTFStatsReplicator::bIsInstagibMatch) — present only in NCPlusCTF instagib.
	 *  Searched lazily while null (so it resolves even if the first footstep precedes replication), then
	 *  latched by bIctfFootstepResolved to avoid iterating every footstep forever in non-iCTF modes. */
	TWeakObjectPtr<ACTFStatsReplicator> CachedCTFRep;
	bool bIctfFootstepResolved = false;
	/** Warmup iCTF signal: the replicated MutInstagibNCP mutator is present from match init (incl. warmup),
	 *  unlike ACTFStatsReplicator which only spawns at match start. Latched once found. */
	bool bIctfMutatorFound = false;
};
