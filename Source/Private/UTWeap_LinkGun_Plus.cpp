#include "UTWeap_LinkGun_Plus.h"
#include "UnrealTournament.h"
#include "UTWeaponStateFiringLinkBeamPlus.h"
#include "UTPlayerController.h"
#include "UTProj_LinkPlasma.h"
#include "UTTeamGameMode.h"
#include "UnrealNetwork.h"
#include "StatNames.h"
#include "UTSquadAI.h"
#include "UTWeaponStateFiring.h"
#include "Animation/AnimInstance.h"
#include "UTRewardMessage.h"
#include "UTCharacter.h"




AUTWeap_LinkGun_Plus::AUTWeap_LinkGun_Plus(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Override the default firing state for the Beam (Mode 1)
	// We assume Mode 0 is Plasma (Projectile) and Mode 1 is Beam

	LowPingThreshold = 80.0f;
	HysteresisBuffer = 5.0f;
	MaxHitDistanceTolerance = 200.0f;
	HighPingBeamWidthPadding = 2.5f;
	BeamTimeoutDuration = 0.5f;
	LastBeamActivityTime = 0.f;
	//MaxHitDistanceTolerance = 300.0f; // Allow 3 meters of lag discrepancy
	ClientDamageBatchSize = 15;
	CurrentLinkedTarget = nullptr;
	LinkStartTime = -100.f;
	if (FiringState.Num() > 0)
	{
		FireInterval[1] = 0.12f;
		InstantHitInfo.AddZeroed();
		InstantHitInfo[0].Damage = 9;
		InstantHitInfo[0].TraceRange = 2200.0f;
	}
	if (AmmoCost.Num() < 2)
	{
		AmmoCost.SetNum(2);
	}
	DefaultGroup = 5;
	AmmoCost[0] = 1;
	AmmoCost[1] = 1;
	FOVOffset = FVector(0.6f, 1.f, 1.f);
	Ammo = 60;
	MaxAmmo = 200;
	AmmoWarningAmount = 25;
	AmmoDangerAmount = 10;

	HUDIcon = MakeCanvasIcon(HUDIcon.Texture, 453.0f, 467.0, 147.0f, 41.0f);

	BeamPulseInterval = 0.3f;
	BeamPulseMomentum = -220000.0f;
	BeamPulseAmmoCost = 4;
	PullWarmupTime = 0.15f;
	LinkPullDamage = 25;
	ReadyToPullColor = FLinearColor::White;
	HUDViewKickback = FVector2D(0.f, 0.05f);

	bRecommendSuppressiveFire = true;

	KillStatsName = NAME_LinkKills;
	AltKillStatsName = NAME_LinkBeamKills;
	DeathStatsName = NAME_LinkDeaths;
	AltDeathStatsName = NAME_LinkBeamDeaths;
	HitsStatsName = NAME_LinkHits;
	ShotsStatsName = NAME_LinkShots;

	ScreenMaterialID = 2;
	SideScreenMaterialID = 1;
	FiringBeamKickbackY = 0.f;
	LinkPullKickbackY = 300.f;

	WeaponCustomizationTag = EpicWeaponCustomizationTags::LinkGun;
	WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::LinkGun;

	TutorialAnnouncements.Add(TEXT("PriLinkGun"));
	TutorialAnnouncements.Add(TEXT("SecLinkGun"));
	HighlightText = NSLOCTEXT("Weapon", "LinkHighlightText", "Plasma Boy");
	LowMeshOffset = FVector(0.f, 0.f, -3.f);
	VeryLowMeshOffset = FVector(0.f, 0.f, -10.f);
}



void AUTWeap_LinkGun_Plus::FireShot()
{
	// Mode 1 is the Beam


	if (GetCurrentFireMode() == 1)
	{
		// BYPASS: Skip AUTWeaponFix's gatekeeper logic.
		// Call the base engine version directly to handle Visuals/Ammo/Inventory events.
		// Note: We scope it to AUTWeapon (Grandparent) explicitly.
		AUTWeapon::FireShot();
	}
	else if (!bIsInCoolDown)
	{
		// Mode 0 is Plasma (Projectile).
		// Use the standard Fix logic (Transactional) for this.
		AUTWeapon::FireShot();
	}
}



bool AUTWeap_LinkGun_Plus::ServerStopBeamFiring_Validate() { return true; }

