// TeamArenaCharacter.cpp
#include "TeamArenaCharacter.h"
#include "UTCharacterMovement.h"
#include "UTWeaponAttachment.h"
#include "UTWeaponFix.h"
#include "GameFramework/PlayerController.h"
#include "UTWorldSettings.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTGameState.h"
#include "UTWeap_LinkGun.h"

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
	NetPriority = 10.0f;
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
			return FMath::Min(OneWayLatency, 0.10f);
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
		float NoSmoothThreshold = UTCharacterMovement->NetworkNoSmoothUpdateDistance;
		float SmoothThreshold = UTCharacterMovement->NetworkMaxSmoothUpdateDistance;
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

				if (GetNetMode() != NM_DedicatedServer)
				{
					APlayerController* LocalPC = GEngine ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr;
					if (LocalPC && LocalPC->GetPawn())
					{
						ATeamArenaCharacter* ViewerChar = Cast<ATeamArenaCharacter>(LocalPC->GetPawn());
						if (ViewerChar)
						{
							float BasePrediction = ViewerChar->GetClientVisualPredictionTime();

							if (BasePrediction > 0.0f)
							{
								// --- ADAPTIVE SCALING (replaces binary bStableDirection) ---
								float VelocityDot = OldVelocity.GetSafeNormal() | NewVelocity.GetSafeNormal();

								// Map dot [-1, 1] -> scale [0, 1]
								// -1 (opposite dir) = 0% prediction
								//  0 (perpendicular) = ~50% prediction
								// +1 (same dir)      = 100% prediction
								float StabilityFactor = FMath::Clamp((VelocityDot + 1.0f) * 0.5f, 0.0f, 1.0f);

								// Smooth the factor itself to prevent frame-to-frame flickering
								// Rate 6.0 = converges in roughly 150-200ms
								SmoothedStabilityFactor = FMath::FInterpTo(
									SmoothedStabilityFactor, StabilityFactor,
									GetWorld()->GetDeltaSeconds(), 6.0f);

								PredictionTime = BasePrediction * SmoothedStabilityFactor;
							}
						}
					}
				}

				// --- RUN SIMULATION & TETHER ---
				if (PredictionTime > 0.005f)
				{
					// 1. Simulate Forward
					UTCharacterMovement->UTSimulateMovement(PredictionTime);

					// 2. Tether (unchanged)
					FVector PredictedLocation = GetActorLocation();
					FVector ErrorDelta = PredictedLocation - NewLocation;
					float Speed = NewVelocity.Size();
					float MaxDistance = FMath::Clamp(Speed * PredictionTime, 40.0f, NoSmoothThreshold);

					if (ErrorDelta.SizeSquared() > MaxDistance * MaxDistance)
					{
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

	//bool bIsSpectating = (UTPC != nullptr && UTPC->UTPlayerState && UTPC->UTPlayerState->bIsSpectator);

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

/*
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
*/

FVector ATeamArenaCharacter::GetHeadLocation(float PredictionTime)
{
	// Force mesh update if necessary (from Epic's implementation)
	if (GetMesh()->IsRegistered() && GetMesh()->MeshComponentUpdateFlag > EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones && !GetMesh()->bRecentlyRendered)
	{
		if (GetMesh()->MeshComponentUpdateFlag > EMeshComponentUpdateFlag::AlwaysTickPose)
		{
			const float Step = 0.1f;
			for (float TickTime = FMath::Min<float>(GetWorld()->TimeSeconds - GetMesh()->LastRenderTime, 1.0f); TickTime > 0.0f; TickTime -= Step)
			{
				GetMesh()->TickAnimation(FMath::Min<float>(TickTime, Step), false);
			}
		}
		GetMesh()->AnimUpdateRateParams->bSkipEvaluation = false;
		GetMesh()->AnimUpdateRateParams->bInterpolateSkippedFrames = false;
		GetMesh()->RefreshBoneTransforms();
		GetMesh()->UpdateComponentToWorld();
	}

	if (PredictionTime <= 0.f)
	{
		// Current head position: socket + HeadHeight offset
		return GetMesh()->GetSocketLocation(HeadBone) + FVector(0.f, 0.f, HeadHeight);
	}

	// --- REWOUND HEAD ---
	FVector RewoundBodyLoc = GetRewindLocation(PredictionTime);

	// Get current head world position (with HeadHeight!)
	FVector CurrentHeadWorld = GetMesh()->GetSocketLocation(HeadBone) + FVector(0.f, 0.f, HeadHeight);

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
	// --- Per-weapon hide: detect weapon switch and apply hide state ---
	// Works for ALL weapons (stock and NetcodePlus) since TeamArenaCharacter
	// is the character class for all players.
	// UPROPERTY ensures LastEquippedWeapon is nulled by GC on weapon destroy (death/drop).
	if (GetNetMode() != NM_DedicatedServer && IsLocallyControlled())
	{
		AUTWeapon* CurrentWeapon = GetWeapon();
		// Only check when weapon actually changes — the pointer comparison
		// is near-zero cost and skips 99.9% of frames instantly.
		if (CurrentWeapon && CurrentWeapon != LastEquippedWeapon
			&& !CurrentWeapon->IsPendingKillPending()
			&& CurrentWeapon->GetMesh() && CurrentWeapon->GetMesh()->IsRegistered())
		{
			LastEquippedWeapon = CurrentWeapon;
			// Check hide by class name (allows Lightning Gun and Sniper to hide independently)
			FName HideKey = FName(*CurrentWeapon->GetClass()->GetName());
			bool bShouldHide = false;
			{
				bool* bHidden = AUTWeaponFix::HiddenWeaponsByTag.Find(HideKey);
				bShouldHide = bHidden && *bHidden;
			}

			if (bShouldHide)
			{
				CurrentWeapon->GetMesh()->SetHiddenInGame(true);
				if (FirstPersonMesh)
				{
					FirstPersonMesh->SetHiddenInGame(true);
				}
			}
			else
			{
				CurrentWeapon->GetMesh()->SetHiddenInGame(false);
				if (FirstPersonMesh)
				{
					FirstPersonMesh->SetHiddenInGame(false);
				}
			}
		}
		// Clear stale reference if weapon was removed (death, drop)
		if (LastEquippedWeapon && (!CurrentWeapon || CurrentWeapon->IsPendingKillPending()))
		{
			LastEquippedWeapon = nullptr;
		}
	}

	// --- PERF: Skip base class spawn protection material loop ---
	// Base AUTCharacter::Tick (line 4698-4734) loops BodyMIs setting SpawnProtectionPct
	// every frame. We handle spawn protection ourselves, so skip the base class work
	// by temporarily clearing the flag. Saves ~27K material calls/sec.
	bool bSavedSpawnProtectionEligible = bSpawnProtectionEligible;
	if (bSpawnProtectionEligible && GetNetMode() != NM_DedicatedServer)
	{
		bSpawnProtectionEligible = false;
	}

	// --- PERF: Throttle OverlayMesh->MarkRenderStateDirty() ---
	// Base AUTCharacter::Tick (line 4743) calls this EVERY FRAME as a workaround for
	// an engine bug. We throttle to every 8th frame (60Hz at 480fps).
	USkeletalMeshComponent* SavedOverlayMesh = nullptr;
	if (OverlayMesh && OverlayMesh->IsRegistered() && GetNetMode() != NM_DedicatedServer)
	{
		OverlayDirtyFrameCounter++;
		if (OverlayDirtyFrameCounter < 8)
		{
			// Hide OverlayMesh from Super::Tick so it won't MarkRenderStateDirty
			SavedOverlayMesh = OverlayMesh;
			OverlayMesh = nullptr;
		}
		else
		{
			OverlayDirtyFrameCounter = 0;
			// Let Super::Tick handle it this frame (every 8th)
		}
	}

	Super::Tick(DeltaTime);

	// Restore OverlayMesh if we hid it
	if (SavedOverlayMesh)
	{
		OverlayMesh = SavedOverlayMesh;
	}

	// Restore spawn protection flag
	bSpawnProtectionEligible = bSavedSpawnProtectionEligible;

	// Visuals are for clients only
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// --- CASE 1: Active Spawn Protection ---
	if (bSpawnProtectionEligible)
	{
		bHasSpawnOverlay = true;

		// --- TEAM FILTERING ---
		bool bShowGlowToViewer = true;
		AUTPlayerController* LocalViewer = GetLocalViewer();
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

		if (GS && LocalViewer && GS->OnSameTeam(this, LocalViewer))
		{
			bShowGlowToViewer = false;
		}

		// --- PERF: Dirty flag — only update materials when state changes ---
		// Values are constant within each state (glow on vs glow off).
		// bLastShowGlowState initialized to 0xFF to force first-frame apply.
		uint8 CurrentState = bShowGlowToViewer ? 1 : 0;
		if (CurrentState == bLastShowGlowState)
		{
			return; // No state change, skip all material work
		}
		bLastShowGlowState = CurrentState;

		static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));
		static FName NAME_HitFlashColor(TEXT("HitFlashColor"));
		static FName NAME_FullBodyFlashPct(TEXT("FullBodyFlashPct"));

		if (bShowGlowToViewer)
		{
			FLinearColor BaseTeamColor = GetTeamColor();
			float BrightnessMult = 20.0f;
			FLinearColor ObviousColor = FLinearColor(
				BaseTeamColor.R * BrightnessMult,
				BaseTeamColor.G * BrightnessMult,
				BaseTeamColor.B * BrightnessMult,
				1.0f
			);

			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI)
				{
					MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
					MI->SetVectorParameterValue(NAME_HitFlashColor, ObviousColor);
					MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.4f);
				}
			}
		}
		else
		{
			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI)
				{
					MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
					MI->SetVectorParameterValue(NAME_HitFlashColor, FLinearColor(0.f, 0.f, 0.f, 0.f));
					MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.0f);
				}
			}
		}
	}
	// --- CASE 2: Cleanup (Runs once when protection ends) ---
	else if (bHasSpawnOverlay)
	{
		bHasSpawnOverlay = false;
		bLastShowGlowState = 0xFF; // Reset dirty flag for next spawn

		static FName NAME_HitFlashColor(TEXT("HitFlashColor"));
		static FName NAME_FullBodyFlashPct(TEXT("FullBodyFlashPct"));
		static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));

		for (UMaterialInstanceDynamic* MI : BodyMIs)
		{
			if (MI)
			{
				MI->SetVectorParameterValue(NAME_HitFlashColor, FLinearColor(0.f, 0.f, 0.f, 0.f));
				MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.0f);
				MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
			}
		}
	}
}


void ATeamArenaCharacter::BecomeViewTarget(APlayerController* PC)
{
	Super::BecomeViewTarget(PC);


	AUTWeap_LinkGun* LinkGun = Cast<AUTWeap_LinkGun>(Weapon);
	if (LinkGun)
	{
		LinkGun->OverheatFactor = 0.0f;
	}

	// Clear any stuck audio on the pawn
	SetAmbientSound(nullptr);
}