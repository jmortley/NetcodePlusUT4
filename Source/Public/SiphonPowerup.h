#pragma once

#include "NetcodePlus.h"
#include "UTTimedPowerup.h"
#include "SiphonPowerup.generated.h"

/**
 * Siphon — timed powerup that grants life steal (vampirism).
 * While held, a percentage of damage dealt to enemies heals the attacker.
 * The actual healing logic lives in WipeoutGame::ScoreDamage_Implementation
 * so this class stays minimal.
 */
UCLASS(Blueprintable)
class NETCODEPLUS_API AUTSiphonPowerup : public AUTTimedPowerup
{
	GENERATED_UCLASS_BODY()

	/** Fraction of damage dealt that heals the attacker (0.5 = 50%) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Siphon")
	float SiphonPercent;

	/** Maximum health the siphon can heal up to (199 = overheal like vials) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Siphon")
	int32 HealCap;

	/** Announcer sound played for the player who picks up the powerup */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Siphon|Audio")
	USoundBase* PickupAnnouncerSound;

	/** Ambient loop played on the character while Siphon is active */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Siphon|Audio")
	USoundBase* SiphonAmbientSound;

	virtual void GivenTo(AUTCharacter* NewOwner, bool bAutoActivate) override;
	virtual void Removed() override;
};
