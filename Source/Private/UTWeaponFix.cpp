
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"
#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTWeaponStateZooming.h"


DEFINE_LOG_CATEGORY_STATIC(LogUTWeaponFix, Log, All);


static TAutoConsoleVariable<int32> CVarProjectileTickRate(
    TEXT("ut.ProjectileTickRate"),
    240,
    TEXT("Client-side projectile simulation rate in Hz.\n")
    TEXT("Snapped to nearest multiple of 60. Range: 60-480.\n")
    TEXT("Server always uses native 120Hz tick."),
    ECVF_Scalability
);

// Helper function (can be static or inline)
static int32 GetSnappedProjectileHz()
{
    int32 TargetHz = CVarProjectileTickRate.GetValueOnGameThread();

    // Clamp range
    TargetHz = FMath::Clamp(TargetHz, 60, 480);

    // Snap to nearest multiple of 60
    TargetHz = FMath::RoundToInt(TargetHz / 60.f) * 60;

    return TargetHz;
}





//extern FCollisionResponseParams WorldResponseParams;

AUTWeaponFix::AUTWeaponFix(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Initialize arrays for standard two fire modes
    AuthoritativeFireEventIndex.SetNum(2);
    ClientFireEventIndex.SetNum(2);
    LastFireTime.SetNum(2);
    FireModeActiveState.SetNum(2);
    bIsTransactionalFire = false;
    bHandlingRetry = false;
    HitScanPadding = 30.f;
    HitScanPaddingStationary = 10.0f;
	FudgeFactorMs = 20;
	ProjectilePredictionCapMs = 120.0f;

    for (int32 i = 0; i < 2; i++)
    {
        AuthoritativeFireEventIndex[i] = 0;
        ClientFireEventIndex[i] = 0;
        LastFireTime[i] = -1.0f;
        FireModeActiveState[i] = 0;
    }

    CurrentlyFiringMode = 255; // No mode currently firing
}



void AUTWeaponFix::PostInitProperties()
{
    Super::PostInitProperties();
    /*
    // SWAP THE STATES
    // Replace standard Firing States with our Transactional State.
    // We do this in PostInit to override Blueprint defaults safely.
    if (FiringState.Num() > 0)
    {
        for (int32 i = 0; i < FiringState.Num(); i++)
        {
            // Construct the new state object
            UUTWeaponStateFiring_Transactional* NewState = NewObject<UUTWeaponStateFiring_Transactional>(this, UUTWeaponStateFiring_Transactional::StaticClass());
            if (NewState)
            {
                FiringState[i] = NewState;
            }
        }
    }
    */
}


void AUTWeaponFix::BeginPlay()
{
    Super::BeginPlay();

    // Clear any residual state
    CurrentlyFiringMode = 255;
    for (int32 i = 0; i < FireModeActiveState.Num(); i++)
    {
        FireModeActiveState[i] = 0;
    }
}

void AUTWeaponFix::OnRetryTimer(uint8 FireModeNum)
{
    
    bHandlingRetry = true;
    //UE_LOG(LogUTWeaponFix, Log, TEXT("[Timer] Retry Timer Firing! Calling StartFire..."));
    StartFire(FireModeNum);
    bHandlingRetry = false;
}









void AUTWeaponFix::StartFire(uint8 FireModeNum)
{
    // ---------------------------------------------------------
    // ZOOM BYPASS (MUST BE FIRST)
    // ---------------------------------------------------------
    // STOCK CODE CONFIRMATION: UTWeaponStateZooming.cpp shows that Zooming
    // does not fire a shot (BeginFiringSequence returns false).
    // Therefore, it should NOT be gated by the weapon's Refire Time.
    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        // Check 1: Is it a child of the Zooming Class?
        // Check 2: Does the name contain "Zoom"? (Safety for BPs)
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateZooming::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Zoom")))
        {
            // Hand off to standard UT Zoom logic immediately
            Super::StartFire(FireModeNum);
            return;
        }
    }

	if (GetCurrentState() &&
		(GetCurrentState()->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
			GetCurrentState()->GetName().Contains(TEXT("Charged"))))
	{
		if (FireModeNum != CurrentFireMode)
		{
			UUTWeaponStateFiringChargedRocket_Transactional* TransState =
				Cast<UUTWeaponStateFiringChargedRocket_Transactional>(GetCurrentState());

			if (TransState && !TransState->bCharging)
			{
				// BURSTING: Buffer M1 input for after burst completes
				if (UTOwner)
				{
					UTOwner->SetPendingFire(FireModeNum, true);
				}
				return;
			}

			// LOADING: Cycle rocket mode
			if (FireModeNum < 2)
			{
				GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
			}
			if (UTOwner)
			{
				UTOwner->SetPendingFire(FireModeNum, false);
			}
			OnMultiPress(FireModeNum);
			return;
		}

		// Same mode - register intent so RefireCheckTimer sees it
		if (UTOwner)
		{
			UTOwner->SetPendingFire(FireModeNum, true);
		}
		return;
	}

	// If the weapon is in Active (Idle) state, it cannot possibly be firing.
	// Any "CurrentlyFiring" flags here are bugs from the Auto-Fire/GraceTimer path.
	// We clear them immediately so they don't block your new input.
	if (GetCurrentState() == ActiveState)
	{
		CurrentlyFiringMode = 255;
		if (FireModeActiveState.IsValidIndex(0)) FireModeActiveState[0] = 0;
		if (FireModeActiveState.IsValidIndex(1)) FireModeActiveState[1] = 0;
	}
    
    // ---------------------------------------------------------
    // 1. SAFETY CHECKS
    // ---------------------------------------------------------
    if (UTOwner && UTOwner->IsFiringDisabled())
    {
        return;
    }

    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
    if (GS && GS->PreventWeaponFire())
    {
        return;
    }

	bool bIsSwitching = (CurrentState == UnequippingState) || (UTOwner && UTOwner->GetPendingWeapon());

	if (bIsSwitching)
	{
		if (UTOwner)
		{
			UTOwner->SetPendingFire(FireModeNum, true);
			UE_LOG(LogUTWeaponFix, Verbose, TEXT("Setting pending fire on swap %d"), FireModeNum);
		}
		return;
	}
    // ---------------------------------------------------------
    // 2. COOLDOWN VALIDATION (MOVED TO TOP)
    // ---------------------------------------------------------
    // We check this FIRST to prevent any "Bypass" logic (like Charged States)
    // from entering a new firing sequence illegally.

    float CurrentTime = GetWorld()->GetTimeSeconds();

    bool bIsSwitchingModes = false;

    // Are we currently in a Charged State?
    if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
        CurrentState->GetName().Contains(TEXT("Charged"))))
    {
        // If we are charging Mode 1, and the player pressed Mode 0, that is a Switch.
        if (FireModeNum != CurrentFireMode)
        {
            bIsSwitchingModes = true;
        }
    }


    if (!bIsSwitchingModes &&  IsFireModeOnCooldown(FireModeNum, CurrentTime))
    {
        // If we are already in the firing state for this mode, 
        // we don't need to do anything (just let the state run).
        if (GetCurrentState() == FiringState[FireModeNum])
        {
			if (UTOwner &&
				(FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
					FiringState[FireModeNum]->GetName().Contains(TEXT("Charged"))))
			{
				UTOwner->SetPendingFire(FireModeNum, true);
			}


			if (FireModeNum < 2)
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            }
            return;
        }

        // RETRY LOGIC (Smart Wait for Local Client)
        if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
        {
            // Find when the cooldown actually ends
            float MaxReadyTime = 0.f;
            for (int32 i = 0; i < LastFireTime.Num(); i++)
            {
                if (LastFireTime[i] > 0.0f)
                {
                    float ModeReadyTime = LastFireTime[i] + GetRefireTime(i);
                    if (ModeReadyTime > MaxReadyTime)
                    {
                        MaxReadyTime = ModeReadyTime;
                    }
                }
            }
			if (EarliestFireTime > MaxReadyTime)
			{
				MaxReadyTime = EarliestFireTime;
			}
            float Delay = MaxReadyTime - CurrentTime;

            // Schedule a retry if the delay is significant
            if (Delay > 0.01f)
            {
                float WaitTime = Delay + 0.01f;
                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, WaitTime, false);
            }
            else
            {
                // Poll next frame if delay is tiny (animation lag)
                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, 0.01f, false);
            }
        }
        return;
    }

    // If we passed cooldown check, clear any pending retries
    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    }


	if (CurrentlyFiringMode != 255 && CurrentlyFiringMode != FireModeNum)
	{
		// 1. IDENTIFY STATE
		UUTWeaponStateFiringChargedRocket_Transactional* TransState =
			Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);

		// Generic check for compatibility/safety
		bool bIsChargedState = (TransState != nullptr) ||
			(CurrentState && CurrentState->GetName().Contains(TEXT("Charged")));

		// 2. BLOCK STANDARD WEAPONS
		// If this isn't a Charged weapon (like Link/Shock), strictly forbid dual firing.
		if (!bIsChargedState)
		{
			return;
		}

		// 3. HANDLE ROCKET LAUNCHER LOGIC
		// We know we are in a Charged State. Now we decide: Cycle Mode or Queue Shot?

		// A) Transactional State Logic (The Fix)
		if (TransState)
		{
			if (TransState->bCharging)
			{
				// LOADING: User input cycles the rocket mode (Spread -> Grenade -> Spiral)
				if (CurrentState->IsFiring())
				{
					// Clear flag to prevent "Ghost Fire" on release
					if (UTOwner) UTOwner->SetPendingFire(FireModeNum, false);
					OnMultiPress(FireModeNum);
				}
				return; // Consumed input
			}
			else
			{
				// BURSTING: User released load and is unloading rockets.
				// Input intent is to fire Primary immediately after burst.
				// We Buffer the input and Return to prevent Double Drawing.
				if (UTOwner)
				{
					UTOwner->SetPendingFire(FireModeNum, true);
				}
				return; // Consumed input
			}
		}

		// B) Legacy/Fallback Logic (Standard UT behavior)
		if (CurrentState->IsFiring())
		{
			if (UTOwner) UTOwner->SetPendingFire(FireModeNum, false);
			OnMultiPress(FireModeNum);
			return;
		}
	}

    // ---------------------------------------------------------
    // 5. CHARGED STATE ENTRY
    // ---------------------------------------------------------
    // Safe to run now because we validated cooldowns at the top.
    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Charged")))
        {
            Super::StartFire(FireModeNum);
            return;
        }
    }

    // ---------------------------------------------------------
    // 6. STANDARD FIRING LOGIC
    // ---------------------------------------------------------

    // Clean up stale flags
    if (GetCurrentState() == ActiveState && CurrentlyFiringMode != 255)
    {
        CurrentlyFiringMode = 255;
        for (int32 i = 0; i < FireModeActiveState.Num(); i++)
        {
            FireModeActiveState[i] = 0;
        }
    }

    // Prevent re-entry if already firing this mode
    if (FiringState.IsValidIndex(FireModeNum) && CurrentState == FiringState[FireModeNum])
    {
        return;
    }

    // Set Active State Flags
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 1;
        CurrentlyFiringMode = FireModeNum;
    }

    // Set Input Flag
    if (UTOwner)
    {
      UTOwner->SetPendingFire(FireModeNum, true);
    }

	// --- FIX: AUTHORIZE LOGICAL SHOTS ---
		// If the server calls StartFire (e.g. from Equipping State finishing),
		// we must flag it as Transactional so the Gatekeeper lets it through.
	if (Role == ROLE_Authority)
	{
		bIsTransactionalFire = true;
	}

	BeginFiringSequence(FireModeNum, false);

	if (Role == ROLE_Authority)
	{
		bIsTransactionalFire = false;
	}
}








