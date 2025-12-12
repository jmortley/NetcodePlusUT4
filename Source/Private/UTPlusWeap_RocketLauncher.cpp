// UTPlusWeap_RocketLauncher.cpp
// Full integration with spiral rockets and new standalone transactional charged state

#include "UTPlusWeap_RocketLauncher.h"
#include "UnrealTournament.h"
#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTProj_RocketSpiral.h"
#include "StatNames.h"
#include "Core.h"
#include "Engine.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Net/UnrealNetwork.h"
#include "UTWeaponStateEquipping.h"
#include "UTProj_Rocket.h"
#include "Particles/ParticleSystemComponent.h"
#include "Animation/AnimMontage.h"
#include "UTBot.h"
#include "UTGameState.h"
#include "UTHUDWidget.h"
#include "CanvasItem.h"

DEFINE_LOG_CATEGORY_STATIC(LogUTRocketLauncher, Log, All);

AUTPlusWeap_RocketLauncher::AUTPlusWeap_RocketLauncher(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DefaultGroup = 8;
    BringUpTime = 0.41f;

    // Rocket Loading
    NumLoadedRockets = 0;
    NumLoadedBarrels = 0;
    MaxLoadedRockets = 3;
    RocketLoadTime = 0.9f;
    FirstRocketLoadTime = 0.4f;
    CurrentRocketFireMode = 0;
    bDrawRocketModeString = false;
    LastLoadTime = 0.0f;

    // Timing
    GracePeriod = 0.6f;
    BurstInterval = 0.f;
    GrenadeBurstInterval = 0.1f;
    SpiralBurstInterval = 0.f; // Spiral fires all at once

    // Spread
    FullLoadSpread = 8.f;
    SeekingLoadSpread = 16.f;
    BarrelRadius = 9.0f;

    // Fire Modes - Enable by default now that we have spiral
    bAllowAltModes = true;
    bAllowGrenades = true; // Legacy compatibility

    // Target Locking
    bLockedOnTarget = false;
    LockCheckTime = 0.1f;
    LockRange = 16000.0f;
    LockAcquireTime = 0.5f;
    LockTolerance = 0.2f;
    LockedTarget = nullptr;
    PendingLockedTarget = nullptr;
    LastLockedOnTime = 0.0f;
    PendingLockedTargetTime = 0.0f;
    LastValidTargetTime = 0.0f;
    LockAim = 0.997f;
    LockOffset = 800.f;
    bTargetLockingActive = true;
    LastTargetLockCheckTime = 0.0f;

    // HUD
    CrosshairRotationTime = 0.3f;
    CurrentRotation = 0.0f;
    HUDViewKickback = FVector2D(0.f, 0.2f);
    FOVOffset = FVector(0.5f, 1.f, 1.f);

    // Stats
    BasePickupDesireability = 0.78f;
    BaseAISelectRating = 0.78f;
    FiringViewKickback = -50.f;
    FiringViewKickbackY = 20.f;
    bRecommendSplashDamage = true;

    KillStatsName = NAME_RocketKills;
    DeathStatsName = NAME_RocketDeaths;
    HitsStatsName = NAME_RocketHits;
    ShotsStatsName = NAME_RocketShots;

    WeaponCustomizationTag = EpicWeaponCustomizationTags::RocketLauncher;
    WeaponSkinCustomizationTag = EpicWeaponSkinCustomizationTags::RocketLauncher;

    TutorialAnnouncements.Add(TEXT("PriRocketLauncher"));
    TutorialAnnouncements.Add(TEXT("SecRocketLauncher"));
    HighlightText = NSLOCTEXT("Weapon", "RockerHighlightText", "I am the Rocketman");

    // AI
    PredicitiveTargetLoc = FVector::ZeroVector;
    LastAttackSkillCheckTime = 0.0f;
    bAttackSkillCheckResult = false;
}

void AUTPlusWeap_RocketLauncher::PostInitProperties()
{
    Super::PostInitProperties();

    // Fire Mode 0: Primary - Single Rocket (Transactional)
    if (FiringState.Num() > 0)
    {
        FiringState[0] = NewObject<UUTWeaponStateFiring_Transactional>(this, UUTWeaponStateFiring_Transactional::StaticClass());
    }

    // Fire Mode 1: Alt - Load Multiple Rockets (NEW Standalone Charged Transactional)
    if (FiringState.Num() > 1)
    {
        FiringState[1] = NewObject<UUTWeaponStateFiringChargedRocket_Transactional>(this, UUTWeaponStateFiringChargedRocket_Transactional::StaticClass());
    }
}





void AUTPlusWeap_RocketLauncher::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AUTPlusWeap_RocketLauncher, LockedTarget);
    DOREPLIFETIME(AUTPlusWeap_RocketLauncher, PendingLockedTarget);
    DOREPLIFETIME(AUTPlusWeap_RocketLauncher, CurrentRocketFireMode);
}

void AUTPlusWeap_RocketLauncher::Destroyed()
{
    Super::Destroyed();
    GetWorldTimerManager().ClearAllTimersForObject(this);
}

// =============================================================================
// ROCKET LOADING
// =============================================================================

