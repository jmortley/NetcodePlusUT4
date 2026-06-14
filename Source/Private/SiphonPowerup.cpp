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

	// HUD icon — reuse UDamage icon from HUDAtlas01 with green tint for Siphon
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDAtlasObj(
		TEXT("/Game/RestrictedAssets/UI/HUDAtlas01"));
	if (HUDAtlasObj.Succeeded())
	{
		HUDIcon = MakeCanvasIcon(HUDAtlasObj.Object, 589.f, 0.f, 45.f, 39.f);
		IconColor = FLinearColor(0.2f, 1.0f, 0.2f, 1.0f); // Green to distinguish from UDamage's magenta
	}

	// Siphon lasts 30 seconds
	TimeRemaining = 30.f;
	TriggeredTime = 15.f;

	// Life steal defaults
	SiphonPercent = 0.5f;
	HealCap = 199;

	// Reuse Berserk stat tracking (same conceptual slot — Berserk never spawns in
	// Wipeout so the slots are free). Time = seconds Siphon was held; Count = number
	// of Siphons picked up (auto-incremented by AUTPickupInventory on pickup when
	// StatsNameCount != NAME_None — see UTPickupInventory.cpp:586). No engine edit /
	// new FName needed; both names are already registered in StatManager.
	StatsNameTime  = NAME_BerserkTime;
	StatsNameCount = NAME_BerserkCount;

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

	// Load sounds at CDO time using LoadObject — fails silently (returns nullptr)
	// instead of popping an error dialog when the pak isn't downloaded yet.
	SiphonAmbientSound = LoadObject<USoundBase>(nullptr,
		TEXT("/Game/RestrictedAssets/Audio/Gameplay/A_Powerup_Invulnerability_PowerLoop.A_Powerup_Invulnerability_PowerLoop"));
	PickupAnnouncerSound = LoadObject<USoundBase>(nullptr,
		TEXT("/Game/RestrictedAssets/Audio/AnnouncerReward/A_Announcer_Siphon.A_Announcer_Siphon"));
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
