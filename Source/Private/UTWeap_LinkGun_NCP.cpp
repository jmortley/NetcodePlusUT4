#include "UTWeap_LinkGun_NCP.h"
#include "NCWeaponColorSettings.h"
#include "NetcodePlus.h"
#include "UTWeaponStateFiringLinkBeam_NCP.h"

#include "Animation/AnimInstance.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/DemoNetDriver.h"
#include "Particles/ParticleSystemComponent.h"
#include "StatNames.h"
#include "UTProj_LinkPlasma.h"
#include "UTRewardMessage.h"
#include "UTSquadAI.h"
#include "UTTeamGameMode.h"
#include "UTWeaponStateFiring.h"
#include "UTWeaponStateFiring_LoopingFire.h"

AUTWeap_LinkGun_NCP::AUTWeap_LinkGun_NCP(const FObjectInitializer& OI)
	: Super(OI.SetDefaultSubobjectClass<UUTWeaponStateFiringLinkBeam_NCP>(TEXT("FiringState1")))
{
	// Continuous beam compensation stays deliberately conservative. This is
	// server-side rewind only: Link sends no per-tick client hit claims.
	LinkBeamMaxRewindMs = 40.0f;
	bTrackHitScanReplication = false;

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

	HUDIcon = MakeCanvasIcon(HUDIcon.Texture, 453.0f, 467.0f, 147.0f, 41.0f);

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
	LastLinkEffectColorGeneration = 0;
	LastLinkMuzzleColorGeneration = 0;
	LastScreenUpdateTime = 0.f;
	LastAppliedPulseScale = -1.f;

	WeaponCustomizationTag = EpicWeaponCustomizationTags::LinkGun;
	WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::LinkGun;

	TutorialAnnouncements.Add(TEXT("PriLinkGun"));
	TutorialAnnouncements.Add(TEXT("SecLinkGun"));
	HighlightText = NSLOCTEXT("Weapon", "LinkHighlightText", "Plasma Boy");
	LowMeshOffset = FVector(0.f, 0.f, -3.f);
	VeryLowMeshOffset = FVector(0.f, 0.f, -10.f);
}

bool AUTWeap_LinkGun_NCP::RefreshConfiguredLinkColor(UObject* BeamEffect,
	UParticleSystemComponent* MuzzleEffect, FLinearColor& OutColor)
{
	OutColor = FLinearColor::White;
	UWorld* World = GetWorld();
	// Beam cleanup is not a color feature. Register any client-side first-person
	// beam before the owner-local color guards so remote first-person spectators
	// retain the old Blueprint watchdog's stuck-effect protection too.
	if (World != nullptr && GetNetMode() != NM_DedicatedServer)
	{
		ArmLinkBeamWatchdog(BeamEffect);
	}
	if (World == nullptr || GetNetMode() == NM_DedicatedServer || UTOwner == nullptr
		|| !UTOwner->IsLocallyControlled() || !UTOwner->IsPlayerControlled()
		|| !ShouldPlay1PVisuals()
		|| (World->DemoNetDriver != nullptr && World->DemoNetDriver->IsPlaying()))
	{
		return false;
	}

	const FNCWeaponColorSnapshot& Colors = NCWeaponColors::GetSnapshot();
	if (!Colors.bHasLinkColor)
	{
		return false;
	}
	OutColor = Colors.LinkColor;

	const bool bValidBeamEffect = BeamEffect != nullptr && !BeamEffect->IsPendingKill();
	const bool bRefreshBeam = bValidBeamEffect
		&& (LastColoredLinkEffect.Get() != BeamEffect
			|| LastLinkEffectColorGeneration != Colors.Generation);

	if (MuzzleEffect != nullptr && !MuzzleEffect->IsPendingKill()
		&& (LastColoredLinkMuzzle.Get() != MuzzleEffect
			|| LastLinkMuzzleColorGeneration != Colors.Generation))
	{
		static const FName NAME_ShockMuzzleColorParam(TEXT("ShockMuzzleColorParam"));
		MuzzleEffect->SetColorParameter(NAME_ShockMuzzleColorParam, Colors.LinkColor);
		LastColoredLinkMuzzle = MuzzleEffect;
		LastLinkMuzzleColorGeneration = Colors.Generation;
	}

	if (bRefreshBeam)
	{
		LastColoredLinkEffect = BeamEffect;
		LastLinkEffectColorGeneration = Colors.Generation;
	}

	return bRefreshBeam;
}