void AUTWeap_LinkGun_Plus::ServerStopBeamFiring_Implementation()
{
	if (UTOwner)
	{
		UTOwner->ClearFiringInfo();
	}
	// Force exit to active state if still in beam firing
	if (CurrentFireMode == 1 && CurrentState != ActiveState)
	{
		GotoActiveState();
	}
}



void AUTWeap_LinkGun_Plus::ProcessClientSideHit(float DeltaTime, AActor* HitActor, FVector HitLoc, const FInstantHitDamageInfo& DamageInfo)
{
	UUTWeaponStateFiringLinkBeamPlus* BeamState = Cast<UUTWeaponStateFiringLinkBeamPlus>(GetCurrentState());
	if (!BeamState) return;

	float RefireTime = GetRefireTime(GetCurrentFireMode());
	float DamagePerSec = float(DamageInfo.Damage) / RefireTime;

	BeamState->ClientDamageAccumulator += DamagePerSec * DeltaTime;

	// Use the Configurable Batch Size
	int32 PendingDamage = FMath::TruncToInt(BeamState->ClientDamageAccumulator);

	if (PendingDamage >= ClientDamageBatchSize)
	{
		ServerProcessBeamHit(HitActor, HitLoc, PendingDamage);
		BeamState->ClientDamageAccumulator -= PendingDamage;
	}
}






/*
bool AUTWeap_LinkGun_Plus::ServerProcessBeamHit_Validate(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount)
{
	if (!HitActor) return false;

	// Base cap = BatchSize + one tick's worth of damage as buffer
	// At 233 DPS and 120 tick: ~2 damage per tick max
	// Add generous headroom for frame spikes
	int32 DamageCap = ClientDamageBatchSize + 10;

	if (UTOwner)
	{
		float FireRateMult = UTOwner->GetFireRateMultiplier();
		DamageCap = FMath::CeilToInt(DamageCap * FireRateMult);
	}

	if (DamageAmount > DamageCap)
	{
		return false;
	}

	return true;
}
*/


bool AUTWeap_LinkGun_Plus::ServerProcessBeamHit_Validate(
	AActor* HitActor,
	FVector_NetQuantize HitLocation,
	int32 DamageAmount)
{
	// If the actor is gone, just ignore this batch.

	if (!HitActor)
	{

		return true; // allow, but do nothing in _Implementation when HitActor is null
	}

	int32 DamageCap = 40;
	if (UTOwner)
	{
		float FireRateMult = UTOwner->GetFireRateMultiplier();
		DamageCap = FMath::CeilToInt(40 * FireRateMult);
	}

	if (DamageAmount > DamageCap)
	{

		// don’t kick – just clamp in _Implementation
		return true;
	}

	return true;
}