void AUTPlusWeap_RocketLauncher::BeginLoadRocket()
{
    if ((GetNetMode() != NM_DedicatedServer) && (GetMesh()->GetAnimInstance() != nullptr))
    {
        UAnimMontage* PickedAnimation = nullptr;
        UAnimMontage* PickedHandAnim = nullptr;

        if (Ammo > 0)
        {
            PickedAnimation = LoadingAnimation.IsValidIndex(NumLoadedBarrels) ? LoadingAnimation[NumLoadedBarrels] : nullptr;
            PickedHandAnim = LoadingAnimationHands.IsValidIndex(NumLoadedBarrels) ? LoadingAnimationHands[NumLoadedBarrels] : nullptr;
        }
        else
        {
            PickedAnimation = EmptyLoadingAnimation.IsValidIndex(NumLoadedBarrels) ? EmptyLoadingAnimation[NumLoadedBarrels] : nullptr;
            PickedHandAnim = EmptyLoadingAnimationHands.IsValidIndex(NumLoadedBarrels) ? EmptyLoadingAnimationHands[NumLoadedBarrels] : nullptr;
        }

        if (PickedAnimation != nullptr)
        {
            GetMesh()->GetAnimInstance()->Montage_Play(PickedAnimation, PickedAnimation->SequenceLength / GetLoadTime(NumLoadedBarrels));
        }

        if (GetUTOwner() != nullptr && GetUTOwner()->FirstPersonMesh != nullptr &&
            GetUTOwner()->FirstPersonMesh->GetAnimInstance() != nullptr && PickedHandAnim != nullptr)
        {
            GetUTOwner()->FirstPersonMesh->GetAnimInstance()->Montage_Play(PickedHandAnim, PickedHandAnim->SequenceLength / GetLoadTime(NumLoadedBarrels));
        }
    }
}

void AUTPlusWeap_RocketLauncher::EndLoadRocket()
{
    NumLoadedBarrels++;

    if (Ammo > 0)
    {
        NumLoadedRockets++;
        SetRocketFlashExtra(CurrentFireMode, NumLoadedRockets + 1, CurrentRocketFireMode, bDrawRocketModeString);
        ConsumeAmmo(CurrentFireMode);

        if ((Ammo <= LowAmmoThreshold) && (Ammo > 0) && (LowAmmoSound != nullptr))
        {
            AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
            if (!GameMode || GameMode->bAmmoIsLimited)
            {
                GetWorldTimerManager().SetTimer(PlayLowAmmoSoundHandle, this, &AUTWeapon::PlayLowAmmoSound, LowAmmoSoundDelay, false);
            }
        }
    }
    else
    {
        PlayLowAmmoSound();
    }

    LastLoadTime = GetWorld()->TimeSeconds;

    // Replicate loading sound to other players
    AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
    if ((PC == nullptr) || !PC->IsLocalPlayerController())
    {
        UUTGameplayStatics::UTPlaySound(GetWorld(), RocketLoadedSound, UTOwner, SRT_AllButOwner, false, FVector::ZeroVector, NULL, NULL, true, SAT_WeaponFire);
    }

    // Bot AI: Maybe shoots rockets early
    AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
    if (B != nullptr && B->GetTarget() != nullptr && B->LineOfSightTo(B->GetTarget()) && !B->NeedToTurn(B->GetFocalPoint()))
    {
        if (NumLoadedRockets == MaxLoadedRockets)
        {
            UTOwner->StopFiring();
        }
        else if (NumLoadedRockets > 1)
        {
            if (B->GetTarget() != B->GetEnemy())
            {
                if (FMath::FRand() < 0.5f)
                {
                    UTOwner->StopFiring();
                }
            }
            else if (FMath::FRand() < 0.3f)
            {
                UTOwner->StopFiring();
            }
            else if (ProjClass.IsValidIndex(CurrentFireMode) && ProjClass[CurrentFireMode] != nullptr)
            {
                AUTCharacter* P = Cast<AUTCharacter>(B->GetEnemy());
                if (P != nullptr && P->HealthMax * B->GetEnemyInfo(B->GetEnemy(), true)->EffectiveHealthPct * 2.0f < ProjClass[CurrentFireMode].GetDefaultObject()->DamageParams.BaseDamage * NumLoadedRockets)
                {
                    UTOwner->StopFiring();
                }
            }
        }
    }
}

void AUTPlusWeap_RocketLauncher::ClearLoadedRockets()
{
    CurrentRocketFireMode = 0;
    NumLoadedBarrels = 0;
    NumLoadedRockets = 0;

    if (Role == ROLE_Authority)
    {
        SetLockTarget(nullptr);
        PendingLockedTarget = nullptr;
        PendingLockedTargetTime = 0.f;
    }

    if (UTOwner != nullptr)
    {
        UTOwner->SetFlashExtra(0, CurrentFireMode);
    }

    bDrawRocketModeString = false;
    LastLoadTime = 0.0f;
}

float AUTPlusWeap_RocketLauncher::GetLoadTime(int32 InNumLoadedRockets)
{
    float BaseTime = (InNumLoadedRockets > 0) ? RocketLoadTime : FirstRocketLoadTime;
    return BaseTime / ((UTOwner != nullptr) ? UTOwner->GetFireRateMultiplier() : 1.0f);
}

void AUTPlusWeap_RocketLauncher::ClientAbortLoad_Implementation()
{
    // Check for NEW transactional state first
    UUTWeaponStateFiringChargedRocket_Transactional* TransactionalLoadState =
        Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);

    if (TransactionalLoadState != nullptr)
    {
        // Abort loading animation
        UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
        if (AnimInstance != nullptr && LoadingAnimation.IsValidIndex(NumLoadedRockets) && LoadingAnimation[NumLoadedRockets] != nullptr)
        {
            AnimInstance->Montage_SetPlayRate(LoadingAnimation[NumLoadedRockets], 0.0f);
        }

        UAnimInstance* AnimInstanceHands = (GetUTOwner() && GetUTOwner()->FirstPersonMesh) ? GetUTOwner()->FirstPersonMesh->GetAnimInstance() : nullptr;
        if (AnimInstanceHands != nullptr && LoadingAnimationHands.IsValidIndex(NumLoadedRockets) && LoadingAnimationHands[NumLoadedRockets] != nullptr)
        {
            AnimInstanceHands->Montage_SetPlayRate(LoadingAnimationHands[NumLoadedRockets], 0.0f);
        }

        // Set grace timer with ping compensation
        float AdjustedGraceTime = GracePeriod;
        if (UTOwner != nullptr && UTOwner->PlayerState != nullptr)
        {
            AdjustedGraceTime = FMath::Max<float>(0.01f, AdjustedGraceTime - UTOwner->PlayerState->ExactPing * 0.0005f);
        }

        GetWorldTimerManager().SetTimer(
            TransactionalLoadState->GraceTimerHandle,
            TransactionalLoadState,
            &UUTWeaponStateFiringChargedRocket_Transactional::GraceTimer,
            AdjustedGraceTime,
            false
        );
        return;
    }
}