void AUTWeap_LinkGun_NCP::ArmLinkBeamWatchdog(UObject* BeamEffect)
{
	if (BeamEffect == nullptr || BeamEffect->IsPendingKill() || GetWorld() == nullptr)
	{
		return;
	}

	TrackedLinkBeamEffect = BeamEffect;
	if (!GetWorldTimerManager().IsTimerActive(LinkBeamWatchdogHandle))
	{
		// Cosmetic cleanup does not need the old render-frame cadence. This timer
		// exists only for the lifetime of an actual beam effect.
		GetWorldTimerManager().SetTimer(LinkBeamWatchdogHandle, this,
			&AUTWeap_LinkGun_NCP::CheckLinkBeamWatchdog, 1.f / 30.f, true);
	}
}

void AUTWeap_LinkGun_NCP::StopLinkBeamWatchdog()
{
	if (GetWorld() != nullptr)
	{
		GetWorldTimerManager().ClearTimer(LinkBeamWatchdogHandle);
	}
	TrackedLinkBeamEffect.Reset();
}

void AUTWeap_LinkGun_NCP::CheckLinkBeamWatchdog()
{
	if (!TrackedLinkBeamEffect.IsValid())
	{
		StopLinkBeamWatchdog();
		return;
	}

	if (!IsFiring())
	{
		// Dispatch through NCPLinkGun's existing Blueprint override: it calls
		// Parent, deactivates BP_LinkAlt_1P, and clears its BeamEffect variable.
		StopFiringEffects();
		StopLinkBeamWatchdog();
	}
}

void AUTWeap_LinkGun_NCP::StopFiringEffects_Implementation()
{
	StopLinkBeamWatchdog();
	Super::StopFiringEffects_Implementation();
}

void AUTWeap_LinkGun_NCP::StartFire(uint8 FireModeNum)
{
	// Link primary's 7+ shots/sec cadence is intentionally not transactional.
	// The beam likewise keeps its stock continuous-state lifecycle.
	AUTWeapon::StartFire(FireModeNum);
}

void AUTWeap_LinkGun_NCP::StopFire(uint8 FireModeNum)
{
	AUTWeapon::StopFire(FireModeNum);
}

bool AUTWeap_LinkGun_NCP::PutDown()
{
	// StartFire bypasses AUTWeaponFix, so its private held-input bookkeeping is
	// intentionally unset. Use stock PutDown to preserve stock hold-through-swap.
	return AUTWeapon::PutDown();
}

void AUTWeap_LinkGun_NCP::FireShot()
{
	if (!bIsInCoolDown)
	{
		AUTWeapon::FireShot();
	}
}

void AUTWeap_LinkGun_NCP::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
	// Link beam damage never consumes a client-named target. Clear the inherited
	// claim cache immediately before every trace so even a stale or unsolicited
	// stock ServerHitScanHit RPC cannot enable claim padding or time search.
	bTrackHitScanReplication = false;
	ReceivedHitScanHitChar = nullptr;
	ReceivedHitScanIndex = 0;
	AUTWeaponFix::FireInstantHit(bDealDamage, OutHit);
}

AUTProjectile* AUTWeap_LinkGun_NCP::FireProjectile()
{
	// Bypass AUTWeaponFix::SpawnNetPredictedProjectile. Rapid plasma uses the
	// stock fake/real lifecycle and has no NetcodePlus fire-event reservation.
	AUTProj_LinkPlasma* LinkProj = Cast<AUTProj_LinkPlasma>(AUTWeapon::FireProjectile());
	if (LinkProj != nullptr)
	{
		LastFiredPlasmaTime = int32(GetWorld()->GetTimeSeconds());
	}
	return LinkProj;
}

AUTProjectile* AUTWeap_LinkGun_NCP::SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile> ProjectileClass, FVector SpawnLocation, FRotator SpawnRotation)
{
	return AUTWeapon::SpawnNetPredictedProjectile(ProjectileClass, SpawnLocation, SpawnRotation);
}

