// TeamArenaCharacter.cpp
#include "TeamArenaCharacter.h"
#include "UTCharacterMovement.h"
#include "UTWeaponAttachment.h"
#include "UTWeaponFix.h"
#include "GameFramework/PlayerController.h"
#include "UTWorldSettings.h"


ATeamArenaCharacter::ATeamArenaCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UTeamArenaCharacterMovement>(ACharacter::CharacterMovementComponentName))
{
    CachedPredictionPC = nullptr;
    bHasCachedPC = false;
    NetUpdateFrequency = 100.0f;
    MinNetUpdateFrequency = 100.0f;
	//MinTimeBetweenClientAdjustments = 0.15f
    //MaxSavedPositionAge = 0.35f;
    //PositionSaveRate = 120.0f;
    //PositionSaveInterval = 1.0f / PositionSaveRate;
    //LastPositionSaveTime = 0.0f;
}




float ATeamArenaCharacter::GetClientVisualPredictionTime() const
{
    return 0.0f;
}

/*
void ATeamArenaCharacter::PositionUpdated(bool bShotSpawned)
{
    // --- HIGH-FPS FIX: Throttle SavedPositions to 120Hz ---
    // This reduces array size and RemoveAt(0) frequency by 4x at 480 FPS

    const float WorldTime = GetWorld()->GetTimeSeconds();

    // ALWAYS save positions when shots are fired (required for hit validation)
    // Otherwise, throttle to PositionSaveRate Hz
    if (!bShotSpawned)
    {
        if ((WorldTime - LastPositionSaveTime) < PositionSaveInterval)
        {
            return;  // Skip this frame, not enough time elapsed
        }
    }

    LastPositionSaveTime = WorldTime;

    // --- Original Epic logic below ---
    if (GetCharacterMovement())
    {
        new(SavedPositions) FSavedPosition(
            GetActorLocation(),
            GetViewRotation(),
            GetCharacterMovement()->Velocity,
            GetCharacterMovement()->bJustTeleported,
            bShotSpawned,
            WorldTime,
            (UTCharacterMovement ? UTCharacterMovement->GetCurrentSynchTime() : 0.f)
        );
    }

    // Maintain one position beyond MaxSavedPositionAge for interpolation
    if (SavedPositions.Num() > 1 && SavedPositions[1].Time < WorldTime - MaxSavedPositionAge)
    {
        SavedPositions.RemoveAt(0);
    }
}
*/

bool ATeamArenaCharacter::IsHeadShot(FVector HitLocation, FVector ShotDirection, float WeaponHeadScaling,
	AUTCharacter* ShotInstigator, float PredictionTime)
{
	// Team check (same as Epic)
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (ShotInstigator && GS && GS->OnSameTeam(this, ShotInstigator))
	{
		return false;
	}

	// CRITICAL FIX: Pass PredictionTime to GetHeadLocation!
	// Epic's bug: FVector HeadLocation = GetHeadLocation(); // Never passed PredictionTime!
	FVector HeadLocation = GetHeadLocation(PredictionTime);

	bool bHeadShot = FMath::PointDistToLine(HeadLocation, ShotDirection, HitLocation)
		< HeadRadius * HeadScale * WeaponHeadScaling;

#if ENABLE_DRAW_DEBUG
	static IConsoleVariable* CVarDebugHeadshots = IConsoleManager::Get().FindConsoleVariable(TEXT("ut.DebugHeadshots"));
	if (CVarDebugHeadshots && CVarDebugHeadshots->GetInt() != 0)
	{
		DrawDebugLine(GetWorld(), HitLocation + (ShotDirection * 1000.f),
			HitLocation - (ShotDirection * 1000.f), FColor::White, true);

		if (bHeadShot)
		{
			DrawDebugSphere(GetWorld(), HeadLocation, HeadRadius * HeadScale * WeaponHeadScaling,
				10, FColor::Green, true);
		}
		else
		{
			DrawDebugSphere(GetWorld(), HeadLocation, HeadRadius * HeadScale * WeaponHeadScaling,
				10, FColor::Red, true);
		}
	}
#endif

	return bHeadShot;
}


