#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTPlusWeap_RocketLauncher.h"
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTBot.h"
// UTWeaponStateFiringChargedRocket_Transactional.cpp


UUTWeaponStateFiringChargedRocket_Transactional::UUTWeaponStateFiringChargedRocket_Transactional(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    bCharging = false;
    ChargeTime = 0.0f;
    RocketLauncher = nullptr;
}

AUTWeaponFix* UUTWeaponStateFiringChargedRocket_Transactional::GetWeaponFix() const
{
    return Cast<AUTWeaponFix>(GetOuterAUTWeapon());
}

void UUTWeaponStateFiringChargedRocket_Transactional::ClearAllTimers()
{
    if (GetOuterAUTWeapon())
    {
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(RefireCheckHandle);
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(GraceTimerHandle);
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(FireLoadedRocketHandle);
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::BeginState(const UUTWeaponState* PrevState)
{
    // 1. Notify weapon that firing has started
    GetOuterAUTWeapon()->OnStartedFiring();
    if (GetOuterAUTWeapon())
 
    GetOuterAUTWeapon()->DeactivateSpawnProtection();
    
    AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
    if (W && W->LastFireTime.IsValidIndex(GetFireMode()))
    {
        W->LastFireTime[GetFireMode()] = GetWorld()->GetTimeSeconds();
    }
    // 2. Safety checks
    if (GetUTOwner() == nullptr || GetOuterAUTWeapon()->GetCurrentState() != this)
    {
        return;
    }

    // 3. Cast to our rocket launcher type
    RocketLauncher = Cast<AUTPlusWeap_RocketLauncher>(GetOuterAUTWeapon());
    if (RocketLauncher == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UTWeaponStateFiringChargedRocket_Transactional::BeginState - Weapon is not AUTPlusWeap_RocketLauncher!"));
        GetOuterAUTWeapon()->GotoActiveState();
        return;
    }

    // 4. Enter charging state
    bCharging = true;
    ChargeTime = 0.0f;

    // If we entered via RefireCheckTimer, EndState() was NOT called, 
    // so NumLoadedBarrels might still be at Max from the previous shot.
    // We must force a clean slate here to prevent immediate Grace Timer triggers.
    RocketLauncher->NumLoadedRockets = 0;
    RocketLauncher->NumLoadedBarrels = 0;

    // 5. Setup visual feedback - flash extra shows loading progress to other players
    RocketLauncher->SetRocketFlashExtra(GetFireMode(), 1, RocketLauncher->CurrentRocketFireMode, RocketLauncher->bDrawRocketModeString);

    // 6. Start loading the first rocket
    RocketLauncher->BeginLoadRocket();

    // 7. Set timer for when first rocket finishes loading
    float LoadTime = RocketLauncher->GetLoadTime(RocketLauncher->NumLoadedRockets);
    GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
        LoadTimerHandle,
        this,
        &UUTWeaponStateFiringChargedRocket_Transactional::LoadTimer,
        LoadTime,
        false
    );

    // NOTE: We do NOT call FireShot() here like the standard Transactional state does.
    // We wait for the player to release the button or for the grace timer to fire.

    // NOTE: We do NOT set RefireCheckHandle here - charging doesn't use refire timing.
}

void UUTWeaponStateFiringChargedRocket_Transactional::EndState()
{
    // 1. Clear all timers
    ClearAllTimers();

    // 2. Reset charging state
    ChargeTime = 0.0f;
    bCharging = false;

    // 3. Clean up rocket launcher state (in case we exit early)
    if (RocketLauncher)
    {
        RocketLauncher->NumLoadedRockets = 0;
        RocketLauncher->NumLoadedBarrels = 0;
    }

    // 4. Standard cleanup
    ToggleLoopingEffects(false);
    GetOuterAUTWeapon()->OnStoppedFiring();
    GetOuterAUTWeapon()->StopFiringEffects();

    if (GetOuterAUTWeapon()->GetUTOwner())
    {
        GetOuterAUTWeapon()->GetUTOwner()->ClearFiringInfo();
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::Tick(float DeltaTime)
{
    // Track charge time (for UI or future features)
    if (bCharging)
    {
        ChargeTime += DeltaTime;
    }

    // CLIENT SIDE: Check if player released the fire button
    if (bCharging &&
        GetOuterAUTWeapon()->GetNetMode() != NM_DedicatedServer &&
        GetUTOwner() &&
        GetUTOwner()->IsLocallyControlled())
    {
        // Check if fire button is still held
        if (!GetUTOwner()->IsPendingFire(GetFireMode()))
        {
            // Player released - fire whatever we have loaded
            GetOuterAUTWeapon()->StopFire(GetFireMode());
        }
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::LoadTimer()
{
    if (!RocketLauncher)
    {
        return;
    }

    // 1. Complete the current rocket load
    RocketLauncher->EndLoadRocket();

    // 2. Check state
    if (!bCharging)
    {
        // Player already released during load - fire immediately
        // This handles the case where they release mid-load
        EndFiringSequence(GetFireMode());
        return;
    }

    // 3. Check if we're fully loaded
    if (RocketLauncher->NumLoadedBarrels >= RocketLauncher->MaxLoadedRockets)
    {
        // Fully loaded - start grace timer
        // After grace period, we auto-fire to prevent holding forever

        // Tell non-local clients to stop loading animation
        if (GetUTOwner() && !GetUTOwner()->IsLocallyControlled() && GetWorld()->GetNetMode() != NM_Client)
        {
            RocketLauncher->ClientAbortLoad();
        }

        // Start grace timer if not already running
        if (!GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(GraceTimerHandle))
        {
            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                GraceTimerHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::GraceTimer,
                RocketLauncher->GracePeriod,
                false
            );
        }
    }
    else
    {
        // Not full yet - start loading the next rocket
        RocketLauncher->BeginLoadRocket();

        float LoadTime = RocketLauncher->GetLoadTime(RocketLauncher->NumLoadedRockets);
        GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
            LoadTimerHandle,
            this,
            &UUTWeaponStateFiringChargedRocket_Transactional::LoadTimer,
            LoadTime,
            false
        );
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::GraceTimer()
{
    // Grace period expired - force fire whatever is loaded
    EndFiringSequence(GetFireMode());
}

/*
void UUTWeaponStateFiringChargedRocket_Transactional::EndFiringSequence(uint8 FireModeNum)
{
    if (FireModeNum != GetFireMode())
    {
        return;
    }

    bCharging = false;

    // If we have 0 rockets, but the Load Timer is still running, the user released the button "early".
    // WE MUST NOT EXIT YET. We must wait for the LoadTimer to finish tick, 
    // which will increment the rocket count to 1 and then call this function again to fire.
    if (RocketLauncher && RocketLauncher->NumLoadedRockets <= 0 && GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(LoadTimerHandle))
    {
        return;
    }


    if (!RocketLauncher || RocketLauncher->NumLoadedRockets <= 0)
    {
        GetOuterAUTWeapon()->GotoActiveState();
        return;
    }

    AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
    if (GameState && (GameState->HasMatchEnded() || GameState->IsMatchIntermission()))
    {
        RocketLauncher->NumLoadedRockets = 0;
        GetOuterAUTWeapon()->GotoActiveState();
        return;
    }

    // Just use stock UT firing - no transactional needed for charged weapons
    FireLoadedRocket();
}
*/
void UUTWeaponStateFiringChargedRocket_Transactional::EndFiringSequence(uint8 FireModeNum)
{
	if (FireModeNum != GetFireMode()) return;

	bCharging = false;

	// --- FIX START: DYNAMIC GHOST ROCKET COMPENSATION ---
	if (RocketLauncher && RocketLauncher->NumLoadedRockets <= 0 && GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(LoadTimerHandle))
	{
		float Remaining = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle);

		float RTT_ms = 0.0f;
		//FString PlayerName = "Unknown"; // For debug logging

		if (GetUTOwner() && GetUTOwner()->PlayerState)
		{
			RTT_ms = GetUTOwner()->PlayerState->ExactPing;
			//PlayerName = GetUTOwner()->PlayerState->PlayerName;
		}

		// Convert RTT to One-Way Seconds
		float OneWaySeconds = RTT_ms * 0.0005f;

		// Add 20ms Jitter Buffer
		float Tolerance = OneWaySeconds + 0.02f;

		// Clamp Tolerance (Max 200ms)
		Tolerance = FMath::Clamp(Tolerance, 0.0f, 0.20f);

		// Debug Log to verify values in Output Log
		// UE_LOG(LogTemp, Log, TEXT("Rocket Check - Player: %s | RTT: %.2f | OneWay: %.4f | Tolerance: %.4f | Remaining: %.4f"), 
		//    *PlayerName, RTT_ms, OneWaySeconds, Tolerance, Remaining);

		if (Remaining < Tolerance)
		{
			GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);
			LoadTimer();
			return;
		}
		else
		{
			// Legit early release (or latency too high) - RESET STATE to prevent jam
			GetOuterAUTWeapon()->GotoActiveState();
			return;
		}
	}
	// --- FIX END ---

	if (!RocketLauncher || RocketLauncher->NumLoadedRockets <= 0)
	{
		GetOuterAUTWeapon()->GotoActiveState();
		return;
	}

	AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
	if (GameState && (GameState->HasMatchEnded() || GameState->IsMatchIntermission()))
	{
		RocketLauncher->NumLoadedRockets = 0;
		GetOuterAUTWeapon()->GotoActiveState();
		return;
	}

	FireLoadedRocket();
}





void UUTWeaponStateFiringChargedRocket_Transactional::FireLoadedRocket()
{
    if (!RocketLauncher || RocketLauncher->NumLoadedRockets <= 0)
    {
        // Done firing - cleanup
        ChargeTime = 0.0f;
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(GraceTimerHandle);
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);

        if (GetOuterAUTWeapon()->GetCurrentState() == this)
        {
            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                RefireCheckHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer,
                GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode()),
                false
            );
        }
        return;
    }

    // Fire one rocket using stock UT logic (bypasses UTWeaponFix transactional)
    RocketLauncher->FireShotDirect();

    // Handle burst
    if (RocketLauncher->NumLoadedRockets > 0)
    {
        if (RocketLauncher->BurstInterval <= 0.f || RocketLauncher->ShouldFireLoad())
        {
            // --- FIX START: SAFETY COUNTER ---
            int32 SafetyCounter = 0;
            while (RocketLauncher->NumLoadedRockets > 0)
            {
                int32 OldCount = RocketLauncher->NumLoadedRockets;

                RocketLauncher->FireShotDirect();

                // If the weapon failed to decrement (bug), force it down to prevent infinite loop
                if (RocketLauncher->NumLoadedRockets >= OldCount)
                {
                    RocketLauncher->NumLoadedRockets--;
                }

                // Hard limit to prevent editor freeze (e.g., if decrement logic is completely broken)
                SafetyCounter++;
                if (SafetyCounter > 50)
                {
                    UE_LOG(LogTemp, Warning, TEXT("Infinite Loop Detected in FireLoadedRocket! Breaking loop."));
                    RocketLauncher->NumLoadedRockets = 0;
                    break;
                }
            }
            // --- FIX END ---
        }
        else
        {
            float BurstTime = (RocketLauncher->CurrentRocketFireMode == 0)
                ? RocketLauncher->BurstInterval
                : RocketLauncher->GrenadeBurstInterval;

            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                FireLoadedRocketHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::FireLoadedRocket,
                BurstTime,
                false
            );
            return;
        }
    }

    // All rockets fired
    ChargeTime = 0.0f;
    GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(GraceTimerHandle);
    GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);

    if (GetOuterAUTWeapon()->GetCurrentState() == this)
    {
        GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
            RefireCheckHandle,
            this,
            &UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer,
            GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode()),
            false
        );
    }
}





void UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer()
{
	// 1. Weapon State Check: If we aren't the current state, abort.
	if (GetOuterAUTWeapon()->GetCurrentState() != this)
	{
		return;
	}

	// 2. Character Intent Check: If the owner is switching weapons, abort.
	// This catches the switch effectively "instantly" before state transitions finish.
	if (GetUTOwner() && GetUTOwner()->GetPendingWeapon() != nullptr)
	{
		GetOuterAUTWeapon()->GotoActiveState();
		return;
	}


	// Query bot AI for firing decisions
    AUTBot* B = Cast<AUTBot>(GetUTOwner() ? GetUTOwner()->Controller : nullptr);
    if (B != nullptr)
    {
        B->CheckWeaponFiring();
    }

    // Make sure owner still exists (bot might have died)
    if (GetUTOwner() == nullptr)
    {
        return;
    }

    // Check if player wants to fire again
    if (GetOuterAUTWeapon()->HandleContinuedFiring())
    {
        // Start a new charge cycle
        bCharging = true;
        BeginState(this);
    }
    else
    {
        // Player stopped firing - return to active state
        GetOuterAUTWeapon()->GotoActiveState();
		//GetOuterAUTWeapon()->StopFire(GetFireMode());
		if (GetUTOwner() && GetUTOwner()->IsPendingFire(0))
		{
			GetOuterAUTWeapon()->StartFire(0);
		}
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::UpdateTiming()
{
    // Update load timer if active (fire rate powerups, etc)
    if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(LoadTimerHandle) && RocketLauncher)
    {
        float RemainingPct = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle)
            / GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRate(LoadTimerHandle);

        GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
            LoadTimerHandle,
            this,
            &UUTWeaponStateFiringChargedRocket_Transactional::LoadTimer,
            RocketLauncher->GetLoadTime(RocketLauncher->NumLoadedRockets) * RemainingPct,
            false
        );
    }

    // Update refire timer if active
    if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(RefireCheckHandle))
    {
        float FirstDelay = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle);
        if (FirstDelay > 0)
        {
            FirstDelay = FMath::Max(FirstDelay, GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode()));
        }
        else
        {
            FirstDelay = GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode());
        }

        GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
            RefireCheckHandle,
            this,
            &UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer,
            FirstDelay,
            false
        );
    }
}