float AUTWeap_LinkGun_NCP::GetHitValidationPredictionTime() const
{
	// AUTWeaponFix derives a one-way server rewind from ExactPing. Clamp that
	// result again at the weapon boundary so a high-ping continuous beam cannot
	// reach arbitrarily far into target history. The hard ceiling also protects
	// against an old or accidentally mistuned cooked Blueprint default.
	const float RequestedRewind = Super::GetHitValidationPredictionTime();
	const float SafeCapMs = FMath::Clamp(LinkBeamMaxRewindMs, 0.0f, 50.0f);
	return FMath::Min(RequestedRewind, SafeCapMs * 0.001f);
}

void AUTWeap_LinkGun_NCP::SpawnDelayedFakeProjectile()
{
	AUTWeapon::SpawnDelayedFakeProjectile();
}

void AUTWeap_LinkGun_NCP::AttachToOwner_Implementation()
{
	Super::AttachToOwner_Implementation();

	if (!IsRunningDedicatedServer() && Mesh != nullptr && ScreenMaterialID < Mesh->GetNumMaterials())
	{
		ScreenMI = Mesh->CreateAndSetMaterialInstanceDynamic(ScreenMaterialID);
		ScreenTexture = UCanvasRenderTarget2D::CreateCanvasRenderTarget2D(this, UCanvasRenderTarget2D::StaticClass(), 64, 64);
		ScreenTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
		ScreenTexture->OnCanvasRenderTargetUpdate.AddDynamic(this, &AUTWeap_LinkGun_NCP::UpdateScreenTexture);
		ScreenMI->SetTextureParameterValue(FName(TEXT("ScreenTexture")), ScreenTexture);
		LastScreenUpdateTime = 0.f;

		if (SideScreenMaterialID < Mesh->GetNumMaterials())
		{
			SideScreenMI = Mesh->CreateAndSetMaterialInstanceDynamic(SideScreenMaterialID);
		}
	}
}

void AUTWeap_LinkGun_NCP::UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height)
{
	if (GetWorld()->TimeSeconds - LastClientKillTime < 2.5f && ScreenKillNotifyTexture != nullptr)
	{
		C->SetDrawColor(FColor::White);
		C->DrawTile(ScreenKillNotifyTexture, 0.0f, 0.0f, float(Width), float(Height), 0.0f, 0.0f,
			ScreenKillNotifyTexture->GetSizeX(), ScreenKillNotifyTexture->GetSizeY());
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

		const FString OverheatText = bIsInCoolDown
			? TEXT("***")
			: FString::FromInt(int32(100.f * FMath::Clamp(OverheatFactor, 0.1f, 1.f)));
		float XL = 0.f;
		float YL = 0.f;
		C->TextSize(ScreenFont, OverheatText, XL, YL);
		if (!WordWrapper.IsValid())
		{
			WordWrapper = MakeShareable(new FCanvasWordWrapper());
		}
		FLinearColor ScreenColor = OverheatFactor <= (IsFiring() ? 0.5f : 0.f) ? FLinearColor::Green : FLinearColor::Yellow;
		if (bIsInCoolDown)
		{
			ScreenColor = FLinearColor::Red;
		}
		if (ScreenMI != nullptr)
		{
			ScreenMI->SetVectorParameterValue(FName(TEXT("Screen Color")), ScreenColor);
			ScreenMI->SetVectorParameterValue(FName(TEXT("Screen Low Color")), ScreenColor);
		}
		if (SideScreenMI != nullptr)
		{
			SideScreenMI->SetVectorParameterValue(FName(TEXT("Screen Color")), ScreenColor);
			SideScreenMI->SetVectorParameterValue(FName(TEXT("Screen Low Color")), ScreenColor);
		}
		C->SetDrawColor(ScreenColor.ToFColor(true));
		// FUTCanvasTextItem's implementation is not exported from UnrealTournament,
		// so plugin DLLs must use the engine canvas item just like LinkGun_Plus.
		FCanvasTextItem Item(FVector2D(Width / 2 - XL * 0.5f, Height / 2 - YL * 0.5f),
			FText::FromString(OverheatText), ScreenFont, ScreenColor);
		Item.FontRenderInfo = RenderInfo;
		C->DrawItem(Item);
	}
}