void AUTWeaponFix::FireShot()
{
	// --- CLIENT SIDE ---
	if (Role < ROLE_Authority)
	{
		// (Keep existing Client Logic unchanged)
		UWorld* World = GetWorld();
		if (!World) return;

		int32 NextEventIndex = GetNextClientFireEventIndex(CurrentFireMode);
		if (ClientFireEventIndex.IsValidIndex(CurrentFireMode))
			ClientFireEventIndex[CurrentFireMode] = NextEventIndex;

		if (LastFireTime.IsValidIndex(CurrentFireMode))
			LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
		FRotator ClientRot = GetUTOwner() ? GetUTOwner()->GetViewRotation() : FRotator::ZeroRotator;
		//EarliestFireTime = 0.f;

		uint8 ZOffset = 0;
		if (UTOwner)
		{
			float RawOffset = UTOwner->GetPawnViewLocation().Z - UTOwner->GetActorLocation().Z;
			float DefaultOffset = UTOwner->BaseEyeHeight;
			if (!FMath::IsNearlyEqual(RawOffset, DefaultOffset, 1.0f))
			{
				ZOffset = (uint8)FMath::Clamp(RawOffset + 127.5f, 0.f, 255.f);
			}
		}

		AUTCharacter* ClientHitChar = nullptr;
		if (bTrackHitScanReplication && InstantHitInfo.IsValidIndex(CurrentFireMode) &&
			InstantHitInfo[CurrentFireMode].DamageType != NULL &&
			InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.0f)
		{
			const FVector SpawnLocation = GetFireStartLoc();
			const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
			const FVector FireDir = SpawnRotation.Vector();
			const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

			FHitResult PreHit;
			HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, PreHit, 0.0f);
			ClientHitChar = Cast<AUTCharacter>(PreHit.Actor.Get());
		}
		ServerStartFireFixed(CurrentFireMode, NextEventIndex, GetWorld()->GetGameState()->GetServerWorldTimeSeconds(), false, ClientRot, ClientHitChar, ZOffset);

		Super::FireShot();
	}
	else
		// --- SERVER SIDE ---
	{
		// 1. GATEKEEPER LOGIC
		bool bInChargedState = false;
		if (CurrentState != nullptr)
		{
			if (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
				CurrentState->GetName().Contains(TEXT("Charged")))
			{
				bInChargedState = true;
			}
		}

		// Fix: Allow shots if State Machine is actively firing (handles "Queued from Equip" shots)
		bool bIsStateFiring = (CurrentState && CurrentState->IsFiring());
		bool bIsListenServerHost = (UTOwner && UTOwner->IsLocallyControlled());

		if (!bIsTransactionalFire && !bNetDelayedShot && !bIsListenServerHost && !bInChargedState && !bIsStateFiring)
		{
			return;
		}

		// 2. RHYTHM COMPENSATION & TIMESTAMP UPDATE
		if (LastFireTime.IsValidIndex(CurrentFireMode))
		{
			float CurrentTime = GetWorld()->GetTimeSeconds();
			float Refire = GetRefireTime(CurrentFireMode);
			float OldTime = LastFireTime[CurrentFireMode];

			// If this is the first shot (OldTime <= 0) OR if the player stopped firing for a while,
			// reset the clock to NOW.
			// (Tolerance: If gap is > Refire + 0.2s, assume they stopped firing).
			if (OldTime <= 0.0f || (CurrentTime - OldTime) > (Refire + 0.2f))
			{
				LastFireTime[CurrentFireMode] = CurrentTime;
			}
			else
			{
				// We are firing continuously. Apply Rhythm Compensation.
				float TheoreticalTime = OldTime + Refire;

				// If the actual fire time is close to the theoretical time (within 200ms jitter),
				// we snap the timer to the Theoretical Time.
				// This ensures that network jitter doesn't lower the player's DPS over time.
				if (CurrentTime < TheoreticalTime + 0.2f)
				{
					LastFireTime[CurrentFireMode] = TheoreticalTime;
				}
				else
				{
					// The delay was too large to be jitter (lag spike or pause). Reset to Now.
					LastFireTime[CurrentFireMode] = CurrentTime;
				}
			}
		}

		// 3. SPAWN PROJECTILE
		Super::FireShot();
	}
}



void AUTWeaponFix::StopFire(uint8 FireModeNum)
{
    
	if (UTOwner)
	{
		UTOwner->SetPendingFire(FireModeNum, false);
		UE_LOG(LogUTWeaponFix, Verbose, TEXT("clearing pending fire on swap %d"), FireModeNum);
	}
    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    }

	// We must clean these flags BEFORE any early returns.
	// Otherwise, the weapon gets stuck thinking it is "Active" in Mode 1.
	if (FireModeActiveState.IsValidIndex(FireModeNum))
	{
		FireModeActiveState[FireModeNum] = 0;
	}

	if (CurrentlyFiringMode == FireModeNum)
	{
		CurrentlyFiringMode = 255;
	}

    if (FiringState.IsValidIndex(FireModeNum))
    {
        if (FiringState[FireModeNum] &&
            FiringState[FireModeNum]->IsA(UUTWeaponStateZooming::StaticClass()))
        {
            //UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Mode %d is Zoom – bypassing transactional stop"), FireModeNum);
            Super::StopFire(FireModeNum);
            return;
        }
    }
    
    bool bIsChargedMode = false;

    // Check if the mode we are stopping is configured as a Charged State
    if (FiringState.IsValidIndex(FireModeNum) &&
        FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()))
    {
        bIsChargedMode = true;
    }

    // Check if the weapon is ACTUALLY in a Charged State right now
    if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
        CurrentState->GetName().Contains(TEXT("Charged"))))
    {
        bIsChargedMode = true;
    }

    if (bIsChargedMode)
    {
        // Don't log for Mode 0 stops (normal during swaps), but log for Mode 1
        if (FireModeNum == 1)
        {
            //UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StopFire] Bypassing Transactional Stop for Charged State (Mode 1)"));
        }

        // Standard UT logic handles the release (launching rockets or clearing pending fire)
        Super::StopFire(FireModeNum);

        // CRITICAL: Return here so we don't hit GotoActiveState() below
        return;
    }

   //if (FireModeNum < 2)
    //{
    //    GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    //}

    EndFiringSequence(FireModeNum);
    if (FiringState.IsValidIndex(FireModeNum) && GetCurrentState() == FiringState[FireModeNum])
    {
        GotoActiveState();
    }
    // Critical Fix #4: Immediate state clearing
    //UE_LOG(LogUTWeaponFix, Log, TEXT("[StopFire] Called for Mode %d. KEEPING TIMER ALIVE."), FireModeNum);
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 0;
    }

    if (CurrentlyFiringMode == FireModeNum)
    {
        CurrentlyFiringMode = 255;
    }

    

    if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
    {
        int32 EventIndex = ClientFireEventIndex.IsValidIndex(FireModeNum) ?
            ClientFireEventIndex[FireModeNum] : 0;
        float CurrentTime = GetWorld()->GetTimeSeconds();
        ServerStopFireFixed(FireModeNum, EventIndex, CurrentTime);
    }
    
}

