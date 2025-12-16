// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.
#include "UTPlusShockRifle.h"
#include "UTProj_ShockBall.h"
#include "UTCanvasRenderTarget2D.h"
#include "StatNames.h"
#include "Core.h"
#include "Engine.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Net/UnrealNetwork.h"

const FName NAME_ShockPrimaryShots(TEXT("ShockPrimaryShots"));
const FName NAME_ShockPrimaryHits(TEXT("ShockPrimaryHits"));

// Suppress DLL linkage warnings when overriding base game functions in a plugin
#ifdef _MSC_VER
#pragma warning(disable: 4273)
#endif

AUTPlusShockRifle::AUTPlusShockRifle(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DefaultGroup = 4;
	BaseAISelectRating = 0.65f;
	BasePickupDesireability = 0.65f;
	ScreenMaterialID = 5;
	LastClientKillTime = -100000.0f;
	bFPIgnoreInstantHitFireOffset = false;
	FOVOffset = FVector(0.6f, 0.9f, 1.2f);

	KillStatsName = NAME_ShockBeamKills;
	AltKillStatsName = NAME_ShockCoreKills;
	DeathStatsName = NAME_ShockBeamDeaths;
	AltDeathStatsName = NAME_ShockCoreDeaths;
	HitsStatsName = NAME_ShockRifleHits;
	ShotsStatsName = NAME_ShockRifleShots;
	bCheckHeadSphere = true;
	bCheckMovingHeadSphere = true;
	bTrackHitScanReplication = true;

	WeaponCustomizationTag = EpicWeaponCustomizationTags::ShockRifle;
	WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::ShockRifle;

	TutorialAnnouncements.Add(TEXT("PriShockRifle"));
	TutorialAnnouncements.Add(TEXT("SecShockRifle"));

	FiringViewKickback = -50.f;
	FiringViewKickbackY = 0.f;
	HighlightText = NSLOCTEXT("Weapon", "ShockHighlightText", "Don't Tase Me Bro");
	LowMeshOffset = FVector(0.f, 0.f, -4.f);
	VeryLowMeshOffset = FVector(0.f, 0.f, -15.f);
}


void AUTPlusShockRifle::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUTPlusShockRifle, ImpressiveStreak);
}

void AUTPlusShockRifle::OnServerHitScanResult(const FHitResult& Hit, float PredictionTime)
{

}

void AUTPlusShockRifle::SetupSpecialMaterials()
{
	Super::SetupSpecialMaterials();

	if (!IsRunningDedicatedServer() && Mesh != NULL && ScreenMaterialID < Mesh->GetNumMaterials())
	{
		ScreenMI = Mesh->CreateAndSetMaterialInstanceDynamic(ScreenMaterialID);
		ScreenTexture = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(), 64, 64);
		ScreenTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		ScreenTexture->OnCanvasRenderTargetUpdate.AddDynamic(this, &AUTPlusShockRifle::UpdateScreenTexture);
		ScreenMI->SetTextureParameterValue(FName(TEXT("ScreenTexture")), ScreenTexture);
	}
}

void AUTPlusShockRifle::UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height)
{
	if (GetWorld()->TimeSeconds - LastClientKillTime < 2.5f && ScreenKillNotifyTexture != NULL)
	{
		C->SetDrawColor(FColor::White);
		C->DrawTile(ScreenKillNotifyTexture, 0.0f, 0.0f, float(Width), float(Height), 0.0f, 0.0f, ScreenKillNotifyTexture->GetSizeX(), ScreenKillNotifyTexture->GetSizeY());
	}
	else
	{
		FFontRenderInfo RenderInfo;
		RenderInfo.bClipText = true;
		RenderInfo.GlowInfo.bEnableGlow = true;
		RenderInfo.GlowInfo.GlowColor = FLinearColor(-0.75f, -0.75f, -0.75f, 1.0f);
		RenderInfo.GlowInfo.GlowOuterRadius.X = 0.45f;
		RenderInfo.GlowInfo.GlowOuterRadius.Y = 0.475f;
		RenderInfo.GlowInfo.GlowInnerRadius.X = 0.475f;
		RenderInfo.GlowInfo.GlowInnerRadius.Y = 0.5f;

		bool bInfiniteAmmo = true;
		for (int32 Cost : AmmoCost)
		{
			if (Cost > 0)
			{
				bInfiniteAmmo = false;
				break;
			}
		}
		FString AmmoText = bInfiniteAmmo ? TEXT("--") : FString::FromInt(Ammo);
		float XL, YL;
		C->TextSize(ScreenFont, AmmoText, XL, YL);

		// Use standard FCanvasTextItem instead of FUTCanvasTextItem to avoid linker issues
		FCanvasTextItem Item(FVector2D(Width / 2 - XL * 0.5f, Height / 2 - YL * 0.5f), FText::FromString(AmmoText), ScreenFont, (Ammo <= 5) ? FLinearColor::Red : FLinearColor::White);
		Item.FontRenderInfo = RenderInfo;
		Item.bOutlined = true;
		Item.OutlineColor = FLinearColor::Black;
		C->DrawItem(Item);
	}
}

