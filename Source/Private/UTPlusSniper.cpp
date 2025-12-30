#include "UTPlusSniper.h"
#include "UnrealTournament.h"
#include "UTProj_Sniper.h"
#include "UTWeaponStateZooming.h"
#include "UTBot.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"




AUTPlusSniper::AUTPlusSniper(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UUTWeaponStateZooming>(TEXT("FiringState1")))
{
	// Standard Sniper Properties (From UTWeap_Sniper)
	DefaultGroup = 9;
	BringUpTime = 0.45f;
	PutDownTime = 0.4f;
	
	StoppedHeadshotScale = 1.1f;
	SlowHeadshotScale = 1.1f;
	AimedHeadshotScale = 1.1f;
	RunningHeadshotScale = 1.1f;
	
	HeadshotDamage = 125;
	BlockedHeadshotDamage = 45;
	
	ProjClass.Insert(AUTProj_Sniper::StaticClass(), 0);
	FOVOffset = FVector(0.1f, 1.f, 1.7f);
	HUDIcon = MakeCanvasIcon(HUDIcon.Texture, 726, 532, 165, 51);
	
	bPrioritizeAccuracy = true;
	BaseAISelectRating = 0.7f;
	BasePickupDesireability = 0.63f;
	
	FiringViewKickback = 0.f;
	FiringViewKickbackY = 0.f;
	bSniping = true;
	HUDViewKickback = FVector2D(0.f, 0.2f);

	KillStatsName = NAME_SniperKills;
	AltKillStatsName = NAME_SniperHeadshotKills;
	DeathStatsName = NAME_SniperDeaths;
	AltDeathStatsName = NAME_SniperHeadshotDeaths;
	HitsStatsName = NAME_SniperHits;
	ShotsStatsName = NAME_SniperShots;
	
	bCheckHeadSphere = true;
	bCheckMovingHeadSphere = true;
	bIgnoreShockballs = true;
	bTrackHitScanReplication = true;

	WeaponCustomizationTag = EpicWeaponCustomizationTags::Sniper;
	WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::Sniper;
	
	TutorialAnnouncements.Add(TEXT("PriSniper"));
	TutorialAnnouncements.Add(TEXT("SecSniper"));
	HighlightText = NSLOCTEXT("Weapon", "SniperHighlightText", "One Man One Bullet");
	
	LowMeshOffset = FVector(0.f, 0.f, -5.f);
	VeryLowMeshOffset = FVector(0.f, 0.f, -11.f);
}


void AUTPlusSniper::ClientNotifyImpressive_Implementation()
{
	OnImpressive();
}

void AUTPlusSniper::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AUTPlusSniper, ImpressiveStreak);
}


float AUTPlusSniper::GetHeadshotScale(AUTCharacter* HeadshotTarget) const
{
	if (HeadshotTarget && HeadshotTarget->UTCharacterMovement && HeadshotTarget->UTCharacterMovement->bIsFloorSliding)
	{
		return RunningHeadshotScale;
	}
	if ((GetUTOwner()->GetVelocity().Size() <= GetUTOwner()->GetCharacterMovement()->MaxWalkSpeedCrouched + 1.0f) &&
		(GetUTOwner()->bIsCrouched || GetUTOwner()->GetCharacterMovement() == NULL || GetUTOwner()->GetCharacterMovement()->GetCurrentAcceleration().Size() < GetUTOwner()->GetCharacterMovement()->MaxWalkSpeedCrouched + 1.0f))
	{
		return (GetUTOwner()->GetVelocity().Size() < 10.f) ? StoppedHeadshotScale : SlowHeadshotScale;
	}
	else if (GetUTOwner()->GetCharacterMovement()->GetCurrentAcceleration().IsZero())
	{
		return AimedHeadshotScale;
	}
	else
	{
		return RunningHeadshotScale;
	}
}

AUTProjectile* AUTPlusSniper::FireProjectile()
{
	AUTProj_Sniper* SniperProj = Cast<AUTProj_Sniper>(Super::FireProjectile());
	if (SniperProj != NULL)
	{
		SniperProj->HeadScaling *= GetHeadshotScale(nullptr);
	}
	return SniperProj;
}