bool AUTWeaponFix::ValidateFireRequest(uint8 FireModeNum, int32 InEventIndex, float ClientTime)
{
    // Critical Fix #5: Multi-layer validation
    // Get player name for logging (do this once at the top)
	FString PlayerName = TEXT("Unknown");
	if (UTOwner && UTOwner->Controller)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
		if (PC && PC->PlayerState)
		{
			PlayerName = PC->PlayerState->PlayerName;
		}
	}
    // Validate fire mode
    if (!FireModeActiveState.IsValidIndex(FireModeNum))
    {
        return false;
    }

    // Validate event sequence
    if (!IsFireEventSequenceValid(FireModeNum, InEventIndex))
    {
        //UE_LOG(LogTemp, Warning, TEXT("WeaponFix: Invalid fire event sequence %d for mode %d"),
        //    InEventIndex, FireModeNum);
        return false;
    }

    // Validate timing with network tolerance
    float ServerTime = GetWorld()->GetTimeSeconds();
    float TimeDiff = FMath::Abs(ServerTime - ClientTime);

    // Allow reasonable network delay but reject obviously wrong timestamps
    if (TimeDiff > 1.0f) // 1 second tolerance should be more than enough
    {
        UE_LOG(LogTemp, Warning, TEXT("WeaponFix: Rejected fire due to time desync: %f"), TimeDiff);
        return false;
    }

    /* Check refire rate
    if (LastFireTime.IsValidIndex(FireModeNum) && LastFireTime[FireModeNum] > 0.0f)
    {
        float TimeSinceLastFire = ServerTime - LastFireTime[FireModeNum];
        float MinInterval = GetRefireTime(FireModeNum) - 0.06f; // 50ms network tolerance

        if (TimeSinceLastFire < MinInterval)
        {
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[Server] REJECTED Rapid Fire. Delta: %.3f < Min: %.3f"), TimeSinceLastFire, MinInterval);
            
            return false;
        }
    }
    */
    for (int32 i = 0; i < LastFireTime.Num(); i++)
    {
        if (LastFireTime[i] > 0.0f)
        {
            float TimeSinceLastFire = ServerTime - LastFireTime[i];

            // Get the refire time for mode [i] (the one that was fired previously)
            // Subtract 65ms (0.06f) for network tolerance
            float MinInterval = GetRefireTime(i) - 0.12f;

            if (TimeSinceLastFire < MinInterval)
            {
                UE_LOG(LogUTWeaponFix, Warning, TEXT("Shot rejected for %s: [Server] REJECTED Rapid Fire. Mode %d blocked by Mode %d recovery. Delta: %.3f < Min: %.3f"),
					*PlayerName, FireModeNum, i, TimeSinceLastFire, MinInterval);
                return false;
            }
        }
    }


    return true;
}




bool AUTWeaponFix::IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime)
{
	// CHECK 1: Weapon switch penalty (EarliestFireTime)
	if (EarliestFireTime > CurrentTime)
	{
		return true;
	}

	// GLOBAL COOLDOWN CHECK
    // Iterate through ALL fire modes. If the weapon is recovering from ANY shot,
    // it cannot fire again.
    for (int32 i = 0; i < LastFireTime.Num(); i++)
    {
        if (LastFireTime[i] > 0.0f)
        {
            float TimeSinceLastFire = CurrentTime - LastFireTime[i];
            float RequiredInterval = GetRefireTime(i); // Get refire time for the mode that WAS fired

            // Client Tolerance (60ms)
            // If we are within the refire window of ANY mode, block the shot.
            if (TimeSinceLastFire < (RequiredInterval - 0.08f))
            {
                return true;
            }
        }
    }

    return false;
}





int32 AUTWeaponFix::GetNextClientFireEventIndex(uint8 FireModeNum)
{
    if (!ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        return 1;
    }

    // Critical Fix #6: Use int32 to prevent overflow issues
    return ClientFireEventIndex[FireModeNum] + 1;
}

bool AUTWeaponFix::IsFireEventSequenceValid(uint8 FireModeNum, int32 InEventIndex)
{
    if (!AuthoritativeFireEventIndex.IsValidIndex(FireModeNum))
    {
        return true; // First event is always valid
    }

    // Event must be newer than last processed, but not too far ahead
    int32 LastProcessed = AuthoritativeFireEventIndex[FireModeNum];
    return (InEventIndex > LastProcessed) && (InEventIndex <= LastProcessed + 10);
}



void AUTWeaponFix::ServerStartFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset)
{
    // 1. VALIDATION (Your existing transactional checks)
    UWorld* World = GetWorld();
    if (!World) return;

    if (!ValidateFireRequest(FireModeNum, InFireEventIndex, ClientTimestamp))
    {
        ClientConfirmFireEvent(FireModeNum, AuthoritativeFireEventIndex.IsValidIndex(FireModeNum) ? AuthoritativeFireEventIndex[FireModeNum] : 0);
        return;
    }
    CachedTransactionalRotation = ClientViewRot;
    if (ZOffset != 0)
    {
        // Decode byte back to float
        FireZOffset = ZOffset - 127;
        // IMPORTANT: Set time to NOW so GetFireStartLoc picks it up
        FireZOffsetTime = GetWorld()->GetTimeSeconds();
    }
    else
    {
        FireZOffset = 0;
        FireZOffsetTime = 0.f;
    }
    if (ClientHitChar != nullptr && bTrackHitScanReplication)
    {
        ReceivedHitScanHitChar = ClientHitChar;
        // InFireEventIndex matches FireEventIndex, so (ReceivedHitScanIndex == FireEventIndex) check passes
        ReceivedHitScanIndex = (uint8)InFireEventIndex;
    }
    else
    {
        ReceivedHitScanHitChar = nullptr;
        ReceivedHitScanIndex = 0;
    }

    // 2. UPDATE STATE
    if (AuthoritativeFireEventIndex.IsValidIndex(FireModeNum)) {

        AuthoritativeFireEventIndex[FireModeNum] = InFireEventIndex;
        FireEventIndex = (uint8)InFireEventIndex;
    }

    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 1;
        CurrentlyFiringMode = FireModeNum;
    }


    TargetedCharacter = nullptr; // Clear Weapon's cached target
    if (UTOwner && UTOwner->Controller)
    {
        AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
        if (PC)
        {
            PC->LastShotTargetGuess = nullptr; // Clear Controller's cached target
        }
    }

    if (UTOwner)
    {
        UTOwner->SetPendingFire(FireModeNum, true);
    }

    bIsTransactionalFire = true;
    // 3. EXECUTE FIRE (The New Logic)

    // Check if we are ALREADY in the transactional state (i.e., holding the button)
    UUTWeaponStateFiring_Transactional* TransState = Cast<UUTWeaponStateFiring_Transactional>(GetCurrentState());

    if (TransState && GetCurrentFireMode() == FireModeNum)
    {
        // STATE IS ACTIVE: Just trigger the next shot in the sequence.
        TransState->TransactionalFire();
    }
    else
    {
        // STATE IS INACTIVE: Enter the state.
        // BeginState() inside the new class will fire the first shot automatically.
        BeginFiringSequence(FireModeNum, bClientPredicted);
    }

    bIsTransactionalFire = false;
	ReceivedHitScanHitChar = nullptr;
    // 4. CONFIRM
    if (UTOwner && UTOwner->IsLocallyControlled())
    {
        ClientConfirmFireEvent(FireModeNum, InFireEventIndex);
    }
}

void AUTWeaponFix::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // --- WATCHDOG UNLOCK ---
    // If the weapon is marked as firing a mode, but the state machine says we are "Active" (Idle),
    // it means the Charged State finished (rockets fired/loaded) and returned to idle
    // without explicitly clearing the CurrentlyFiringMode flag.
    // We must clear it here to unlock the weapon for the next shot.
    if (CurrentlyFiringMode != 255 && GetCurrentState() == ActiveState)
    {
        
        // Clean up the active state array as well just to be safe
        if (FireModeActiveState.IsValidIndex(CurrentlyFiringMode))
        {
            FireModeActiveState[CurrentlyFiringMode] = 0;
        }
		CurrentlyFiringMode = 255;
    }


    // WATCHDOG: Prevent "Infinite Loop" audio/anim if Client disconnects or loses Stop packet.
    if (Role == ROLE_Authority && IsFiring())
    {
        
        if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            CurrentState->GetName().Contains(TEXT("Charged"))))
        {
            return;
        }
        
        float RefireTime = GetRefireTime(CurrentFireMode);

        // If we haven't received a valid RPC in > 2.5x the refire time, assume connection loss.
        // (e.g., for Link Gun (0.12s), if silent for 0.3s, kill it).
        float TimeoutThreshold = FMath::Max(0.25f, RefireTime * 2.5f);

        // LastFireTime is updated in ServerStartFireFixed
        if (GetWorld()->GetTimeSeconds() - LastFireTime[CurrentFireMode] > TimeoutThreshold)
        {
            // Force stop. This kills the looping audio and resets the state.
            StopFire(CurrentFireMode);
        }
    }
}


bool AUTWeaponFix::ServerStartFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset)
{
    return FireModeNum < GetNumFireModes() &&
        InFireEventIndex > 0 &&
        ClientTimestamp > 0.0f;
}




void AUTWeaponFix::ServerStopFireFixed_Implementation(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp)
{
    // 1. Clear authoritative state flags
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 0;
    }
    if (CurrentlyFiringMode == FireModeNum)
    {
        CurrentlyFiringMode = 255;
    }

    // 2. Standard cleanup
    EndFiringSequence(FireModeNum);

    // 3. FORCE STATE EXIT (Critical for Transactional Logic)
    // Since the server has no timer loop to transition naturally, 
    // we must force it back to 'Active' (Idle) immediately.
    if (GetCurrentState() == FiringState[FireModeNum])
    {
        GotoActiveState();
    }

    TargetedCharacter = nullptr; // Clear Weapon's cached target
    if (UTOwner && UTOwner->Controller)
    {
        AUTPlayerController* PC = Cast<AUTPlayerController>(UTOwner->Controller);
        if (PC)
        {
            PC->LastShotTargetGuess = nullptr; // Clear Controller's cached target
        }
    }

}




bool AUTWeaponFix::ServerStopFireFixed_Validate(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp)
{
    return FireModeNum < GetNumFireModes();
}

void AUTWeaponFix::ClientConfirmFireEvent_Implementation(uint8 FireModeNum, int32 InAuthorizedEventIndex)
{
    // Critical Fix #8: Sync client with server's authoritative state
    if (ClientFireEventIndex.IsValidIndex(FireModeNum))
    {
        ClientFireEventIndex[FireModeNum] = InAuthorizedEventIndex;
    }
}

void AUTWeaponFix::OnRep_FireModeState()
{
    // Handle fire mode state replication for non-owning clients
    for (int32 i = 0; i < FireModeActiveState.Num(); i++)
    {
        if (FireModeActiveState[i] == 0 && CurrentlyFiringMode == i)
        {
            CurrentlyFiringMode = 255;
        }
        else if (FireModeActiveState[i] == 1)
        {
            CurrentlyFiringMode = i;
        }
    }
}