void AUTPlusShockRifle::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (ScreenTexture != NULL && Mesh->IsRegistered() && GetWorld()->TimeSeconds - Mesh->LastRenderTime < 0.1f)
	{
		ScreenTexture->FastUpdateResource();
	}
}

UAnimMontage* AUTPlusShockRifle::GetFiringAnim(uint8 FireMode, bool bOnHands) const
{
	if (FireMode == 0 && bPlayComboEffects && ComboFireAnim != NULL)
	{
		return (bOnHands ? ComboFireAnimHands : ComboFireAnim);
	}
	else
	{
		return Super::GetFiringAnim(FireMode, bOnHands);
	}
}

void AUTPlusShockRifle::PlayFiringEffects()
{
	Super::PlayFiringEffects();

	if (bPlayComboEffects && ShouldPlay1PVisuals())
	{
		Play1PComboEffects();
		bPlayComboEffects = false;
	}
}

void AUTPlusShockRifle::HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime)
{
	// UTWeaponFix will automatically use GetHitValidationPredictionTime()
	// which we override below to provide different values for beam vs projectile
	Super::HitScanTrace(StartLocation, EndTrace, TraceRadius, Hit, PredictionTime);

	bPlayComboEffects = (Cast<AUTProj_ShockBall>(Hit.GetActor()) != NULL);
}


void AUTPlusShockRifle::ClientNotifyImpressive_Implementation()
{
	OnImpressive();
}




float AUTPlusShockRifle::GetHitValidationPredictionTime() const
{
	// Just use parent's implementation (120ms default)
	// Projectiles don't use HitScanTrace anyway, so this only affects beam
	return Super::GetHitValidationPredictionTime();
}

bool AUTPlusShockRifle::WaitingForCombo()
{
	if (ComboTarget != NULL && !ComboTarget->IsPendingKillPending() && !ComboTarget->bExploded)
	{
		return true;
	}
	else
	{
		ComboTarget = NULL;
		return false;
	}
}

void AUTPlusShockRifle::DoCombo()
{
	ComboTarget = NULL;
	if (UTOwner != NULL)
	{
		UTOwner->StartFire(0);
	}
}

bool AUTPlusShockRifle::IsPreparingAttack_Implementation()
{
	return !bMovingComboCheckResult && WaitingForCombo();
}

float AUTPlusShockRifle::SuggestAttackStyle_Implementation()
{
	return -0.4f;
}

float AUTPlusShockRifle::GetAISelectRating_Implementation()
{
	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	if (B == NULL || B->GetEnemy() == NULL || Cast<APawn>(B->GetTarget()) == NULL)
	{
		return BaseAISelectRating;
	}
	else if (WaitingForCombo())
	{
		return 1.5f;
	}
	else if (!B->WeaponProficiencyCheck())
	{
		return BaseAISelectRating;
	}
	else
	{
		FVector EnemyLoc = B->GetEnemyLocation(B->GetEnemy(), true);
		if (B->IsStopped())
		{
			if (!B->LineOfSightTo(B->GetEnemy()) && (EnemyLoc - UTOwner->GetActorLocation()).Size() < 11000.0f)
			{
				return BaseAISelectRating + 0.5f;
			}
			else
			{
				return BaseAISelectRating + 0.3f;
			}
		}
		else if ((EnemyLoc - UTOwner->GetActorLocation()).Size() > 3500.0f)
		{
			return BaseAISelectRating + 0.1f;
		}
		else if (EnemyLoc.Z > UTOwner->GetActorLocation().Z + 325.0f)
		{
			return BaseAISelectRating + 0.15f;
		}
		else
		{
			return BaseAISelectRating;
		}
	}
}

bool AUTPlusShockRifle::ShouldAIDelayFiring_Implementation()
{
	if (!WaitingForCombo())
	{
		return false;
	}
	else if (bMovingComboCheckResult)
	{
		return true;
	}
	else
	{
		AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
		if (B != NULL && !B->IsStopped())
		{
			ComboTarget->ClearBotCombo();
			ComboTarget = NULL;
			return false;
		}
		else
		{
			return true;
		}
	}
}