void AUTWeap_LinkGun_Plus::ServerProcessBeamHit_Implementation(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount)
{
	if (!UTOwner || !InstantHitInfo.IsValidIndex(1)) return;
	LastBeamActivityTime = GetWorld()->GetTimeSeconds();

	if (!HitActor)
	{
		// actor already dead or gone, nothing to do
		return;
	}

	int32 DamageCap = 40;
	if (UTOwner)
	{
		float FireRateMult = UTOwner->GetFireRateMultiplier();
		DamageCap = FMath::CeilToInt(40 * FireRateMult);
	}

	DamageAmount = FMath::Min(DamageAmount, DamageCap);


	AUTPlayerState* PS = UTOwner->Controller ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : nullptr;
	if (!PS) return;

	// ---------------------------------------------------------
	// STEP 1: GLOBAL SANITY CHECKS (Walls, Max Dist)
	// ---------------------------------------------------------
	FVector FireStart = GetFireStartLoc();
	float DistSq = FVector::DistSquared(FireStart, HitLocation);
	float MaxDist = InstantHitInfo[1].TraceRange + MaxHitDistanceTolerance;

	if (DistSq > FMath::Square(MaxDist)) return;

	// Simple wall check (prevents shooting through geometry)
	FHitResult WallHit;
	FCollisionQueryParams Params(FName(TEXT("LinkValidation")), true, UTOwner);
	if (GetWorld()->LineTraceSingleByChannel(WallHit, FireStart, HitLocation, COLLISION_TRACE_WEAPONNOCHARACTER, Params))
	{
		return;
	}

	// ---------------------------------------------------------
	// STEP 2: ADAPTIVE VALIDATION
	// ---------------------------------------------------------

	// Config: 80ms is a safe crossover point
	//const float LowPingThreshold = 80.0f;
	bool bHitValidated = false;
	float CurrentPing = PS->ExactPing;
	// HYSTERESIS LOGIC
	// Define the buffer zone (e.g. +/- 5ms around your 80ms target)

	if (CurrentPing > (LowPingThreshold + HysteresisBuffer))
	{
		bHighPingMode = true;
	}
	else if (CurrentPing < (LowPingThreshold - HysteresisBuffer))
	{
		bHighPingMode = false;
	}
	// If between 75 and 85, keep previous state (bHighPingMode doesn't change)

	// LOGIC SELECTION
	if (!bHighPingMode)
	{
		// PATH A: LOW PING (Trust Client / CSHD)
		bHitValidated = true;
	}
	else
	{
		// --- PATH B: HIGH PING (Lenient Rewind) ---
		// The player is lagging. The server's rewind might be imperfect.
		// We compensate by making the beam THICKER on the server side.

		// 1. Calculate Rewind Time based on their ping
		float PredictionTime = Super::GetHitValidationPredictionTime();

		// 2. Define Leniency
		// Normal beam might be 0.0f or 12.0f. 
		// For high ping, we bump it to 25.0f+ (Approx width of a character torso).
		// This means if the rewind is "close enough", we grant the hit.
		float BaseWidth = InstantHitInfo[1].TraceHalfSize;
		float LenientWidth = BaseWidth + HighPingBeamWidthPadding;

		// 3. Perform the Server-Side Rewind Trace with EXTRA WIDTH
		FHitResult ServerHit;
		FVector TraceEnd = FireStart + (HitLocation - FireStart).GetSafeNormal() * (InstantHitInfo[1].TraceRange);

		// We use HitScanTrace (from your WeaponFix) because it handles the rewind logic internally.
		// We pass 'LenientWidth' instead of the default.
		HitScanTrace(FireStart, TraceEnd, LenientWidth, ServerHit, PredictionTime);

		if (ServerHit.Actor.Get() == HitActor)
		{
			// Server confirms the hit (using the wider beam)
			bHitValidated = true;
		}
		else
		{
			// Even with leniency, the server says you missed.
			// This usually means the player is shooting at a "ghost" or lagging excessively.
			bHitValidated = false;
		}
	}

	if (!bHitValidated) return;

	// ---------------------------------------------------------
	// STEP 3: APPLY DAMAGE
	// ---------------------------------------------------------
	// ... (Same damage logic as before) ...
	CurrentLinkedTarget = HitActor;
	bLinkBeamImpacting = true;
	bLinkCausingDamage = true;

	if (GetWorld()->GetTimeSeconds() - LinkStartTime > 0.5f)
	{
		LinkStartTime = GetWorld()->GetTimeSeconds();
	}

	FVector FireDir = (HitLocation - FireStart).GetSafeNormal();
	HitActor->TakeDamage(DamageAmount,
		FUTPointDamageEvent(DamageAmount, FHitResult(HitActor, nullptr, HitLocation, -FireDir), FireDir, InstantHitInfo[1].DamageType, FireDir * 1000.f),
		UTOwner->Controller,
		this);

	if (PS && HitsStatsName != NAME_None)
	{
		PS->ModifyStatsValue(HitsStatsName, DamageAmount);
	}
}





void AUTWeap_LinkGun_Plus::AttachToOwner_Implementation()
{
	Super::AttachToOwner_Implementation();

	if (!IsRunningDedicatedServer() && Mesh != NULL && ScreenMaterialID < Mesh->GetNumMaterials())
	{
		ScreenMI = Mesh->CreateAndSetMaterialInstanceDynamic(ScreenMaterialID);
		ScreenTexture = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(), 64, 64);
		ScreenTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		ScreenTexture->OnCanvasRenderTargetUpdate.AddDynamic(this, &AUTWeap_LinkGun_Plus::UpdateScreenTexture);
		ScreenMI->SetTextureParameterValue(FName(TEXT("ScreenTexture")), ScreenTexture);

		if (SideScreenMaterialID < Mesh->GetNumMaterials())
		{
			SideScreenMI = Mesh->CreateAndSetMaterialInstanceDynamic(SideScreenMaterialID);
		}
	}
}

