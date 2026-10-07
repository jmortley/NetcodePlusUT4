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
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual float TakeDamage(float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer) override;
    virtual void PlayTakeHitEffects_Implementation() override;
    virtual FVector GetHeadLocation(float PredictionTime = 0.f) override;
    virtual void NotifyBlockedHeadShot(AUTCharacter* ShotInstigator) override;

    void ActivateTarget(const FVector& Location, bool bStrafe);
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
    bool TryTrainerDodge(float DirectionRoll);
    /** Uses UT's floor-slide physics toward the trainee, along world -X. */
    bool TryTrainerSlideForward();
    /** Platform targets slide forward; the near-left floor lane slides inward. */
    bool TryTrainerPopupSlide(int32 Slot);
    /** Native lateral slide for the tracking target; turns inward near lane edges. */
    bool TryTrainerTrackingSlide(float DirectionRoll);
    bool IsTrainerSliding() const;

private:
    UPROPERTY(ReplicatedUsing=OnRep_TrainerVisible)
    bool bTrainerVisible = false;
    UFUNCTION() void OnRep_TrainerVisible();
    bool bTrainerStrafe = false;
    bool bTrainerWiggle = false;
    float StrafeDirection = 1.0f;
    float StrafeRange = 800.f;
    float WiggleRange = 0.f;
    float PopupLongStrafeEndTime = 0.f;
    bool bRecenterWiggleAfterSlide = false;
    FVector StrafeCenter = FVector::ZeroVector;
    FVector TrainerSlideDirection = FVector::ZeroVector;
    float AppearanceTime = 0.f;
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
    UPROPERTY(ReplicatedUsing=OnRep_Scenario) uint8 Scenario = 0;
    UFUNCTION() void OnRep_Scenario();
    class UStaticMeshComponent* AddBlock(FName Name, const FVector& Center, const FVector& Size);
};