/*
void UUTWeaponStateFiringChargedRocket_Transactional::PutDown()
{
    // Don't allow putdown while:
    // 1. Actively charging
    // 2. In the middle of burst fire
    // 3. Grace timer is active

    if (bCharging ||
        GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(FireLoadedRocketHandle) ||
        GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(GraceTimerHandle))
    {
        // Can't putdown yet - maybe queue it?
        return;
    }

    // If we have loaded rockets, fire them before switching
    if (RocketLauncher && RocketLauncher->NumLoadedRockets > 0)
    {
        FireLoadedRocket();

		// --- FIX: DO NOT SCHEDULE REFIRE TIMER HERE ---
		// We are switching weapons. We just dumped the ammo. 
		// We do NOT want to check if we should fire again.
		
        if (GetOuterAUTWeapon()->GetCurrentState() == this)
        {
            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                RefireCheckHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer,
                GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode()),
                false
            );
        }

    }
    else
    {
        // No rockets loaded - allow putdown with standard firing state delay logic
        float TimeTillPutDown = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle)
            * GetOuterAUTWeapon()->RefirePutDownTimePercent;

        if (TimeTillPutDown <= GetOuterAUTWeapon()->GetPutDownTime())
        {
            GetOuterAUTWeapon()->EarliestFireTime = GetWorld()->GetTimeSeconds() + TimeTillPutDown;
            UUTWeaponState::PutDown(); // Skip firing state's PutDown, go to base
        }
        else
        {
            TimeTillPutDown -= GetOuterAUTWeapon()->GetPutDownTime();
            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                PutDownHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::PutDown,
                TimeTillPutDown,
                false
            );
        }
    }
}
*/