bool AUTPlusWeap_RocketLauncher::ShouldFireLoad()
{
    return !UTOwner || UTOwner->IsPendingKillPending() || (UTOwner->Health <= 0) || UTOwner->IsRagdoll();
}

// =============================================================================
// FIRING
// =============================================================================

void AUTPlusWeap_RocketLauncher::FireShot()
{
    Super::FireShot();
}


void AUTPlusWeap_RocketLauncher::FireShotDirect()
{
    if (UTOwner)
    {
        UTOwner->DeactivateSpawnProtection();
    }
    if (LastFireTime.IsValidIndex(CurrentFireMode))
    {
        LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
    }
    // 1. Fire the projectile directly
    // (This function has internal logic to NOT consume ammo for Alt-Fire)
    FireProjectile();

    // 2. We must manually play effects since we skipped the base FireShot
    PlayFiringEffects();

    // 3. Trigger inventory event
    if (GetUTOwner())
    {
        GetUTOwner()->InventoryEvent(InventoryEventName::FiredWeapon);
    }
    // We must tell the WeaponFix system that we just fired NOW.
    // Otherwise, it calculates cooldown from when we STARTED charging (seconds ago),
    // allowing an instant double-tap after release.

    // Reset offset logic (from base class)
    FireZOffsetTime = 0.f;
}


AUTProjectile* AUTPlusWeap_RocketLauncher::FireProjectile()
{
    if (GetUTOwner() == nullptr)
    {
        UE_LOG(LogUTRocketLauncher, Warning, TEXT("%s::FireProjectile(): Weapon is not owned"), *GetName());
        return nullptr;
    }

    if (UTOwner)
    {
        UTOwner->DeactivateSpawnProtection();
    }

    // Alt-fire already consumed ammo during loading
    if (CurrentFireMode != 1)
    {
        ConsumeAmmo(CurrentFireMode);
    }


    UTOwner->SetFlashExtra(0, CurrentFireMode);
    AUTPlayerState* PS = UTOwner->Controller ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : nullptr;

    // Alt-fire: Use multi-mode rocket projectile function
    if (CurrentFireMode == 1)
    {
        // Bot AI chooses mode
        if (bAllowAltModes || bAllowGrenades)
        {
            AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
            if (B != nullptr)
            {
                if (B->GetTarget() == B->GetEnemy())
                {
                    if (B->GetEnemy() == nullptr || B->LostContact(1.0f))
                    {
                        CurrentRocketFireMode = 1; // Grenades when retreating
                    }
                    else
                    {
                        CurrentRocketFireMode = 0; // Standard spread
                    }
                }
                else
                {
                    CurrentRocketFireMode = 1; // Grenades for non-enemy targets
                }
            }
        }

        // Stats and flash
        if (Role == ROLE_Authority)
        {
            UTOwner->IncrementFlashCount(NumLoadedRockets);
            if (PS && (ShotsStatsName != NAME_None))
            {
                PS->ModifyStatsValue(ShotsStatsName, NumLoadedRockets);
            }
        }

        return FireRocketProjectile();
    }
    else
    {
        // Primary fire: Single rocket
        checkSlow(ProjClass.IsValidIndex(CurrentFireMode) && ProjClass[CurrentFireMode] != nullptr);

        if (Role == ROLE_Authority)
        {
            UTOwner->IncrementFlashCount(CurrentFireMode);
            if (PS && (ShotsStatsName != NAME_None))
            {
                PS->ModifyStatsValue(ShotsStatsName, 1);
            }
        }

        FVector SpawnLocation = GetFireStartLoc();
        FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);

        // Adjust from center to barrel
        FVector AdjustedSpawnLoc = SpawnLocation + FRotationMatrix(SpawnRotation).GetUnitAxis(EAxis::Z) * BarrelRadius;
        FHitResult Hit;
        if (GetWorld()->LineTraceSingleByChannel(Hit, SpawnLocation, AdjustedSpawnLoc, COLLISION_TRACE_WEAPON, FCollisionQueryParams(NAME_None, true, UTOwner)))
        {
            SpawnLocation = Hit.Location - (AdjustedSpawnLoc - SpawnLocation).GetSafeNormal();
        }
        else
        {
            SpawnLocation = AdjustedSpawnLoc;
        }

        AUTProjectile* SpawnedProjectile = SpawnNetPredictedProjectile(RocketFireModes[0].ProjClass, SpawnLocation, SpawnRotation);
        NumLoadedRockets = 0;
        NumLoadedBarrels = 0;
        return SpawnedProjectile;
    }
}