void AUTWeap_LinkGun_NCP::Removed()
{
	StopLinkBeamWatchdog();
	if (UTOwner != nullptr)
	{
		UTOwner->SetAmbientSound(OverheatSound, true);
	}
	AUTWeapon::Removed();
}

void AUTWeap_LinkGun_NCP::ClientRemoved()
{
	StopLinkBeamWatchdog();
	if (UTOwner != nullptr)
	{
		UTOwner->SetAmbientSound(OverheatSound, true);
	}
	Super::ClientRemoved();
}

void AUTWeap_LinkGun_NCP::PlayWeaponAnim(UAnimMontage* WeaponAnim, UAnimMontage* HandsAnim, float RateOverride)
{
	if (WeaponAnim == PulseAnim || PulseAnim == nullptr || Mesh->GetAnimInstance() == nullptr ||
		!Mesh->GetAnimInstance()->Montage_IsPlaying(PulseAnim))
	{
		Super::PlayWeaponAnim(WeaponAnim, HandsAnim, RateOverride);
	}
}

bool AUTWeap_LinkGun_NCP::IsLinkPulsing()
{
	return GetWorld()->GetTimeSeconds() - LastBeamPulseTime < BeamPulseInterval;
}

void AUTWeap_LinkGun_NCP::Tick(float DeltaTime)
{
	// The AUTWeaponFix tick watchdog is driven by transactional LastFireTime.
	// This class deliberately has no transactional shots, so that clock would
	// remain unset and falsely stop a healthy continuous beam after 0.25s.
	AUTWeapon::Tick(DeltaTime);

	if (bIsInCoolDown)
	{
		OverheatFactor -= 2.f * DeltaTime;
		if (OverheatFactor <= 0.f)
		{
			OverheatFactor = 0.f;
			bIsInCoolDown = false;
			if (UTOwner != nullptr)
			{
				UTOwner->SetAmbientSound(OverheatSound, true);
			}
		}
		else if (UTOwner != nullptr && OverheatSound != nullptr &&
			(!IsFiring() || !FireLoopingSound.IsValidIndex(CurrentFireMode) || FireLoopingSound[CurrentFireMode] == nullptr))
		{
			UTOwner->SetAmbientSound(OverheatSound, false);
			UTOwner->ChangeAmbientSoundPitch(OverheatSound, 0.5f + OverheatFactor);
		}
	}
	else
	{
		OverheatFactor = UTOwner != nullptr && IsFiring() && CurrentFireMode == 0 && UTOwner->GetFireRateMultiplier() <= 1.f
			? OverheatFactor + 0.42f * DeltaTime
			: FMath::Max(0.f, OverheatFactor - 2.2f * FMath::Max(0.f,
				DeltaTime - FMath::Max(0.f, 0.3f + LastFiredPlasmaTime - GetWorld()->GetTimeSeconds())));
		bIsInCoolDown = OverheatFactor > 1.f;
	}

	const float Now = GetWorld()->TimeSeconds;
	if (ScreenTexture != nullptr && Mesh->IsRegistered() && Now - Mesh->LastRenderTime < 0.1f
		&& Now - LastScreenUpdateTime >= 1.f / 30.f)
	{
		LastScreenUpdateTime = Now;
		ScreenTexture->FastUpdateResource();
	}

	if (UTOwner != nullptr && UTOwner->GetWeapon() == this && MuzzleFlash.IsValidIndex(1) && MuzzleFlash[1] != nullptr)
	{
		static FName NAME_PulseScale(TEXT("PulseScale"));
		UParticleSystemComponent* const PulseEffect = MuzzleFlash[1];
		const float PulseAge = Now - LastBeamPulseTime;
		const float NewScale = 1.0f + FMath::Max<float>(0.0f, 1.0f - PulseAge / 0.35f);
		const bool bEffectChanged = LastPulseScaleEffect.Get() != PulseEffect;
		// Animate while the pulse is live, then write the settled value once.
		if (bEffectChanged || PulseAge < 0.35f || LastAppliedPulseScale != NewScale)
		{
			PulseEffect->SetVectorParameter(NAME_PulseScale, FVector(NewScale, NewScale, NewScale));
			LastPulseScaleEffect = PulseEffect;
			LastAppliedPulseScale = NewScale;
		}
	}

	if (UTOwner != nullptr && IsFiring() && Role == ROLE_Authority &&
		FireLoopingSound.IsValidIndex(CurrentFireMode) && FireLoopingSound[CurrentFireMode] != nullptr && !IsLinkPulsing())
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

	if (IsLinkPulsing())
	{
		const FVector SpawnLocation = GetFireStartLoc();
		const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
		const FVector FireDir = SpawnRotation.Vector();
		PulseLoc = PulseTarget != nullptr && !PulseTarget->IsPendingKillPending()
			? PulseTarget->GetActorLocation()
			: SpawnLocation + GetBaseFireRotation().RotateVector(MissedPulseOffset) + 100.f * FireDir;
	}
	else if (UTOwner != nullptr)
	{
		UTOwner->PulseTarget = nullptr;
	}

	UUTWeaponStateFiring_LoopingFire* LoopingState = Cast<UUTWeaponStateFiring_LoopingFire>(GetCurrentState());
	if (LoopingState != nullptr)
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
}

