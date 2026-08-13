#include "UTWeaponStateFiringLinkBeam_NCP.h"
#include "NetcodePlus.h"
#include "UTWeap_LinkGun_NCP.h"
#include "Animation/AnimInstance.h"

UUTWeaponStateFiringLinkBeam_NCP::UUTWeaponStateFiringLinkBeam_NCP(const FObjectInitializer& OI)
	: Super(OI)
{
	AccumulatedFiringTime = 0.f;
}

void UUTWeaponStateFiringLinkBeam_NCP::BeginState(const UUTWeaponState* PrevState)
{
	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun != nullptr)
	{
		LinkGun->CurrentLinkedTarget = nullptr;
		LinkGun->LinkStartTime = -100.f;
	}
	Super::BeginState(PrevState);
}

void UUTWeaponStateFiringLinkBeam_NCP::FireShot()
{
	// Beam damage is accumulated in Tick(); the refire pulse only drives stock
	// effects, ammo consumption, and inventory notification.
	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun != nullptr)
	{
		LinkGun->PlayFiringEffects();
		LinkGun->ConsumeAmmo(LinkGun->GetCurrentFireMode());
	}

	if (GetUTOwner() != nullptr && LinkGun != nullptr)
	{
		GetUTOwner()->TargetEyeOffset.Y = LinkGun->FiringBeamKickbackY;
		GetUTOwner()->InventoryEvent(InventoryEventName::FiredWeapon);
	}
}

void UUTWeaponStateFiringLinkBeam_NCP::EndFiringSequence(uint8 FireModeNum)
{
	if (FireModeNum == GetFireMode())
	{
		AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
		if (LinkGun == nullptr || (!LinkGun->bReadyToPull && !LinkGun->IsLinkPulsing()))
		{
			Super::EndFiringSequence(FireModeNum);
			if (FireModeNum == GetOuterAUTWeapon()->GetCurrentFireMode())
			{
				PlayEndFireAnims();
				GetOuterAUTWeapon()->GotoActiveState();
			}
		}
		else
		{
			bPendingEndFire = true;
			bPendingStartFire = false;
		}
	}
}

void UUTWeaponStateFiringLinkBeam_NCP::PendingFireStarted()
{
	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun != nullptr && LinkGun->IsLinkPulsing())
	{
		bPendingStartFire = true;
	}
	else
	{
		bPendingEndFire = false;
	}
}

void UUTWeaponStateFiringLinkBeam_NCP::RefireCheckTimer()
{
	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun == nullptr || !LinkGun->IsLinkPulsing())
	{
		Super::RefireCheckTimer();
	}
}

void UUTWeaponStateFiringLinkBeam_NCP::EndState()
{
	bPendingStartFire = false;
	bPendingEndFire = false;

	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun != nullptr)
	{
		LinkGun->bReadyToPull = false;
	}
	Super::EndState();
}