AUTProjectile* AUTPlusWeap_RocketLauncher::FireRocketProjectile()
{
    TSubclassOf<AUTProjectile> RocketProjClass = nullptr;

    switch (CurrentRocketFireMode)
    {
    case 0: // Spread
        if (HasLockedTarget() && SeekingRocketClass)
            RocketProjClass = SeekingRocketClass;
        else if (RocketFireModes.IsValidIndex(0))
            RocketProjClass = RocketFireModes[0].ProjClass;
        break;

    case 1: // Grenades
        if (RocketFireModes.IsValidIndex(1) && RocketFireModes[1].ProjClass)
            RocketProjClass = RocketFireModes[1].ProjClass;
        else if (RocketFireModes.IsValidIndex(0)) // Fallback to Spread
            RocketProjClass = RocketFireModes[0].ProjClass;
        break;

    case 2: // Spiral
        if (RocketFireModes.IsValidIndex(2) && RocketFireModes[2].ProjClass)
            RocketProjClass = RocketFireModes[2].ProjClass;
        else if (SpiralRocketClass)
            RocketProjClass = SpiralRocketClass;
        else if (RocketFireModes.IsValidIndex(0)) // Fallback to Spread
            RocketProjClass = RocketFireModes[0].ProjClass;
        break;
    }

    if (!RocketProjClass)
    {
        // --- FIX START: INFINITE LOOP PREVENTION ---
        // We must decrement this, otherwise the "Fire all rockets" while-loop
        // in the firing state will run forever and freeze the editor.
        if (NumLoadedRockets > 0)
        {
            NumLoadedRockets--;
        }
        // --- FIX END ---

        UE_LOG(LogUTRocketLauncher, Warning, TEXT("No valid projectile class for fire mode %d"), CurrentRocketFireMode);
        return nullptr;
    }

    const FVector SpawnLocation = GetFireStartLoc();
    FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
    AUTProjectile* ResultProj = nullptr;
    switch (CurrentRocketFireMode)
    {
    case 0: // SPREAD
    {
        // --- FIX: Only spawn on Server to prevent Ghosting ---
        if (Role == ROLE_Authority)
        {
            if (NumLoadedRockets > 1)
            {
                FVector FireDir = SpawnRotation.Vector();
                FVector SideDir = (FireDir ^ FVector(0.f, 0.f, 1.f)).GetSafeNormal();
                float SpreadAmount = HasLockedTarget() ? SeekingLoadSpread : FullLoadSpread;
                FireDir = FireDir + 0.01f * SideDir * float((NumLoadedRockets % 3) - 1.f) * SpreadAmount;
                SpawnRotation = FireDir.Rotation();
            }

            NetSynchRandomSeed();
            FVector Offset = (FMath::Sin(NumLoadedRockets * PI * 0.667f) * FRotationMatrix(SpawnRotation).GetUnitAxis(EAxis::Z) +
                FMath::Cos(NumLoadedRockets * PI * 0.667f) * FRotationMatrix(SpawnRotation).GetUnitAxis(EAxis::X)) * BarrelRadius * 2.f;

            ResultProj = SpawnNetPredictedProjectile(RocketProjClass, SpawnLocation + Offset, SpawnRotation);

            // Setup seeking
            AUTProj_Rocket* SpawnedRocket = Cast<AUTProj_Rocket>(ResultProj);
            if (HasLockedTarget() && SpawnedRocket)
            {
                SpawnedRocket->TargetActor = LockedTarget;
                TrackingRockets.AddUnique(SpawnedRocket);
            }
        }

        // IMPORTANT: Decrement on BOTH Client and Server to prevent infinite loops
        NumLoadedRockets--;
        break;
    }

    case 1: // GRENADES
    {
        // --- FIX: Only spawn on Server ---
        if (Role == ROLE_Authority)
        {
            float GrenadeSpread = GetSpread(1);
            float RotDegree = 360.0f / MaxLoadedRockets;
            SpawnRotation.Roll = RotDegree * MaxLoadedRockets;
            FRotator SpreadRot = SpawnRotation;
            SpreadRot.Yaw += GrenadeSpread * float(MaxLoadedRockets) - GrenadeSpread;

            ResultProj = SpawnNetPredictedProjectile(RocketProjClass, SpawnLocation, SpreadRot);

            if (ResultProj != nullptr && ResultProj->ProjectileMovement)
            {
                ResultProj->ProjectileMovement->Velocity.Z += (MaxLoadedRockets % 2) * GetSpread(2);
            }
        }

        // IMPORTANT: Decrement on BOTH Client and Server
        NumLoadedRockets--;
        break;
    }

    case 2: // SPIRAL
    {
        // --- FIX: Only spawn on Server ---
        if (Role == ROLE_Authority)
        {
            TArray<AUTProj_RocketSpiral*> SpiralRockets;
            int32 RocketsToSpawn = NumLoadedRockets;

            for (int32 i = 0; i < RocketsToSpawn; i++)
            {
                float Angle = (i * 360.0f / RocketsToSpawn) * (PI / 180.0f);
                FVector Offset = (FMath::Sin(Angle) * FRotationMatrix(SpawnRotation).GetUnitAxis(EAxis::Z) +
                    FMath::Cos(Angle) * FRotationMatrix(SpawnRotation).GetUnitAxis(EAxis::X)) * BarrelRadius;

                AUTProjectile* NewProj = SpawnNetPredictedProjectile(RocketProjClass, SpawnLocation + Offset, SpawnRotation);

                AUTProj_RocketSpiral* SpiralProj = Cast<AUTProj_RocketSpiral>(NewProj);
                if (SpiralProj)
                {
                    SpiralRockets.Add(SpiralProj);
                }
                if (i == 0) ResultProj = NewProj;
            }

            // Link Flocking
            for (int32 i = 0; i < SpiralRockets.Num(); i++)
            {
                AUTProj_RocketSpiral* RocketA = SpiralRockets[i];
                if (RocketA)
                {
                    int32 FlockIndex = 0;
                    for (int32 j = 0; j < SpiralRockets.Num() && FlockIndex < 3; j++)
                    {
                        if (i != j && SpiralRockets[j])
                        {
                            RocketA->Flock[FlockIndex++] = SpiralRockets[j];
                        }
                    }
                    RocketA->bCurl = (i % 2 == 0);
                }
            }
        }

        // IMPORTANT: Clear on BOTH Client and Server
        NumLoadedRockets = 0;
        break;
    }
    }

    // Reset Mode when empty
    if (NumLoadedRockets <= 0)
    {
        CurrentRocketFireMode = 0;
        bDrawRocketModeString = false;
        if (Role == ROLE_Authority)
        {
            SetRocketFlashExtra(CurrentFireMode, 0, 0, false);
        }
    }

    return ResultProj;
}

