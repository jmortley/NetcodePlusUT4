#pragma once

#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "TimerManager.h"
#include "UTWeap_LinkGun_NCP.generated.h"

class AUTProjectile;
class UAnimMontage;
class UCanvas;
class UCanvasRenderTarget2D;
class UFont;
class UMaterialInstanceDynamic;
class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;
class UTexture2D;
class UUTHUDWidget;
class FCanvasWordWrapper;

/**
 * Stock-faithful Link Gun backed by AUTWeaponFix's rewind-aware hitscan trace.
 *
 * Both fire modes retain stock input/state timing. In particular, rapid primary
 * plasma does not use NetcodePlus fire transactions; it also explicitly uses
 * AUTWeapon's stock predicted-projectile path. The continuous secondary beam
 * remains server authoritative and receives rewind only through virtual
 * FireInstantHit/HitScanTrace dispatch.
 *
 * This native class is intentionally content-free. Production assets must be
 * made by duplicating the stock BP_LinkGun and reparenting that duplicate so
 * its components, graphs, and tuned defaults are retained.
 */
UCLASS(Abstract)
class NETCODEPLUS_API AUTWeap_LinkGun_NCP : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

public:
	/**
	 * Maximum raw-validation/rollback history queried by the Link beam, in ms.
	 * Runtime-clamped to 50ms even if a cooked Blueprint contains a larger value.
	 * With ncp.RenderCredit=1, remote-human target selection instead uses the
	 * estimated render-time sample (half RTT + render extra, capped at 250ms).
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Lag Compensation|Link Beam",
		meta = (ClampMin = "0.0", ClampMax = "50.0", UIMin = "0.0", UIMax = "50.0"))
	float LinkBeamMaxRewindMs;

	/** Minimum interval between link-pull attempts while the beam is held. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	float BeamPulseInterval;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	int32 BeamPulseAmmoCost;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	float BeamPulseMomentum;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	TSubclassOf<UDamageType> BeamPulseDamageType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	UAnimMontage* PulseAnim;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	UAnimMontage* PulseAnimHands;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	UParticleSystem* PulseSuccessEffect;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	UParticleSystem* PulseFailEffect;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	int32 LinkPullDamage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	FLinearColor ReadyToPullColor;

	/**
	 * Returns true only when the persistent first-person Link beam needs its
	 * configured local color reapplied (new effect instance or an F5 color
	 * change). The muzzle particle color is updated here on the same change.
	 *
	 * This is deliberately owner-local cosmetic state: it never replicates and
	 * is disabled for remote weapons, dedicated servers, and replay playback.
	 */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Cosmetics")
	bool RefreshConfiguredLinkColor(UObject* BeamEffect, UParticleSystemComponent* MuzzleEffect,
		FLinearColor& OutColor);

	UPROPERTY(BlueprintReadWrite, Category = LinkGun)
	int32 LastFiredPlasmaTime;

	UPROPERTY(BlueprintReadOnly, Category = LinkGun)
	AActor* CurrentLinkedTarget;

	/** Returns true if InTarget is a valid link target. */
	virtual bool IsValidLinkTarget(AActor* InTarget);

	UPROPERTY(BlueprintReadOnly, Category = LinkGun)
	float LinkStartTime;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = LinkGun)
	float PullWarmupTime;

	UPROPERTY(BlueprintReadOnly, Category = LinkGun)
	bool bReadyToPull;

	/** Stock Link Gun pulse RPC, retained with its original signature. */
	UFUNCTION(Unreliable, Server, WithValidation)
	void ServerSetPulseTarget(AActor* InTarget);

	UPROPERTY(BlueprintReadWrite, Category = LinkGun)
	float OverheatFactor;

	UPROPERTY(BlueprintReadWrite, Category = LinkGun)
	bool bIsInCoolDown;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	USoundBase* OverheatFPFireSound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	USoundBase* NormalFPFireSound;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	USoundBase* OverheatSound;

	UPROPERTY(EditDefaultsOnly, Category = Effects)
	float FiringBeamKickbackY;

	UPROPERTY(EditDefaultsOnly, Category = Effects)
	float LinkPullKickbackY;

	UPROPERTY(Transient, BlueprintReadWrite, Category = LinkGun)
	float LastBeamPulseTime;

	UPROPERTY(BlueprintReadOnly)
	AActor* PulseTarget;

	UPROPERTY()
	FVector PulseLoc;

	UPROPERTY(EditDefaultsOnly, Category = LinkGun)
	FVector MissedPulseOffset;

	virtual bool IsLinkPulsing();

	virtual void StartFire(uint8 FireModeNum) override;
	virtual void StopFire(uint8 FireModeNum) override;
	virtual bool PutDown() override;
	virtual void FireShot() override;
	virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit = nullptr) override;
	virtual AUTProjectile* FireProjectile() override;
	virtual AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile> ProjectileClass, FVector SpawnLocation, FRotator SpawnRotation) override;
	virtual float GetHitValidationPredictionTime() const override;
	virtual float GetHitscanTimeSearchWindow() const override { return 0.f; }

	/** Beam ticks are claim-incapable by design (claims are cleared before each
	 *  trace), so remote-human target selection uses the estimated render-time
	 *  capsule instead of accepting raw OR render history. See
	 *  ncp.RenderCredit (legacy name; 0 restores raw rewind). */
	virtual bool SupportsRenderCredit() const override { return true; }

	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	UCanvasRenderTarget2D* ScreenTexture;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Mesh)
	UTexture2D* ScreenKillNotifyTexture;

	UPROPERTY(BlueprintReadWrite, EditDefaultsOnly, Category = Mesh)
	UFont* ScreenFont;

	UPROPERTY(EditDefaultsOnly, Category = Mesh)
	int32 ScreenMaterialID;

	UPROPERTY(EditDefaultsOnly, Category = Mesh)
	int32 SideScreenMaterialID;

	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	UMaterialInstanceDynamic* ScreenMI;

	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	UMaterialInstanceDynamic* SideScreenMI;

	virtual void AttachToOwner_Implementation() override;
	virtual void StopFiringEffects_Implementation() override;

	UFUNCTION()
	virtual void UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height);

	TSharedPtr<FCanvasWordWrapper> WordWrapper;

	UPROPERTY(BlueprintReadWrite, Category = Mesh)
	float LastClientKillTime;

	virtual void NotifyKillWhileHolding_Implementation(TSubclassOf<UDamageType> DmgType) override
	{
		LastClientKillTime = GetWorld()->TimeSeconds;
	}

	UPROPERTY(BlueprintReadOnly, Category = LinkGun)
	bool bLinkCausingDamage;

	UPROPERTY(BlueprintReadOnly, Category = LinkGun)
	bool bLinkBeamImpacting;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	USoundBase* PullSucceeded;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = LinkGun)
	USoundBase* PullFailed;

	/** Whether releasing a charged beam may perform the stock Link pull. */
	virtual bool SupportsLinkPull() const { return true; }

	virtual void StartLinkPull();
	virtual void Tick(float DeltaTime) override;
	virtual void FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode) override;
	virtual void StateChanged() override;
	virtual void PlayWeaponAnim(UAnimMontage* WeaponAnim, UAnimMontage* HandsAnim = nullptr, float RateOverride = 0.0f) override;

	UFUNCTION()
	virtual void CheckBotPulseFire();

	virtual float SuggestAttackStyle_Implementation() override
	{
		return 0.8f;
	}

	virtual float SuggestDefenseStyle_Implementation() override
	{
		return -0.4f;
	}

	virtual void DrawWeaponCrosshair_Implementation(UUTHUDWidget* WeaponHudWidget, float RenderDelta) override;
	virtual void Removed() override;
	virtual void ClientRemoved() override;

protected:
	/** Stock-adjacent Link uses mode-zero plasma overheat; Shaft beam does not. */
	virtual bool UsesPrimaryOverheat() const { return true; }

	/** Keep the stock high-ping delayed fake callback for rapid plasma. */
	virtual void SpawnDelayedFakeProjectile() override;

	/** Poll for the rare stuck Blueprint beam only while a beam actor exists. */
	void ArmLinkBeamWatchdog(UObject* BeamEffect);
	void StopLinkBeamWatchdog();
	void CheckLinkBeamWatchdog();

	/** Weak identity caches avoid retaining transient Blueprint beam actors. */
	TWeakObjectPtr<UObject> LastColoredLinkEffect;
	TWeakObjectPtr<UParticleSystemComponent> LastColoredLinkMuzzle;
	uint32 LastLinkEffectColorGeneration;
	uint32 LastLinkMuzzleColorGeneration;

	TWeakObjectPtr<UObject> TrackedLinkBeamEffect;
	FTimerHandle LinkBeamWatchdogHandle;

	/** Render/material update caches; none of these values replicate. */
	float LastScreenUpdateTime;
	TWeakObjectPtr<UParticleSystemComponent> LastPulseScaleEffect;
	float LastAppliedPulseScale;
};