bool AUTPlusShockRifle::CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc)
{
	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	if (B == NULL)
	{
		return Super::CanAttack_Implementation(Target, TargetLoc, bDirectOnly, bPreferCurrentMode, BestFireMode, OptimalTargetLoc);
	}
	else if (WaitingForCombo() && (Target == ComboTarget || Target == B->GetTarget()))
	{
		BestFireMode = 0;
		return true;
	}
	else if (Super::CanAttack_Implementation(Target, TargetLoc, bDirectOnly, bPreferCurrentMode, BestFireMode, OptimalTargetLoc))
	{
		if (bPreferCurrentMode)
		{
			return true;
		}
		else
		{
			if (Cast<APawn>(Target) == NULL)
			{
				BestFireMode = 0;
			}
			else
			{
				float EnemyDist = (TargetLoc - UTOwner->GetActorLocation()).Size();
				const AUTProjectile* DefAltProj = (ProjClass.IsValidIndex(1) && ProjClass[1] != NULL) ? ProjClass[1].GetDefaultObject() : NULL;
				const float AltSpeed = (DefAltProj != NULL && DefAltProj->ProjectileMovement != NULL) ? DefAltProj->ProjectileMovement->InitialSpeed : FLT_MAX;

				if (EnemyDist > 4.0f * AltSpeed)
				{
					bPlanningCombo = false;
					BestFireMode = 0;
				}
				else
				{
					ComboTarget = NULL;
					if (EnemyDist > 5500.0f && FMath::FRand() < 0.5f)
					{
						BestFireMode = 0;
					}
					else if (B->CanCombo() && B->WeaponProficiencyCheck())
					{
						bPlanningCombo = true;
						BestFireMode = 1;
					}
					else
					{
						AUTCharacter* EnemyChar = Cast<AUTCharacter>(Target);
						if (EnemyDist < 2200.0f && EnemyChar != NULL && EnemyChar->GetWeapon() != NULL && EnemyChar->GetWeapon()->GetClass() != GetClass() && B->WeaponProficiencyCheck())
						{
							BestFireMode = (FMath::FRand() < 0.3f) ? 0 : 1;
						}
						else
						{
							BestFireMode = (FMath::FRand() < 0.7f) ? 0 : 1;
						}
					}
				}
			}
			return true;
		}
	}
	else if (bDirectOnly)
	{
		return false;
	}
	else if ((bPreferCurrentMode && bPlanningCombo) || GetWorld()->TimeSeconds - LastPredictiveComboCheckTime >= 1.0f)
	{
		LastPredictiveComboCheckTime = GetWorld()->TimeSeconds;
		if (!bPlanningCombo && !B->CanCombo())
		{
			bPlanningCombo = false;
			return false;
		}
		else if (bPreferCurrentMode && !PredictiveComboTargetLoc.IsZero() && !GetWorld()->LineTraceTestByChannel(GetFireStartLoc(1), PredictiveComboTargetLoc, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionQueryParams(FName(TEXT("PredictiveCombo")), true, UTOwner)))
		{
			OptimalTargetLoc = PredictiveComboTargetLoc;
			BestFireMode = 1;
			bPlanningCombo = true;
			return true;
		}
		else
		{
			PredictiveComboTargetLoc = FVector::ZeroVector;
			TArray<FVector> FoundPoints;
			B->GuessAppearancePoints(Target, TargetLoc, true, FoundPoints);
			if (FoundPoints.Num() > 0)
			{
				int32 StartIndex = FMath::RandHelper(FoundPoints.Num());
				int32 i = StartIndex;
				do
				{
					i = (i + 1) % FoundPoints.Num();
					if (!GetWorld()->LineTraceTestByChannel(GetFireStartLoc(1), FoundPoints[i], COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionQueryParams(FName(TEXT("PredictiveCombo")), true, UTOwner)))
					{
						PredictiveComboTargetLoc = FoundPoints[i];
						break;
					}
				} while (i != StartIndex);

				bPlanningCombo = !PredictiveComboTargetLoc.IsZero();
				return bPlanningCombo;
			}
			else
			{
				bPlanningCombo = false;
				return false;
			}
		}
	}
	else
	{
		return false;
	}
}

AUTProjectile* AUTPlusShockRifle::FireProjectile()
{
	AUTProjectile* Result = Super::FireProjectile();
	if (bPlanningCombo && UTOwner != NULL)
	{
		AUTProj_ShockBall* ShockBall = Cast<AUTProj_ShockBall>(Result);
		if (ShockBall != NULL)
		{
			ShockBall->StartBotComboMonitoring();
			ComboTarget = ShockBall;
			AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
			if (B != NULL)
			{
				bMovingComboCheckResult = B->MovingComboCheck();
			}
			bPlanningCombo = false;
			PredictiveComboTargetLoc = FVector::ZeroVector;
		}
	}
	return Result;
}

