// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.
#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTPlusShockRifle.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UParticleSystemComponent;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4273)
#endif

UCLASS(abstract)
class NETCODEPLUS_API AUTPlusShockRifle : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Impressive")
	bool bTrackImpressive = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Impressive", meta = (ClampMin = "1"))
	int32 ImpressiveThreshold = 5;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Impressive")
	int32 ImpressiveStreak = 0;

	// BP event you�ll use to play the sound / widget
	UFUNCTION(BlueprintImplementableEvent, Category = "Impressive")
	void OnImpressive();

	// internal RPC: server to owning client, then calls OnImpressive()
	UFUNCTION(Client, Reliable)
	void ClientNotifyImpressive();

	/** Apply the owning player's local Shock color to a newly spawned first-person beam. */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Cosmetics")
	void ApplyConfiguredShockBeamColor(UParticleSystemComponent* Effect);

	/** shock ball bot is waiting to combo */
	UPROPERTY()
	class AUTProj_ShockBall* ComboTarget;

	/** set when bot intends to throw a prediction shot at an enemy not currently visible */
	UPROPERTY()
	FVector PredictiveComboTargetLoc;

	/** last time we did the expensive prediction check */
	UPROPERTY()
	float LastPredictiveComboCheckTime;

	/** set when bot wants next shock ball fired to set up combo */
	UPROPERTY()
	bool bPlanningCombo;

	/** result of MovingComboCheck() when bot starts a potential combo */
	UPROPERTY()
	bool bMovingComboCheckResult;

	/** effects flag to play combo-specific animations and effects */
	bool bPlayComboEffects;

	/** render target for on-mesh display screen */
	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	UCanvasRenderTarget2D* ScreenTexture;

	/** drawn to the display screen for a short time after holder kills an enemy */
	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Mesh)
	UTexture2D* ScreenKillNotifyTexture;

	/** font for text on the display screen */
	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Mesh)
	UFont* ScreenFont;

	/** material ID for the screen */
	UPROPERTY(EditDefaultsOnly, Category = Mesh)
	int32 ScreenMaterialID;

	/** material instance showing the screen */
	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	UMaterialInstanceDynamic* ScreenMI;

	/** anim to play for a shock combo instead of the normal one */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	UAnimMontage* ComboFireAnim;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	UAnimMontage* ComboFireAnimHands;

	/** set (client only) to last time user recorded a kill while holding this weapon */
	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	float LastClientKillTime;

	/** Last time screen texture was updated (for 30Hz throttle) */
	float LastScreenUpdateTime = 0.0f;

	virtual void SetupSpecialMaterials() override;

	UFUNCTION()
	virtual void UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height);

	virtual void Tick(float DeltaTime) override;

	virtual void NotifyKillWhileHolding_Implementation(TSubclassOf<UDamageType> DmgType) override
	{
		LastClientKillTime = GetWorld()->TimeSeconds;
		// Force immediate screen update so kill notification displays instantly
		LastScreenUpdateTime = 0.0f;
	}

	virtual UAnimMontage* GetFiringAnim(uint8 FireMode, bool bOnHands = false) const;
	virtual void PlayFiringEffects();
	virtual void PlayPredictedImpactEffects(FVector ImpactLoc) override;
	virtual void PlayImpactEffects_Implementation(const FVector& TargetLoc, uint8 FireMode,
		const FVector& SpawnLocation, const FRotator& SpawnRotation) override;

	UFUNCTION(BlueprintImplementableEvent, BlueprintCosmetic)
	void Play1PComboEffects();

	virtual void HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime) override;
	virtual AUTProjectile* FireProjectile() override;

	/** returns whether AI using this weapon shouldn't fire because it's waiting for a combo trigger */
	virtual bool WaitingForCombo();

	virtual void DoCombo();
	virtual void OnServerHitScanResult(const FHitResult& Hit, float PredictionTime) override;
	virtual float SuggestAttackStyle_Implementation() override;
	virtual float GetAISelectRating_Implementation() override;
	virtual bool CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc) override;

	virtual bool IsPreparingAttack_Implementation() override;
	virtual bool ShouldAIDelayFiring_Implementation() override;

	virtual int32 GetWeaponKillStats(AUTPlayerState* PS) const override;
	virtual int32 GetWeaponKillStatsForRound(AUTPlayerState* PS) const override;
	virtual int32 GetWeaponDeathStats(AUTPlayerState* PS) const override;

	virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;
	virtual void FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode) override;

	virtual bool CancelImpactEffect(const FHitResult& ImpactHit) const
	{
		return Super::CancelImpactEffect(ImpactHit) || Cast<AUTProjectile>(ImpactHit.GetActor()) != nullptr;
	}

	/**
	 * Override to provide different prediction for projectile vs beam.
	 * Projectile mode uses tighter validation since projectiles are easier to hit.
	 * Beam mode uses standard validation for forgiving hitscan.
	 */
	virtual float GetHitValidationPredictionTime() const override;

private:
	/** One weapon-owned material instance is shared by its short-lived beam PSCs. */
	UPROPERTY(Transient)
	UMaterialInstanceDynamic* CachedShockBeamMID;

	/** Authored slot-1 material used to build CachedShockBeamMID. */
	UPROPERTY(Transient)
	UMaterialInterface* CachedShockBeamSourceMaterial;

	/** Settings generation baked into CachedShockBeamMID. */
	UPROPERTY(Transient)
	uint32 CachedShockBeamColorGeneration;

	/** True only for the Instagib shock-rifle variants; normal Shock children stay stock. */
	bool IsInstagibBeamWeapon() const;
	bool ShouldShowOwnInstagibBeam() const;

	/** Lazily-resolved caches for the two checks above, so the fire path costs no
	 *  FString building, name Contains() scans, or config lookups per shot.
	 *  -1 = unresolved.
	 *
	 *  IsInstagibBeamWeapon's inputs really are fixed for the life of an instance
	 *  (class identity, stats names, damage type, attachment).
	 *
	 *  ShouldShowOwnInstagibBeam's is NOT, and caching it is a deliberate
	 *  trade: the F5 menu writes bShowOwnBeam through GConfig itself
	 *  (SUTNCPlusMenu.cpp SetString + Flush), which updates the same in-memory
	 *  cache the old per-shot GetString read — so toggling "Show Own Beam"
	 *  mid-match used to apply on the very next shot, and now applies when the
	 *  next weapon instance is built (your next respawn or re-pickup). Only a
	 *  hand-edit of Mod.ini was ever ignored. If live toggling is wanted back,
	 *  bump a generation counter on menu save and re-resolve when it changes. */
	mutable int8 CachedIsInstagibBeamWeapon = -1;
	mutable int8 CachedShowOwnBeam = -1;
	bool NeedsLegacyInstagibBeamLayer() const;
	void SpawnLegacyInstagibBeamLayer(const FVector& TargetLoc, uint8 FireMode,
		const FVector& SpawnLocation, const FRotator& SpawnRotation);
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