void AUTWeap_LinkGun_NCP::StartLinkPull()
{
	bReadyToPull = false;
	PulseTarget = nullptr;
	if (UTOwner != nullptr && IsValidLinkTarget(CurrentLinkedTarget) && UTOwner->IsLocallyControlled())
	{
		LastBeamPulseTime = GetWorld()->TimeSeconds;
		PulseTarget = CurrentLinkedTarget;
		UTOwner->PulseTarget = PulseTarget;
		PulseLoc = PulseTarget->GetActorLocation();
		UTOwner->TargetEyeOffset.Y = LinkPullKickbackY;
		ServerSetPulseTarget(CurrentLinkedTarget);

		if (MuzzleFlash.IsValidIndex(FiringState.Num()) && MuzzleFlash[FiringState.Num()] != nullptr)
		{
			MuzzleFlash[FiringState.Num()]->SetTemplate(PulseSuccessEffect);
			MuzzleFlash[FiringState.Num()]->SetActorParameter(FName(TEXT("Player")), CurrentLinkedTarget);
		}
		PlayWeaponAnim(PulseAnim, PulseAnimHands);
		AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
		if (PC != nullptr)
		{
			PC->AddHUDImpulse(FVector2D(0.f, 0.3f));
		}
	}
	CurrentLinkedTarget = nullptr;
	LinkStartTime = -100.f;
}

bool AUTWeap_LinkGun_NCP::IsValidLinkTarget(AActor* InTarget)
{
	AUTCharacter* TargetCharacter = Cast<AUTCharacter>(InTarget);
	AUTCharacter* OwnerCharacter = GetUTOwner();
	if (TargetCharacter == nullptr || OwnerCharacter == nullptr || TargetCharacter == OwnerCharacter ||
		TargetCharacter->bTearOff || TargetCharacter->IsDead())
	{
		return false;
	}

	// Use the canonical game-state test when available, then retain a direct
	// team-number guard so a transient/missing GameState cannot make teammates
	// pullable. Team 255 is the normal no-team/FFA value.
	AUTGameState* GameState = GetWorld() != nullptr ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (GameState != nullptr && GameState->OnSameTeam(OwnerCharacter, TargetCharacter))
	{
		return false;
	}

	const uint8 OwnerTeam = OwnerCharacter->GetTeamNum();
	const uint8 TargetTeam = TargetCharacter->GetTeamNum();
	return OwnerTeam == 255 || TargetTeam == 255 || OwnerTeam != TargetTeam;
}

bool AUTWeap_LinkGun_NCP::ServerSetPulseTarget_Validate(AActor* InTarget)
{
	return true;
}