void AUTWeap_LinkGun_Plus::UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height)
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

		FString OverheatText = bIsInCoolDown ? TEXT("***") : FString::FromInt(int32(100.f * FMath::Clamp(OverheatFactor, 0.1f, 1.f)));
		float XL, YL;
		C->TextSize(ScreenFont, OverheatText, XL, YL);
		if (!WordWrapper.IsValid())
		{
			WordWrapper = MakeShareable(new FCanvasWordWrapper());
		}
		FLinearColor ScreenColor = (OverheatFactor <= (IsFiring() ? 0.5f : 0.f)) ? FLinearColor::Green : FLinearColor::Yellow;
		if (bIsInCoolDown)
		{
			ScreenColor = FLinearColor::Red;
		}
		if (ScreenMI)
		{
			ScreenMI->SetVectorParameterValue(FName(TEXT("Screen Color")), ScreenColor);
			ScreenMI->SetVectorParameterValue(FName(TEXT("Screen Low Color")), ScreenColor);
		}
		if (SideScreenMI)
		{
			SideScreenMI->SetVectorParameterValue(FName(TEXT("Screen Color")), ScreenColor);
			SideScreenMI->SetVectorParameterValue(FName(TEXT("Screen Low Color")), ScreenColor);
		}
		C->SetDrawColor(ScreenColor.ToFColor(true));
		FCanvasTextItem Item(
			FVector2D(Width / 2 - XL * 0.5f, Height / 2 - YL * 0.5f), // Position
			FText::FromString(OverheatText),                          // Text
			ScreenFont,                                               // Font
			ScreenColor                                               // Color
		);
		Item.FontRenderInfo = RenderInfo;
		C->DrawItem(Item);
	}
}

void AUTWeap_LinkGun_Plus::Removed()
{
	if (UTOwner)
	{
		UTOwner->SetAmbientSound(OverheatSound, true);
	}
	Super::Removed();
}

void AUTWeap_LinkGun_Plus::ClientRemoved()
{
	if (UTOwner)
	{
		UTOwner->SetAmbientSound(OverheatSound, true);
	}
	Super::ClientRemoved();
}

void AUTWeap_LinkGun_Plus::StartFire(uint8 FireModeNum)
{
	if (FireModeNum == 1)
	{
		// BYPASS: For the Beam, skip the "Fix" logic (Transactions/Retry Timers).
		// Go straight to the Grandparent (Standard UT logic).
		AUTWeapon::StartFire(FireModeNum);
	}
	else
	{
		// KEEP: For Plasma (Mode 0), use the "Fix" logic (Rewind/Lag Comp).
		AUTWeapon::StartFire(FireModeNum);
	}
}


AUTProjectile* AUTWeap_LinkGun_Plus::FireProjectile()
{
	AUTProj_LinkPlasma* LinkProj = Cast<AUTProj_LinkPlasma>(Super::FireProjectile());
	if (LinkProj != NULL)
	{
		LastFiredPlasmaTime = GetWorld()->GetTimeSeconds();
	}

	return LinkProj;
}

void AUTWeap_LinkGun_Plus::PlayWeaponAnim(UAnimMontage* WeaponAnim, UAnimMontage* HandsAnim, float RateOverride)
{
	// give pull anim priority
	if (WeaponAnim == PulseAnim || PulseAnim == NULL || Mesh->GetAnimInstance() == NULL || !Mesh->GetAnimInstance()->Montage_IsPlaying(PulseAnim))
	{
		Super::PlayWeaponAnim(WeaponAnim, HandsAnim, RateOverride);
	}
}

bool AUTWeap_LinkGun_Plus::IsLinkPulsing()
{
	return (GetWorld()->GetTimeSeconds() - LastBeamPulseTime < BeamPulseInterval);
}

