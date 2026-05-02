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
        // FIX: Clear visual state flags to prevent stale HUD text
        RocketLauncher->CurrentRocketFireMode = 0;
        RocketLauncher->bDrawRocketModeString = false;
        
        if (RocketLauncher->Role == ROLE_Authority)
        {
            RocketLauncher->SetRocketFlashExtra(
                RocketLauncher->GetCurrentFireMode(), 0, 0, false);
        }
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



void UUTWeaponStateFiringChargedRocket_Transactional::EndFiringSequence(uint8 FireModeNum)
{
    if (FireModeNum != GetFireMode()) return;

    // Re-entry guard: if a burst is already in progress (FireLoadedRocketHandle
    // timer scheduled by a prior EndFiringSequence call this frame), drop this
    // call. This closes the LoadTimer + ServerStopFire RPC race where two
    // callers reach EndFiringSequence in the same server tick: each invocation
    // would fire one rocket via FireLoadedRocket and schedule an overlapping
    // timer, producing a same-frame double-spawn. The asymmetric timing
    // (server: 2 instant + remainder timed; client: all timed at 150ms)
    // caused server's end-of-burst CRFM=0 reset to replicate to the client
    // mid-burst and corrupt the next client fake to spawn as a rocket
    // (orphan). The OnRep_CurrentRocketFireMode guard on the client catches
    // this defensively, but blocking the race here removes the source.
    //
    // The guard is correct because:
    //  - First EndFiringSequence call passes (timer not yet active), starts
    //    the burst, FireLoadedRocket schedules timer for the next shot.
    //  - Same-frame re-entry sees active timer, returns; the just-loaded
    //    extra rocket (if LoadTimer fired this same tick) is still picked
    //    up because NumLoadedRockets is checked at each timer tick.
    //  - Legitimate "load completes after release" path (LoadTimer fires
    //    on a tick where no burst is in progress) is unaffected because
    //    that scenario has the timer cleared between calls.
    if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(FireLoadedRocketHandle))
    {
        UE_LOG(LogTemp, Verbose, TEXT("[NCFire] EndFiringSequence re-entry blocked (LoadTimer + StopFire race) Loaded=%d"),
            RocketLauncher ? RocketLauncher->NumLoadedRockets : -1);
        return;
    }

    bCharging = false;


    // --- HELPER LAMBDA FOR BUFFERED FIRE ---
    // Fixes the "Dead End" where holding M1 after a cancelled load does nothing.
    auto AttemptBufferedFire = [this]()
        {
            if (GetUTOwner() && GetUTOwner()->IsPendingFire(0))
            {
                TWeakObjectPtr<AUTWeapon> WeakWeapon = GetOuterAUTWeapon();

                // Syntax Fix: [Capture List] (Params) { Body }
                GetOuterAUTWeapon()->GetWorldTimerManager().SetTimerForNextTick(
                    FTimerDelegate::CreateLambda([WeakWeapon]()
                        {
                            if (WeakWeapon.IsValid() && WeakWeapon->GetUTOwner() &&
                                WeakWeapon->GetUTOwner()->IsPendingFire(0))
                            {
                                // Fix: Reset the lock flag so StartFire doesn't reject the input
                                AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(WeakWeapon.Get());
                                if (FixWeapon)
                                {
                                    // Use the public setter or direct access if you made it public
                                    // FixWeapon->CurrentlyFiringMode = 255; 
                                    FixWeapon->ResetFiringModeTracker();
                                }

                                WeakWeapon->StartFire(0);
                            }
                        })
                );
            }
        };

    // --- FIX START: DYNAMIC GHOST ROCKET COMPENSATION ---
    if (RocketLauncher && RocketLauncher->NumLoadedRockets <= 0 && GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(LoadTimerHandle))
    {
        float Remaining = GetOuterAUTWeapon()->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle);
        float RTT_ms = (GetUTOwner() && GetUTOwner()->PlayerState) ? GetUTOwner()->PlayerState->ExactPing : 0.0f;

        // Convert RTT to One-Way Seconds + Jitter Buffer
        float Tolerance = FMath::Clamp((RTT_ms * 0.0005f) + 0.02f, 0.0f, 0.20f);

        if (Remaining < Tolerance)
        {
            GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);
            LoadTimer();
            return;
        }
        else
        {
            // Legit early release. 
            // DO NOT abort the state here! Let the LoadTimer finish naturally
            // so the single rocket fires and triggers the global refire cooldown.
            return;
        }
    }
    // --- FIX END ---

    if (!RocketLauncher)
    {
        GetOuterAUTWeapon()->GotoActiveState();
        AttemptBufferedFire(); // <--- Check input
        return;
    }

    if (RocketLauncher->NumLoadedRockets <= 0)
    {
        // Player released before first rocket loaded. Let LoadTimer finish.
        return;
    }

    AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
    if (GameState && (GameState->HasMatchEnded() || GameState->IsMatchIntermission()))
    {
        RocketLauncher->NumLoadedRockets = 0;
        GetOuterAUTWeapon()->GotoActiveState();
        AttemptBufferedFire(); // <--- Check input
        return;
    }

    // Normal path: Fire the rockets.
    // NOTE: The transition logic for a SUCCESSFUL fire happens in RefireCheckTimer().
    FireLoadedRocket();
}




