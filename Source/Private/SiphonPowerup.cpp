#include "SiphonPowerup.h"
#include "UnrealTournament.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "StatNames.h"

AUTSiphonPowerup::AUTSiphonPowerup(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Display name for pickup message
	DisplayName = NSLOCTEXT("Powerup", "SiphonName", "Siphon");

	// Siphon lasts 30 seconds
	TimeRemaining = 30.f;
	TriggeredTime = 15.f;

	// Life steal defaults
	SiphonPercent = 0.5f;
	HealCap = 199;

	// Reuse Berserk stat tracking (same conceptual slot)
	StatsNameTime = NAME_BerserkTime;

	bAlwaysRelevant = true;

	// Berserk overlay — reuse the same material so player glows while Siphon is active
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OverlayMat3P(
		TEXT("/Game/RestrictedAssets/Pickups/Powerups/Assets/M_Berserk_Overlay"));
	if (OverlayMat3P.Succeeded())
	{
		OverlayEffect.Material = OverlayMat3P.Object;
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OverlayMat1P(
		TEXT("/Game/RestrictedAssets/Pickups/Powerups/Assets/M_Berserk_Overlay_1P"));
	if (OverlayMat1P.Succeeded())
	{
		OverlayEffect1P.Material = OverlayMat1P.Object;
	}

	// Load the ambient loop sound (audible to nearby players)
	static ConstructorHelpers::FObjectFinder<USoundBase> AmbientLoopObj(
		TEXT("/Game/RestrictedAssets/Audio/Gameplay/A_Powerup_Invulnerability_PowerLoop"));
	if (AmbientLoopObj.Succeeded())
	{
		SiphonAmbientSound = AmbientLoopObj.Object;
	}
	else
	{
		SiphonAmbientSound = nullptr;
	}

	// Load the "Siphon!" announcer sound
	static ConstructorHelpers::FObjectFinder<USoundBase> SiphonAnnouncerObj(
		TEXT("/Game/RestrictedAssets/Audio/AnnouncerReward/A_Announcer_Siphon"));
	if (SiphonAnnouncerObj.Succeeded())
	{
		PickupAnnouncerSound = SiphonAnnouncerObj.Object;
	}
	else
	{
		PickupAnnouncerSound = nullptr;
	}
}


void AUTSiphonPowerup::GivenTo(AUTCharacter* NewOwner, bool bAutoActivate)
{
	Super::GivenTo(NewOwner, bAutoActivate);

	// Play "Siphon!" announcer for the player who picked it up
	if (NewOwner && PickupAnnouncerSound)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(NewOwner->GetController());
		if (PC)
		{
			PC->UTClientPlaySound(PickupAnnouncerSound);
		}
	}

	// Ambient loop so nearby players can hear someone has Siphon
	if (NewOwner && SiphonAmbientSound)
	{
		NewOwner->SetAmbientSound(SiphonAmbientSound);
	}

	// Body overlay (UTTimedPowerup only does weapon overlay via SetWeaponOverlayEffect)
	if (NewOwner && OverlayEffect.IsValid())
	{
		NewOwner->SetCharacterOverlayEffect(OverlayEffect, true);
	}
}

void AUTSiphonPowerup::Removed()
{
	// Clear ambient sound and body overlay before Super nulls UTOwner
	if (UTOwner)
	{
		if (SiphonAmbientSound)
		{
			UTOwner->SetAmbientSound(SiphonAmbientSound, true);
		}
		if (OverlayEffect.IsValid())
		{
			UTOwner->SetCharacterOverlayEffect(OverlayEffect, false);
		}
	}
	Super::Removed();
}