void AUTWeap_LinkGun_NCP::ServerSetPulseTarget_Implementation(AActor* InTarget)
{
	if (UTOwner == nullptr || UTOwner->Controller == nullptr || InTarget == nullptr)
	{
		return;
	}

	AActor* ClientPulseTarget = InTarget;
	FHitResult Hit;
	FireInstantHit(false, &Hit);
	const FVector PulseStart = UTOwner->GetActorLocation();

	PulseTarget = IsValidLinkTarget(Hit.Actor.Get()) ? Hit.Actor.Get() : nullptr;
	if (PulseTarget == nullptr && IsValidLinkTarget(ClientPulseTarget))
	{
		const FVector SpawnLocation = GetFireStartLoc();
		const FVector FireDir = GetAdjustedAim(SpawnLocation).Vector();
		const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;
		if (FMath::PointDistToSegment(ClientPulseTarget->GetActorLocation(), SpawnLocation, EndTrace) <
			100.0f + ClientPulseTarget->GetSimpleCollisionRadius() &&
			!GetWorld()->LineTraceTestByChannel(SpawnLocation, EndTrace, COLLISION_TRACE_WEAPONNOCHARACTER,
				FCollisionQueryParams(NAME_None, true)))
		{
			PulseTarget = ClientPulseTarget;
		}
	}

	// Revalidate immediately before the authoritative damage/momentum path.
	// This closes both the client-hint fallback and any mid-RPC team-state edge.
	if (!IsValidLinkTarget(PulseTarget))
	{
		PulseTarget = nullptr;
	}

	if (PulseTarget != nullptr)
	{
		const FVector Dir = (PulseTarget->GetActorLocation() - PulseStart).GetSafeNormal();
		UTOwner->PulseTarget = PulseTarget;
		PulseLoc = PulseTarget->GetActorLocation();
		PulseTarget->TakeDamage(LinkPullDamage,
			FUTPointDamageEvent(0.0f, Hit, Dir, BeamPulseDamageType, BeamPulseMomentum * Dir),
			UTOwner->Controller, this);
		UUTGameplayStatics::UTPlaySound(GetWorld(), PullSucceeded, UTOwner, SRT_All, false, FVector::ZeroVector,
			Cast<AUTPlayerController>(PulseTarget->GetInstigatorController()), UTOwner, true, SAT_WeaponFire);

		UTOwner->SetFlashExtra(UTOwner->FlashExtra + 1, CurrentFireMode);
		if (Role == ROLE_Authority)
		{
			AUTCharacter* PulledChar = Cast<AUTCharacter>(PulseTarget);
			AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->GetController());
			if (PulledChar != nullptr && !PulledChar->IsDead() && BeamPulseDamageType && PC != nullptr)
			{
				TSubclassOf<UUTDamageType> UTDamage(*BeamPulseDamageType);
				if (UTDamage && UTDamage.GetDefaultObject()->RewardAnnouncementClass)
				{
					PC->SendPersonalMessage(UTDamage.GetDefaultObject()->RewardAnnouncementClass, 0, PC->UTPlayerState, nullptr);
				}
			}
			AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
			if (GameMode == nullptr || GameMode->bAmmoIsLimited || Ammo > 11)
			{
				AddAmmo(-BeamPulseAmmoCost);
			}
		}
		PlayWeaponAnim(PulseAnim, PulseAnimHands);
		if (MuzzleFlash.IsValidIndex(FiringState.Num()) && MuzzleFlash[FiringState.Num()] != nullptr)
		{
			MuzzleFlash[FiringState.Num()]->SetTemplate(PulseSuccessEffect);
			MuzzleFlash[FiringState.Num()]->SetActorParameter(FName(TEXT("Player")), PulseTarget);
			MuzzleFlash[FiringState.Num()]->ActivateSystem();
		}
		LastBeamPulseTime = GetWorld()->TimeSeconds;
	}
	else
	{
		const FVector SpawnLocation = GetFireStartLoc();
		const FVector FireDir = GetAdjustedAim(SpawnLocation).Vector();
		PulseLoc = SpawnLocation + GetBaseFireRotation().RotateVector(MissedPulseOffset) + 100.f * FireDir;
		UUTGameplayStatics::UTPlaySound(GetWorld(), PullFailed, UTOwner, SRT_All, false, FVector::ZeroVector,
			nullptr, UTOwner, true, SAT_WeaponFoley);
	}
	CurrentLinkedTarget = nullptr;
	LinkStartTime = -100.f;
}

void AUTWeap_LinkGun_NCP::StateChanged()
{
	Super::StateChanged();

	static FName NAME_CheckBotPulseFire(TEXT("CheckBotPulseFire"));
	if (CurrentFireMode == 1 && Cast<UUTWeaponStateFiring>(CurrentState) != nullptr &&
		UTOwner != nullptr && Cast<AUTBot>(UTOwner->Controller) != nullptr)
	{
		SetTimerUFunc(this, NAME_CheckBotPulseFire, 0.2f, true);
	}
	else
	{
		ClearTimerUFunc(this, NAME_CheckBotPulseFire);
	}
}

