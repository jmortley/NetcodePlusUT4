// TeamArenaCharacter.cpp
#include "TeamArenaCharacter.h"
#include "UTCharacterMovement.h"
#include "UTWeaponAttachment.h"
#include "UTWeaponFix.h"
#include "GameFramework/PlayerController.h"
#include "UTWorldSettings.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"

static TAutoConsoleVariable<int32> CVarEnableProjectilePrediction(
	TEXT("ut.EnableProjectilePrediction"),
	0, // Default: 1 (Enabled by default)
	TEXT("If 1, enables one-way latency visual prediction for non hitscan weapons.\n")
	TEXT("Players can set to 0 to opt-out (force server positions)."),
	ECVF_Default); // Saves to user config


ATeamArenaCharacter::ATeamArenaCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UTeamArenaCharacterMovement>(ACharacter::CharacterMovementComponentName))
{
    CachedPredictionPC = nullptr;
    bHasCachedPC = false;
    NetUpdateFrequency = 100.0f;
    MinNetUpdateFrequency = 100.0f;
    //MaxSavedPositionAge = 0.35f;
    //PositionSaveRate = 120.0f;
    //PositionSaveInterval = 1.0f / PositionSaveRate;
    //LastPositionSaveTime = 0.0f;
}

int32 ATeamArenaCharacter::GetNetcodeVersion()
{
	// Reads the #define NETCODE_PLUGIN_VERSION from NetcodePlus.h
	return NETCODE_PLUGIN_VERSION;
}