void UUTWeaponStateFiringChargedRocket_Transactional::PutDown()
{
	// Mid-burst: wait for it to complete
	if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(FireLoadedRocketHandle))
	{
		return;
	}

	// Still charging: let them finish loading, switch will happen after firing
	if (bCharging)
	{
		return;
	}

	// Grace timer active: waiting to auto-fire, let it complete
	if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(GraceTimerHandle))
	{
		return;
	}

	// Have loaded rockets ready to fire: fire them, then switch
	if (RocketLauncher && RocketLauncher->NumLoadedRockets > 0)
	{
		int32 SafetyCounter = 0;
		while (RocketLauncher->NumLoadedRockets > 0)
		{
			int32 OldCount = RocketLauncher->NumLoadedRockets;
			RocketLauncher->FireShotDirect();

			if (RocketLauncher->NumLoadedRockets >= OldCount)
			{
				RocketLauncher->NumLoadedRockets--;
			}
			SafetyCounter++;
			if (SafetyCounter > 50)
			{
				RocketLauncher->NumLoadedRockets = 0;
				break;
			}
		}

		GetOuterAUTWeapon()->UnEquip();
		return;
	}

	// Nothing loaded, not charging: standard putdown with refire timing
	float TimeTillPutDown = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle)
		* GetOuterAUTWeapon()->RefirePutDownTimePercent;

	if (TimeTillPutDown <= GetOuterAUTWeapon()->GetPutDownTime())
	{
		GetOuterAUTWeapon()->EarliestFireTime = GetWorld()->GetTimeSeconds() + TimeTillPutDown;
		UUTWeaponState::PutDown();
	}
	else
	{
		TimeTillPutDown -= GetOuterAUTWeapon()->GetPutDownTime();
		GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
			PutDownHandle,
			this,
			&UUTWeaponStateFiringChargedRocket_Transactional::PutDown,
			TimeTillPutDown,
			false
		);
	}
}