void AUTPlusSniper::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
	bool bHitEnemyPawn = false;
	// ----------------------------------------------------------------------
	// PART 1: TRACE SETUP (Matches UTWeaponFix)
	// ----------------------------------------------------------------------
	checkSlow(InstantHitInfo.IsValidIndex(CurrentFireMode));

	const FVector SpawnLocation = GetFireStartLoc();
	const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
	const FVector FireDir = SpawnRotation.Vector();
	const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

	FHitResult Hit;
	AUTPlayerController* UTPC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : NULL;
	AUTPlayerState* PS = (UTOwner && UTOwner->Controller) ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : NULL;

	// Critical: Use the Fix's Hit Validation time (120ms rewind limit)
	float PredictionTime = GetHitValidationPredictionTime();

	// This calls UTWeaponFix::HitScanTrace, which now handles the detailed rewinding
	HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, Hit, PredictionTime);


	

	// ----------------------------------------------------------------------
	// PART 3: SNIPER SPECIFIC HEAD SEARCH (Standard UT Sniper logic)
	// ----------------------------------------------------------------------
	// Do a second search specifically for the Head Sphere if the capsule miss was close
	/* stock code
	if (UTOwner && Cast<AUTCharacter>(Hit.Actor.Get()) == NULL)
	{
		AUTCharacter* AltTarget = Cast<AUTCharacter>(UUTGameplayStatics::ChooseBestAimTarget(GetUTOwner()->Controller, SpawnLocation, FireDir, 0.7f, (Hit.Location - SpawnLocation).Size(), 150.0f, AUTCharacter::StaticClass()));
		if (AltTarget != NULL && AltTarget->IsHeadShot(SpawnLocation, FireDir, GetHeadshotScale(AltTarget), UTOwner, PredictionTime))
		{
			Hit = FHitResult(AltTarget, AltTarget->GetCapsuleComponent(), SpawnLocation + FireDir * ((AltTarget->GetHeadLocation() - SpawnLocation).Size() - AltTarget->GetCapsuleComponent()->GetUnscaledCapsuleRadius()), -FireDir);
		}
	}
	*/
	if (UTOwner && Cast<AUTCharacter>(Hit.Actor.Get()) == NULL)
	{
		// Find potential head targets using Epic's ChooseBestAimTarget
		AUTCharacter* AltTarget = Cast<AUTCharacter>(UUTGameplayStatics::ChooseBestAimTarget(
			GetUTOwner()->Controller, SpawnLocation, FireDir, 0.7f,
			(Hit.Location - SpawnLocation).Size(), 150.0f, AUTCharacter::StaticClass()));

		if (AltTarget != NULL)
		{
			// Calculate effective head scale
			float EffectiveHeadScale = GetHeadshotScale(AltTarget);

			// Apply padding for client-claimed targets
			if (AltTarget == ReceivedHitScanHitChar)
			{
				bool bTargetMoving = !AltTarget->GetVelocity().IsNearlyZero(10.0f);
				float HeadPadding = bTargetMoving ? HeadSphereHitPadding : HeadSphereHitPaddingStationary;

				// Convert padding to scale factor: NewRadius = OldRadius * NewScale
				// OldRadius + Padding = OldRadius * NewScale
				// NewScale = (OldRadius + Padding) / OldRadius = 1 + Padding/OldRadius
				// Extra padding for vertical movement (falling/dodging)
				bool bVerticalMovement = FMath::Abs(AltTarget->GetVelocity().Z) > 300.f;
				if (bVerticalMovement)
				{
					HeadPadding *= 1.2f;  // 20% more padding during vertical movement
				}
				if (AltTarget->HeadRadius > 0.f)
				{
					float PaddingAsScale = HeadPadding / (AltTarget->HeadRadius * AltTarget->HeadScale);
					EffectiveHeadScale += PaddingAsScale;
				}
			}

			// NOW IsHeadShot will properly rewind (with our TeamArenaCharacter fix)
			if (AltTarget->IsHeadShot(SpawnLocation, FireDir, EffectiveHeadScale, UTOwner, PredictionTime))
			{
				// Construct hit result using REWOUND head position
				FVector RewoundHeadLoc = AltTarget->GetHeadLocation(PredictionTime);

				float HitDist = (RewoundHeadLoc - SpawnLocation).Size()
					- AltTarget->GetCapsuleComponent()->GetUnscaledCapsuleRadius();

				Hit = FHitResult(AltTarget, AltTarget->GetCapsuleComponent(),
					SpawnLocation + FireDir * FMath::Max(0.f, HitDist), -FireDir);

#if !UE_BUILD_SHIPPING
				if (Role == ROLE_Authority)
				{
					UE_LOG(LogTemp, Verbose, TEXT("[Sniper] Secondary head hit on %s, Scale=%.2f (base %.2f + padding)"),
						*AltTarget->GetName(), EffectiveHeadScale, GetHeadshotScale(AltTarget));
				}
#endif
			}
		}
	}

	// ----------------------------------------------------------------------
	// PART 4: SERVER SIDE VISUALS & WARNINGS
	// ----------------------------------------------------------------------
	if (Role == ROLE_Authority)
	{
		if (PS && (ShotsStatsName != NAME_None))
		{
			PS->ModifyStatsValue(ShotsStatsName, 1);
		}
		uint8 FlashExtra = 0;
		if (Hit.Actor.Get() != nullptr && Cast<APawn>(Hit.Actor.Get()) != nullptr)
		{
			FlashExtra = 1;
		}

		UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
		UTOwner->SetFlashExtra(FlashExtra, CurrentFireMode);
		UTOwner->ForceNetUpdate();

		// Warn Bots
		if (UTPC != NULL)
		{
			APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
			if (bDealDamage && PawnTarget != NULL)
			{
				AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
				if (EnemyBot != NULL) EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
			}
		}
		else if (bDealDamage)
		{
			AUTBot* B = UTOwner ? Cast<AUTBot>(UTOwner->Controller) : nullptr;
			if (B != NULL)
			{
				APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
				if (PawnTarget == NULL) PawnTarget = Cast<APawn>(B->GetTarget());
				if (PawnTarget != NULL)
				{
					AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
					if (EnemyBot != NULL) EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
				}
			}
		}
	}
	else 
	{
		// CLIENT SIDE VISUALS
		if (PredictionTime > 0.f)
		{
			PlayPredictedImpactEffects(Hit.Location);
		}
		else
		{
			// Ensure 0ms prediction (Instant Hit) still draws the beam locally!
			UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
		}
	}


	// ----------------------------------------------------------------------
	// PART 5: DAMAGE CALCULATION (Sniper Specific Logic)
	// ----------------------------------------------------------------------
	if (Hit.Actor != NULL && Hit.Actor->bCanBeDamaged && bDealDamage)
	{
		int32 Damage = GetHitScanDamage();
		TSubclassOf<UDamageType> DamageType = InstantHitInfo[CurrentFireMode].DamageType;
		bool bIsHeadShot = false;
		bool bBlockedHeadshot = false;
		AUTCharacter* C = Cast<AUTCharacter>(Hit.Actor.Get());

		if (C != NULL && CanHeadShot())
		{
			// Calculate effective head scale with padding (same as Part 3)
			float EffectiveHeadScale = GetHeadshotScale(C);

			if (C == ReceivedHitScanHitChar)
			{
				bool bTargetMoving = !C->GetVelocity().IsNearlyZero(10.0f);
				float HeadPadding = bTargetMoving ? HeadSphereHitPadding : HeadSphereHitPaddingStationary;

				bool bVerticalMovement = FMath::Abs(C->GetVelocity().Z) > 300.f;
				if (bVerticalMovement)
				{
					HeadPadding *= 1.2f;
				}

				if (C->HeadRadius > 0.f)
				{
					float PaddingAsScale = HeadPadding / (C->HeadRadius * C->HeadScale);
					EffectiveHeadScale += PaddingAsScale;
				}
			}

			if (C->IsHeadShot(Hit.Location, FireDir, EffectiveHeadScale, UTOwner, PredictionTime))
			{
				bIsHeadShot = true;
				if (C->BlockedHeadShot(Hit.Location, FireDir, EffectiveHeadScale, true, UTOwner))
				{
					Damage = BlockedHeadshotDamage;
					bBlockedHeadshot = true;
				}
				else
				{
					AUTBot* B = UTOwner ? Cast<AUTBot>(UTOwner->Controller) : nullptr;
					if (!B || (B->Skill + B->Personality.Accuracy > 3.5f))
					{
						Damage = HeadshotDamage;
					}
				}
				if (HeadshotDamageType != NULL)
				{
					DamageType = HeadshotDamageType;
				}
			}
		}

		OnHitScanDamage(Hit, FireDir);
		Hit.Actor->TakeDamage(Damage, FUTPointDamageEvent(Damage, Hit, FireDir, DamageType, FireDir * InstantHitInfo[CurrentFireMode].Momentum), (UTOwner ? UTOwner->Controller : nullptr), this);

		if ((Role == ROLE_Authority) && bIsHeadShot && C && (C->Health > 0) && (bBlockedHeadshot || (Damage >= 100)))
		{
			C->NotifyBlockedHeadShot(UTOwner);
		}
		if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None))
		{
			PS->ModifyStatsValue(HitsStatsName, 1);
		}
		if (Role == ROLE_Authority && C && C != UTOwner)
		{
			bHitEnemyPawn = true;
		}
	}

	if (OutHit != NULL)
	{
		*OutHit = Hit;
	}

	if (Role == ROLE_Authority && bTrackImpressive)
	{
		if (bHitEnemyPawn)
		{
			ImpressiveStreak++;

			if (ImpressiveStreak >= ImpressiveThreshold)
			{
				// Server to owning client; BP implements OnImpressive()
				ClientNotifyImpressive();
			}
		}
		else
		{
			ImpressiveStreak = 0;
		}
	}
	// Clean up caches from UTWeaponFix
	if (UTOwner)
	{
		TargetedCharacter = nullptr;
		if (UTPC) UTPC->LastShotTargetGuess = nullptr;
	}
}


