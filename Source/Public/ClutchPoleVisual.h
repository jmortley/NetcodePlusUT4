#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ClutchPoleVisual.generated.h"

class UStaticMeshComponent;

/** Replicated, collision-free presentation of the recovered Clutch pole. */
UCLASS(NotPlaceable)
class NETCODEPLUS_API AClutchPoleVisual : public AActor
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Clutch|Pole")
	UStaticMeshComponent* PoleMesh;

	bool HasValidMesh() const;
};
