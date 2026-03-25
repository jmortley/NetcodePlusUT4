// HitsoundPack.h
// Data asset for defining hitsound presets

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Sound/SoundBase.h"
#include "HitsoundPack.generated.h"

/**
 * UHitsoundPack - Data asset defining a hitsound preset.
 *
 * Place instances of this asset in your Content directory to register
 * hitsound presets. They are discovered at runtime via the Asset Registry.
 */
UCLASS(BlueprintType)
class NETCODEPLUS_API UHitsoundPack : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable)
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	USoundBase* Low;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	USoundBase* Med;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	USoundBase* High;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float DefaultVolume = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float DefaultPitch = 1.0f;
};