void AUTPlusSniper::OnServerHitScanResult(const FHitResult& Hit, float PredictionTime)
{
	if (!bTrackImpressive || Role != ROLE_Authority)
	{
		return;
	}
	/*
	AUTCharacter* HitChar = Cast<AUTCharacter>(Hit.GetActor());
	const bool bHitEnemyPawn = (HitChar != nullptr && HitChar != UTOwner);

	if (bHitEnemyPawn)
	{
		ImpressiveStreak++;

		if (ImpressiveStreak >= ImpressiveThreshold)
		{
			AUTPlayerController* PC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
			if (PC && PC->IsLocalController())
			{
				ClientNotifyImpressive();
			}
		}
	}
	else
	{
		ImpressiveStreak = 0;
	}
	*/
}



bool AUTPlusSniper::CanHeadShot()
{
	return true;
}


int32 AUTPlusSniper::GetHitScanDamage()
{
	return InstantHitInfo[CurrentFireMode].Damage;;
}



void AUTPlusSniper::PlayPredictedImpactEffects(FVector ImpactLoc)
{
	// Clear the flash extra so we don't accidentally predict a "flesh hit" 
	/* on the client before the server confirms it.
	if (UTOwner)
	{
		UTOwner->SetFlashExtra(0, CurrentFireMode);
	}
	*/
	SetFlashExtra(nullptr);
	Super::PlayPredictedImpactEffects(ImpactLoc);
}


