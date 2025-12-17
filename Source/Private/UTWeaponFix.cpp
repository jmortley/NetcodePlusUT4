
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





/*

void AUTWeaponFix::StartFire(uint8 FireModeNum)
{

 

    if (CurrentlyFiringMode != 255 && CurrentlyFiringMode != FireModeNum)
    {
        // EXCEPTION: Allow multi-press if we are currently in a Charged State (Rocket Launcher Mode Switching)
        // We check if the *current* state is a charged state.
        bool bIsChargedState = false;
        if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            CurrentState->GetName().Contains(TEXT("Charged"))))
        {
            bIsChargedState = true;
        }

        // If we are NOT in a charged state, block the input.
        // If we ARE in a charged state, we let it pass so it hits the "OnMultiPress" logic further down.
        if (!bIsChargedState)
        {
            return;
        }
    }
    
    
    // Bypass for Zooming State
    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        // Check 1: Is it a child of the Zooming Class? (Standard Check)
        // Check 2: Does the name contain "Zoom"? (Safety net for BP variations)
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateZooming::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Zoom")))
        {
            // SUCCESS: Identified as Zoom. 
            // Hand off to parent (AUTWeapon) to handle state transition.
            // This prevents entering the Transactional Fire loop.
            //UE_LOG(LogUTWeaponFix, Log, TEXT("Early exit for zoom"));
            Super::StartFire(FireModeNum);
            return;
        }
    }


    if (FiringState.IsValidIndex(FireModeNum) && FiringState[FireModeNum])
    {
        if (FiringState[FireModeNum]->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            FiringState[FireModeNum]->GetName().Contains(TEXT("Charged")))
        {
           // UE_LOG(LogUTWeaponFix, Log, TEXT("[StartFire] Bypassing transactional for Charged state on Mode %d"), FireModeNum);
            Super::StartFire(FireModeNum);
            return;
        }
    }


    if (GetCurrentState() &&
        (GetCurrentState()->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            GetCurrentState()->GetName().Contains(TEXT("Charged"))))
    {
        // Do NOT call Super::StartFire. That sets PendingFire=true, which queues a shot 
            // that will fire immediately after you release the charge.

            // Instead, manually check for mode switch:
        if (FireModeNum != CurrentFireMode)
        {
            // 1. Clear any retry timers for this mode so it doesn't fire later
            if (FireModeNum < 2)
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            }

            // 2. Explicitly ensure PendingFire is FALSE for this mode
            if (UTOwner)
            {
                UTOwner->SetPendingFire(FireModeNum, false);
            }

            // 3. Perform the switch
            OnMultiPress(FireModeNum);
        }

        return;
    }





    if (CurrentState != nullptr && CurrentState != ActiveState)
    {
        // Check if current state is a charged state
        bool bInChargedState = false;

        if (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            CurrentState->GetName().Contains(TEXT("Charged")))
        {
            bInChargedState = true;
        }

        if (bInChargedState && CurrentState->IsFiring() && CurrentFireMode != FireModeNum)
        {
            // We're charging rockets and player pressed the other fire button
            // This should switch modes (spread -> grenades -> spiral), not start new fire
            //UE_LOG(LogUTWeaponFix, Log, TEXT("[StartFire] In charged state, calling OnMultiPress for mode %d"), FireModeNum);

            // Set pending fire so the weapon knows the button is pressed
            if (UTOwner)
            {
                UTOwner->SetPendingFire(FireModeNum, true);
            }

            // Call OnMultiPress to handle mode switching
            OnMultiPress(FireModeNum);
            return;
        }
    }


    if (GetCurrentState() == ActiveState && CurrentlyFiringMode != 255)
    {
        //UE_LOG(LogUTWeaponFix, Warning, TEXT("[StartFire] Fixing Stale FiringMode: Was %d, Resetting to 255"), CurrentlyFiringMode);
        CurrentlyFiringMode = 255;
        for (int32 i = 0; i < FireModeActiveState.Num(); i++)
        {
            FireModeActiveState[i] = 0;
        }
    }



    // Critical Fix #1: Prevent simultaneous fire modes
    if (CurrentlyFiringMode != 255 && CurrentlyFiringMode != FireModeNum)
    {
        // Optional: Logging disabled to prevent log spam during rapid clicks
        // UE_LOG(LogTemp, Warning, TEXT("WeaponFix: Blocked FireMode %d - FireMode %d already active"), FireModeNum, CurrentlyFiringMode);
        return;
    }
    if (GetCurrentState() == ActiveState && CurrentlyFiringMode != 255)
    {
        CurrentlyFiringMode = 255;
        if (FireModeActiveState.IsValidIndex(CurrentlyFiringMode))
            FireModeActiveState[CurrentlyFiringMode] = 0;
    }

    if (FiringState.IsValidIndex(FireModeNum) && CurrentState == FiringState[FireModeNum])
    {
        //UE_LOG(LogUTWeaponFix, Log, TEXT("[StartFire] Already in firing state for Mode %d - ignoring"), FireModeNum);
        return;
    }

    // Standard validation
    if (UTOwner && UTOwner->IsFiringDisabled())
    {
        return;
    }

    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
    if (GS && GS->PreventWeaponFire())
    {
        return;
    }

    float CurrentTime = GetWorld()->GetTimeSeconds();

    // Critical Fix #2: Strict cooldown validation
    if (IsFireModeOnCooldown(FireModeNum, CurrentTime))
    {
        //UE_LOG(LogTemp, Warning, TEXT("WeaponFix: Blocked on cooldown setting timer."))
            // NO TICK SOLUTION: 
        // If we are locally controlled and clicked early, schedule a retry.
        if (GetCurrentState() == FiringState[FireModeNum])
        {
            // Clear any pending retry timer just in case
            if (FireModeNum < 2)
            {
                GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
            }
            return;
            
        }

        if (Role < ROLE_Authority && UTOwner && UTOwner->IsLocallyControlled())
        {
            // --- STEP 1: Find the REAL cooldown end time ---
            float WorldTime = GetWorld()->GetTimeSeconds();
            float MaxReadyTime = 0.f;

            // Check ALL fire modes to see if a different mode is blocking us
            for (int32 i = 0; i < LastFireTime.Num(); i++)
            {
                // Get when this specific mode will be ready
                float ModeReadyTime = LastFireTime[i] + GetRefireTime(i);

                // Keep the latest time found
                if (ModeReadyTime > MaxReadyTime)
                {
                    MaxReadyTime = ModeReadyTime;
                }
            }

            // --- STEP 2: Calculate Delay ---
            float Delay = MaxReadyTime - WorldTime;

            // --- STEP 3: Smart Wait vs. Poll ---
            // If the Delay is real (e.g. > 0.02), we Smart Wait.
            // If the Delay is tiny or negative (meaning math says ready, but engine says no), 
            // we default to 0.01 polling to handle Animation/State lags.

            if (Delay > 0.01f) // Only Smart Wait if there is a significant delay
            {
                // Add a tiny buffer so we land explicitly AFTER the cooldown
                float WaitTime = Delay + 0.01f;

                //UE_LOG(LogUTWeaponFix, Log, TEXT("[StartFire] BLOCKED by Global Cooldown. Smart Wait: %.4fs."), WaitTime);

                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, WaitTime, false);
            }
            else
            {
                // Fallback: Math says ready, but StartFire failed. 
                // Likely animation state or replication lag. Poll next frame.
                UE_LOG(LogUTWeaponFix, Verbose, TEXT("[StartFire] Blocked by State/Anim. Retrying next frame."));

                FTimerDelegate RetryDel;
                RetryDel.BindUObject(this, &AUTWeaponFix::OnRetryTimer, FireModeNum);
                GetWorldTimerManager().SetTimer(RetryFireHandle[FireModeNum], RetryDel, 0.01f, false);
            }
        }
        return;

    }
    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
    }
    // Set active state immediately to prevent race conditions
    if (FireModeActiveState.IsValidIndex(FireModeNum))
    {
        FireModeActiveState[FireModeNum] = 1;
        CurrentlyFiringMode = FireModeNum;
    }

    // START LOCAL SEQUENCE ONLY
    // We do NOT send the RPC here anymore. 
    // This function starts the state machine, which calls BeginState -> FireShot.
    // The FireShot() override now handles sending the ServerStartFireFixed RPC.
    if (UTOwner)
    {
        UTOwner->SetPendingFire(FireModeNum, true);
    }
    //UE_LOG(LogUTWeaponFix, Log, TEXT("[StartFire] SUCCESS! Starting Sequence for Mode %d"), FireModeNum);
    BeginFiringSequence(FireModeNum, false);
}


*/



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

    // ---------------------------------------------------------
    // 3. MODE SWITCHING LOGIC (Charged State)
    // ---------------------------------------------------------
    if (CurrentlyFiringMode != 255 && CurrentlyFiringMode != FireModeNum)
    {
        // Check if we are in a Charged State (e.g., holding Right Click)
        bool bIsChargedState = false;
        if (CurrentState && (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            CurrentState->GetName().Contains(TEXT("Charged"))))
        {
            bIsChargedState = true;
        }

        // If NOT charged, block simultaneous input
        if (!bIsChargedState)
        {
            return;
        }
    }

    // Handle the specific "Mode Switch" action
    if (CurrentState != nullptr && CurrentState != ActiveState)
    {
        bool bInChargedState = false;
        if (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
            CurrentState->GetName().Contains(TEXT("Charged")))
        {
            bInChargedState = true;
        }

        if (bInChargedState && CurrentState->IsFiring() && CurrentFireMode != FireModeNum)
        {
            // FIX: Explicitly set PendingFire to FALSE.
            // This prevents the "Ghost Shot" bug where release triggers primary fire.
            if (UTOwner)
            {
                UTOwner->SetPendingFire(FireModeNum, false);
            }

            // Perform the switch (Spread -> Grenades -> Spiral)
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





/*
void AUTWeaponFix::FireShot()
{
    // --- CLIENT SIDE ---
    
    if (Role < ROLE_Authority)
    {
        UWorld* World = GetWorld();
        if (!World) return;
        // 1. Prepare Transaction ID
        int32 NextEventIndex = GetNextClientFireEventIndex(CurrentFireMode);

        // 2. Update Local History
        if (ClientFireEventIndex.IsValidIndex(CurrentFireMode))
            ClientFireEventIndex[CurrentFireMode] = NextEventIndex;

        if (LastFireTime.IsValidIndex(CurrentFireMode))
            LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
        FRotator ClientRot = GetUTOwner() ? GetUTOwner()->GetViewRotation() : FRotator::ZeroRotator;
        // 3. SEND THE PACKET
        // This ensures Shot #2, Shot #3, etc. are actually sent to the server.
        //UE_LOG(LogUTWeaponFix, Log, TEXT("[FireShot] Sending RPC. EventID: %d"), NextEventIndex);

        uint8 ZOffset = 0;
        if (UTOwner)
        {
            // Calculate difference between Camera Z and Actor Z
            // 127 is the "Zero" point. >127 is higher, <127 is lower.
            float RawOffset = UTOwner->GetPawnViewLocation().Z - UTOwner->GetActorLocation().Z;
            float DefaultOffset = UTOwner->BaseEyeHeight;

            // Only send if there is a discrepancy > 1.0 unit
            if (!FMath::IsNearlyEqual(RawOffset, DefaultOffset, 1.0f))
            {
                ZOffset = (uint8)FMath::Clamp(RawOffset + 127.5f, 0.f, 255.f);
            }
        }

        AUTCharacter* ClientHitChar = nullptr;

        // Only do this for Instant Hit weapons (HitScan)
        if (bTrackHitScanReplication && InstantHitInfo.IsValidIndex(CurrentFireMode) &&
            InstantHitInfo[CurrentFireMode].DamageType != NULL &&
            InstantHitInfo[CurrentFireMode].ConeDotAngle <= 0.0f)
        {
            const FVector SpawnLocation = GetFireStartLoc();
            const FRotator SpawnRotation = GetAdjustedAim(SpawnLocation);
            const FVector FireDir = SpawnRotation.Vector();
            const FVector EndTrace = SpawnLocation + FireDir * InstantHitInfo[CurrentFireMode].TraceRange;

            FHitResult PreHit;
            // Use 0.0f prediction time because we are aiming at what we see right now
            HitScanTrace(SpawnLocation, EndTrace, InstantHitInfo[CurrentFireMode].TraceHalfSize, PreHit, 0.0f);

            ClientHitChar = Cast<AUTCharacter>(PreHit.Actor.Get());
        }
        ServerStartFireFixed(CurrentFireMode, NextEventIndex, GetWorld()->GetGameState()->GetServerWorldTimeSeconds(), false, ClientRot, ClientHitChar, ZOffset);

        // 4. Play Visuals
        Super::FireShot();
    }
    else
    // --- SERVER SIDE ---
    {
        // GATEKEEPER LOGIC
        // We block auto-firing, BUT we must allow:
        // 1. bIsTransactionalFire (The standard path from RPC)
        // 2. bNetDelayedShot (The delayed path from Tick/HandleDelayedShot)
        bool bInChargedState = false;
        if (CurrentState != nullptr)
        {
            if (CurrentState->IsA(UUTWeaponStateFiringChargedRocket_Transactional::StaticClass()) ||
                CurrentState->GetName().Contains(TEXT("Charged")))
            {
                bInChargedState = true;
            }
        }
        bool bIsListenServerHost = (UTOwner && UTOwner->IsLocallyControlled());

        if (!bIsTransactionalFire && !bNetDelayedShot && !bIsListenServerHost && !bInChargedState)

        {
            return;
        }

		if (bIsListenServerHost && LastFireTime.IsValidIndex(CurrentFireMode))
		{
			LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
		}

        Super::FireShot();
    }
}
*/


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
    

    if (FireModeNum < 2)
    {
        GetWorldTimerManager().ClearTimer(RetryFireHandle[FireModeNum]);
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
            // Subtract 60ms (0.06f) for network tolerance
            float MinInterval = GetRefireTime(i) - 0.06f;

            if (TimeSinceLastFire < MinInterval)
            {
                UE_LOG(LogUTWeaponFix, Warning, TEXT("[Server] REJECTED Rapid Fire. Mode %d blocked by Mode %d recovery. Delta: %.3f < Min: %.3f"),
                    FireModeNum, i, TimeSinceLastFire, MinInterval);
                return false;
            }
        }
    }


    return true;
}