int32 AUTPlusShockRifle::GetWeaponKillStats(AUTPlayerState* PS) const
{
	int32 KillCount = Super::GetWeaponKillStats(PS);
	if (PS)
	{
		KillCount += PS->GetStatsValue(NAME_ShockComboKills);
	}
	return KillCount;
}

int32 AUTPlusShockRifle::GetWeaponKillStatsForRound(AUTPlayerState* PS) const
{
	int32 KillCount = Super::GetWeaponKillStatsForRound(PS);
	if (PS)
	{
		KillCount += PS->GetRoundStatsValue(NAME_ShockComboKills);
	}
	return KillCount;
}

int32 AUTPlusShockRifle::GetWeaponDeathStats(AUTPlayerState* PS) const
{
	int32 DeathCount = Super::GetWeaponDeathStats(PS);
	if (PS)
	{
		DeathCount += PS->GetStatsValue(NAME_ShockComboDeaths);
	}
	return DeathCount;
}

/*
void AUTPlusShockRifle::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
	// Store this now since it might get cleared below
	bool bIsCombo = bPlayComboEffects;

	Super::FireInstantHit(bDealDamage, OutHit);

	if (Role == ROLE_Authority && bTrackImpressive && OutHit && OutHit->bBlockingHit)
	{
		AUTCharacter* HitChar = Cast<AUTCharacter>(OutHit->Actor.Get());
		bool bHitEnemyPawn = (HitChar != nullptr && HitChar != UTOwner && !HitChar->IsDead());

		if (bHitEnemyPawn)
		{
			ImpressiveStreak++;

			if (ImpressiveStreak >= ImpressiveThreshold)
			{
				// Tell owning client to fire BP event
				ClientNotifyImpressive();
			}
		}
		else
		{
			ImpressiveStreak = 0;
		}
	}
	// Parent (UTWeaponFix) handles all the hybrid networking:
	// - Transaction validation (event indices, correction)
	// - Epic's lag compensation (GetRewindLocation with hit validation time)
	// - Split prediction (uses GetHitValidationPredictionTime() we override)
	
	//OnServerHitScanResult(OutHit, GetHitValidationPredictionTime());
	// FlashExtra 1 will play the ComboEffects for the other clients
	if (Role == ROLE_Authority && UTOwner != nullptr && bIsCombo)
	{
		UTOwner->SetFlashExtra(1, CurrentFireMode);
	}
}
*/


void AUTPlusShockRifle::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
	// Store this now since it might get cleared below
	bool bIsCombo = bPlayComboEffects;

	Super::FireInstantHit(bDealDamage, OutHit);

	// --- SERVER ONLY LOGIC (Stats & Impressive) ---
	if (Role == ROLE_Authority)
	{
		AUTPlayerState* PS = UTOwner ? Cast<AUTPlayerState>(UTOwner->PlayerState) : nullptr;

		// 1. Record Primary SHOT Attempt
		// We do this before checking hits. If we fired mode 0, count it.
		if (CurrentFireMode == 0 && PS)
		{
			PS->ModifyStatsValue(NAME_ShockPrimaryShots, 1);
		}

		// 2. Process Hit Results (for both Impressive Streak AND Accuracy)
		if (OutHit && OutHit->bBlockingHit)
		{
			AUTCharacter* HitChar = Cast<AUTCharacter>(OutHit->Actor.Get());
			// Define a valid hit: Must be a character, not us, and not dead
			bool bHitEnemyPawn = (HitChar != nullptr && HitChar != UTOwner && !HitChar->IsDead());

			// A) Impressive Logic
			if (bTrackImpressive)
			{
				if (bHitEnemyPawn)
				{
					ImpressiveStreak++;
					if (ImpressiveStreak >= ImpressiveThreshold)
					{
						ClientNotifyImpressive();
					}
				}
				else
				{
					// Hit something else (wall, core, etc) -> Reset Streak
					ImpressiveStreak = 0;
				}
			}

			// B) Primary Accuracy Hit Logic
			// If we hit an enemy pawn while using Primary Fire, count the hit.
			// (Intentionally ignores Cores/Combos because bHitEnemyPawn is false for cores)
			if (CurrentFireMode == 0 && bHitEnemyPawn && PS && bDealDamage)
			{
				PS->ModifyStatsValue(NAME_ShockPrimaryHits, 1);
			}
		}
	}

	// --- COMBO FX REPLICATION ---
	// FlashExtra 1 will play the ComboEffects for the other clients
	if (Role == ROLE_Authority && UTOwner != nullptr && bIsCombo)
	{
		UTOwner->SetFlashExtra(1, CurrentFireMode);
	}
}


void AUTPlusShockRifle::FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode)
{
	bPlayComboEffects = (InFireMode == 0 && (NewFlashExtra > 0));
}