void AUTWeaponFix::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(AUTWeaponFix, AuthoritativeFireEventIndex);
    DOREPLIFETIME(AUTWeaponFix, FireModeActiveState);
}





float AUTWeaponFix::GetHitValidationPredictionTime() const
{
    if (Role != ROLE_Authority || !UTOwner || !UTOwner->PlayerState)
    {
        return 0.0f;
    }

    APlayerState* PS = Cast<APlayerState>(UTOwner->PlayerState);
    if (!PS)
    {
        return 0.0f;
    }

	float ExactPing = UTOwner->PlayerState->ExactPing;

	// 2. Subtract Fudge Factor (Epic uses 20ms)
	// This subtracts the "Processing/Jitter" time so we don't over-rewind.
	float AdjustedPing = ExactPing - FudgeFactorMs;

	// 3. Clamp (0 to Max Cap)
	float CappedPing = FMath::Clamp(AdjustedPing, 0.0f, MaxRewindMs);

	// 4. Convert to One-Way Seconds
	// (Ping / 2) / 1000  ==  Ping * 0.0005
	return CappedPing * 0.0005f;
}


void AUTWeaponFix::HitScanTrace(const FVector& StartLocation, const FVector& EndTrace, float TraceRadius, FHitResult& Hit, float PredictionTime)
{
    // Override the prediction time parameter with hit validation time
    // This ensures we use split prediction's hit validation time (120ms)
    // instead of visual time (0ms) for server-side hit validation
    float ActualPredictionTime = GetHitValidationPredictionTime();

    // Call parent with corrected prediction time
    // Epic's GetRewindLocation() will be called with this value
    // NOTE: We cannot simply call Super::HitScanTrace because it doesn't support our custom padding logic.
    // We must reimplement the trace logic here.

    ECollisionChannel TraceChannel = COLLISION_TRACE_WEAPONNOCHARACTER;
    FCollisionQueryParams QueryParams(GetClass()->GetFName(), true, UTOwner);
    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

    // Perform the initial trace against world geometry
    if (TraceRadius <= 0.0f)
    {
        GetWorld()->LineTraceSingleByChannel(Hit, StartLocation, EndTrace, TraceChannel, QueryParams);
    }
    else
    {
        GetWorld()->SweepSingleByChannel(Hit, StartLocation, EndTrace, FQuat::Identity, TraceChannel, FCollisionShape::MakeSphere(TraceRadius), QueryParams);
    }

    if (!Hit.bBlockingHit)
    {
        Hit.Location = EndTrace;
    }


    // Now check against pawns
    AUTCharacter* BestTarget = NULL;
    FVector BestPoint(0.f);
    FVector BestCapsulePoint(0.f);
    float BestCollisionRadius = 0.f;

    for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
    {
        AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
        if (Target && (Target != UTOwner))
        {

            // Standard logic: Teammate checks, etc.
            if (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target))
            {
                
                float ExtraHitPadding = 0.f;

                // Only apply padding if the client explicitly claimed THIS target.
                // If client missed (ReceivedHitScanHitChar is null), this block is skipped (Padding = 0).
                if (Target == ReceivedHitScanHitChar)
                {
                    // Check velocity to decide WHICH padding to use
                    bool bIsMoving = !Target->GetVelocity().IsNearlyZero(1.0f);
					//ExtraHitPadding = bIsMoving ? HitScanPadding : HitScanPaddingStationary;
					if (bIsMoving)
					{
						float OwnerPing = (UTOwner && UTOwner->PlayerState) ? UTOwner->PlayerState->ExactPing : 0.0f;

						// TIERED PADDING SYSTEM
						// Running (940 u/s): 55 units = ~59ms jitter protection
						// Dodging (1700 u/s): 55 units = ~32ms jitter protection

						if (OwnerPing > 120.0f)
						{
							ExtraHitPadding = 60.0f; // Extreme Latency (Bad internet)
						}
						else if (OwnerPing > 90.0f)
						{
							ExtraHitPadding = 50.0f; // High Latency
						}
						else if (OwnerPing > 60.0f)
						{
							ExtraHitPadding = 45.0f; // Moderate Latency
						}
						else
						{
							ExtraHitPadding = 40.0f; // Low Ping / LAN
						}
					}
					else
					{
						// Stationary targets don't need velocity compensation
						ExtraHitPadding = HitScanPaddingStationary;
					}
                }
                // find appropriate rewind position, and test against trace from StartLocation to Hit.Location
                FVector TargetLocation = ((ActualPredictionTime > 0.f) && (Role == ROLE_Authority)) ? Target->GetRewindLocation(ActualPredictionTime) : Target->GetActorLocation();
                if (Role == ROLE_Authority && ActualPredictionTime > 0.f)
                {
                    float RTTms = UTOwner && UTOwner->PlayerState ? Cast<APlayerState>(UTOwner->PlayerState)->ExactPing : 0.f;
                    float RewindDistance = (Target->GetActorLocation() - TargetLocation).Size();

      
                }
                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                if (Target->UTCharacterMovement && Target->UTCharacterMovement->bIsFloorSliding)
                {
                    TargetLocation.Z = TargetLocation.Z - CollisionHeight + Target->SlideTargetHeight;
                    CollisionHeight = Target->SlideTargetHeight;
                }
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();

                bool bCheckOutsideHit = false;
                bool bHitTarget = false;
                FVector ClosestPoint(0.f);
                FVector ClosestCapsulePoint = TargetLocation;
                if (CollisionRadius >= CollisionHeight)
                {
                    ClosestPoint = FMath::ClosestPointOnSegment(TargetLocation, StartLocation, Hit.Location);
                    bHitTarget = ((ClosestPoint - TargetLocation).SizeSquared() < FMath::Square(CollisionHeight + TraceRadius + ExtraHitPadding));
                    if (!bHitTarget && (ExtraHitPadding > 0.f))
                    {
                        bCheckOutsideHit = true;
                    }
                }
                else
                {
                    FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
                    FMath::SegmentDistToSegmentSafe(StartLocation, Hit.Location, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
                    bHitTarget = ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CollisionRadius + TraceRadius + ExtraHitPadding));
                }

                // If we hit, update best target
                if (bHitTarget && (!BestTarget || ((ClosestPoint - StartLocation).SizeSquared() < (BestPoint - StartLocation).SizeSquared())))
                {
                    BestTarget = Target;
                    BestPoint = ClosestPoint;
                    BestCapsulePoint = ClosestCapsulePoint;
                    BestCollisionRadius = CollisionRadius;
                }
            }
        }
        // --- FIX END ---
    }

	// ============================================================
	// NEWNET-STYLE BIDIRECTIONAL TIME SEARCH
	// If client claimed a hit but we didn't find it, search through time
	// ============================================================
	if (Role == ROLE_Authority &&
		ReceivedHitScanHitChar != nullptr &&
		BestTarget != ReceivedHitScanHitChar)
	{
		AUTCharacter* ClaimedTarget = ReceivedHitScanHitChar;

		float CapRadius = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleRadius();
		float CapHeight = ClaimedTarget->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

		const float SearchStep = 0.015f;      // 15ms steps
		const float MaxSearchOffset = 0.050f; // ±50ms max search
		float SearchOffset = SearchStep;

		while (FMath::Abs(SearchOffset) <= MaxSearchOffset)
		{
			float AltRewindTime = ActualPredictionTime + SearchOffset;

			// Sanity bounds
			if (AltRewindTime > 0.0f && AltRewindTime < 0.25f)
			{
				FVector AltTargetLoc = ClaimedTarget->GetRewindLocation(AltRewindTime);

				// Handle floor sliding at alternate time
				float AltCapHeight = CapHeight;
				if (ClaimedTarget->UTCharacterMovement && ClaimedTarget->UTCharacterMovement->bIsFloorSliding)
				{
					AltTargetLoc.Z = AltTargetLoc.Z - CapHeight + ClaimedTarget->SlideTargetHeight;
					AltCapHeight = ClaimedTarget->SlideTargetHeight;
				}

				// Capsule-to-line distance check
				FVector ClosestPoint, ClosestCapsulePoint;

				if (CapRadius >= AltCapHeight)
				{
					ClosestPoint = FMath::ClosestPointOnSegment(AltTargetLoc, StartLocation, Hit.Location);
					ClosestCapsulePoint = AltTargetLoc;
				}
				else
				{
					FVector CapsuleSegment = FVector(0.f, 0.f, AltCapHeight - CapRadius);
					FMath::SegmentDistToSegmentSafe(
						StartLocation, Hit.Location,
						AltTargetLoc - CapsuleSegment, AltTargetLoc + CapsuleSegment,
						ClosestPoint, ClosestCapsulePoint);
				}

				// Generous padding for fallback search
				float SearchPadding = 50.0f;
				float CombinedRadius = CapRadius + TraceRadius + SearchPadding;

				if ((ClosestPoint - ClosestCapsulePoint).SizeSquared() < FMath::Square(CombinedRadius))
				{
					// Found the hit at alternate time
					BestTarget = ClaimedTarget;
					BestPoint = ClosestPoint;
					BestCapsulePoint = ClosestCapsulePoint;
					BestCollisionRadius = CapRadius;

					UE_LOG(LogUTWeaponFix, Verbose,
						TEXT("TimeSearch: Found claimed hit at offset %.1fms (base %.1fms)"),
						SearchOffset * 1000.f, ActualPredictionTime * 1000.f);
					break;
				}
			}

			// Oscillate: +15ms, -15ms, +30ms, -30ms, +45ms, -45ms, +60ms, -60ms
			if (SearchOffset > 0.f)
				SearchOffset = -SearchOffset;
			else
				SearchOffset = -SearchOffset + SearchStep;
		}
	}


    if (BestTarget)
    {
        // we found a player to hit, so update hit result
        // first find proper hit location on surface of capsule
        float ClosestDistSq = (BestPoint - BestCapsulePoint).SizeSquared();
        float BackDist = FMath::Sqrt(FMath::Max(0.f, BestCollisionRadius * BestCollisionRadius - ClosestDistSq));

        Hit.Location = BestPoint + BackDist * (StartLocation - EndTrace).GetSafeNormal();
        Hit.Normal = (Hit.Location - BestCapsulePoint).GetSafeNormal();
        Hit.ImpactNormal = Hit.Normal;
        Hit.Actor = BestTarget;
        Hit.bBlockingHit = true;
        Hit.Component = BestTarget->GetCapsuleComponent();
        Hit.ImpactPoint = BestPoint;
        Hit.Time = (BestPoint - StartLocation).Size() / (EndTrace - StartLocation).Size();
    }

    if (Role == ROLE_Authority)
    {
        OnServerHitScanResult(Hit, ActualPredictionTime);
    }
}