// Copied directly from UTWeap_Sniper to handle Zooming Vis/Audio
void AUTPlusSniper::OnRep_ZoomState_Implementation()
{
	// Note: We call Super (UTWeapon) but we want the Zoom specific logic here
	// UTWeaponFix doesn't override this, so it goes to UTWeapon's implementation which is fine
	//Super::OnRep_ZoomState_Implementation();
	if (GetNetMode() != NM_DedicatedServer && ZoomState == EZoomState::EZS_NotZoomed && GetUTOwner() && GetUTOwner()->GetPlayerCameraManager())
	{
		GetUTOwner()->GetPlayerCameraManager()->UnlockFOV();
	}

	if (GetNetMode() != NM_DedicatedServer)
	{
		UUTWeaponStateZooming* WeaponStateZooming = FiringState.IsValidIndex(1) ? Cast<UUTWeaponStateZooming>(FiringState[1]) : nullptr;
		if (WeaponStateZooming != nullptr)
		{
			if (ZoomState == EZoomState::EZS_NotZoomed)
			{
				SetActorHiddenInGame(false);
				WeaponStateZooming->ToggleZoomInSound(false);
				UUTGameplayStatics::UTPlaySound(GetWorld(), WeaponStateZooming->ZoomOutSound, GetUTOwner(), SRT_None, false, FVector::ZeroVector, NULL, NULL, false);
			}
			else
			{
				SetActorHiddenInGame(true);

				if (ZoomState == EZoomState::EZS_ZoomingIn)
				{
					WeaponStateZooming->ToggleZoomInSound(true);
					UUTGameplayStatics::UTPlaySound(GetWorld(), WeaponStateZooming->ZoomInSound, GetUTOwner(), SRT_None, false, FVector::ZeroVector, NULL, NULL, false);
				}
				else
				{
					WeaponStateZooming->ToggleZoomInSound(false);
				}
			}
		}
	}
}

// Copied directly from UTWeap_Sniper
float AUTPlusSniper::GetAISelectRating_Implementation()
{
	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	if (B == NULL)
	{
		return BaseAISelectRating;
	}
	else if (Cast<APawn>(B->GetTarget()) == NULL)
	{
		return BaseAISelectRating - 0.15f;
	}
	else if (B->GetEnemy() == NULL)
	{
		return BaseAISelectRating;
	}
	else
	{
		float Result = B->IsStopped() ? (BaseAISelectRating + 0.1f) : (BaseAISelectRating - 0.1f);
		const FVector EnemyLoc = B->GetEnemyLocation(B->GetEnemy(), false);
		float ZDiff = UTOwner->GetActorLocation().Z - EnemyLoc.Z;
		if (ZDiff < -B->TacticalHeightAdvantage)
		{
			Result += 0.1;
		}
		float Dist = (EnemyLoc - UTOwner->GetActorLocation()).Size();
		if (Dist > 4500.0f)
		{
			if (!CanAttack(B->GetEnemy(), EnemyLoc, false))
			{
				Result -= 0.15f;
			}
			return FMath::Min<float>(2.0f, Result + (Dist - 4500.0f) * 0.0001);
		}
		else if (!CanAttack(B->GetEnemy(), EnemyLoc, false))
		{
			return BaseAISelectRating - 0.1f;
		}
		else
		{
			return Result;
		}
	}
}
