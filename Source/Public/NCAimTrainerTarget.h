#pragma once

#include "NetcodePlus.h"
#include "TeamArenaCharacter.h"
#include "NCAimTrainerTarget.generated.h"

/** A real UT pawn with its normal skeleton, animations and headshot geometry.
 * Only ANCAimTrainerGame uses this class. It never runs a combat bot brain. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerTarget : public ATeamArenaCharacter
{
    GENERATED_BODY()
public:
    ANCAimTrainerTarget(const FObjectInitializer& ObjectInitializer);
    virtual void PostInitializeComponents() override;
    virtual void ApplyCharacterData(TSubclassOf<AUTCharacterContent> Data) override;
    virtual void BeginPlay() override;
    virtual void UpdateWeaponAttachment() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual float TakeDamage(float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer) override;
    virtual void PlayTakeHitEffects_Implementation() override;
    virtual FVector GetHeadLocation(float PredictionTime = 0.f) override;
    void SetTrainerHeadshotScale(float Scale);
    virtual void NotifyBlockedHeadShot(AUTCharacter* ShotInstigator) override;

    void ActivateTarget(const FVector& Location, bool bStrafe);
    /** Native fall/launch; FlightRate slows vertical time while preserving horizontal velocity. */
    void ActivateAirborneTarget(const FVector& Location, const FVector& LaunchVelocity, float FlightRate = 1.f);
    bool LaunchAirborneTarget(const FVector& LaunchVelocity);
    bool IsAirborneTarget() const { return bTrainerAirborne; }
    /** Derive horizontal speeds from this variant's CDO; never multiply a previous run. */
    void SetTrainerSpeedScale(float Scale);
    void StartWiggle(float HalfWidth);
    /** Temporarily replaces short A/D decisions with a longer native strafe. */
    bool StartPopupLongStrafe(float HalfWidth, float HoldSeconds, float DirectionRoll);
    bool IsTrainerLongStrafing() const { return PopupLongStrafeEndTime > 0.f; }
    /** Uses normal UT posture/collision; an active floor slide owns its posture. */
    bool SetTrainerCrouched(bool bCrouch);
    void HideTarget();
    bool IsAvailable() const { return bTrainerVisible; }
    float GetAppearanceTime() const { return AppearanceTime; }
    bool HasCharacterAssets() const;
    void ReverseStrafe();
    void ConfigurePopupStrafe(const FVector& Center, float HalfWidth, float DirectionRoll);
    bool TryTrainerDodge(float DirectionRoll);
    /** Guarded native diagonal dodge, optionally holding slide through a backward landing. */
    bool TryTrainerPopupDodge(int32 Slot, const FVector& Direction, const FVector& ArenaOrigin, bool bSlideOnLanding = false);
    /** Uses UT's floor-slide physics toward the trainee, along world -X. */
    bool TryTrainerSlideForward();
    /** Platform targets slide forward; near floor variants slide inward. */
    bool TryTrainerPopupSlide(int32 Slot, int32 Variant = 0);
    /** Native lateral slide for the tracking target; turns inward near lane edges. */
    bool TryTrainerTrackingSlide(float DirectionRoll);
    bool IsTrainerSliding() const;

private:
    UPROPERTY(ReplicatedUsing=OnRep_TrainerVisible)
    bool bTrainerVisible = false;
    UFUNCTION() void OnRep_TrainerVisible();
    UPROPERTY(ReplicatedUsing=OnRep_TrainerHeadshotScale) float TrainerHeadshotScale = 1.f;
    UFUNCTION() void OnRep_TrainerHeadshotScale();
    UPROPERTY(ReplicatedUsing=OnRep_TrainerFlightRate) float TrainerFlightRate = 1.f;
    UFUNCTION() void OnRep_TrainerFlightRate();
    bool bTrainerAirborne = false;
    bool bTrainerStrafe = false;
    bool bTrainerWiggle = false;
    float StrafeDirection = 1.0f;
    float StrafeRange = 800.f;
    float WiggleRange = 0.f;
    float PopupLongStrafeEndTime = 0.f;
    bool bRecenterWiggleAfterSlide = false;
    bool bRecenterWiggleAfterDodge = false;
    bool bTrainerDodgeSlidePending = false;
    FVector StrafeCenter = FVector::ZeroVector;
    FVector TrainerSlideDirection = FVector::ZeroVector;
    float AppearanceTime = 0.f;
    struct FTrainerMaterialTint
    {
        TWeakObjectPtr<UMaterialInstanceDynamic> Material;
        TMap<FName, FLinearColor> Vectors;
        TMap<FName, float> Scalars;
    };
    TArray<FTrainerMaterialTint> TrainerTintMaterials;
    float NextTrainerTintTime = 0.f;
    void UpdateTrainerTint();
    void ResetTargetMovement();
    bool StartTrainerSlide(const FVector& Direction);
};

/** IGCharacterFootsteps-sized target; the CDO also governs native uncrouching. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerInstagibTarget : public ANCAimTrainerTarget
{
    GENERATED_BODY()
public:
    ANCAimTrainerInstagibTarget(const FObjectInitializer& ObjectInitializer);
};

/** SACTF dimensions and native movement for the matching rifle presets. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerSACTFTarget : public ANCAimTrainerTarget
{
    GENERATED_BODY()
public:
    ANCAimTrainerSACTFTarget(const FObjectInitializer& ObjectInitializer);
};

/** Runtime room: hard references keep the stock cube/material in the cook.
 * Every client constructs the same fixed geometry, so no map asset is needed. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerArena : public AActor
{
    GENERATED_BODY()
public:
    ANCAimTrainerArena(const FObjectInitializer& ObjectInitializer);
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    void SetScenario(uint8 NewScenario);
    bool HasArenaAssets() const;

private:
    UPROPERTY() UStaticMesh* BlockMesh;
    UPROPERTY() UMaterialInterface* BlockMaterial;
    UPROPERTY() TArray<class UStaticMeshComponent*> Cover;
    UPROPERTY() TArray<class UStaticMeshComponent*> AirbornePlatforms;
    UPROPERTY() TArray<class UStaticMeshComponent*> AirbornePadVisuals;
    UPROPERTY() class UStaticMeshComponent* GooSurface;
    UPROPERTY(ReplicatedUsing=OnRep_Scenario) uint8 Scenario = 0;
    UFUNCTION() void OnRep_Scenario();
    class UStaticMeshComponent* AddBlock(FName Name, const FVector& Center, const FVector& Size);
};