void AUTPlusWeap_RocketLauncher::PlayDelayedFireSound()
{
    if (UTOwner && RocketFireModes.IsValidIndex(CurrentRocketFireMode))
    {
        USoundBase* FireSound = RocketFireModes[CurrentRocketFireMode].FireSound;
        USoundBase* FPFireSound = RocketFireModes[CurrentRocketFireMode].FPFireSound;

        if (FPFireSound != nullptr && Cast<APlayerController>(UTOwner->Controller) != nullptr && UTOwner->IsLocallyControlled())
        {
            UUTGameplayStatics::UTPlaySound(GetWorld(), FPFireSound, UTOwner, SRT_AllButOwner, false, FVector::ZeroVector, GetCurrentTargetPC(), NULL, true, SAT_WeaponFire);
        }
        else if (FireSound != nullptr)
        {
            UUTGameplayStatics::UTPlaySound(GetWorld(), FireSound, UTOwner, SRT_AllButOwner, false, FVector::ZeroVector, GetCurrentTargetPC(), NULL, true, SAT_WeaponFire);
        }
    }
}

void AUTPlusWeap_RocketLauncher::PlayFiringEffects()
{
    if (CurrentFireMode == 1 && UTOwner != nullptr)
    {
        UTOwner->TargetEyeOffset.X = FiringViewKickback;
        UTOwner->TargetEyeOffset.Y = FiringViewKickbackY;

        AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
        //if (PC != nullptr)
        //{ remove for now. this causes netstats to show up for some insane reason
        //   PC->AddHUDImpulse(HUDViewKickback);
        //}

        // Delayed sound based on rockets remaining
        if (NumLoadedRockets > 0)
        {
            FTimerHandle TempHandle;
            GetWorld()->GetTimerManager().SetTimer(TempHandle, this, &AUTPlusWeap_RocketLauncher::PlayDelayedFireSound, 0.1f * NumLoadedRockets);
        }
        else
        {
            PlayDelayedFireSound();
        }

        if (ShouldPlay1PVisuals())
        {
            // Firing animation
            if (FiringAnimation.IsValidIndex(NumLoadedRockets) && FiringAnimation[NumLoadedRockets] != nullptr)
            {
                UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
                if (AnimInstance != nullptr)
                {
                    AnimInstance->Montage_Play(FiringAnimation[NumLoadedRockets], 1.f);
                }
            }

            // Hand animation
            if (FiringAnimationHands.IsValidIndex(NumLoadedRockets) && FiringAnimationHands[NumLoadedRockets] != nullptr)
            {
                UAnimInstance* AnimInstanceHands = (GetUTOwner() && GetUTOwner()->FirstPersonMesh) ? GetUTOwner()->FirstPersonMesh->GetAnimInstance() : nullptr;
                if (AnimInstanceHands != nullptr)
                {
                    AnimInstanceHands->Montage_Play(FiringAnimationHands[NumLoadedRockets], 1.f);
                }
            }

            // Muzzle flash for each loaded rocket
            if (RocketFireModes.IsValidIndex(CurrentRocketFireMode) && RocketFireModes[CurrentRocketFireMode].bCauseMuzzleFlash)
            {
                for (int32 i = 0; i < NumLoadedRockets; i++)
                {
                    if (MuzzleFlash.IsValidIndex(i) && MuzzleFlash[i] != nullptr && MuzzleFlash[i]->Template != nullptr)
                    {
                        //if (!MuzzleFlash[i]->bIsActive || MuzzleFlash[i]->Template->Emitters[0] == nullptr ||
							//IsLoopingParticleSystem(MuzzleFlash[i]->Template)) in case this plays effects with looping emitters in future
                        if (MuzzleFlash.IsValidIndex(i) && MuzzleFlash[i] != nullptr && MuzzleFlash[i]->Template != nullptr)
                        {
                            MuzzleFlash[i]->ActivateSystem();
                        }
                    }
                }
            }
        }
    }
    else
    {
        Super::PlayFiringEffects();
    }
}







bool AUTPlusWeap_RocketLauncher::ServerCycleRocketMode_Validate()
{
    return true;
}

void AUTPlusWeap_RocketLauncher::ServerCycleRocketMode_Implementation()
{
    // The server just needs to run the exact same logic
    OnMultiPress_Implementation(0);
}