void AUTWeap_LinkGun_Plus::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (bIsInCoolDown)
	{
		OverheatFactor -= 2.f * DeltaTime;
		if (OverheatFactor <= 0.f)
		{
			OverheatFactor = 0.f;
			bIsInCoolDown = false;
			if (UTOwner)
			{
				UTOwner->SetAmbientSound(OverheatSound, true);
			}
		}
		else if (UTOwner && OverheatSound && (!IsFiring() || !FireLoopingSound.IsValidIndex(CurrentFireMode) || !FireLoopingSound[CurrentFireMode]))
		{
			UTOwner->SetAmbientSound(OverheatSound, false);
			UTOwner->ChangeAmbientSoundPitch(OverheatSound, 0.5f + OverheatFactor);
		}
	}
	else
	{
		OverheatFactor = (UTOwner && IsFiring() && (CurrentFireMode == 0) && (UTOwner->GetFireRateMultiplier() <= 1.f)) ? OverheatFactor + 0.42f * DeltaTime : FMath::Max(0.f, OverheatFactor - 2.2f * FMath::Max(0.f, DeltaTime - FMath::Max(0.f, 0.3f + LastFiredPlasmaTime - GetWorld()->GetTimeSeconds())));
		bIsInCoolDown = (OverheatFactor > 1.f);

	}

	if (!IsLinkPulsing())
	{
		// Identify the Pulse MuzzleFlash Index (It is always at the end of the array)
		int32 PulseIndex = FiringState.Num();

		if (MuzzleFlash.IsValidIndex(PulseIndex) && MuzzleFlash[PulseIndex] != nullptr)
		{
			if (MuzzleFlash[PulseIndex]->IsActive())
			{
				MuzzleFlash[PulseIndex]->DeactivateSystem();
				// Optional: Reset template if needed, though Deactivate is usually enough
			}
		}
	}

	if (ScreenTexture != NULL && Mesh->IsRegistered() && GetWorld()->TimeSeconds - Mesh->LastRenderTime < 0.1f)
	{
		ScreenTexture->FastUpdateResource();
	}

	if (UTOwner && (UTOwner->GetWeapon() == this) && MuzzleFlash.IsValidIndex(1) && MuzzleFlash[1] != NULL)
	{
		static FName NAME_PulseScale(TEXT("PulseScale"));
		float NewScale = 1.0f + FMath::Max<float>(0.0f, 1.0f - (GetWorld()->TimeSeconds - LastBeamPulseTime) / 0.35f);
		MuzzleFlash[1]->SetVectorParameter(NAME_PulseScale, FVector(NewScale, NewScale, NewScale));
	}

	if (UTOwner && IsFiring())
	{
		if ((Role == ROLE_Authority) && FireLoopingSound.IsValidIndex(CurrentFireMode) && FireLoopingSound[CurrentFireMode] != NULL && !IsLinkPulsing())
		{
			if (!bLinkBeamImpacting)
			{
				UTOwner->ChangeAmbientSoundPitch(FireLoopingSound[CurrentFireMode], 0.7f);
			}
			else if (bLinkCausingDamage)
			{
				UTOwner->ChangeAmbientSoundPitch(FireLoopingSound[CurrentFireMode], bReadyToPull ? 2.f : 1.7f);

			}
			else
			{
				UTOwner->ChangeAmbientSoundPitch(FireLoopingSound[CurrentFireMode], 1.f);
			}
		}
	}
	if (IsLinkPulsing())
	{
		// update link pull pulse beam endpoint
		const FVector SpawnLocation = GetFireStartLoc();
		const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
		const FVector FireDir = SpawnRotation.Vector();
		PulseLoc = (PulseTarget && !PulseTarget->IsPendingKillPending()) ? PulseTarget->GetActorLocation() : SpawnLocation + GetBaseFireRotation().RotateVector(MissedPulseOffset) + 100.f * FireDir;
		/*
				// don't allow beam to go behind player
				FVector PulseDir = PulseLoc - SpawnLocation;
				float PulseDist = PulseDir.Size();
				PulseDir = (PulseDist > 0.f) ? PulseDir / PulseDist : PulseDir;
				if ((PulseDir | FireDir) < 0.7f)
				{
					PulseDir = PulseDir - FireDir * ((PulseDir | FireDir) - 0.7f);
					PulseDir = PulseDir.GetSafeNormal() * PulseDist;
					PulseLoc = PulseDir + SpawnLocation;
				}

				// make sure beam doesn't clip through geometry
				FHitResult Hit;
				HitScanTrace(SpawnLocation, PulseLoc, 0.f, Hit, 0.f);
				if (Hit.Time < 1.f)
				{
					PulseLoc = Hit.Location;
				}*/
	}
	else if (UTOwner != nullptr)
	{
		UTOwner->PulseTarget = nullptr;
	}

	UUTWeaponStateFiring_LoopingFire* LoopingState = Cast<UUTWeaponStateFiring_LoopingFire>(GetCurrentState());
	if (LoopingState)
	{
		if (bIsInCoolDown)
		{
			LoopingState->EnterCooldown();
		}
		else
		{
			LoopingState->ExitCooldown();
		}
	}
	if (Role == ROLE_Authority && CurrentFireMode == 1 && CurrentState != ActiveState)
	{
		// Only check if we're handling a remote client (not listen server host)
		if (UTOwner && !UTOwner->IsLocallyControlled())
		{
			float TimeSinceActivity = GetWorld()->GetTimeSeconds() - LastBeamActivityTime;

			if (LastBeamActivityTime > 0.f && TimeSinceActivity > BeamTimeoutDuration)
			{
				// Timeout - client probably stopped firing but RPC was lost
				//UE_LOG(LogTemp, Warning, TEXT("LinkGun: Beam timeout - no activity for %.2fs, forcing exit"), TimeSinceActivity);

				if (UTOwner)
				{
					UTOwner->ClearFiringInfo();
				}
				GotoActiveState();
				LastBeamActivityTime = 0.f;
			}
		}
	}
}


