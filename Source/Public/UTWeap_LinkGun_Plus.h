#pragma once

#include "UTWeap_LinkGun.h"
#include "UTWeap_LinkGun_Plus.generated.h"

UCLASS()
class UNREALTOURNAMENT_API AUTWeap_LinkGun_Plus : public AUTWeap_LinkGun
{
	GENERATED_UCLASS_BODY()

public:
    // RPC to receive client hit claims
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerProcessBeamHit(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount);

    // Helper for the State to call
    void ProcessClientSideHit(float DeltaTime, AActor* HitActor, FVector HitLoc, const FInstantHitDamageInfo& DamageInfo);

protected:
    // Anti-cheat / Lag compensation tolerance buffer
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Validation")
    float MaxHitDistanceTolerance;
};