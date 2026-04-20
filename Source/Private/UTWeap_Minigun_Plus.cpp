// UTWeap_Minigun_Plus.cpp
#include "UTWeap_Minigun_Plus.h"
#include "UnrealTournament.h"
#include "UTWeaponState.h"
#include "UTWeaponStateFiring.h"
#include "UTWeaponStateFiringSpinUp.h"
#include "UTSquadAI.h"
#include "StatNames.h"

AUTWeap_Minigun_Plus::AUTWeap_Minigun_Plus(const FObjectInitializer& OI)
	: Super(OI.SetDefaultSubobjectClass<UUTWeaponStateFiringSpinUp>(TEXT("FiringState0")))
{
	DefaultGroup = 6;
	FireEffectInterval = 2;
	Ammo = 80;
	MaxAmmo = 240;
	FOVOffset = FVector(0.01f, 1.f, 1.6f);
	bIgnoreShockballs = true;

	AmmoWarningAmount = 25;
	AmmoDangerAmount = 10;
	HUDIcon = MakeCanvasIcon(HUDIcon.Texture, 453.0f, 509.0f, 148.0f, 53.0f);

	BaseAISelectRating = 0.71f;
	BasePickupDesireability = 0.73f;
	bRecommendSuppressiveFire = true;

	KillStatsName = NAME_MinigunKills;
	AltKillStatsName = NAME_MinigunShardKills;
	DeathStatsName = NAME_MinigunDeaths;
	AltDeathStatsName = NAME_MinigunShardDeaths;
	HitsStatsName = NAME_MinigunHits;
	ShotsStatsName = NAME_MinigunShots;

	WeaponCustomizationTag = EpicWeaponCustomizationTags::Minigun;
	WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::Minigun;
	HighlightText = NSLOCTEXT("Weapon", "StingerHighlightText", "Sting Like a Bee");
	VeryLowMeshOffset = FVector(0.f, 0.f, -7.f);
	HUDViewKickback = FVector2D(0.03f, 0.05f);
	bShouldPrecacheTutorialAnnouncements = false;
}

void AUTWeap_Minigun_Plus::StartFire(uint8 FireModeNum)
{
	// Bypass AUTWeaponFix transactional RPC path. Stock spin-up state handles
	// timing; we want AUTWeaponFix's HitScanTrace override via vtable only.
	AUTWeapon::StartFire(FireModeNum);
}

void AUTWeap_Minigun_Plus::StopFire(uint8 FireModeNum)
{
	AUTWeapon::StopFire(FireModeNum);
}

void AUTWeap_Minigun_Plus::FireShot()
{
	AUTWeapon::FireShot();
}

void AUTWeap_Minigun_Plus::PlayFiringEffects()
{
	HUDViewKickback.X *= -1.f;
	Super::PlayFiringEffects();
}

float AUTWeap_Minigun_Plus::GetAISelectRating_Implementation()
{
	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	if (B == NULL || B->GetEnemy() == NULL)
	{
		return BaseAISelectRating;
	}
	else if (!CanAttack(B->GetEnemy(), B->GetEnemyLocation(B->GetEnemy(), false), false))
	{
		return BaseAISelectRating - 0.15f;
	}
	else
	{
		return BaseAISelectRating * FMath::Min<float>(UTOwner->DamageScaling * UTOwner->GetFireRateMultiplier(), 1.5f);
	}
}

bool AUTWeap_Minigun_Plus::CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc)
{
	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	// prefer to keep current fire mode — spindown cost when switching
	bPreferCurrentMode = bPreferCurrentMode || (Cast<UUTWeaponStateFiringSpinUp>(CurrentState) != nullptr && FMath::FRand() < 0.9f);
	if (Super::CanAttack_Implementation(Target, TargetLoc, bDirectOnly, bPreferCurrentMode, BestFireMode, OptimalTargetLoc))
	{
		if (!bPreferCurrentMode && B != nullptr)
		{
			if (UTOwner->DamageScaling * UTOwner->GetFireRateMultiplier() > 1.0f)
			{
				BestFireMode = 0;
			}
			else
			{
				const float TargetDist = (TargetLoc - B->GetPawn()->GetActorLocation()).Size();
				if (TargetDist < 3000.0f)
				{
					BestFireMode = 0;
				}
				else if (TargetDist < 5000.0f)
				{
					BestFireMode = FMath::FRand() < TargetDist / 5000.0f ? 1 : 0;
				}
				else
				{
					BestFireMode = 1;
				}
			}
		}
		return true;
	}
	else if (B != nullptr && Target == B->GetEnemy() && GetWorld()->TimeSeconds - B->GetEnemyInfo(B->GetEnemy(), false)->LastSeenTime < 1.0f)
	{
		// alt fire radius might still hit
		OptimalTargetLoc = B->GetEnemyInfo(B->GetEnemy(), false)->LastSeenLoc;
		BestFireMode = 1;
		return true;
	}
	else
	{
		return false;
	}
}

bool AUTWeap_Minigun_Plus::HasAmmo(uint8 FireModeNum)
{
	return (Ammo >= 1);
}