void AUTWeap_LinkGun_Plus::StopFire(uint8 FireModeNum)
{
	if (FireModeNum == 1)
	{
		// Beam mode - use standard UT logic, skip transactional stuff
		AUTWeapon::StopFire(FireModeNum);
	}
	else
	{
		// Plasma - use Fix logic
		AUTWeapon::StopFire(FireModeNum);
	}
}


void AUTWeap_LinkGun_Plus::StartLinkPull()
{
	bReadyToPull = false;
	PulseTarget = nullptr;
	if (UTOwner && CurrentLinkedTarget && UTOwner->IsLocallyControlled())
	{
		LastBeamPulseTime = GetWorld()->TimeSeconds;
		PulseTarget = CurrentLinkedTarget;
		UTOwner->PulseTarget = PulseTarget;
		PulseLoc = PulseTarget->GetActorLocation();
		UTOwner->TargetEyeOffset.Y = LinkPullKickbackY;
		ServerSetPulseTarget(CurrentLinkedTarget);

		MuzzleFlash[FiringState.Num()]->SetTemplate(PulseSuccessEffect);
		MuzzleFlash[FiringState.Num()]->SetActorParameter(FName(TEXT("Player")), CurrentLinkedTarget);
		PlayWeaponAnim(PulseAnim, PulseAnimHands);
		AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
		//if (PC != NULL)
		//{
		//	PC->AddHUDImpulse(FVector2D(0.f, 0.3f));
		//}
	}
	CurrentLinkedTarget = nullptr;
	LinkStartTime = -100.f;
}

bool AUTWeap_LinkGun_Plus::IsValidLinkTarget(AActor* InTarget)
{
	return (InTarget && Cast<AUTCharacter>(InTarget) && !InTarget->bTearOff && InTarget != GetUTOwner());
}

bool AUTWeap_LinkGun_Plus::ServerSetPulseTarget_Validate(AActor* InTarget)
{
	return true;
}