float ATeamArenaCharacter::GetClientVisualPredictionTime() const
{
	if (PlayerState && GetNetMode() == NM_Client)
	{
		// 1. Opt-Out Check
		if (CVarEnableProjectilePrediction.GetValueOnGameThread() == 0)
		{
			return 0.0f;
		}

		// 2. Weapon Logic - Hitscan weapons get 0 prediction
		AUTWeapon* MyWeapon = GetWeapon();
		if (MyWeapon)
		{
			// Hitscan weapons: No visual prediction (server authoritative)
			if (Cast<AUTPlusSniper>(MyWeapon) || Cast<AUTPlusShockRifle>(MyWeapon))
			{
				return 0.0f;
			}

			// Projectile weapons: Apply visual prediction
			float Fudge = 20.0f;
			float AdjustedPing = FMath::Max(0.0f, PlayerState->ExactPing - Fudge);
			float OneWayLatency = AdjustedPing * 0.0005f;
			return FMath::Min(OneWayLatency, 0.12f);
		}
	}

	return 0.0f;
}




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
	if (UTCharacterMovement)
	{
		// Cache the OLD velocity before we update it
		FVector OldVelocity = GetVelocity();

		UTCharacterMovement->SimulatedVelocity = NewVelocity;

		// If location changed or just spawned...
		if ((NewLocation != GetActorLocation()) || (CreationTime == GetWorld()->TimeSeconds))
		{
			// Standard geometry check
			if (GetWorld()->EncroachingBlockingGeometry(this, NewLocation, NewRotation))
			{
				bSimGravityDisabled = true;
			}
			else
			{
				bSimGravityDisabled = false;
			}

			// 1. Move Capsule to EXACT Server Location (The Anchor)
			SetActorLocationAndRotation(NewLocation, NewRotation, false);

			// 2. Prediction Logic
			if (GetCharacterMovement())
			{
				GetCharacterMovement()->bJustTeleported = true;

				float PredictionTime = 0.0f;

				// --- A. VELOCITY AGREEMENT CHECK (Fixes ADAD Spam) ---
				// Only predict if Client and Server roughly agree on direction.
				// If they are moving opposite directions (ADAD spam), DotProduct will be negative.
				// We disable prediction in that case to prevent "Overshoot Jitter."
				float VelocityDot = OldVelocity.GetSafeNormal() | NewVelocity.GetSafeNormal();
				bool bStableDirection = (VelocityDot > 0.0f); // True if moving roughly same direction

				if (bStableDirection && GetNetMode() != NM_DedicatedServer)
				{
					APlayerController* LocalPC = GEngine ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr;
					if (LocalPC && LocalPC->GetPawn())
					{
						ATeamArenaCharacter* ViewerChar = Cast<ATeamArenaCharacter>(LocalPC->GetPawn());
						if (ViewerChar)
						{
							PredictionTime = ViewerChar->GetClientVisualPredictionTime();
						}
					}
				}

				// --- B. RUN SIMULATION & TETHER ---
				if (PredictionTime > 0.0f)
				{
					// 1. Simulate Forward
					UTCharacterMovement->UTSimulateMovement(PredictionTime);

					// 2. THE TETHER (Clamp Max Distance)
					// Even with valid prediction, don't let the visual ghost get too far 
					// from the Server Reality. This stops "Warps".
					FVector PredictedLocation = GetActorLocation();
					FVector ErrorDelta = PredictedLocation - NewLocation; // Difference between Predict & Server
					float MaxDistance = 40.0f; // 40 units (Capsule Radius) is a safe limit

					if (ErrorDelta.SizeSquared() > MaxDistance * MaxDistance)
					{
						// Pull them back towards the server position
						FVector ClampedLocation = NewLocation + (ErrorDelta.GetSafeNormal() * MaxDistance);
						SetActorLocation(ClampedLocation);
					}
				}
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


FVector ATeamArenaCharacter::GetHeadLocation(float PredictionTime)
{
	if (PredictionTime <= 0.f)
	{
		if (GetMesh() && GetMesh()->DoesSocketExist(FName("head")))
		{
			return GetMesh()->GetSocketLocation(FName("head"));
		}
		return GetActorLocation() + FVector(0.f, 0.f, BaseEyeHeight);
	}

	// --- REWOUND HEAD ---
	// no longer need const_cast<ATeamArenaCharacter*>(this) because the function is not const!
	FVector RewoundBodyLoc = GetRewindLocation(PredictionTime);

	// Get current head offset from body center (captures lean, crouch, animation)
	FVector CurrentHeadWorld = GetActorLocation() + FVector(0.f, 0.f, BaseEyeHeight);
	if (GetMesh() && GetMesh()->DoesSocketExist(FName("head")))
	{
		CurrentHeadWorld = GetMesh()->GetSocketLocation(FName("head"));
	}

	// Calculate head offset relative to current body
	FVector HeadOffset = CurrentHeadWorld - GetActorLocation();

	// Apply that offset to rewound position
	return RewoundBodyLoc + HeadOffset;
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

		// SERVER: Turn on the Shell (Replicates to everyone)
		if (Role == ROLE_Authority && SpawnProtectionMaterial)
		{
			SetCharacterOverlayEffect(FOverlayEffect(SpawnProtectionMaterial), true);
		}

		// CLIENT: Handle Visuals & Team Filtering
		if (GetNetMode() != NM_DedicatedServer)
		{
			// 1. Always Hide the Default Skin Effect (Cyan Pulse)
			static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));
			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI) MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
			}

			// 2. Determine Visibility (Show Enemy Only)
			bool bShowShell = true;
			AUTPlayerController* LocalViewer = GetLocalViewer();
			AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

			// If viewing a Teammate (or Self), HIDE the shell.
			if (GS && LocalViewer && GS->OnSameTeam(this, LocalViewer))
			{
				bShowShell = false;
			}

			// 3. Apply Visuals to the Mesh
			if (OverlayMesh)
			{
				if (bShowShell)
				{
					// --- ENEMY: Show & Color ---
					if (OverlayMesh->bHiddenInGame)
					{
						OverlayMesh->SetHiddenInGame(false);
					}

					// Apply Color/Opacity to all materials
					static FName NAME_Color(TEXT("Color"));
					FLinearColor FinalColor = SpawnProtectionColor;
					FinalColor.A = SpawnProtectionOpacity;

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
				else
				{
					// --- TEAMMATE: Hide ---
					if (!OverlayMesh->bHiddenInGame)
					{
						OverlayMesh->SetHiddenInGame(true);
					}
				}
			}
		}
	}
	// --- CASE 2: Cleanup ---
	else if (bHasSpawnOverlay)
	{
		bHasSpawnOverlay = false;

		// SERVER: Turn off effect
		if (Role == ROLE_Authority && SpawnProtectionMaterial)
		{
			SetCharacterOverlayEffect(FOverlayEffect(SpawnProtectionMaterial), false);
			UpdateArmorOverlay(); // Restore standard armor visuals
		}

		// CLIENT: Instant Reset
		if (GetNetMode() != NM_DedicatedServer)
		{
			UpdateArmorOverlay();
		}
	}
}
