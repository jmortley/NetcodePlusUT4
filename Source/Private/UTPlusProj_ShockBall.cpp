
#include "UTPlusProj_ShockBall.h"
#include "UTPlusShockRifle.h"
#include "Particles/ParticleSystemComponent.h"


AUTPlusProj_ShockBall::AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FlightEffectVisual = nullptr;
	bVisualInitialized = false; // Start as false
	VisualInterpSpeed = 100.0f;
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





void AUTPlusProj_ShockBall::BeginPlay()
{
	Super::BeginPlay();

	// 1. Find the component named "FlightEffect" that you created in Blueprint
	TArray<UParticleSystemComponent*> Components;
	GetComponents(Components);

	for (UActorComponent* Comp : Components)
	{
		// Check if the component name matches your Blueprint name "FlightEffect"
		// (Note: UE4 sometimes appends generic suffixes, so finding the first particle system is often safer if it's the only one)
		if (Comp->GetName().Contains(TEXT("FlightEffect")))
		{
			FlightEffectVisual = Cast<UParticleSystemComponent>(Comp);
			break;
		}
	}

	// Fallback: If name check failed, just grab the first ParticleSystem found (likely correct)
	if (!FlightEffectVisual && Components.Num() > 0)
	{
		FlightEffectVisual = Cast<UParticleSystemComponent>(Components[0]);
	}

}



void AUTPlusProj_ShockBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (GetNetMode() != NM_DedicatedServer && FlightEffectVisual)
	{
		// 1. INITIALIZATION: Run this ONCE when the projectile is actually valid
		if (!bVisualInitialized)
		{
			// Calculate the offset (e.g. alignment to gun barrel)
			FVector RootLoc = GetRootComponent()->GetComponentLocation();

			// Only initialize if we are NOT at 0,0,0 (wait for valid spawn data)
			if (!RootLoc.IsNearlyZero())
			{
				InitialVisualOffset = FlightEffectVisual->GetComponentLocation() - RootLoc;

				// Detach
				FlightEffectVisual->bAbsoluteLocation = true;

				// FORCE SNAP: Teleport visual to the Muzzle immediately
				FlightEffectVisual->SetWorldLocation(RootLoc + InitialVisualOffset);

				// Mark as done so we never snap again
				bVisualInitialized = true;
			}
			// If RootLoc IS zero, we return and try again next frame. 
			// This prevents the "Floor to Muzzle" J-hook.
			return;
		}

		// 2. SMOOTHING: Run this every frame after initialization

		// Ensure absolute location is still true (just in case)
		FlightEffectVisual->bAbsoluteLocation = true;

		FVector TargetPos = GetRootComponent()->GetComponentLocation() + InitialVisualOffset;
		FVector CurrentPos = FlightEffectVisual->GetComponentLocation();

		//const float InterpolationSpeed = 100.0f;
		FVector NewPos = FMath::VInterpTo(CurrentPos, TargetPos, DeltaTime, VisualInterpSpeed);

		FlightEffectVisual->SetWorldLocation(NewPos);
	}
}