void AUTWeap_LinkGun_Plus::ServerSetPulseTarget_Implementation(AActor* InTarget)
{
	if (!UTOwner || !UTOwner->Controller || !InTarget)
	{
		return;
	}
	AActor* ClientPulseTarget = InTarget;
	FHitResult Hit;
	Super::FireInstantHit(false, &Hit);
	FVector PulseStart = UTOwner->GetActorLocation();

	PulseTarget = IsValidLinkTarget(Hit.Actor.Get()) ? Hit.Actor.Get() : nullptr;
	// try CSHD result if it's reasonable
	if (PulseTarget == NULL && ClientPulseTarget != NULL)
	{
		const FVector SpawnLocation = GetFireStartLoc();
		const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
		const FVector FireDir = SpawnRotation.Vector();
		const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;
		if (FMath::PointDistToSegment(ClientPulseTarget->GetActorLocation(), SpawnLocation, EndTrace) < 100.0f + ClientPulseTarget->GetSimpleCollisionRadius() && !GetWorld()->LineTraceTestByChannel(SpawnLocation, EndTrace, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionQueryParams(NAME_None, true)))
		{
			PulseTarget = ClientPulseTarget;
		}
	}
	if (PulseTarget != NULL)
	{
		// use owner to target direction instead of exactly the weapon orientation so that shooting below center doesn't cause the pull to send them over the shooter's head
		const FVector Dir = (PulseTarget->GetActorLocation() - PulseStart).GetSafeNormal();
		UTOwner->PulseTarget = PulseTarget;
		PulseLoc = PulseTarget->GetActorLocation();
		PulseTarget->TakeDamage(LinkPullDamage, FUTPointDamageEvent(0.0f, Hit, Dir, BeamPulseDamageType, BeamPulseMomentum * Dir), UTOwner->Controller, this);
		UUTGameplayStatics::UTPlaySound(GetWorld(), PullSucceeded, UTOwner, SRT_All, false, FVector::ZeroVector, Cast<AUTPlayerController>(PulseTarget->GetInstigatorController()), UTOwner, true, SAT_WeaponFire);

		UTOwner->SetFlashExtra(UTOwner->FlashExtra + 1, CurrentFireMode);
		if (Role == ROLE_Authority)
		{
			AUTCharacter* PulledChar = Cast<AUTCharacter>(PulseTarget);
			if (PulledChar && !PulledChar->IsDead() && BeamPulseDamageType && Cast<AUTPlayerController>(UTOwner->GetController()))
			{
				TSubclassOf<UUTDamageType> UTDamage(*BeamPulseDamageType);
				if (UTDamage && UTDamage.GetDefaultObject()->RewardAnnouncementClass)
				{
					AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->GetController());
					if (PC)
					{
						PC->SendPersonalMessage(UTDamage.GetDefaultObject()->RewardAnnouncementClass, 0, PC->UTPlayerState, nullptr);
					}
				}
			}
			AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
			if (!GameMode || GameMode->bAmmoIsLimited || (Ammo > 11))
			{
				AddAmmo(-BeamPulseAmmoCost);
			}
		}
		PlayWeaponAnim(PulseAnim, PulseAnimHands);
		// use an extra muzzle flash slot at the end for the pulse effect
		if (MuzzleFlash.IsValidIndex(FiringState.Num()) && MuzzleFlash[FiringState.Num()] != NULL)
		{
			if (PulseTarget != NULL)
			{
				MuzzleFlash[FiringState.Num()]->SetTemplate(PulseSuccessEffect);
				MuzzleFlash[FiringState.Num()]->SetActorParameter(FName(TEXT("Player")), PulseTarget);
			}
			else
			{
				MuzzleFlash[FiringState.Num()]->SetTemplate(PulseFailEffect);
			}
			MuzzleFlash[FiringState.Num()]->ActivateSystem();
		}
		LastBeamPulseTime = GetWorld()->TimeSeconds;
	}
	else
	{
		const FVector SpawnLocation = GetFireStartLoc();
		const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
		const FVector FireDir = SpawnRotation.Vector();
		PulseLoc = SpawnLocation + GetBaseFireRotation().RotateVector(MissedPulseOffset) + 100.f * FireDir;
		UUTGameplayStatics::UTPlaySound(GetWorld(), PullFailed, UTOwner, SRT_All, false, FVector::ZeroVector, nullptr, UTOwner, true, SAT_WeaponFoley);
	}
	CurrentLinkedTarget = nullptr;
	LinkStartTime = -100.f;
}


void AUTWeap_LinkGun_Plus::StateChanged()
{
	Super::StateChanged();

	// set AI timer for beam pulse
	static FName NAME_CheckBotPulseFire(TEXT("CheckBotPulseFire"));
	if (CurrentFireMode == 1 && Cast<UUTWeaponStateFiring>(CurrentState) != NULL && Cast<AUTBot>(UTOwner->Controller) != NULL)
	{
		SetTimerUFunc(this, NAME_CheckBotPulseFire, 0.2f, true);
	}
	else
	{
		ClearTimerUFunc(this, NAME_CheckBotPulseFire);
	}
}

void AUTWeap_LinkGun_Plus::CheckBotPulseFire()
{
	if (UTOwner != NULL && CurrentFireMode == 1 && InstantHitInfo.IsValidIndex(1))
	{
		AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
		if (B != NULL && B->WeaponProficiencyCheck() && B->GetEnemy() != NULL && B->GetTarget() == B->GetEnemy() &&
			(B->IsCharging() || B->GetSquad()->MustKeepEnemy(B, B->GetEnemy()) || B->RelativeStrength(B->GetEnemy()) < 0.0f))
		{
			bool bTryPulse = FMath::FRand() < (B->IsFavoriteWeapon(GetClass()) ? 0.1f : 0.05f);
			if (bTryPulse)
			{
				// if bot has good reflexes only pulse if enemy is being hit
				if (FMath::FRand() < 0.07f * B->Skill + B->Personality.ReactionTime)
				{
					const FVector SpawnLocation = GetFireStartLoc();
					const FVector EndTrace = SpawnLocation + GetAdjustedAim(SpawnLocation).Vector() * InstantHitInfo[1].TraceRange;
					FHitResult Hit;
					HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[1].TraceHalfSize, Hit, 0.0f);
					bTryPulse = Hit.Actor.Get() == B->GetEnemy();
				}
				if (bTryPulse)
				{
					StartFire(0);
					StopFire(0);
				}
			}
		}
	}
}

