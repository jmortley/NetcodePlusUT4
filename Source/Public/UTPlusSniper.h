#pragma once
#include "NetcodePlus.h"
#include "UTWeaponFix.h"
#include "UTPlusSniper.generated.h"

/**
 * UTPlusSniper
 * Combines Epic's Sniper Rifle logic with UTWeaponFix netcode architecture.
 */
UCLASS(abstract)
class NETCODEPLUS_API AUTPlusSniper : public AUTWeaponFix
{
	GENERATED_UCLASS_BODY()

public:
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Impressive")
	bool bTrackImpressive = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Impressive", meta = (ClampMin = "1"))
	int32 ImpressiveThreshold = 3;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Impressive")
	int32 ImpressiveStreak = 0;

	UFUNCTION(BlueprintImplementableEvent, Category = "Impressive")
	void OnImpressive();

	UFUNCTION(Client, Reliable)
	void ClientNotifyImpressive();

	/** Headshot damage amount */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	int32 HeadshotDamage;

	/** Damage to deal if a headshot was blocked by a helmet */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	int32 BlockedHeadshotDamage;

	/** Head scaling for a static target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	float StoppedHeadshotScale;

	/** Head scaling for a slowly moving target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	float SlowHeadshotScale;

	/** Head scaling for a moving target */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	float RunningHeadshotScale;


	/** Head scaling for a target standing still but not crouched */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sniper")
	float AimedHeadshotScale;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = InstantHitDamage)
	TSubclassOf<UDamageType> HeadshotDamageType;

	/** Returns head scaling based on Target's movement state */
	virtual float GetHeadshotScale(class AUTCharacter* HeadshotTarget) const;

	// --- Overrides from UTWeaponFix/AUTWeapon ---

	/** Hybrid implementation: Fix's trace logic + Sniper's damage logic */
	virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;

	/** Applies head scaling to projectile if used */
	virtual AUTProjectile* FireProjectile() override;

	/** Handles Zoom visual toggling */
	virtual void OnRep_ZoomState_Implementation() override;

	/** AI Logic */
	virtual float GetAISelectRating_Implementation() override;

	virtual bool CanHeadShot();

	virtual void PlayPredictedImpactEffects(FVector ImpactLoc) override;

	virtual void OnServerHitScanResult(const FHitResult& Hit, float PredictionTime) override;

	/** Return hitscan damage amount. */
	virtual int32 GetHitScanDamage();
};