void AUTWeap_LinkGun_NCP::CheckBotPulseFire()
{
	if (UTOwner == nullptr || CurrentFireMode != 1 || !InstantHitInfo.IsValidIndex(1))
	{
		return;
	}

	AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
	if (B != nullptr && B->WeaponProficiencyCheck() && B->GetEnemy() != nullptr && B->GetTarget() == B->GetEnemy() &&
		(B->IsCharging() || B->GetSquad()->MustKeepEnemy(B, B->GetEnemy()) || B->RelativeStrength(B->GetEnemy()) < 0.0f))
	{
		bool bTryPulse = FMath::FRand() < (B->IsFavoriteWeapon(GetClass()) ? 0.1f : 0.05f);
		if (bTryPulse)
		{
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

void AUTWeap_LinkGun_NCP::FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode)
{
	if (NewFlashExtra > 0 && InFireMode == 1)
	{
		LastBeamPulseTime = GetWorld()->TimeSeconds;
		if (MuzzleFlash.IsValidIndex(FiringState.Num()) && MuzzleFlash[FiringState.Num()] != nullptr)
		{
			AActor* GuessTarget = PulseTarget;
			if (GuessTarget == nullptr && UTOwner != nullptr && Role < ROLE_Authority)
			{
				TArray<FOverlapResult> Hits;
				GetWorld()->OverlapMultiByChannel(Hits, UTOwner->FlashLocation.Position, FQuat::Identity,
					COLLISION_TRACE_WEAPON, FCollisionShape::MakeSphere(10.0f),
					FCollisionQueryParams(NAME_None, true, UTOwner));
				for (const FOverlapResult& Candidate : Hits)
				{
					if (Cast<APawn>(Candidate.Actor.Get()) != nullptr)
					{
						GuessTarget = Candidate.Actor.Get();
					}
				}
			}
			if (GuessTarget != nullptr)
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

void AUTWeap_LinkGun_NCP::DrawWeaponCrosshair_Implementation(UUTHUDWidget* WeaponHudWidget, float RenderDelta)
{
	Super::DrawWeaponCrosshair_Implementation(WeaponHudWidget, RenderDelta);

	if (OverheatFactor > 0.f && WeaponHudWidget != nullptr && WeaponHudWidget->UTHUDOwner != nullptr)
	{
		const float Width = 150.f;
		const float Height = 21.f;
		const float WidthScale = 0.625f;
		const float HeightScale = bIsInCoolDown ? 1.f : 0.5f;
		const float ChargePct = FMath::Clamp(OverheatFactor, 0.f, 1.f);
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 40.f,
			WidthScale * Width * ChargePct, HeightScale * Height, 127, 641, Width, Height,
			bIsInCoolDown ? OverheatFactor : 0.7f, REDHUDCOLOR, FVector2D(0.5f, 0.5f));
		if (bIsInCoolDown)
		{
			WeaponHudWidget->DrawText(NSLOCTEXT("LinkGun", "Overheat", "OVERHEAT"), 0.f, 37.f,
				WeaponHudWidget->UTHUDOwner->TinyFont, 0.75f, FMath::Min(3.f * OverheatFactor, 1.f),
				FLinearColor::Yellow, ETextHorzPos::Center, ETextVertPos::Center);
		}
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 40.f,
			WidthScale * Width, HeightScale * Height, 127, 612, Width, Height, 1.f,
			FLinearColor::White, FVector2D(0.5f, 0.5f));
	}

	if (bReadyToPull && Ammo > 0 && WeaponHudWidget != nullptr && WeaponHudWidget->UTHUDOwner != nullptr)
	{
		const float CircleSize = 76.f;
		const float CrosshairScale = GetCrosshairScale(WeaponHudWidget->UTHUDOwner);
		WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, 0.f,
			0.75f * CircleSize * CrosshairScale, 0.75f * CircleSize * CrosshairScale,
			98, 936, CircleSize, CircleSize, 1.f, FLinearColor::Red, FVector2D(0.5f, 0.5f));
	}
}
