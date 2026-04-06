// ShockDomControlPoint — Touchable control point for Domination game mode
#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "ShockDomControlPoint.generated.h"

class AUTCharacter;

UCLASS()
class NETCODEPLUS_API AShockDomControlPoint : public AActor
{
	GENERATED_UCLASS_BODY()

public:

	// ── Replicated state ──

	/** Team that currently controls this point (255 = neutral/uncaptured) */
	UPROPERTY(ReplicatedUsing = OnRep_OwningTeamIndex, BlueprintReadOnly, Category = "DomPoint")
	uint8 OwningTeamIndex;

	/** Index in the game mode's control point array (0, 1, 2) */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "DomPoint")
	int32 PointIndex;

	/** Display label ("A", "B", "C") */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "DomPoint")
	FString PointLabel;

	/** World time when this point was last captured (for HUD pulse animation) */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "DomPoint")
	float LastCaptureTime;


	// ── Components ──

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DomPoint")
	USphereComponent* CaptureVolume;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DomPoint")
	UStaticMeshComponent* VisualMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DomPoint")
	UPointLightComponent* TeamLight;


	// ── Configuration ──

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "DomPoint")
	float CaptureRadius;


	// ── Methods ──

	/** Server: set new owning team, replicate, notify game mode */
	void CapturePoint(uint8 NewTeamIndex, AUTCharacter* Capturer);

	/** Get the team color for the current owner (grey if neutral) */
	FLinearColor GetOwnerColor() const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

protected:

	UFUNCTION()
	void OnRep_OwningTeamIndex();

	UFUNCTION()
	void OnCaptureVolumeOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

	/** Update visual mesh material and light color to match owner */
	void UpdateVisuals();

	UPROPERTY(Transient)
	UMaterialInstanceDynamic* DynMaterial;
};