// ---------------------------------------------------------
// UPDATED OnMultiPress
// ---------------------------------------------------------
void AUTPlusWeap_RocketLauncher::OnMultiPress_Implementation(uint8 OtherFireMode)
{
    // Only allow mode switch if currently in Alt-Fire (Mode 1)
    if ((bAllowAltModes || bAllowGrenades) && (CurrentFireMode == 1))
    {
        // 1. Check State Validity
        UUTWeaponStateFiringChargedRocket_Transactional* TransactionalAltState =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);

        bool bIsCharging = false;
        FTimerHandle* GraceHandle = nullptr;

        if (TransactionalAltState != nullptr)
        {
            bIsCharging = TransactionalAltState->bCharging;
            GraceHandle = &TransactionalAltState->GraceTimerHandle;
        }

        // 2. Perform Timing Safety Check (Prevent switching right as you fire)
        if (bIsCharging && GraceHandle != nullptr)
        {
            if ((GetWorldTimerManager().IsTimerActive(*GraceHandle) &&
                GetWorldTimerManager().GetTimerRemaining(*GraceHandle) < 0.05f) ||
                GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle))
            {
                return;
            }

            // --- FIX START: SYNC WITH SERVER ---
            // If we are a client, tell the server we pushed the button.
            //
            
            if (Role < ROLE_Authority)
            {
               ServerCycleRocketMode();
            }
            // --- FIX END ---

            // 3. Cycle Mode Locally
            CurrentRocketFireMode++;
            bDrawRocketModeString = true;

            // Determine Max Modes (Spread, Grenade, Spiral)
            int32 MaxModes = RocketFireModes.Num();
            if (SpiralRocketClass != nullptr || MaxModes < 3)
            {
                MaxModes = 3;
            }

            if (CurrentRocketFireMode >= MaxModes)
            {
                CurrentRocketFireMode = 0;
            }

            // 4. Feedback
            UUTGameplayStatics::UTPlaySound(GetWorld(), AltFireModeChangeSound, UTOwner, SRT_AllButOwner, false, FVector::ZeroVector, NULL, NULL, true, SAT_WeaponFoley);

            // 5. Update Server-Side Flash (so other clients see the text change)
            if (Role == ROLE_Authority)
            {
                SetRocketFlashExtra(CurrentFireMode, NumLoadedRockets + 1, CurrentRocketFireMode, bDrawRocketModeString);
            }
        }
    }
}


float AUTPlusWeap_RocketLauncher::GetSpread(int32 ModeIndex)
{
    if (RocketFireModes.IsValidIndex(ModeIndex))
    {
        return RocketFireModes[ModeIndex].Spread;
    }
    return 0.0f;
}

void AUTPlusWeap_RocketLauncher::SetRocketFlashExtra(uint8 InFireMode, int32 InNumLoadedRockets, int32 InCurrentRocketFireMode, bool bInDrawRocketModeString)
{
    if (UTOwner != nullptr && Role == ROLE_Authority)
    {
        if (InFireMode == 0)
        {
            GetUTOwner()->SetFlashExtra(0, InFireMode);
        }
        else
        {
            uint8 NewFlashExtra = InNumLoadedRockets;
            if (bInDrawRocketModeString)
            {
                NewFlashExtra |= 1 << 7;
                NewFlashExtra |= InCurrentRocketFireMode << 4;
            }
            GetUTOwner()->SetFlashExtra(NewFlashExtra, InFireMode);
        }
    }
}

void AUTPlusWeap_RocketLauncher::GetRocketFlashExtra(uint8 InFlashExtra, uint8 InFireMode, int32& OutNumLoadedRockets, int32& OutCurrentRocketFireMode, bool& bOutDrawRocketModeString)
{
    if (InFireMode == 1)
    {
        if (InFlashExtra >> 7 > 0)
        {
            bOutDrawRocketModeString = true;
            OutCurrentRocketFireMode = (InFlashExtra >> 4) & 0x07;
        }
        OutNumLoadedRockets = FMath::Min(static_cast<int32>(InFlashExtra & 0x0F), MaxLoadedRockets);
    }
}

void AUTPlusWeap_RocketLauncher::FiringExtraUpdated_Implementation(uint8 NewFlashExtra, uint8 InFireMode)
{
    if (InFireMode == 1)
    {
        int32 NewNumLoadedRockets;
        GetRocketFlashExtra(NewFlashExtra, InFireMode, NewNumLoadedRockets, CurrentRocketFireMode, bDrawRocketModeString);

        if (NewNumLoadedRockets != NumLoadedRockets && GetNetMode() != NM_DedicatedServer)
        {
            NumLoadedRockets = NewNumLoadedRockets;

            if (NumLoadedRockets > 0)
            {
                UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
                if (AnimInstance != nullptr && LoadingAnimation.IsValidIndex(NumLoadedRockets - 1) && LoadingAnimation[NumLoadedRockets - 1] != nullptr)
                {
                    AnimInstance->Montage_Play(LoadingAnimation[NumLoadedRockets - 1], LoadingAnimation[NumLoadedRockets - 1]->SequenceLength / GetLoadTime(NumLoadedRockets - 1));
                }

                UAnimInstance* AnimInstanceHands = (GetUTOwner() && GetUTOwner()->FirstPersonMesh) ? GetUTOwner()->FirstPersonMesh->GetAnimInstance() : nullptr;
                if (AnimInstanceHands != nullptr && LoadingAnimationHands.IsValidIndex(NumLoadedRockets - 1) && LoadingAnimationHands[NumLoadedRockets - 1] != nullptr)
                {
                    AnimInstanceHands->Montage_Play(LoadingAnimationHands[NumLoadedRockets - 1], LoadingAnimationHands[NumLoadedRockets - 1]->SequenceLength / GetLoadTime(NumLoadedRockets - 1));
                }
            }
        }
    }
}

void AUTPlusWeap_RocketLauncher::FiringInfoUpdated_Implementation(uint8 InFireMode, uint8 FlashCount, FVector InFlashLocation)
{
    Super::FiringInfoUpdated_Implementation(InFireMode, FlashCount, InFlashLocation);
    CurrentRocketFireMode = 0;
    bDrawRocketModeString = false;
    NumLoadedRockets = 0;
    NumLoadedBarrels = 0;
}

// =============================================================================
// TARGET LOCKING
// =============================================================================

void AUTPlusWeap_RocketLauncher::StateChanged()
{
    Super::StateChanged();

    if (Role == ROLE_Authority && CurrentState != InactiveState && CurrentState != EquippingState && CurrentState != UnequippingState)
    {
        GetWorldTimerManager().SetTimer(UpdateLockHandle, this, &AUTPlusWeap_RocketLauncher::UpdateLock, LockCheckTime, true);
    }
    else
    {
        GetWorldTimerManager().ClearTimer(UpdateLockHandle);
    }

    if (CurrentState == InactiveState)
    {
        ClearLoadedRockets();
    }
}

