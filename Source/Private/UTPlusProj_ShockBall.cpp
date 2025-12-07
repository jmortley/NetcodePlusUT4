
#include "UTPlusProj_ShockBall.h"
#include "UTPlusShockRifle.h"

AUTPlusProj_ShockBall::AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Everything else is inherited from parent
}

void AUTPlusProj_ShockBall::PerformCombo(class AController* InstigatedBy, class AActor* DamageCauser)
{
	// Consume extra ammo for the combo
	if (Role == ROLE_Authority)
	{
		AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
		AUTWeapon* Weapon = Cast<AUTWeapon>(DamageCauser);
		if (Weapon && (!GameMode || GameMode->bAmmoIsLimited || (Weapon->Ammo > 9)))
		{
			Weapon->AddAmmo(-ComboAmmoCost);
		}

		// This gets called before server startfire(). bPlayComboEffects = true will send the FireExtra when fired
		AUTCharacter* UTC = (InstigatedBy != nullptr) ? Cast<AUTCharacter>(InstigatedBy->GetPawn()) : nullptr;

		// CHANGED: Look for UTPlusShockRifle instead of UTWeap_ShockRifle
		AUTPlusShockRifle* ShockRifle = (UTC != nullptr) ? Cast<AUTPlusShockRifle>(UTC->GetWeapon()) : nullptr;
		if (ShockRifle != nullptr)
		{
			ShockRifle->bPlayComboEffects = true;
		}
	}

	// The player who combos gets the credit
	InstigatorController = InstigatedBy;

	// Replicate combo and execute locally
	bComboExplosion = true;
	OnRep_ComboExplosion();
	Explode(GetActorLocation(), FVector(1.0f, 0.0f, 0.0f));
}