void AUTWeaponFix::OnServerHitScanResult(const FHitResult& Hit, float PredictionTime)
{
    // Default: do nothing. Custom weapons (Shock/Sniper) override this.
}


FRotator AUTWeaponFix::GetAdjustedAim_Implementation(FVector StartFireLoc)
{
    // 1. Get the Raw Aim (Standard View Rotation)
    // This relies on the controller's view rotation, not a cached target.
    FRotator BaseAim;

    // 1. USE TRANSACTIONAL ROTATION
    // If we are processing a Transactional Shot (bIsTransactionalFire is set in ServerStartFireFixed),
    // use the explicit rotation provided by the client.
    if (Role == ROLE_Authority && bIsTransactionalFire)
    {
		BaseAim = CachedTransactionalRotation;



		if (BaseAim.IsZero())
		{
			BaseAim = GetBaseFireRotation();
			UE_LOG(LogTemp, Error, TEXT("Cached was zero, using base: %s"), *BaseAim.ToString());
		}
    }
    else
    {
        // Standard path for Client prediction or non-transactional fire
        BaseAim = GetBaseFireRotation();
    }

    // CRITICAL: We do NOT call GuessPlayerTarget().
    // The base implementation calls GuessPlayerTarget(), which traces 
    // and updates 'LastShotTargetGuess', causing the magnetism loop.
    // By skipping it, we ensure the weapon fires exactly where the crosshair is.

    // 2. Apply Spread (If applicable)
    // We must re-implement the spread logic since we aren't calling Super.
    if (Spread.IsValidIndex(CurrentFireMode) && Spread[CurrentFireMode] > 0.0f)
    {
        FRotationMatrix Mat(BaseAim);
        FVector X, Y, Z;
        Mat.GetScaledAxes(X, Y, Z);

        // Deterministic spread syncing
        NetSynchRandomSeed();

        float RandY = 0.5f * (FMath::FRand() + FMath::FRand() - 1.f);
        float RandZ = FMath::Sqrt(0.25f - FMath::Square(RandY)) * (FMath::FRand() + FMath::FRand() - 1.f);

        return (X + RandY * Spread[CurrentFireMode] * Y + FMath::Clamp(RandZ * VerticalSpreadScaling, -1.f * MaxVerticalSpread, MaxVerticalSpread) * Spread[CurrentFireMode] * Z).Rotation();
    }

    // 3. Return Raw Aim
    return BaseAim;
}



FRotator AUTWeaponFix::GetBaseFireRotation()
{
    // Only hijack it for transactional shots on the server,
    // and only if the cached value is actually valid.
    if (Role == ROLE_Authority && bIsTransactionalFire && !CachedTransactionalRotation.IsZero())
    {
        return CachedTransactionalRotation;
    }

    return Super::GetBaseFireRotation();
}



FVector AUTWeaponFix::GetFireStartLoc(uint8 FireMode)
{
    // 1. Get the standard start location (Muzzle offset, etc applied to CURRENT Actor Location)
    FVector StartLoc = Super::GetFireStartLoc(FireMode);

    // 2. If this is a Transactional Shot on the Server, we must rewind the SHOOTER 
    /* to where they were when they fired, to align with the Client's Rotation.
    if (Role == ROLE_Authority && bIsTransactionalFire && UTOwner)
    {
        float PredictionTime = GetHitValidationPredictionTime();

        // Use the same rewind logic used for targets to find where the Shooter was
        FVector RewoundShooterLoc = UTOwner->GetRewindLocation(PredictionTime);

        // Apply the difference. 
        // This shifts the Muzzle Position by the distance the player moved during the RTT.
        StartLoc += (RewoundShooterLoc - UTOwner->GetActorLocation());
    }
    */
    return StartLoc;
}


void AUTWeaponFix::SpawnDelayedFakeProjectile()
{
	// Updated variable name
	if (NetcodeDelayedProjectile.ProjectileClass != nullptr)
	{
		SpawnNetPredictedProjectile(NetcodeDelayedProjectile.ProjectileClass, NetcodeDelayedProjectile.SpawnLocation, NetcodeDelayedProjectile.SpawnRotation);
	}
}


AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectile(
	TSubclassOf<AUTProjectile> ProjectileClass,
	FVector SpawnLocation,
	FRotator SpawnRotation)
{
	// Pitch clamp for shells/rockets firing straight down
	FRotator AdjustedRot = SpawnRotation;
	AdjustedRot.Normalize();
	bool bIsShellOrRocket = ProjectileClass &&
		(ProjectileClass->GetName().Contains(TEXT("Shell")) ||
			ProjectileClass->GetName().Contains(TEXT("Rocket")));
	if (bIsShellOrRocket && AdjustedRot.Pitch < -83.5f)
	{
		SpawnRotation.Pitch = -85.0f;
	}

	AUTPlayerController* OwningPlayer = UTOwner ? Cast<AUTPlayerController>(UTOwner->GetController()) : nullptr;
	// --- ADDED: Needed for Team Checks during Tunnel Sweep ---
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

	// ----------------------------------------
	// 1) Get Current Ping
	// ----------------------------------------
	float CurrentPing = 0.0f;
	if (UTOwner && UTOwner->PlayerState)
	{
		CurrentPing = UTOwner->PlayerState->ExactPing;
	}

	// ----------------------------------------
	// 2) Compute CatchupTickDelta (Half RTT)
	// ----------------------------------------
	float CatchupTickDelta = 0.0f;

	if (CurrentPing >= 20.0f)
	{
		float AdjustedPing = CurrentPing; // -FudgeFactorMs;
		float CappedPing = FMath::Clamp(AdjustedPing, 0.0f, ProjectilePredictionCapMs);
		CatchupTickDelta = CappedPing * 0.0005f;  // Half RTT in seconds
	}

	// ----------------------------------------
	// 3) Client: Check if we should delay spawn for extreme ping
	// ----------------------------------------
	if ((Role != ROLE_Authority) && OwningPlayer)
	{
		float ExcessPing = CurrentPing - FudgeFactorMs - ProjectilePredictionCapMs;

		if (ExcessPing > 10.0f)  // More than 10ms over cap
		{
			float SleepTime = ExcessPing * 0.001f;

			if (!GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle))
			{
				NetcodeDelayedProjectile.ProjectileClass = ProjectileClass;
				NetcodeDelayedProjectile.SpawnLocation = SpawnLocation;
				NetcodeDelayedProjectile.SpawnRotation = SpawnRotation;

				GetWorldTimerManager().SetTimer(
					SpawnDelayedFakeProjHandle,
					this,
					&AUTWeaponFix::SpawnDelayedFakeProjectile,
					SleepTime,
					false);
			}
			return nullptr;
		}
	}

	// ----------------------------------------
	// 4) Spawn the projectile
	// ----------------------------------------
	FActorSpawnParameters Params;
	Params.Instigator = UTOwner;
	Params.Owner = UTOwner;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AUTProjectile* NewProjectile = GetWorld()->SpawnActor<AUTProjectile>(
		ProjectileClass,
		SpawnLocation,
		SpawnRotation,
		Params);

	if (!NewProjectile)
	{
		return nullptr;
	}

	// ----------------------------------------
	// 5) Visual offsets (weapon hand)
	// ----------------------------------------
	if (NewProjectile->OffsetVisualComponent)
	{
		switch (GetWeaponHand())
		{
		case EWeaponHand::HAND_Center:
			NewProjectile->InitialVisualOffset = NewProjectile->InitialVisualOffset + LowMeshOffset;
			NewProjectile->OffsetVisualComponent->RelativeLocation = NewProjectile->InitialVisualOffset;
			break;
		case EWeaponHand::HAND_Hidden:
			NewProjectile->InitialVisualOffset = NewProjectile->InitialVisualOffset + VeryLowMeshOffset;
			NewProjectile->OffsetVisualComponent->RelativeLocation = NewProjectile->InitialVisualOffset;
			break;
		default:
			break;
		}
	}

	if (UTOwner)
	{
		UTOwner->LastFiredProjectile = NewProjectile;
		NewProjectile->ShooterLocation = UTOwner->GetActorLocation();
		NewProjectile->ShooterRotation = UTOwner->GetActorRotation();
	}

	// ----------------------------------------
	// 6) SERVER: Fast-forward authoritative projectile
	// ----------------------------------------
	if (Role == ROLE_Authority)
	{
		NewProjectile->HitsStatsName = HitsStatsName;

		// GUARD RAIL: Minimum Threshold (prevents 0-ping PIE physics bugs)
		const float MinCatchupThreshold = 0.005f;

		if ((CatchupTickDelta > MinCatchupThreshold) && NewProjectile->ProjectileMovement)
		{
			// =========================================================================
			// LAG COMPENSATION: REWIND CHECK
			// 
			// Because clients don't predict enemy positions (GetClientVisualPredictionTime = 0),
			// targets on the client's screen are behind their actual server position.
			// 
			// This check rewinds enemies to where they were when the client fired,
			// then tests if the projectile path would have hit them. This provides
			// lag compensation for both:
			// - Fast projectiles that could tunnel through targets
			// - Projectiles aimed at where the enemy appeared on screen
			// =========================================================================

			FVector CatchupStart = SpawnLocation;
			FVector CatchupVelocity = NewProjectile->ProjectileMovement->Velocity;

			if (CatchupVelocity.IsZero())
			{
				CatchupVelocity = SpawnRotation.Vector() * NewProjectile->ProjectileMovement->InitialSpeed;
			}
			FVector CatchupEnd = CatchupStart + (CatchupVelocity * CatchupTickDelta);

			// Get projectile's effective hit detection radius
			// Priority: CollisionComp > PawnOverlapSphere > fallback
			float ProjHitRadius = 0.f;
			if (NewProjectile->CollisionComp)
			{
				ProjHitRadius = NewProjectile->CollisionComp->GetScaledSphereRadius();
			}
			// Flak shards have CollisionComp = 0 but use PawnOverlapSphere (36 units) for hit detection
			if (ProjHitRadius <= 0.f && NewProjectile->PawnOverlapSphere)
			{
				ProjHitRadius = NewProjectile->PawnOverlapSphere->GetScaledSphereRadius();
			}
			// Final fallback for projectiles with neither
			if (ProjHitRadius <= 0.f)
			{
				ProjHitRadius = 10.f;
			}

			// Optimize Search Area
			FVector MinVec = CatchupStart.ComponentMin(CatchupEnd);
			FVector MaxVec = CatchupStart.ComponentMax(CatchupEnd);
			FBox PathBounds(MinVec, MaxVec);
			PathBounds = PathBounds.ExpandBy(200.0f);
			const float MaxRewindTime = 0.1f;
			float RewindTime = FMath::Min(CatchupTickDelta, MaxRewindTime);
			bool bHitRegistered = false;

			for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
			{
				AUTCharacter* Target = Cast<AUTCharacter>(*It);

				if (Target && Target != UTOwner && !Target->IsDead() &&
					PathBounds.IsInside(Target->GetActorLocation()))
				{
					// Skip teammates
					if (GS && GS->OnSameTeam(UTOwner, Target)) continue;

					// 1. REWIND: Where was the target when the client fired?
					//FVector RewoundLoc = Target->GetRewindLocation(CatchupTickDelta);
					FVector RewoundLoc = Target->GetRewindLocation(RewindTime);
					// 2. GEOMETRY: Construct Rewound Capsule centerline
					float CapRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();
					float CapHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

					FVector CapsuleTop = RewoundLoc + FVector(0, 0, CapHeight - CapRadius);
					FVector CapsuleBot = RewoundLoc - FVector(0, 0, CapHeight - CapRadius);

					// 3. MATH: Find closest points between projectile path and capsule centerline
					FVector PointOnPath, PointOnCapsule;
					FMath::SegmentDistToSegmentSafe(
						CatchupStart, CatchupEnd,
						CapsuleBot, CapsuleTop,
						PointOnPath, PointOnCapsule
					);

					float DistSqr = FVector::DistSquared(PointOnPath, PointOnCapsule);

					// 4. COLLISION CHECK: Replicate what the actual collision system would do
					// Combined radius = capsule surface + projectile hit detection sphere
					float CombinedRadius = CapRadius + ProjHitRadius;

					if (DistSqr < (CombinedRadius * CombinedRadius))
					{
						// Construct hit location on capsule surface
						FVector DirToPath = (PointOnPath - PointOnCapsule).GetSafeNormal();
						FVector HitLocation = PointOnCapsule + (DirToPath * CapRadius);
						FVector HitNormal = (CatchupStart - CatchupEnd).GetSafeNormal();

						// =========================================================
						// CRITICAL: Use ProcessHit, not Explode
						// 
						// ProcessHit correctly handles ALL projectile types:
						// - Calls DamageImpactedActor() for direct damage
						// - Sets ImpactedActor to avoid double-damage
						// - Then calls Explode() for splash/visuals
						// 
						// Direct damage projectiles (flak shards, OuterRadius = 0):
						//   -> FUTPointDamageEvent
						// 
						// Splash damage projectiles (rockets, OuterRadius > 0):
						//   -> FUTRadialDamageEvent + radial splash
						// =========================================================
						NewProjectile->ProcessHit(Target, Target->GetCapsuleComponent(), HitLocation, HitNormal);

						bHitRegistered = true;
						break;
					}
				}
			}

			// Only fast-forward if we didn't hit a rewound target
			if (!bHitRegistered)
			{
				const float ScaledDelta = CatchupTickDelta * NewProjectile->CustomTimeDilation;

				if (NewProjectile->PrimaryActorTick.IsTickFunctionEnabled())
				{
					NewProjectile->TickActor(ScaledDelta, LEVELTICK_All, NewProjectile->PrimaryActorTick);
				}

				NewProjectile->ProjectileMovement->TickComponent(ScaledDelta, LEVELTICK_All, nullptr);
				NewProjectile->SetForwardTicked(true);

				if (NewProjectile->GetLifeSpan() > 0.f)
				{
					NewProjectile->SetLifeSpan(
						0.1f + FMath::Max(0.01f, NewProjectile->GetLifeSpan() - CatchupTickDelta)
					);
				}
			}
			else
			{
				// Hit registered via rewind check - projectile already processed
				return nullptr;
			}
		}
		else
		{
			NewProjectile->SetForwardTicked(false);
		}
	}
	// ----------------------------------------
	// 7) CLIENT: Setup fake projectile
	// ----------------------------------------
	else
	{
		NewProjectile->InitFakeProjectile(OwningPlayer);

		if (CatchupTickDelta > 0.f)
		{
			// Optional: Shorten fake lifespan so it dies before big correction
			NewProjectile->SetLifeSpan(
				FMath::Min(NewProjectile->GetLifeSpan(), 2.f * FMath::Max(0.f, CatchupTickDelta))
			);
		}
	}

	// ----------------------------------------
	// 8) High-FPS stability (Fixed Tick Rate)
	// ----------------------------------------
	if (NewProjectile->ProjectileMovement)
	{
		if (Role == ROLE_Authority)
		{
			const float ServerRate = 1.f / 240.f;
			NewProjectile->PrimaryActorTick.TickInterval = ServerRate;
			NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = ServerRate;
		}
		else if (GetNetMode() != NM_DedicatedServer)
		{
			const int32 ClientHz = GetSnappedProjectileHz();
			const float ClientInterval = 1.f / static_cast<float>(ClientHz);
			NewProjectile->PrimaryActorTick.TickInterval = ClientInterval;
			NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = ClientInterval;
		}
	}

	return NewProjectile;
}




void AUTWeaponFix::FireInstantHit(bool bDealDamage, FHitResult* OutHit)
{
    // COMPLETE REIMPLEMENTATION - Don't call Super!
    // Calculate aim ONCE and use those values throughout

    checkSlow(InstantHitInfo.IsValidIndex(CurrentFireMode));

    // 1. Calculate aim ONCE - these values will be used for the entire function
    const FVector SpawnLocation = GetFireStartLoc();
    const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
    const FVector FireDir = SpawnRotation.Vector();
    const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

    // DEBUG: Log what we calculated


    // 2. Do the hit trace
    FHitResult Hit;
    AUTPlayerController* UTPC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
    AUTPlayerState* PS = (UTOwner && UTOwner->Controller) ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : nullptr;
    float PredictionTime = GetHitValidationPredictionTime();
    HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, Hit, PredictionTime);



    // --------------------------------------------------------------------------