bool AUTPlusWeap_RocketLauncher::CanLockTarget(AActor* Target)
{
    if (Target != nullptr && !Target->bTearOff && !IsPendingKillPending())
    {
        AUTCharacter* UTP = Cast<AUTCharacter>(Target);
        return (UTP != nullptr && (UTP->GetTeamNum() == 255 || UTP->GetTeamNum() != UTOwner->GetTeamNum()));
    }
    return false;
}

bool AUTPlusWeap_RocketLauncher::WithinLockAim(AActor* Target)
{
    if (CanLockTarget(Target))
    {
        const FVector FireLoc = UTOwner->GetPawnViewLocation();
        const FVector Dir = GetBaseFireRotation().Vector();
        const FVector TargetDir = (Target->GetActorLocation() - UTOwner->GetActorLocation()).GetSafeNormal();
        return (FVector::DotProduct(Dir, TargetDir) > LockAim ||
            UUTGameplayStatics::ChooseBestAimTarget(UTOwner->Controller, FireLoc, Dir, LockAim, LockRange, LockOffset, AUTCharacter::StaticClass()) == Target);
    }
    return false;
}

void AUTPlusWeap_RocketLauncher::SetLockTarget(AActor* NewTarget)
{
    LockedTarget = NewTarget;
    bLockedOnTarget = (LockedTarget != nullptr);
}

void AUTPlusWeap_RocketLauncher::UpdateLock()
{
    // Implementation would go here - keeping it simple for now
    // This is called on a timer to update target lock state
}

void AUTPlusWeap_RocketLauncher::OnRep_LockedTarget()
{
    SetLockTarget(LockedTarget);
}

void AUTPlusWeap_RocketLauncher::OnRep_PendingLockedTarget()
{
    if (PendingLockedTarget != nullptr)
    {
        PendingLockedTargetTime = GetWorld()->GetTimeSeconds();
    }
}

// =============================================================================
// AI
// =============================================================================

bool AUTPlusWeap_RocketLauncher::IsPreparingAttack_Implementation()
{
    if (GracePeriod <= 0.0f)
    {
        return false;
    }

    // Check NEW transactional state first
    UUTWeaponStateFiringChargedRocket_Transactional* TransactionalChargeState =
        Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);
    if (TransactionalChargeState != nullptr)
    {
        return TransactionalChargeState->bCharging;
    }
    return false;
}

float AUTPlusWeap_RocketLauncher::GetAISelectRating_Implementation()
{
    AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
    if (B == nullptr || B->GetEnemy() == nullptr)
    {
        return BaseAISelectRating;
    }

    float Rating = BaseAISelectRating;
    FVector EnemyDir = B->GetEnemyLocation(B->GetEnemy(), true) - UTOwner->GetActorLocation();
    float EnemyDist = EnemyDir.Size();

    // Too close - might blow self up
    if (EnemyDist < 750.0f)
    {
        if (UTOwner->GetWeapon() == this && (EnemyDist > 550.0f || (UTOwner->Health < 50 && UTOwner->GetEffectiveHealthPct(false) < B->GetEnemyInfo(B->GetEnemy(), true)->EffectiveHealthPct)))
        {
            return Rating;
        }
        return 0.05f + EnemyDist * 0.00045;
    }

    // Height advantage adjustments
    float ZDiff = EnemyDir.Z;
    if (ZDiff < -250.0f)
    {
        Rating += 0.25;
    }
    else if (ZDiff > 350.0f)
    {
        Rating -= 0.35;
    }
    else if (ZDiff > 175.0f)
    {
        Rating -= 0.1;
    }

    // Good against melee
    AUTCharacter* EnemyChar = Cast<AUTCharacter>(B->GetEnemy());
    if (EnemyChar != nullptr && EnemyChar->GetWeapon() != nullptr && EnemyChar->GetWeapon()->bMeleeWeapon && EnemyDist < 5500.0f)
    {
        Rating += 0.1f;
    }

    return Rating;
}

float AUTPlusWeap_RocketLauncher::SuggestAttackStyle_Implementation()
{
    AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
    if (B != nullptr && B->GetEnemy() != nullptr)
    {
        float EnemyDist = (B->GetEnemyLocation(B->GetEnemy(), false) - UTOwner->GetActorLocation()).Size();
        if (EnemyDist < 1600.0f)
        {
            return (EnemyDist < 1100.0f) ? -1.5f : -0.7f;
        }
        else if (EnemyDist > 3500.0f)
        {
            return 0.5f;
        }
    }
    return -0.1f;
}

bool AUTPlusWeap_RocketLauncher::CanAttack_Implementation(AActor* Target, const FVector& TargetLoc, bool bDirectOnly, bool bPreferCurrentMode, uint8& BestFireMode, FVector& OptimalTargetLoc)
{
    // Simplified AI attack logic - full implementation would include predictive firing
    return Super::CanAttack_Implementation(Target, TargetLoc, bDirectOnly, bPreferCurrentMode, BestFireMode, OptimalTargetLoc);
}