void ATeamArenaCharacter::UTUpdateSimulatedPosition(const FVector& NewLocation, const FRotator& NewRotation, const FVector& NewVelocity)
{
    // 1. Update Velocity (Standard UT logic)
    if (UTCharacterMovement)
    {
        UTCharacterMovement->SimulatedVelocity = NewVelocity;

        // 2. Update Location (The "Correction" Logic)
        // If the location has changed, or we just spawned...
        if ((NewLocation != GetActorLocation()) || (CreationTime == GetWorld()->TimeSeconds))
        {
            // Standard check to disable gravity if we are stuck in geometry
            if (GetWorld()->EncroachingBlockingGeometry(this, NewLocation, NewRotation))
            {
                bSimGravityDisabled = true;
            }
            else
            {
                bSimGravityDisabled = false;
            }

            // --- FORCE PREDICT 0 LOGIC STARTS HERE ---

            // A. Move the Capsule to the EXACT Server Location.
            // This sets the "Target" for the interpolation smoothing.
            SetActorLocationAndRotation(NewLocation, NewRotation, false);

            // B. Notify Movement Component
            if (GetCharacterMovement())
            {
                GetCharacterMovement()->bJustTeleported = true;
				//UTSimulateMovement
                // --- CRITICAL CHANGE ---
                // The base game (AUTCharacter.cpp) fetches the PC here and calls 
                // UTSimulateMovement(PredictionTime) to forecast ahead.
                //
                // WE DO NOTHING HERE.
                //
                // By deleting the UTSimulateMovement call, we force the capsule 
                // to stay exactly where the server said it was (NewLocation).
                // 
                // The Engine's "SmoothCorrection" (triggered by Super::OnRep) 
                // will now take over and smoothly slide the visual mesh 
                // from the Old Location to this New Location.
            }
        }
        else if (NewRotation != GetActorRotation())
        {
            GetRootComponent()->MoveComponent(FVector::ZeroVector, NewRotation, false);
        }
    }
}


void ATeamArenaCharacter::FiringInfoUpdated()
{
    // 1. Interrupt Animation (Standard)
    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance != NULL)
    {
        AnimInstance->Montage_Stop(0.2f);
    }

    //if (Weapon && Weapon->ZoomState != EZoomState::EZS_NotZoomed)
    //{
        // Prevent any FiringInfoUpdated visual calls while zoomed
    //    return;
    //}

    if (IsLocallyControlled() && !bLocalFlashLoc)
    {
        // If this is our custom weapon, we know we handled visuals locally in FireShot.
        // We suppress the server echo to prevent Double Tracers.
        if (Weapon && Weapon->IsA(AUTWeaponFix::StaticClass()))
        {
            return;
        }

        // If this is a Stock Weapon (Sniper, etc), it DOES NOT predict locally.
        // It relies on this server echo to draw the beam. We let it pass.
    }


    // 2. Safe Getters
    AUTPlayerController* UTPC = GetLocalViewer();

    // Returns 0.0f in your override, so this is safe/dependency-free
    float MyPredictionTime = GetClientVisualPredictionTime();

    // --- FIX START: NULL-SAFE LOGIC ---
    bool bShouldPlayServerEffect = true;


    if (UTPC != nullptr && !bLocalFlashLoc && MyPredictionTime > 0.f)
    {
        bShouldPlayServerEffect = false;
    }

    // 4. Play Effects if Allowed
    if (bShouldPlayServerEffect && Weapon != NULL && Weapon->ShouldPlay1PVisuals())
    {
        if (!FlashLocation.Position.IsZero())
        {
            uint8 EffectFiringMode = Weapon->GetCurrentFireMode();

            // Spectator Logic (Replication)
            if (Controller == NULL)
            {
                EffectFiringMode = FireMode;
                Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
                Weapon->FiringEffectsUpdated(FireMode, FlashLocation.Position);
            }
            // Other Player Logic (Local View)
            else
            {
                FVector SpawnLocation;
                FRotator SpawnRotation;
                Weapon->GetImpactSpawnPosition(FlashLocation.Position, SpawnLocation, SpawnRotation);
                Weapon->PlayImpactEffects(FlashLocation.Position, EffectFiringMode, SpawnLocation, SpawnRotation);
            }
        }
        else if (Controller == NULL)
        {
            Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
        }

        if (FlashCount == 0 && FlashLocation.Position.IsZero() && WeaponAttachment != NULL)
        {
            WeaponAttachment->StopFiringEffects();
        }
    }
    // 4. Standard Third Person Effects (Always Safe)
    // 4. Play 3P Effects (Enemies / Other Players)
    else if (WeaponAttachment != NULL)
    {
        if (FlashCount != 0 || !FlashLocation.Position.IsZero())
        {
            // A. Always run standard logic (Audio, Muzzle Flash, etc.)
            WeaponAttachment->PlayFiringEffects();

            // B. ALWAYS FORCE BEAM VISIBILITY
            // We removed the 'bEngineHandledIt' check. We now force this 100% of the time.
            // This ensures hits on yourself (close range) are drawn even if the engine 
            // glitches out on the Enemy mesh visibility.

            if (!FlashLocation.Position.IsZero() &&
                WeaponAttachment->FireEffect.IsValidIndex(FireMode) &&
                WeaponAttachment->FireEffect[FireMode] != nullptr)
            {
                FVector SpawnLocation = GetActorLocation();

                // Calculate Start Location from Attachment
                if (WeaponAttachment->MuzzleFlash.IsValidIndex(FireMode) && WeaponAttachment->MuzzleFlash[FireMode] != nullptr)
                {
                    SpawnLocation = WeaponAttachment->MuzzleFlash[FireMode]->GetComponentLocation();
                }
                else if (WeaponAttachment->Mesh != nullptr)
                {
                    SpawnLocation = WeaponAttachment->Mesh->GetSocketLocation(WeaponAttachment->AttachSocket);
                }

                // Force Spawn the Beam
                UParticleSystemComponent* PSC = UGameplayStatics::SpawnEmitterAtLocation(
                    GetWorld(),
                    WeaponAttachment->FireEffect[FireMode],
                    SpawnLocation,
                    (FlashLocation.Position - SpawnLocation).Rotation(),
                    true
                );

                if (PSC)
                {
                    static FName NAME_HitLocation(TEXT("HitLocation"));
                    static FName NAME_LocalHitLocation(TEXT("LocalHitLocation"));

                    PSC->SetVectorParameter(NAME_HitLocation, FlashLocation.Position);
                    PSC->SetVectorParameter(NAME_LocalHitLocation, PSC->ComponentToWorld.InverseTransformPosition(FlashLocation.Position));

                    // CRITICAL: Ensure visual parameters (Colors, Lightning Arcs) are applied
                    WeaponAttachment->ModifyFireEffect(PSC);
                }
            }
        }
        else
        {
            WeaponAttachment->StopFiringEffects();
        }
    }

    K2_FiringInfoUpdated();
}