// START DEBUG LOGGING
// --------------------------------------------------------------------------
    if (Role == ROLE_Authority)
    {
        // Case 1: Client claimed a hit, but Server disagrees
        if (ReceivedHitScanHitChar != nullptr && Hit.Actor != ReceivedHitScanHitChar)
        {
            // Calculate how close the shot actually came on the Server
            float ClosestDist = 9999.f;
            FVector ClosestPointOnRay, ClosestPointOnCapsule;

            // Rewind the claimed target to where the Server thinks it was
            FVector RewoundLoc = ReceivedHitScanHitChar->GetRewindLocation(PredictionTime);
            float CapRadius = ReceivedHitScanHitChar->GetCapsuleComponent()->GetScaledCapsuleRadius();
            float CapHeight = ReceivedHitScanHitChar->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

            // Math: Distance between the Shot Ray and the Rewound Capsule Segment
            FVector CapsuleSegTop = RewoundLoc + FVector(0, 0, CapHeight - CapRadius);
            FVector CapsuleSegBot = RewoundLoc - FVector(0, 0, CapHeight - CapRadius);

            FMath::SegmentDistToSegmentSafe(
                SpawnLocation, EndTrace,
                CapsuleSegBot, CapsuleSegTop,
                ClosestPointOnRay, ClosestPointOnCapsule
            );

            ClosestDist = FVector::Dist(ClosestPointOnRay, ClosestPointOnCapsule);
            float MissMargin = ClosestDist - CapRadius; // How far off the "skin" of the capsule
            /*
            UE_LOG(LogUTWeaponFix, Warning, TEXT("[DEBUG] HIT REJECTED! Client Claimed: %s | Server Hit: %s | RewindTime: %.3fms | Missed Capsule By: %.2f units"),
                *ReceivedHitScanHitChar->GetName(),
                Hit.Actor.Get() ? *Hit.Actor->GetName() : TEXT("None"),
                PredictionTime * 1000.f,
                MissMargin); 
			*/
        }

        // Case 2: Ghost Miss (Both missed, but maybe it was close?)
        // Useful for checking if your Rewind Math is aligning the hitbox correctly
        else if (ReceivedHitScanHitChar == nullptr && Hit.Actor == nullptr)
        {
            // Scan for nearest player to see how close we were
            float BestDist = 9999.f;
            AUTCharacter* NearestChar = nullptr;

            for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
            {
                AUTCharacter* TestChar = Cast<AUTCharacter>(*It);
                if (TestChar && TestChar != UTOwner && !TestChar->IsDead())
                {
                    FVector TestRewind = TestChar->GetRewindLocation(PredictionTime);
                    // Simple point-to-line check for debug speed
                    float Dist = FMath::PointDistToLine(TestRewind, EndTrace - SpawnLocation, SpawnLocation);
                    if (Dist < BestDist) { BestDist = Dist; NearestChar = TestChar; }
                }
            }

            if (NearestChar && BestDist < 80.0f) // Only log if reasonably close (e.g. < 80 units)
            {
                //UE_LOG(LogUTWeaponFix, Log, TEXT("[DEBUG] NEAR MISS. Nearest: %s | Dist: %.2f | RewindTime: %.3fms"),
                //    *NearestChar->GetName(), BestDist, PredictionTime * 1000.f);
            }
        }
    }



    // 3. Check for headshot (using the SAME SpawnLocation and FireDir)
    if (UTPC && bCheckHeadSphere && (Cast<AUTCharacter>(Hit.Actor.Get()) == nullptr) &&
        ((Spread.Num() <= GetCurrentFireMode()) || (Spread[GetCurrentFireMode()] == 0.f)) &&
        (UTOwner->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere))
    {
        AUTCharacter* AltTarget = Cast<AUTCharacter>(UUTGameplayStatics::ChooseBestAimTarget(
            UTPC, SpawnLocation, FireDir, 0.7f, (Hit.Location - SpawnLocation).Size(),
            150.f, AUTCharacter::StaticClass()));
        if (AltTarget != nullptr && (AltTarget->GetVelocity().IsNearlyZero() || bCheckMovingHeadSphere) &&
            AltTarget->IsHeadShot(SpawnLocation, FireDir, 1.1f, UTOwner, PredictionTime))
        {
            Hit = FHitResult(AltTarget, AltTarget->GetCapsuleComponent(),
                SpawnLocation + FireDir * ((AltTarget->GetHeadLocation() - SpawnLocation).Size() -
                    AltTarget->GetCapsuleComponent()->GetUnscaledCapsuleRadius()), -FireDir);
        }
    }

    // 4. Server-side processing
    if (Role == ROLE_Authority)
    {
        if (PS && (ShotsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(ShotsStatsName, 1);
        }
        UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
        UTOwner->SetFlashExtra(0, CurrentFireMode);
        UTOwner->ForceNetUpdate();
        // Bot warnings
        if (UTPC != nullptr)
        {
            APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
            if (PawnTarget != nullptr)
            {
                // DON'T cache this! That's what causes the ghost hits
                // UTPC->LastShotTargetGuess = PawnTarget;
            }
            if (bDealDamage && PawnTarget != nullptr)
            {
                AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                if (EnemyBot != nullptr)
                {
                    EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                }
            }
        }
        else if (bDealDamage)
        {
            AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
            if (B != nullptr)
            {
                APawn* PawnTarget = Cast<APawn>(Hit.Actor.Get());
                if (PawnTarget == nullptr)
                {
                    PawnTarget = Cast<APawn>(B->GetTarget());
                }
                if (PawnTarget != nullptr)
                {
                    AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                    if (EnemyBot != nullptr)
                    {
                        EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                    }
                }
            }
        }
    }
    else
    {
        // CLIENT SIDE:
        // If we have prediction time (delayed shot), queue the effect.
        if (PredictionTime > 0.f)
        {
            PlayPredictedImpactEffects(Hit.Location);
        }
        // If Prediction is 0 (Instant Hit / Your Setup), set it NOW.
        // This was missing! Without this, the local beam never draws.
        else
        {
            UTOwner->SetFlashLocation(Hit.Location, CurrentFireMode);
        }
    }
    // 5. Deal damage
    if (Hit.Actor != nullptr && Hit.Actor->bCanBeDamaged && bDealDamage)
    {
        if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(HitsStatsName, 1);
        }
        OnHitScanDamage(Hit, FireDir);
        Hit.Actor->TakeDamage(InstantHitInfo[CurrentFireMode].Damage,
            FUTPointDamageEvent(InstantHitInfo[CurrentFireMode].Damage, Hit, FireDir,
                InstantHitInfo[CurrentFireMode].DamageType, FireDir * GetImpartedMomentumMag(Hit.Actor.Get())),
            UTOwner->Controller, this);
    }

    if (OutHit != nullptr)
    {
        *OutHit = Hit;
    }

    // 6. Clear caches
    if (UTOwner)
    {
        if (UTPC)
        {
            UTPC->LastShotTargetGuess = nullptr;
        }
        TargetedCharacter = nullptr;
    }
}


void AUTWeaponFix::DetachFromOwner_Implementation()
{
    // Safety: Kill timers if the weapon is destroyed or dropped
    for (int32 i = 0; i < 2; i++)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
    }

    // Call the base class implementation (which does the unregistering/holstering logic you pasted)
    Super::DetachFromOwner_Implementation();
}

bool AUTWeaponFix::PutDown()
{
    // 1. Try to put the weapon down via the base class
    bool bPutDownResult = Super::PutDown();

    // 2. If it succeeded, kill the timers immediately.
    // This prevents the "Backpack Fire" bug where a buffered shot 
    // goes off 0.1s after you switched weapons.
    if (bPutDownResult)
    {

		// If we have a Retry Timer running, it means the user is holding Fire 
		// waiting for cooldown. Since we are putting this gun away, we must 
		// tell the Pawn "User is holding fire" so the NEXT gun picks it up.
		if (UTOwner)
		{
			for (int32 i = 0; i < 2; i++)
			{
				if (GetWorldTimerManager().IsTimerActive(RetryFireHandle[i]))
				{
					// "Graduate" the local retry timer to a persistent Pawn flag
					UTOwner->SetPendingFire(i, true);
					UE_LOG(LogUTWeaponFix, Warning, TEXT("PutDown: Transferring Retry %d to Pawn PendingFire"), i);
				}
			}
		}

		// A) Kill any pending retry timers
        for (int32 i = 0; i < 2; i++)
        {
            GetWorldTimerManager().ClearTimer(RetryFireHandle[i]);
        }

        // B) Reset the Gatekeeper Flags
        // This fixes the "Jam" bug where the weapon remembers it was firing Mode 1.
        CurrentlyFiringMode = 255;

        // C) Clear Replication Flags
        // Ensures the server state is clean for this weapon instance.
        for (int32 i = 0; i < FireModeActiveState.Num(); i++)
        {
            FireModeActiveState[i] = 0;
        }

        // --- FIX: CLEAR PAWN INPUT ---
            // This stops the "PendingFire" flag from bleeding into the next weapon
            // causing it to auto-fire immediately upon equip.
        //if (UTOwner)
        //{
        //    UTOwner->SetPendingFire(0, false);
        //    UTOwner->SetPendingFire(1, false);
        //}

    }
    return bPutDownResult;
}