void AUTPlusWeap_RocketLauncher::DrawWeaponCrosshair_Implementation(UUTHUDWidget* WeaponHudWidget, float RenderDelta)
{
    if (!WeaponHudWidget || !WeaponHudWidget->UTHUDOwner) return;

    float ScaledPadding = 50.f * WeaponHudWidget->GetRenderScale();

    // 1. Draw the Rocket Firemode Text (e.g. "Grenades", "Spiral")
    if (bDrawRocketModeString && RocketModeFont != nullptr && RocketFireModes.IsValidIndex(CurrentRocketFireMode))
    {
        FText RocketModeText = RocketFireModes[CurrentRocketFireMode].DisplayString;
        float PosY = 0.3f * ScaledPadding;
        WeaponHudWidget->DrawText(RocketModeText, 0.0f, PosY, RocketModeFont, FLinearColor::Black, 1.0f, 1.0f, FLinearColor::White, ETextHorzPos::Center, ETextVertPos::Top);
    }

    // 2. Draw the Standard Crosshair
    Super::DrawWeaponCrosshair_Implementation(WeaponHudWidget, RenderDelta);

    // 3. Draw Loaded Rocket Indicators (The 3 dots)
    float Scale = GetCrosshairScale(WeaponHudWidget->UTHUDOwner);
    if ((CurrentFireMode == 1) && (NumLoadedRockets > 0))
    {
        float DotSize = 12.f * Scale;
        // Draw 1st Dot
        WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 0.f, ScaledPadding, DotSize, DotSize, 894.f, 38.f, 26.f, 26.f, 1.f, FLinearColor::White, FVector2D(0.5f, 0.5f));

        // Draw 2nd Dot
        if (NumLoadedRockets > 1)
        {
            WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, 50.f * Scale, ScaledPadding, DotSize, DotSize, 894.f, 38.f, 26.f, 26.f, 1.f, FLinearColor::White, FVector2D(0.5f, 0.5f));

            // Draw 3rd Dot
            if (NumLoadedRockets > 2)
            {
                WeaponHudWidget->DrawTexture(WeaponHudWidget->UTHUDOwner->HUDAtlas, -50.f * Scale, ScaledPadding, DotSize, DotSize, 894.f, 38.f, 26.f, 26.f, 1.f, FLinearColor::White, FVector2D(0.5f, 0.5f));
            }
        }
    }

    // 4. Draw Lock-On Reticles
    if (LockCrosshairTexture && WeaponHudWidget->GetCanvas())
    {
        float ResScaling = WeaponHudWidget->GetCanvas()->ClipX / 1920.f;
        float W = LockCrosshairTexture->GetSurfaceWidth();
        float H = LockCrosshairTexture->GetSurfaceHeight();
        float CrosshairRot = GetWorld()->TimeSeconds * 90.0f;
        const float AcquireDisplayTime = 0.6f;

        // A. Draw Locked Target (Solid Red)
        if (HasLockedTarget() && LockedTarget)
        {
            FVector ScreenTarget = WeaponHudWidget->GetCanvas()->Project(LockedTarget->GetActorLocation());

            // Adjust for canvas center offset if necessary, though Project usually returns absolute screen coordinates.
            // Stock UT offset logic:
            ScreenTarget.X -= WeaponHudWidget->GetCanvas()->SizeX * 0.5f;
            ScreenTarget.Y -= WeaponHudWidget->GetCanvas()->SizeY * 0.5f;

            WeaponHudWidget->DrawTexture(LockCrosshairTexture, ScreenTarget.X, ScreenTarget.Y, W * ResScaling, H * ResScaling, 0.f, 0.f, W, H, 1.f, FLinearColor::Red, FVector2D(0.5f, 0.5f), CrosshairRot);
        }
        // B. Draw Pending Target (Fading/Scaling White)
        else if (PendingLockedTarget && (GetWorld()->GetTimeSeconds() - PendingLockedTargetTime > LockAcquireTime - AcquireDisplayTime))
        {
            FVector ScreenTarget = WeaponHudWidget->GetCanvas()->Project(PendingLockedTarget->GetActorLocation());
            ScreenTarget.X -= WeaponHudWidget->GetCanvas()->SizeX * 0.5f;
            ScreenTarget.Y -= WeaponHudWidget->GetCanvas()->SizeY * 0.5f;

            float Opacity = (GetWorld()->GetTimeSeconds() - PendingLockedTargetTime - LockAcquireTime + AcquireDisplayTime) / AcquireDisplayTime;
            float PendingScale = 1.f + 5.f * (1.f - Opacity);

            WeaponHudWidget->DrawTexture(LockCrosshairTexture, ScreenTarget.X, ScreenTarget.Y, W * PendingScale * ResScaling, H * PendingScale * ResScaling, 0.f, 0.f, W, H, 0.2f + 0.5f * Opacity, FLinearColor::White, FVector2D(0.5f, 0.5f), 2.5f * CrosshairRot);
        }

        // C. Draw Seeking Rockets (Reticles following missiles)
        for (int32 i = 0; i < TrackingRockets.Num(); i++)
        {
            if (TrackingRockets[i] && TrackingRockets[i]->MasterProjectile)
            {
                TrackingRockets[i] = Cast<AUTProj_Rocket>(TrackingRockets[i]->MasterProjectile);
            }

            if ((TrackingRockets[i] == nullptr) || TrackingRockets[i]->bExploded || TrackingRockets[i]->IsPendingKillPending() || (TrackingRockets[i]->TargetActor == nullptr) || TrackingRockets[i]->TargetActor->IsPendingKillPending())
            {
                TrackingRockets.RemoveAt(i, 1);
                i--;
            }
            else
            {
                FVector ScreenTarget = WeaponHudWidget->GetCanvas()->Project(TrackingRockets[i]->TargetActor->GetActorLocation());
                ScreenTarget.X -= WeaponHudWidget->GetCanvas()->SizeX * 0.5f;
                ScreenTarget.Y -= WeaponHudWidget->GetCanvas()->SizeY * 0.5f;

                WeaponHudWidget->DrawTexture(LockCrosshairTexture, ScreenTarget.X, ScreenTarget.Y, 2.f * W * ResScaling, 2.f * H * ResScaling, 0.f, 0.f, W, H, 1.f, FLinearColor::Red, FVector2D(0.5f, 0.5f), CrosshairRot);
            }
        }
    }
}