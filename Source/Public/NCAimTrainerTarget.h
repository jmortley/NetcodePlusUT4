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

    void ActivateTarget(const FVector& Location, bool bStrafe);
    void HideTarget();
    bool IsAvailable() const { return bTrainerVisible; }
    float GetAppearanceTime() const { return AppearanceTime; }
    bool HasCharacterAssets() const;
    void SetStrafeDirection(float Direction);

private:
    UPROPERTY(ReplicatedUsing=OnRep_TrainerVisible)
    bool bTrainerVisible = false;
    UFUNCTION() void OnRep_TrainerVisible();
    bool bTrainerStrafe = false;
    float StrafeDirection = 1.0f;
    FVector StrafeCenter = FVector::ZeroVector;
    float AppearanceTime = 0.f;
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