void AUTWeaponFix::FireCone()
{
    //UE_LOG(LogUTWeapon, Verbose, TEXT("%s::FireCone()"), *GetName());

    checkSlow(InstantHitInfo.IsValidIndex(CurrentFireMode));
    checkSlow(InstantHitInfo[CurrentFireMode].ConeDotAngle > 0.0f);

    const FVector SpawnLocation = GetFireStartLoc();
    const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
    const FVector FireDir = SpawnRotation.Vector();
    const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

    AUTPlayerController* UTPC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : NULL;
    AUTPlayerState* PS = (UTOwner && UTOwner->Controller) ? Cast<AUTPlayerState>(UTOwner->Controller->PlayerState) : NULL;
    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

    // --- FIX START ---
    // Use custom prediction time logic (Transactional 120ms cap logic)
    float PredictionTime = GetHitValidationPredictionTime();
    // --- FIX END ---

    // --- FIX START ---
    // Use DefaultResponseParam instead of the private global 'WorldResponseParams'
    FCollisionResponseParams TraceResponseParams = FCollisionResponseParams::DefaultResponseParam;
    // --- FIX END ---

    TraceResponseParams.CollisionResponse.SetResponse(COLLISION_PROJECTILE_SHOOTABLE, ECR_Block);
    TArray<FOverlapResult> OverlapHits;
    TArray<FHitResult> RealHits;
    GetWorld()->OverlapMultiByChannel(OverlapHits, SpawnLocation, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceRange));
    for (const FOverlapResult& Overlap : OverlapHits)
    {
        if (Overlap.GetActor() != nullptr)
        {
            FVector ObjectLoc = Overlap.GetComponent()->Bounds.Origin;
            if (((ObjectLoc - SpawnLocation).GetSafeNormal() | FireDir) >= InstantHitInfo[CurrentFireMode].ConeDotAngle)
            {
                bool bClear;
                int32 Retries = 2;
                FCollisionQueryParams QueryParams(NAME_None, true, UTOwner);
                do
                {
                    FHitResult Hit;
                    if (InstantHitInfo[CurrentFireMode].TraceHalfSize <= 0.0f)
                    {
                        bClear = !GetWorld()->LineTraceSingleByChannel(Hit, SpawnLocation, ObjectLoc, COLLISION_TRACE_WEAPONNOCHARACTER, QueryParams, TraceResponseParams);
                    }
                    else
                    {
                        bClear = !GetWorld()->SweepSingleByChannel(Hit, SpawnLocation, ObjectLoc, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceHalfSize), QueryParams, TraceResponseParams);
                    }
                    if (bClear || Hit.GetActor() == nullptr || !ShouldTraceIgnore(Hit.GetActor()))
                    {
                        break;
                    }
                    else
                    {
                        QueryParams.AddIgnoredActor(Hit.GetActor());
                    }
                } while (Retries-- > 0);
                if (bClear)
                {
                    // trace only against target to get good hit info
                    FHitResult Hit;
                    if (!Overlap.GetComponent()->LineTraceComponent(Hit, SpawnLocation, ObjectLoc, FCollisionQueryParams(NAME_None, true, UTOwner)))
                    {
                        Hit = FHitResult(Overlap.GetActor(), Overlap.GetComponent(), ObjectLoc, -FireDir);
                    }
                    RealHits.Add(Hit);
                }
            }
        }
    }
    // do characters separately to handle forward prediction
    for (FConstPawnIterator Iterator = GetWorld()->GetPawnIterator(); Iterator; ++Iterator)
    {
        AUTCharacter* Target = Cast<AUTCharacter>(*Iterator);
        if (Target && (Target != UTOwner) && (bTeammatesBlockHitscan || !GS || !GS->OnSameTeam(UTOwner, Target)))
        {
            // find appropriate rewind position, and test against trace from StartLocation to Hit.Location
            // NOTE: This uses GetRewindLocation, which in your Character override respects 'PredictionTime' on the server
            FVector TargetLocation = ((PredictionTime > 0.f) && (Role == ROLE_Authority)) ? Target->GetRewindLocation(PredictionTime) : Target->GetActorLocation();

            const FVector Diff = TargetLocation - SpawnLocation;
            if (Diff.Size() <= InstantHitInfo[CurrentFireMode].TraceRange && (Diff.GetSafeNormal() | FireDir) >= InstantHitInfo[CurrentFireMode].ConeDotAngle)
            {
                // now see if trace would hit the capsule
                float CollisionHeight = Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
                if (Target->UTCharacterMovement && Target->UTCharacterMovement->bIsFloorSliding)
                {
                    TargetLocation.Z = TargetLocation.Z - CollisionHeight + Target->SlideTargetHeight;
                    CollisionHeight = Target->SlideTargetHeight;
                }
                float CollisionRadius = Target->GetCapsuleComponent()->GetScaledCapsuleRadius();

                bool bHitTarget = false;
                FVector ClosestPoint(0.f);
                FVector ClosestCapsulePoint = TargetLocation;
                if (CollisionRadius >= CollisionHeight)
                {
                    ClosestPoint = TargetLocation;
                }
                else
                {
                    FVector CapsuleSegment = FVector(0.f, 0.f, CollisionHeight - CollisionRadius);
                    FMath::SegmentDistToSegmentSafe(SpawnLocation, TargetLocation, TargetLocation - CapsuleSegment, TargetLocation + CapsuleSegment, ClosestPoint, ClosestCapsulePoint);
                }
                // first find proper hit location on surface of capsule
                float ClosestDistSq = (ClosestPoint - ClosestCapsulePoint).SizeSquared();
                float BackDist = FMath::Sqrt(FMath::Max(0.f, CollisionRadius * CollisionRadius - ClosestDistSq));
                const FVector HitLocation = ClosestPoint + BackDist * (SpawnLocation - EndTrace).GetSafeNormal();

                bool bClear;
                int32 Retries = 2;
                FCollisionQueryParams QueryParams(NAME_None, true, UTOwner);
                do
                {
                    FHitResult Hit;
                    if (InstantHitInfo[CurrentFireMode].TraceHalfSize <= 0.0f)
                    {
                        bClear = !GetWorld()->LineTraceTestByChannel(SpawnLocation, HitLocation, COLLISION_TRACE_WEAPONNOCHARACTER, QueryParams, TraceResponseParams);
                    }
                    else
                    {
                        bClear = !GetWorld()->SweepTestByChannel(SpawnLocation, HitLocation, FQuat::Identity, COLLISION_TRACE_WEAPONNOCHARACTER, FCollisionShape::MakeSphere(InstantHitInfo[CurrentFireMode].TraceHalfSize), QueryParams, TraceResponseParams);
                    }
                    if (bClear || Hit.GetActor() == nullptr || !ShouldTraceIgnore(Hit.GetActor()))
                    {
                        break;
                    }
                    else
                    {
                        QueryParams.AddIgnoredActor(Hit.GetActor());
                    }
                } while (Retries-- > 0);
                if (bClear)
                {
                    FHitResult* NewHit = new(RealHits) FHitResult;
                    NewHit->Location = HitLocation;
                    NewHit->Normal = (EndTrace - ClosestCapsulePoint).GetSafeNormal();
                    NewHit->ImpactNormal = NewHit->Normal;
                    NewHit->Actor = Target;
                    NewHit->bBlockingHit = true;
                    NewHit->Component = Target->GetCapsuleComponent();
                    NewHit->ImpactPoint = ClosestPoint; //FIXME
                    NewHit->Time = (ClosestPoint - SpawnLocation).Size() / (EndTrace - SpawnLocation).Size();
                }
            }
        }
    }
    RealHits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });

    if (Role == ROLE_Authority)
    {
        if (PS && (ShotsStatsName != NAME_None))
        {
            PS->ModifyStatsValue(ShotsStatsName, 1);
        }
        //UTOwner->IncrementFlashCount(CurrentFireMode);
        // fix projectile spawning of flak shards for others
        FVector FlashLoc = RealHits.Num() > 0 ? RealHits[0].Location : EndTrace;
        UTOwner->SetFlashLocation(FlashLoc, CurrentFireMode);
        // warn bot target, if any
        if (UTPC != nullptr)
        {
            APawn* PawnTarget = RealHits.Num() > 0 ? Cast<APawn>(RealHits[0].Actor.Get()) : nullptr;
            if (PawnTarget != nullptr)
            {
                // UTPC->LastShotTargetGuess = PawnTarget; // Disabled for transactional accuracy
            }
            if (PawnTarget) // Added check to prevent crash if cast failed
            {
                AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                if (EnemyBot != nullptr)
                {
                    EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                }
            }
        }
        else
        {
            AUTBot* B = Cast<AUTBot>(UTOwner->Controller);
            if (B != NULL)
            {
                APawn* PawnTarget = RealHits.Num() > 0 ? Cast<APawn>(RealHits[0].Actor.Get()) : nullptr;
                if (PawnTarget == NULL)
                {
                    PawnTarget = Cast<APawn>(B->GetTarget());
                }
                if (PawnTarget != nullptr)
                {
                    AUTBot* EnemyBot = Cast<AUTBot>(PawnTarget->Controller);
                    if (EnemyBot != nullptr)
                    {
                        EnemyBot->ReceiveInstantWarning(UTOwner, FireDir);
                    }
                }
            }
        }
    }
    for (const FHitResult& Hit : RealHits)
    {
        if (UTOwner && Hit.Actor != NULL && Hit.Actor->bCanBeDamaged)
        {
            if ((Role == ROLE_Authority) && PS && (HitsStatsName != NAME_None))
            {
                PS->ModifyStatsValue(HitsStatsName, 1);
            }
            Hit.Actor->TakeDamage(InstantHitInfo[CurrentFireMode].Damage, FUTPointDamageEvent(InstantHitInfo[CurrentFireMode].Damage, Hit, FireDir, InstantHitInfo[CurrentFireMode].DamageType, FireDir * GetImpartedMomentumMag(Hit.Actor.Get())), UTOwner->Controller, this);
        }
    }
}





void AUTWeaponFix::BringUp(float OverflowTime)
{
	float CurrentTime = GetWorld()->GetTimeSeconds();
	float MaxBlockTime = 0.f;

	// =======================================================================
	// FIX #1: CHECK THIS WEAPON'S OWN COOLDOWN DEBT FIRST
	// =======================================================================
	// When switching Sniper → Shock → Sniper, the Sniper's own LastFireTime
	// still has the cooldown debt from before the switch.
	for (int32 i = 0; i < LastFireTime.Num(); i++)
	{
		if (LastFireTime[i] > 0.f)
		{
			float RefireEnd = LastFireTime[i] + GetRefireTime(i);

			// If cooldown hasn't expired yet, we must wait
			if (RefireEnd > CurrentTime && RefireEnd > MaxBlockTime)
			{
				MaxBlockTime = RefireEnd;
			}
		}
	}

	// =======================================================================
	// FIX #2: CHECK OTHER WEAPONS 
	// =======================================================================
	// This handles the case where you fire Shock → switch to Sniper
	// The Sniper inherits the Shock's remaining cooldown (scaled)
	if (UTOwner)
	{
		for (TInventoryIterator<AUTWeapon> It(UTOwner); It; ++It)
		{
			AUTWeapon* OtherWeapon = *It;

			// Only check OTHER valid AUTWeaponFix weapons
			if (OtherWeapon && OtherWeapon != this && OtherWeapon->IsA(AUTWeaponFix::StaticClass()))
			{
				AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(OtherWeapon);
				if (FixWeapon)
				{
					// Back-calculate when the switch actually started
					float PutDownDuration = FixWeapon->GetPutDownTime();
					float SwitchStartTime = CurrentTime - OverflowTime - PutDownDuration;

					for (int32 i = 0; i < FixWeapon->LastFireTime.Num(); i++)
					{
						if (FixWeapon->LastFireTime[i] > 0.f)
						{
							float RefireEnd = FixWeapon->LastFireTime[i] + FixWeapon->GetRefireTime(i);
							float RemainingAtSwitch = RefireEnd - SwitchStartTime;

							// Only penalize if there was actual debt at moment of switch
							if (RemainingAtSwitch > 0.f)
							{
								// Apply scaling (e.g., 0.65 for fast-switch gamemodes)
								float ScaledRemaining = RemainingAtSwitch * FixWeapon->RefirePutDownTimePercent;
								float TheoreticalReadyTime = SwitchStartTime + ScaledRemaining;

								if (TheoreticalReadyTime > MaxBlockTime)
								{
									MaxBlockTime = TheoreticalReadyTime;
								}
							}
						}
					}
				}
			}
		}
	}

	// =======================================================================
	// APPLY THE RESTRICTION
	// =======================================================================
	if (MaxBlockTime > CurrentTime)
	{
		if (MaxBlockTime > EarliestFireTime)
		{
			EarliestFireTime = MaxBlockTime;
			UE_LOG(LogUTWeaponFix, Verbose,
				TEXT("[BringUp] %s: EarliestFireTime set to %.3f (blocks for %.3fms)"),
				*GetName(), EarliestFireTime, (MaxBlockTime - CurrentTime) * 1000.f);
		}
	}

	Super::BringUp(OverflowTime);
}
