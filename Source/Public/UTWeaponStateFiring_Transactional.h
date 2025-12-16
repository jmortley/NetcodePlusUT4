#pragma once

#include "NetcodePlus.h"
#include "UTWeaponStateFiring.h"
#include "UTWeaponStateFiring_Transactional.generated.h"

/**
 * A Firing State that does NOT use a timer loop.
 * It waits for explicit "TransactionalFire" calls from the Weapon.
 */
UCLASS()
class NETCODEPLUS_API UUTWeaponStateFiring_Transactional : public UUTWeaponStateFiring
{
	GENERATED_UCLASS_BODY()

public:
	// Override BeginState to initialize effects WITHOUT starting the auto-fire timer
	virtual void BeginState(const UUTWeaponState* PrevState) override;

	// Override to do nothing (disable server auto-pilot)
	virtual void RefireCheckTimer() override;
	virtual void EndState() override;
	//virtual void Tick(float DeltaTime) override;
	virtual void Tick(float DeltaTime) override;
	// The new "Tick": Called explicitly when a valid RPC arrives
	virtual void TransactionalFire();
};