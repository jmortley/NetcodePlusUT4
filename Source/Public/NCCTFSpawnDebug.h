#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCCTFSpawnDebug.generated.h"

class APlayerStart;
class ULineBatchComponent;
class UTextRenderComponent;

// Server-side output of the actual selector. State: 0 eligible, 1 primary
// rejected (fallback allowed), 2 excluded, 3 chosen, 4 selector inactive.
struct FNCCTFSpawnDebugEntry
{
	APlayerStart* Start = nullptr;
	uint8 State = 0;
	int32 Reason = 0;
};

USTRUCT()
struct FNCCTFSpawnMarker
{
	GENERATED_BODY()
	UPROPERTY() FVector Location;
	UPROPERTY() FString Label;
	UPROPERTY() uint8 State = 0;
};

// Owner-only, warmup-only diagnostics. Uses runtime components so Shipping
// clients can see the markers too (DrawDebug helpers are compiled out there).
UCLASS(NotPlaceable, Transient)
class NETCODEPLUS_API ANCCTFSpawnDebug : public AInfo
{
	GENERATED_UCLASS_BODY()
public:
	int32 TargetPlayerId = -1;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
protected:
	UPROPERTY(Replicated) TArray<FNCCTFSpawnMarker> Markers;
	UPROPERTY(Transient) ULineBatchComponent* Lines;
	UPROPERTY(Transient) TArray<UTextRenderComponent*> Labels;
	float RefreshRemaining = 0.f;
	void RefreshMarkers(APlayerController* Viewer);
	void RenderMarkers(APlayerController* Viewer);
};