/*
void ATeamArenaCharacter::FiringInfoUpdated()
{
    // 1. Interrupt Animation (Standard UT behavior)
    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance != NULL)
    {
        AnimInstance->Montage_Stop(0.2f);
    }

    AUTPlayerController* UTPC = GetLocalViewer();

    // --- FIX START ---
    // Instead of UTPC->GetPredictionTime(), we use my custom getter.
    // Since you return 0.0f, MyPredictionTime will be 0.
    float MyPredictionTime = GetClientVisualPredictionTime();

    // The Logic: 
    // If MyPredictionTime == 0.f, the condition becomes TRUE.
    // This BYPASSES the suppression logic that normally hides server packets when prediction is on.
    if ((bLocalFlashLoc || UTPC == NULL || MyPredictionTime == 0.f || !IsLocallyControlled()) && Weapon != NULL && Weapon->ShouldPlay1PVisuals())
    {
        if (!FlashLocation.Position.IsZero())
        {
            uint8 EffectFiringMode = Weapon->GetCurrentFireMode();

            // If non-local first person spectator, use replicated FireMode
            if (Controller == NULL)
            {
                EffectFiringMode = FireMode;
                Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
                Weapon->FiringEffectsUpdated(FireMode, FlashLocation.Position);
            }
            else
            {
                FVector SpawnLocation;
                FRotator SpawnRotation;
                Weapon->GetImpactSpawnPosition(FlashLocation.Position, SpawnLocation, SpawnRotation);
                Weapon->PlayImpactEffects(FlashLocation.Position, EffectFiringMode, SpawnLocation, SpawnRotation);
            }
        }
        else if (Controller == NULL)
        {
            Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
        }

        if (FlashCount == 0 && FlashLocation.Position.IsZero() && WeaponAttachment != NULL)
        {
            WeaponAttachment->StopFiringEffects();
        }
    }
    // --- FIX END ---
    else if (WeaponAttachment != NULL)
    {
        if (FlashCount != 0 || !FlashLocation.Position.IsZero())
        {
            // Standard Third Person Logic (Other players)
            if ((!IsLocallyControlled() || UTPC == NULL || UTPC->IsBehindView()))
            {
                WeaponAttachment->PlayFiringEffects();
            }
        }
        else
        {
            // Always call Stop to avoid effects mismatches
            WeaponAttachment->StopFiringEffects();
        }
    }

    K2_FiringInfoUpdated();
}
*/