void AUTWeap_LinkGun_Plus::FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode)
{

	if (GetUTOwner() && GetUTOwner()->IsLocallyControlled())
	{
		// If we are not in a firing state, we are done. Do not let the server drag us back in.
		if (!IsFiring() || GetCurrentState() == ActiveState)
		{
			return;
		}
	}
	if (NewFlashExtra > 0 && InFireMode == 1)
	{
		LastBeamPulseTime = GetWorld()->TimeSeconds;
		// use an extra muzzle flash slot at the end for the pulse effect
		if (MuzzleFlash.IsValidIndex(FiringState.Num()) && MuzzleFlash[FiringState.Num()] != NULL)
		{
			AActor* GuessTarget = PulseTarget;
			if (GuessTarget == NULL && UTOwner != NULL && Role < ROLE_Authority)
			{
				TArray<FOverlapResult> Hits;
				GetWorld()->OverlapMultiByChannel(Hits, UTOwner->FlashLocation.Position, FQuat::Identity, COLLISION_TRACE_WEAPON, FCollisionShape::MakeSphere(10.0f), FCollisionQueryParams(NAME_None, true, UTOwner));
				for (const FOverlapResult& Hit : Hits)
				{
					if (Cast<APawn>(Hit.Actor.Get()) != NULL)
					{
						GuessTarget = Hit.Actor.Get();
					}
				}
			}
			if (GuessTarget != NULL)
			{
				MuzzleFlash[FiringState.Num()]->SetTemplate(PulseSuccessEffect);
				MuzzleFlash[FiringState.Num()]->SetActorParameter(FName(TEXT("Player")), GuessTarget);
			}
			else
			{
				MuzzleFlash[FiringState.Num()]->SetTemplate(PulseFailEffect);
			}
		}
		PlayWeaponAnim(PulseAnim, PulseAnimHands);
	}
}

void AUTWeap_LinkGun_Plus::DrawWeaponCrosshair_Implementation(UUTHUDWidget* WeaponHudWidget, float RenderDelta)
{
	Super::DrawWeaponCrosshair_Implementation(WeaponHudWidget, RenderDelta);

	if ((OverheatFactor > 0.f) && WeaponHudWidget && WeaponHudWidget->UTHUDOwner)
	{
		float Width = 150.f;
		float Height = 21.f;
		float WidthScale = 0.625f;
		float HeightScale = bIsInCoolDown ? 1.f : 0.5f;
		//	WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 96.f, Scale*Width, Scale*Height, 127, 671, Width, Height, 0.7f, FLinearColor::White, FVector2D(0.5f, 0.5f));
		float ChargePct = FMath::Clamp(OverheatFactor, 0.f, 1.f);
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 40.f, WidthScale * Width * ChargePct, HeightScale * Height, 127, 641, Width, Height, bIsInCoolDown ? OverheatFactor : 0.7f, REDHUDCOLOR, FVector2D(0.5f, 0.5f));
		if (bIsInCoolDown)
		{
			WeaponHudWidget->DrawText(NSLOCTEXT("LinkGun", "Overheat", "OVERHEAT"), 0.f, 37.f, WeaponHudWidget->UTHUDOwner->TinyFont, 0.75f, FMath::Min(3.f * OverheatFactor, 1.f), FLinearColor::Yellow, ETextHorzPos::Center, ETextVertPos::Center);
		}
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 40.f, WidthScale * Width, HeightScale * Height, 127, 612, Width, Height, 1.f, FLinearColor::White, FVector2D(0.5f, 0.5f));
	}
	if (bReadyToPull && (Ammo > 0) && WeaponHudWidget && WeaponHudWidget->UTHUDOwner)
	{
		float CircleSize = 76.f;
		float CrosshairScale = GetCrosshairScale(WeaponHudWidget->UTHUDOwner);
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0, 0, 0.75f * CircleSize * CrosshairScale, 0.75f * CircleSize * CrosshairScale, 98, 936, CircleSize, CircleSize, 1.f, FLinearColor::Red, FVector2D(0.5f, 0.5f));
	}
}