void UUTWeaponStateFiringLinkBeam_NCP::Tick(float DeltaTime)
{
	AUTWeap_LinkGun_NCP* LinkGun = Cast<AUTWeap_LinkGun_NCP>(GetOuterAUTWeapon());
	if (LinkGun != nullptr && LinkGun->Role == ROLE_Authority)
	{
		LinkGun->bLinkCausingDamage = false;
	}

	if (bPendingEndFire)
	{
		if (LinkGun != nullptr && LinkGun->IsLinkPulsing())
		{
			if (LinkGun->GetUTOwner() != nullptr)
			{
				LinkGun->GetUTOwner()->SetFlashLocation(LinkGun->PulseLoc, LinkGun->GetCurrentFireMode());
			}
			return;
		}
		else if (LinkGun != nullptr && LinkGun->bReadyToPull && LinkGun->CurrentLinkedTarget != nullptr)
		{
			LinkGun->StartLinkPull();
			return;
		}

		if (bPendingStartFire)
		{
			bPendingEndFire = false;
		}
		else
		{
			EndFiringSequence(GetFireMode());
			return;
		}
	}

	bPendingStartFire = false;
	HandleDelayedShot();

	if (LinkGun != nullptr && !LinkGun->FireShotOverride() && LinkGun->InstantHitInfo.IsValidIndex(LinkGun->GetCurrentFireMode()))
	{
		const FInstantHitDamageInfo& DamageInfo = LinkGun->InstantHitInfo[LinkGun->GetCurrentFireMode()];
		FHitResult Hit;
		const FName RealShotsStatsName = LinkGun->ShotsStatsName;
		LinkGun->ShotsStatsName = NAME_None;
		const FName RealHitsStatsName = LinkGun->HitsStatsName;
		LinkGun->HitsStatsName = NAME_None;
		LinkGun->FireInstantHit(false, &Hit);
		LinkGun->ShotsStatsName = RealShotsStatsName;
		LinkGun->HitsStatsName = RealHitsStatsName;

		AccumulatedFiringTime += DeltaTime;
		const float RefireTime = LinkGun->GetRefireTime(LinkGun->GetCurrentFireMode());
		AUTPlayerState* PS = (LinkGun->Role == ROLE_Authority && LinkGun->GetUTOwner() != nullptr && LinkGun->GetUTOwner()->Controller != nullptr)
			? Cast<AUTPlayerState>(LinkGun->GetUTOwner()->Controller->PlayerState)
			: nullptr;

		// Shaft HUD/scoreboard accuracy uses a Quake-style denominator: one
		// sample per beam refire interval, not stock NAME_LinkShots (one trigger
		// pull). Keep the accumulator bounded on every role, but only authority
		// writes stats. Floor handles an unusually long frame without silently
		// losing one or more beam samples.
		if (RefireTime > KINDA_SMALL_NUMBER && AccumulatedFiringTime >= RefireTime)
		{
			const int32 BeamSamples = FMath::FloorToInt(AccumulatedFiringTime / RefireTime);
			AccumulatedFiringTime -= float(BeamSamples) * RefireTime;
			if (PS != nullptr)
			{
				static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));
				PS->ModifyStatsValue(NAME_LinkBeamShots, BeamSamples);
			}
		}

		LinkGun->bLinkBeamImpacting = Hit.Time < 1.f;
		AActor* OldLinkedTarget = LinkGun->CurrentLinkedTarget;
		LinkGun->CurrentLinkedTarget = nullptr;

		if (Hit.Actor.IsValid() && Hit.Actor.Get()->bCanBeDamaged)
		{
			if (LinkGun->IsValidLinkTarget(Hit.Actor.Get()))
			{
				LinkGun->CurrentLinkedTarget = Hit.Actor.Get();
			}
			if (LinkGun->Role == ROLE_Authority)
			{
				LinkGun->bLinkCausingDamage = true;
			}

			const float LinkedDamage = float(DamageInfo.Damage);
			Accumulator += LinkedDamage / RefireTime * DeltaTime;
			if (Accumulator >= MinDamage)
			{
				const int32 AppliedDamage = FMath::TruncToInt(Accumulator);
				Accumulator -= AppliedDamage;
				const FVector FireDir = (Hit.Location - Hit.TraceStart).GetSafeNormal();
				AController* LinkDamageInstigator = LinkGun->GetUTOwner() != nullptr ? LinkGun->GetUTOwner()->Controller : nullptr;
				Hit.Actor->TakeDamage(
					AppliedDamage,
					FUTPointDamageEvent(AppliedDamage, Hit, FireDir, DamageInfo.DamageType,
						FireDir * (LinkGun->GetImpartedMomentumMag(Hit.Actor.Get()) * float(AppliedDamage) / float(DamageInfo.Damage))),
					LinkDamageInstigator,
					LinkGun);
				// No accuracy credit for melting projectiles (cores/rockets) —
				// same rule as UTWeaponFix::FireInstantHit (2026-08-10).
				if (PS != nullptr && LinkGun->HitsStatsName != NAME_None
					&& Cast<AUTProjectile>(Hit.Actor.Get()) == nullptr)
				{
					PS->ModifyStatsValue(LinkGun->HitsStatsName, AppliedDamage / FMath::Max(LinkedDamage, 1.f));
				}
			}
		}
		if (OldLinkedTarget != LinkGun->CurrentLinkedTarget)
		{
			LinkGun->LinkStartTime = GetWorld()->GetTimeSeconds();
			LinkGun->bReadyToPull = false;
		}
		else if (LinkGun->CurrentLinkedTarget != nullptr && !LinkGun->IsLinkPulsing())
		{
			LinkGun->bReadyToPull = LinkGun->SupportsLinkPull() &&
				GetWorld()->GetTimeSeconds() - LinkGun->LinkStartTime > LinkGun->PullWarmupTime;
		}

		// The owning client traces only its beam endpoint. Damage above remains
		// authoritative because Role < ROLE_Authority cannot apply it.
		if (LinkGun->Role < ROLE_Authority && LinkGun->GetUTOwner() != nullptr)
		{
			LinkGun->GetUTOwner()->SetFlashLocation(Hit.Location, LinkGun->GetCurrentFireMode());
		}
	}
}