void UUTWeaponStateFiringChargedRocket_Transactional::FireLoadedRocket()
{
    if (!RocketLauncher || RocketLauncher->NumLoadedRockets <= 0)
    {
        // Done firing - cleanup
        ChargeTime = 0.0f;
        AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
        if (W && W->LastFireTime.IsValidIndex(GetFireMode()))
        {
            W->LastFireTime[GetFireMode()] = GetWorld()->GetTimeSeconds();
        }

        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(GraceTimerHandle);
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);

        if (GetOuterAUTWeapon()->GetCurrentState() == this)
        {
            // --- SYNC REFIRE TIMER ---
            // Ensure the Refire Timer strictly respects the timestamp set in FireShotDirect.
            // This prevents "Drift" where the timer expires slightly before IsFireModeOnCooldown thinks it should.
            float RefireDelay = GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode());

            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                RefireCheckHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::RefireCheckTimer,
                RefireDelay,
                false
            );
        }
        return;
    }

    // Fire one rocket using stock UT logic (bypasses UTWeaponFix transactional)
    RocketLauncher->FireShotDirect();

    // Decide if we need to wait before firing the next one.
    // Per-mode interval check — restoring previously-removed logic. Without this,
    // grenades fall through to the instant-dump while-loop because BurstInterval
    // is 0 (rockets dump instantly). Grenades need GrenadeBurstInterval (0.1s)
    // timed bursts; firing all 3 in one frame causes ghost rocket fakes on the
    // client due to prediction/replication race on CurrentRocketFireMode.
    if (RocketLauncher->NumLoadedRockets > 0)
    {
        float IntervalToCheck = RocketLauncher->BurstInterval;
        if (RocketLauncher->CurrentRocketFireMode == 1)
        {
            IntervalToCheck = RocketLauncher->GrenadeBurstInterval;
        }
        else if (RocketLauncher->CurrentRocketFireMode == 2)
        {
            IntervalToCheck = RocketLauncher->SpiralBurstInterval;
        }

        if (IntervalToCheck <= 0.f || RocketLauncher->ShouldFireLoad())
        {
            // INSTANT DUMP — only for modes with 0 interval (Spread, by default)
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
        }
        else
        {
            // TIMED BURST — Grenades (and any mode with non-zero interval).
            // The timer schedules the next FireLoadedRocket call; one projectile
            // per call lets CurrentRocketFireMode replicate cleanly between shots.
            GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
                FireLoadedRocketHandle,
                this,
                &UUTWeaponStateFiringChargedRocket_Transactional::FireLoadedRocket,
                IntervalToCheck,
                false
            );
            return;
        }
    }


    // All rockets fired
    ChargeTime = 0.0f;
    GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(GraceTimerHandle);
    GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(LoadTimerHandle);
    AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
    if (W && W->LastFireTime.IsValidIndex(GetFireMode()))
    {
        W->LastFireTime[GetFireMode()] = GetWorld()->GetTimeSeconds();
    }

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
    // 1. Integrity Check
    if (GetOuterAUTWeapon()->GetCurrentState() != this) return;

    // 2. Switch Check
    if (GetUTOwner() && GetUTOwner()->GetPendingWeapon() != nullptr)
    {
        GetOuterAUTWeapon()->GotoActiveState();
        return;
    }

    // AI Check
    AUTBot* B = Cast<AUTBot>(GetUTOwner() ? GetUTOwner()->Controller : nullptr);
    if (B != nullptr) B->CheckWeaponFiring();

    if (GetUTOwner() == nullptr) return;

    // -----------------------------------------------------------
    // LOGIC START
    // -----------------------------------------------------------

    // A. PRIORITY 1: CONTINUE CHARGING (Mode 1)
    if (GetOuterAUTWeapon()->HandleContinuedFiring())
    {
        bCharging = true;
        BeginState(this);
        return;
    }

    // B. CLEANUP (Fixes the "Client 1: 1" stuck log)
    // Force the Transactional Gatekeeper to reset so it accepts new input
    AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
    if (FixWeapon)
    {
        FixWeapon->ResetFiringModeTracker();
        // Uses the new public helper
    }

    // C. PRIORITY 2: SWITCH TO PRIMARY (Mode 0)
    // Critical Fix: prevent ActiveState from firing a "Ghost Shot" that ignores cooldowns
    bool bWantsPrimary = GetUTOwner()->IsPendingFire(0);

    if (bWantsPrimary)
    {
        // 1. Temporarily hide input so ActiveState doesn't auto-fire immediately
        GetUTOwner()->SetPendingFire(0, false);

        // 2. Transition to Active (safe now because input is hidden)
        GetOuterAUTWeapon()->GotoActiveState();

        // 3. Restore input and trigger Transactional Fire
        GetUTOwner()->SetPendingFire(0, true);

        GetOuterAUTWeapon()->StartFire(0);
        return;
    }

    // D. NO INPUT - Return to Idle
    GetOuterAUTWeapon()->GotoActiveState();
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





void UUTWeaponStateFiringChargedRocket_Transactional::PutDown()
{
	// Owner-gone guard. PutDown can be deferred via PutDownHandle (see the
	// SetTimer below); if the player dies between defer and fire, the weapon's
	// UTOwner is null by the time we get back here. The only legal transition
	// for an owner-less weapon is to InactiveState — anything else trips the
	// "Attempt to send X to state Y while not owned" ensure in
	// AUTWeapon::GotoState. Was reproducible on death with loaded rockets:
	//   [DeathRelease] Firing N loaded rockets on death
	//   Ensure failed: UTOwner != NULL || NewState == InactiveState
	{
		AUTWeapon* Wpn = GetOuterAUTWeapon();
		if (!Wpn || Wpn->GetUTOwner() == nullptr)
		{
			if (Wpn && Wpn->InactiveState)
			{
				Wpn->GotoState(Wpn->InactiveState);
			}
			return;
		}
	}

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