/*
bool AUTWeaponFix::IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime)
{
    if (!LastFireTime.IsValidIndex(FireModeNum) || LastFireTime[FireModeNum] <= 0.0f)
    {
        return false;
    }
    
    float TimeSinceLastFire = CurrentTime - LastFireTime[FireModeNum];
    float RequiredInterval = GetRefireTime(FireModeNum);

    // QUICK FIX: Client Tolerance
    // We allow the client to fire 50ms early. 
    // The server allows 60ms early, so this is safe.
    return TimeSinceLastFire < (RequiredInterval - 0.05f);

    //return TimeSinceLastFire < RequiredInterval;
}
*/


bool AUTWeaponFix::IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime)
{
    // GLOBAL COOLDOWN CHECK
    // Iterate through ALL fire modes. If the weapon is recovering from ANY shot,
    // it cannot fire again.
    for (int32 i = 0; i < LastFireTime.Num(); i++)
    {
        if (LastFireTime[i] > 0.0f)
        {
            float TimeSinceLastFire = CurrentTime - LastFireTime[i];
            float RequiredInterval = GetRefireTime(i); // Get refire time for the mode that WAS fired

            // Client Tolerance (50ms)
            // If we are within the refire window of ANY mode, block the shot.
            if (TimeSinceLastFire < (RequiredInterval - 0.05f))
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

/*
float AUTWeaponFix::GetHitValidationPredictionTime() const
{
    // 1. Must have an Owner (Pawn) and a Controller to calculate Ping
    if (UTOwner && UTOwner->GetController())
    {
        // 2. Priority: Try your Custom Controller (Handles the 0ms vs 120ms split)
        ATeamArenaPredictionPC* TeamPC = Cast<ATeamArenaPredictionPC>(UTOwner->GetController());
        if (TeamPC)
        {
            return TeamPC->GetVisualPredictionTime();
        }

        // 3. Fallback: Standard Controller (Standard UT4 Ping Logic)
        // Use Cast<> instead of (AUTPlayerController*) for crash safety
        AUTPlayerController* StandardPC = Cast<AUTPlayerController>(UTOwner->GetController());
        if (StandardPC)
        {
            return StandardPC->GetPredictionTime();
        }
    }

    // 4. Safety Net: Dropped weapon, Dead pawn, or Disconnected player.
    // No ping data available, so return 0.
    return 0.0f;
}
*/




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
	/*
    const float RTTms = PS->ExactPing;

    float IdealMs = (RTTms / 2.0f) + SmoothingMs;
    float RewindMs = FMath::Clamp(IdealMs, 0.0f, MaxRewindMs);

    return RewindMs * 0.001f;
	*/
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

                    ExtraHitPadding = bIsMoving ? HitScanPadding : HitScanPaddingStationary;
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





/*
AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectile(
    TSubclassOf<AUTProjectile> ProjectileClass,
    FVector SpawnLocation,
    FRotator SpawnRotation)
{
    AUTPlayerController* OwningPlayer =
        UTOwner ? Cast<AUTPlayerController>(UTOwner->GetController()) : nullptr;

    // ----------------------------------------
    // 1) Compute CatchupTickDelta
    // ----------------------------------------
    float CatchupTickDelta = 0.0f;

    if (Role == ROLE_Authority)
    {
        // SERVER: use our hit validation time (RTT/2)
        float PingSeconds = 0.0f;
        if (UTOwner && UTOwner->PlayerState)
        {
            // ExactPing is RTT in ms. 
            PingSeconds = (UTOwner->PlayerState->ExactPing * 0.001f);
        }

        // FIX: Multiply by 0.7 to be conservative and prevent overshooting/tunneling.
        // This reduces "dusting" where the rocket passes through a target during the catch-up tick.
        float IdealCatchup = (PingSeconds * 0.9f) / 2.0f;

        // FIX: Hard clamp to 150ms (0.15f) to prevent massive jumps on lag spikes
        CatchupTickDelta = FMath::Clamp(IdealCatchup, 0.0f, 0.12f);
        
    }
    else
    {
        // CLIENT: no forward prediction for projectiles
        // (still spawn immediately for visuals)
        CatchupTickDelta = 0.0f;
    }

    // Optional: if you still want super-high-lag "sleep" behavior on the client:
    if ((CatchupTickDelta > 0.f) && (Role != ROLE_Authority) && OwningPlayer)
    {
        float SleepTime = OwningPlayer->GetProjectileSleepTime();
        if (SleepTime > 0.f)
        {
            if (!GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle))
            {
                DelayedProjectile.ProjectileClass = ProjectileClass;
                DelayedProjectile.SpawnLocation = SpawnLocation;
                DelayedProjectile.SpawnRotation = SpawnRotation;
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
    // 2) Spawn the projectile
    // ----------------------------------------
    FActorSpawnParameters Params;
    Params.Instigator = UTOwner;
    Params.Owner = UTOwner;
    Params.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // IMPORTANT: always spawn on server AND owning client
    AUTProjectile* NewProjectile =
        GetWorld()->SpawnActor<AUTProjectile>(ProjectileClass,
            SpawnLocation,
            SpawnRotation,
            Params);

    if (!NewProjectile)
    {
        return nullptr;
    }

    // ----------------------------------------
    // 3) Visual offsets (same as stock)
    // ----------------------------------------
    if (NewProjectile->OffsetVisualComponent)
    {
        switch (GetWeaponHand())
        {
        case EWeaponHand::HAND_Center:
            NewProjectile->InitialVisualOffset =
                NewProjectile->InitialVisualOffset + LowMeshOffset;
            NewProjectile->OffsetVisualComponent->RelativeLocation =
                NewProjectile->InitialVisualOffset;
            break;
        case EWeaponHand::HAND_Hidden:
            NewProjectile->InitialVisualOffset =
                NewProjectile->InitialVisualOffset + VeryLowMeshOffset;
            NewProjectile->OffsetVisualComponent->RelativeLocation =
                NewProjectile->InitialVisualOffset;
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
    // 4) Server catch-up tick
    // ----------------------------------------
    if (Role == ROLE_Authority)
    {
        NewProjectile->HitsStatsName = HitsStatsName;

        if ((CatchupTickDelta > 0.f) && NewProjectile->ProjectileMovement)
        {
            const float ScaledDelta =
                CatchupTickDelta * NewProjectile->CustomTimeDilation;

            if (NewProjectile->PrimaryActorTick.IsTickFunctionEnabled())
            {
                NewProjectile->TickActor(
                    ScaledDelta, LEVELTICK_All, NewProjectile->PrimaryActorTick);
            }

            NewProjectile->ProjectileMovement->TickComponent(
                ScaledDelta, LEVELTICK_All, nullptr);

            NewProjectile->SetForwardTicked(true);

            if (NewProjectile->GetLifeSpan() > 0.f)
            {
                NewProjectile->SetLifeSpan(
                    0.1f + FMath::Max(
                        0.01f,
                        NewProjectile->GetLifeSpan() - CatchupTickDelta));
            }
        }
        else
        {
            NewProjectile->SetForwardTicked(false);
        }
    }
    else
    {
        // ----------------------------------------
        // 5) Client fake projectile
        // ----------------------------------------
        // CatchupTickDelta is 0 here, so this just marks it as fake
        // and optionally clamps lifespan a bit.
        NewProjectile->InitFakeProjectile(OwningPlayer);

        if (CatchupTickDelta > 0.f)
        {
            NewProjectile->SetLifeSpan(
                FMath::Min(NewProjectile->GetLifeSpan(),
                    2.f * FMath::Max(0.f, CatchupTickDelta)));
        }
    }

    // ----------------------------------------
    // 6) High-FPS stability (240 Hz lock)
    // ----------------------------------------
    if (NewProjectile->ProjectileMovement)
    {
        const float StableRate = 1.f / 240.f;
        NewProjectile->PrimaryActorTick.TickInterval = StableRate;
        NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval =
            StableRate;
    }
    
    // Apply to anyone rendering graphics (Client OR Listen Server Host)
    // Skip only the Dedicated Server (which has no screen/FPS issues)
    if (NewProjectile->ProjectileMovement && GetNetMode() != NM_DedicatedServer)
    {
        const int32 ClientHz = GetSnappedProjectileHz();

        // Safety Logic:
        // If we are the Host (Authority), we must be careful not to lower the tick 
        // below the Server Tick Rate (usually 120).
        // Your CVar defaults to 240, so this is safe (240 > 120).
        // If user sets CVar to 60, the Host's physics would stutter. 
        // This is an acceptable risk for a user-config setting.

        const float StableInterval = 1.f / static_cast<float>(ClientHz);

        NewProjectile->PrimaryActorTick.TickInterval = StableInterval;
        NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = StableInterval;
    }



    return NewProjectile;
}
*/




AUTProjectile* AUTWeaponFix::SpawnNetPredictedProjectile(
	TSubclassOf<AUTProjectile> ProjectileClass,
	FVector SpawnLocation,
	FRotator SpawnRotation)
{

	/*
	if (UTOwner && UTOwner->GetCapsuleComponent())
	{
		// 1. CAPSULE SAFETY CHECK: Prevent spawning inside self (Apex/Suicide fix)
		// Only run this check if aiming steep down
		if (SpawnRotation.Pitch < -70.0f)
		{
			float CapRadius = UTOwner->GetCapsuleComponent()->GetScaledCapsuleRadius();
			FVector ActorLoc = UTOwner->GetActorLocation();

			// Calculate horizontal distance from Player Center to Spawn Point
			FVector2D ActorLoc2D(ActorLoc.X, ActorLoc.Y);
			FVector2D SpawnLoc2D(SpawnLocation.X, SpawnLocation.Y);
			float DistXY = FVector2D::Distance(ActorLoc2D, SpawnLoc2D);

			// Flak Shell Radius is approx 15-20 units. 
			// We need: Dist > (CapsuleRadius + ShellRadius + Buffer)
			float RequiredDist = CapRadius + 20.0f;

			if (DistXY < RequiredDist)
			{
				// We are inside! Push the spawn point out horizontally.
				FVector ForwardDir = SpawnRotation.Vector();
				ForwardDir.Z = 0.0f; // Flatten to horizontal only
				if (ForwardDir.IsNearlyZero())
				{
					ForwardDir = UTOwner->GetActorForwardVector();
				}
				ForwardDir.Normalize();

				// Move it just enough to clear the capsule
				float PushAmount = RequiredDist - DistXY;
				SpawnLocation += (ForwardDir * PushAmount);

				// Optional: Nudge down slightly to ensure we don't snag the bottom hemisphere
				SpawnLocation.Z -= 5.0f;
			}
		}

		// 2. GEOMETRY CHECK: Prevent spawning inside walls/floors (Jump Pad fix)
		FVector EyeLoc;
		FRotator EyeRot;
		UTOwner->GetActorEyesViewPoint(EyeLoc, EyeRot);

		FCollisionQueryParams ClipParams(FName(TEXT("MuzzleClipTrace")), true, UTOwner);
		FHitResult ClipHit;

		// Trace from Eyes to the (potentially adjusted) SpawnLocation
		if (GetWorld()->LineTraceSingleByChannel(ClipHit, EyeLoc, SpawnLocation, ECC_WorldStatic, ClipParams))
		{
			// We hit a wall/floor/pad! Pull back to surface + buffer
			SpawnLocation = ClipHit.Location + (ClipHit.Normal * 2.0f);
		}
	}
	*/

	FRotator AdjustedRot = SpawnRotation;
	AdjustedRot.Normalize();
	AUTProjectile* DefaultProj = ProjectileClass ? ProjectileClass->GetDefaultObject<AUTProjectile>() : nullptr;
	bool bIsFlakShell = ProjectileClass && (ProjectileClass->GetName().Contains(TEXT("Shell")) || ProjectileClass->GetName().Contains(TEXT("Rocket")));
	if (bIsFlakShell && AdjustedRot.Pitch < -83.5f)
	{

		SpawnRotation.Pitch = -85.0f;
		
	}

	AUTPlayerController* OwningPlayer = UTOwner ? Cast<AUTPlayerController>(UTOwner->GetController()) : nullptr;

	// ----------------------------------------
	// 1) Compute CatchupTickDelta (Configurable)
	// ----------------------------------------

	float CatchupTickDelta = 0.0f;
	float CurrentPing = 0.0f;

	if (Role == ROLE_Authority)
	{
		// UTOwner->PlayerState->ExactPing;
		if (UTOwner && UTOwner->PlayerState)
		{
			CurrentPing = UTOwner->PlayerState->ExactPing;
		}
		if (CurrentPing < 20.0f)
		{
			CatchupTickDelta = 0.0f;
		}
		else
		{
			float AdjustedPing = CurrentPing - FudgeFactorMs;
			float CappedPing = FMath::Clamp(AdjustedPing, 0.0f, ProjectilePredictionCapMs);
			CatchupTickDelta = CappedPing * 0.0005f;
		}


	}
	else
	{
		// CLIENT: always 0
		CatchupTickDelta = 0.0f;
	}
	// ----------------------------------------
	// 2) Client High-Ping "Sleep" Check
	// ----------------------------------------

	if ((CatchupTickDelta > 0.f) && (Role != ROLE_Authority) && OwningPlayer)
	{
		// Calculate Sleep based on BP variables
		// Logic: If Ping > (Cap + Fudge), we sleep the excess
		// 0.001f * FMath::Max(0.f, PlayerState->ExactPing - PredictionFudgeFactor - MaxPredictionPing)
		float ExcessPing = (CurrentPing - FudgeFactorMs) - ProjectilePredictionCapMs;
		float SleepTime = FMath::Max(0.0f, ExcessPing * 0.001f);

		if (SleepTime > 0.f)
		{
			if (!GetWorldTimerManager().IsTimerActive(SpawnDelayedFakeProjHandle))
			{
				DelayedProjectile.ProjectileClass = ProjectileClass;
				DelayedProjectile.SpawnLocation = SpawnLocation;
				DelayedProjectile.SpawnRotation = SpawnRotation;

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
	// 3) Spawn the projectile
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
	// 4) Visual offsets
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
	// 5) Server catch-up tick (Fast Forward)
	// ----------------------------------------
	if (Role == ROLE_Authority)
	{
		NewProjectile->HitsStatsName = HitsStatsName;



		if ((CatchupTickDelta > 0.f) && NewProjectile->ProjectileMovement)
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
			NewProjectile->SetForwardTicked(false);
		}
	}
	else
	{
		// ----------------------------------------
		// 6) Client fake projectile
		// ----------------------------------------
		NewProjectile->InitFakeProjectile(OwningPlayer);

		if (CatchupTickDelta > 0.f)
		{
			NewProjectile->SetLifeSpan(
				FMath::Min(NewProjectile->GetLifeSpan(), 2.f * FMath::Max(0.f, CatchupTickDelta))
			);
		}
	}

	// -----------------------------------------------------------
	// 7) High-FPS stability (Fixed Tick Rate)
	// -----------------------------------------------------------
	if (NewProjectile->ProjectileMovement)
	{
		const float StableRate = 1.f / 240.f;
		NewProjectile->PrimaryActorTick.TickInterval = StableRate;
		NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval =
			StableRate;
	}
	if (NewProjectile->ProjectileMovement && GetNetMode() != NM_DedicatedServer)
	{

		const int32 ClientHz = GetSnappedProjectileHz();
		const float StableInterval = 1.f / static_cast<float>(ClientHz);

		NewProjectile->PrimaryActorTick.TickInterval = StableInterval;
		NewProjectile->ProjectileMovement->PrimaryComponentTick.TickInterval = StableInterval;

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
	// FIX: Fast Weapon Switch Exploit (Refire Preservation)
	if (UTOwner)
	{
		float MaxBlockTime = 0.f;
		float CurrentTime = GetWorld()->GetTimeSeconds();

		// Use Iterator to safely find "Hot" weapons currently in inventory
		for (TInventoryIterator<AUTWeapon> It(UTOwner); It; ++It)
		{
			AUTWeapon* OtherWeapon = *It;

			// Only check valid AUTWeaponFix weapons (avoiding self and incompatible types)
			if (OtherWeapon && OtherWeapon != this && OtherWeapon->IsA(AUTWeaponFix::StaticClass()))
			{
				AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(OtherWeapon);
				if (FixWeapon)
				{
					// 1. Back-calculate when the switch actually started.
					// This finds the exact moment the player pressed the switch key relative to the fire cycle.
					float PutDownDuration = FixWeapon->GetPutDownTime();
					float SwitchStartTime = CurrentTime - OverflowTime - PutDownDuration;

					for (int32 i = 0; i < FixWeapon->LastFireTime.Num(); i++)
					{
						if (FixWeapon->LastFireTime[i] > 0.f)
						{
							float RefireEnd = FixWeapon->LastFireTime[i] + FixWeapon->GetRefireTime(i);
							float RemainingAtSwitch = RefireEnd - SwitchStartTime;

							// 2. Only penalize if there was actual debt remaining at the moment of switch
							if (RemainingAtSwitch > 0.f)
							{
								// 3. Apply the scaling (e.g., 0.65 for fast-switch gamemodes)
								float ScaledRemaining = RemainingAtSwitch * FixWeapon->RefirePutDownTimePercent;

								// 4. Determine when the new weapon is allowed to fire
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

		// Apply the restriction if it extends into the future
		if (MaxBlockTime > CurrentTime)
		{
			if (MaxBlockTime > EarliestFireTime)
			{
				EarliestFireTime = MaxBlockTime;
			}
		}
	}

	Super::BringUp(OverflowTime);
}
