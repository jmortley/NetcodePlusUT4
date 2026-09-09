#pragma once

#include "NetcodePlus.h"
#include "UTMutator.h"
#include "UTPickupInventory.h"
#include "NCPickupBaseMutator.generated.h"

/** Shared parent for NCWepMut, NCStockWeapons and WipeoutMutator.
 * Their existing parent CheckRelevance calls run this after inventory substitution.
 * Class references are assigned in those Blueprints so cooking includes the copies. */
UCLASS(Blueprintable)
class NETCODEPLUS_API ANCPickupBaseMutator : public AUTMutator
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus|Pickup Bases")
	TSubclassOf<AUTPickupInventory> NCWeaponBaseClass;

	UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus|Pickup Bases")
	TSubclassOf<AUTPickupInventory> NCPowerupBaseClass;

	UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus|Pickup Bases")
	TSubclassOf<AUTPickupInventory> NCPowerupTimerBaseClass;

	virtual bool CheckRelevance_Implementation(AActor* Other) override;
};