FVector ATeamArenaCharacter::GetRewindLocation(float PredictionTime, AUTPlayerController* DebugViewer)
{
    float ActualPredictionTime = PredictionTime;

    // --- CRITICAL FIX ---
    // CLIENT: Force 0ms. 
    // We are viewing a replicated transaction. We want the tracer to align 
    // with the mesh exactly as it is currently rendered on screen.
    if (GetNetMode() == NM_Client)
    {
        ActualPredictionTime = GetClientVisualPredictionTime(); // Returns 0.0f
    }
    // SERVER: Keep 'PredictionTime'. 
    // The server passes in the exact rewind amount needed for lag compensation 
    // (calculated in UTWeaponFix::HitScanTrace). If we zero this out, 
    // we break hit registration.

    // --- STANDARD UT LOGIC (Adapted) ---
    FVector TargetLocation = GetActorLocation();
    FVector PrePosition = GetActorLocation();
    FVector PostPosition = GetActorLocation();

    // Use the calculated time based on the logic above
    float TargetTime = GetWorld()->GetTimeSeconds() - ActualPredictionTime;
    float Percent = 0.999f;
    bool bTeleported = false;

    if (ActualPredictionTime > 0.f)
    {
        for (int32 i = SavedPositions.Num() - 1; i >= 0; i--)
        {
            TargetLocation = SavedPositions[i].Position;
            if (SavedPositions[i].Time < TargetTime)
            {
                if (!SavedPositions[i].bTeleported && (i < SavedPositions.Num() - 1))
                {
                    PrePosition = SavedPositions[i].Position;
                    PostPosition = SavedPositions[i + 1].Position;
                    if (SavedPositions[i + 1].Time == SavedPositions[i].Time)
                    {
                        Percent = 1.f;
                        TargetLocation = SavedPositions[i + 1].Position;
                    }
                    else
                    {
                        Percent = (TargetTime - SavedPositions[i].Time) / (SavedPositions[i + 1].Time - SavedPositions[i].Time);
                        TargetLocation = SavedPositions[i].Position + Percent * (SavedPositions[i + 1].Position - SavedPositions[i].Position);
                    }
                }
                else
                {
                    bTeleported = SavedPositions[i].bTeleported;
                }
                break;
            }
        }
    }

    if (DebugViewer)
    {
        DebugViewer->ClientDebugRewind(GetActorLocation(), TargetLocation, PrePosition, PostPosition, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), ActualPredictionTime, Percent, bTeleported);
    }

    return TargetLocation;
}


void ATeamArenaCharacter::BeginPlay()
{
	Super::BeginPlay();

	// SERVER ONLY: Register our custom material so SetCharacterOverlayEffect works without warnings
	if (Role == ROLE_Authority && SpawnProtectionMaterial)
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS)
		{
			// This adds your material to the replicated list.
			// The warnings will stop immediately.
			GS->AddOverlayMaterial(SpawnProtectionMaterial, nullptr);
		}
	}
}


void ATeamArenaCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// --- CASE 1: Active Spawn Protection ---
	if (bSpawnProtectionEligible)
	{
		bHasSpawnOverlay = true;

		// SERVER: Turn on the Shell (Now warning-free!)
		if (Role == ROLE_Authority && SpawnProtectionMaterial)
		{
			SetCharacterOverlayEffect(FOverlayEffect(SpawnProtectionMaterial), true);
			// ENABLE OUTLINE
			// Param 1: bNowOutlined = true (Turn it on)
			// Param 2: bWhenUnoccluded = false (Do NOT draw through walls)
			// Param 3: TeamMask = 255 (Show for everyone)
			//SetOutlineServer(true, false, 255);
		}

		// CLIENT: Tweak Visuals (Hide Skin + Set Opacity)
		if (GetNetMode() != NM_DedicatedServer)
		{
			// 1. Hide the Cyan Skin
			static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));
			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI) MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
			}

			// 2. Apply Custom Color/Opacity to the Mesh the System Created
			// Note: We don't create the mesh. We just tweak the one SetCharacterOverlayEffect made.
			if (OverlayMesh && OverlayMesh->IsVisible())
			{
				static FName NAME_Color(TEXT("Color"));
				FLinearColor FinalColor = SpawnProtectionColor;
				FinalColor.A = SpawnProtectionOpacity;

				// Loop needed if your mesh has multiple material slots
				const int32 NumMaterials = OverlayMesh->GetNumMaterials();
				for (int32 i = 0; i < NumMaterials; i++)
				{
					UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(OverlayMesh->GetMaterial(i));
					if (MID)
					{
						MID->SetVectorParameterValue(NAME_Color, FinalColor);
					}
				}
			}
		}
	}
	// --- CASE 2: Protection Just Ended (Cleanup) ---
	else if (bHasSpawnOverlay)
	{
		bHasSpawnOverlay = false;

		// SERVER: Turn off the effect
		if (Role == ROLE_Authority && SpawnProtectionMaterial)
		{
			SetCharacterOverlayEffect(FOverlayEffect(SpawnProtectionMaterial), false);
			//SetOutlineServer(false, false, 255);
			UpdateArmorOverlay(); // Restore standard armor visuals
		}

		// CLIENT: Instant Reset
		if (GetNetMode() != NM_DedicatedServer)
		{
			UpdateArmorOverlay();
		}
	}
}
