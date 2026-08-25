#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTPlusWeap_RocketLauncher.h"
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTBot.h"
#include "HAL/IConsoleManager.h"
// UTWeaponStateFiringChargedRocket_Transactional.cpp

static FORCEINLINE bool RocketPrimaryChargedDiag(AUTWeapon* Weapon)
{
	static IConsoleVariable* DiagVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ncp.RocketPrimaryDiag"));
	return Weapon != nullptr && DiagVar && DiagVar->GetInt() >= 2
		&& Cast<AUTPlusWeap_RocketLauncher>(Weapon) != nullptr;
}


UUTWeaponStateFiringChargedRocket_Transactional::UUTWeaponStateFiringChargedRocket_Transactional(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    bCharging = false;
    ChargeTime = 0.0f;
    RocketLauncher = nullptr;
    bReleaseRequested = false;
    bReleaseCommitted = false;
    bCompletingLoadTimer = false;
    bDuplicateReleaseLogged = false;
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
        GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(PutDownHandle);
    }
}

void UUTWeaponStateFiringChargedRocket_Transactional::BeginState(const UUTWeaponState* PrevState)
{
	// State objects are reused. RefireCheckTimer can begin the next charge without
	// EndState(), so release idempotency must be reset here, not only in EndState().
	bReleaseRequested = false;
	bReleaseCommitted = false;
	bCompletingLoadTimer = false;
	bDuplicateReleaseLogged = false;

	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		AUTPlusWeap_RocketLauncher* RL = Cast<AUTPlusWeap_RocketLauncher>(W);
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_BEGIN frame=%u t=%.4f role=%d net=%d local=%d state=%p prev=%s fireMode=%d currentMode=%d pending0=%d pending1=%d charging=%d loadedR=%d loadedB=%d"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0, this,
			PrevState ? *PrevState->GetClass()->GetName() : TEXT("null"), GetFireMode(), W->GetCurrentFireMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(1)) ? 1 : 0,
			bCharging ? 1 : 0, RL ? RL->NumLoadedRockets : -1, RL ? RL->NumLoadedBarrels : -1);
	}

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

	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* DiagWeapon = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_ARM frame=%u t=%.4f role=%d net=%d player=%s loadedR=%d loadedB=%d maxR=%d firstLoad=%.4f nextLoad=%.4f grace=%.4f timerRate=%.4f timerRemain=%.4f"),
			(uint32)GFrameCounter,
			DiagWeapon->GetWorld() ? DiagWeapon->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)DiagWeapon->Role, (int32)DiagWeapon->GetNetMode(),
			(DiagWeapon->GetUTOwner() && DiagWeapon->GetUTOwner()->PlayerState)
				? *DiagWeapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
			RocketLauncher->NumLoadedRockets, RocketLauncher->NumLoadedBarrels,
			RocketLauncher->MaxLoadedRockets,
			RocketLauncher->FirstRocketLoadTime, RocketLauncher->RocketLoadTime,
			RocketLauncher->GracePeriod,
			DiagWeapon->GetWorldTimerManager().GetTimerRate(LoadTimerHandle),
			DiagWeapon->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle));
	}

    // NOTE: We do NOT call FireShot() here like the standard Transactional state does.
    // We wait for the player to release the button or for the grace timer to fire.

    // NOTE: We do NOT set RefireCheckHandle here - charging doesn't use refire timing.
}

void UUTWeaponStateFiringChargedRocket_Transactional::EndState()
{
	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		AUTPlusWeap_RocketLauncher* RL = Cast<AUTPlusWeap_RocketLauncher>(W);
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_END frame=%u t=%.4f role=%d net=%d local=%d state=%p fireMode=%d currentMode=%d pending0=%d pending1=%d charging=%d releaseReq=%d releaseCommit=%d completingLoad=%d loadedR=%d loadedB=%d timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0, this,
			GetFireMode(), W->GetCurrentFireMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(1)) ? 1 : 0,
			bCharging ? 1 : 0, bReleaseRequested ? 1 : 0, bReleaseCommitted ? 1 : 0,
			bCompletingLoadTimer ? 1 : 0,
			RL ? RL->NumLoadedRockets : -1, RL ? RL->NumLoadedBarrels : -1,
			W->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(GraceTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(FireLoadedRocketHandle),
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}

    // 1. Clear all timers
    ClearAllTimers();

    // 2. Reset charging state
    ChargeTime = 0.0f;
    bCharging = false;
    bReleaseRequested = false;
    bReleaseCommitted = false;
    bCompletingLoadTimer = false;
    bDuplicateReleaseLogged = false;

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
    AUTWeapon* Weapon = GetOuterAUTWeapon();
    if (!Weapon)
    {
        return;
    }

    FTimerManager& TimerManager = Weapon->GetWorldTimerManager();
    const float CallbackRemaining = TimerManager.GetTimerRemaining(LoadTimerHandle);

    // UE4.15 keeps a due timer discoverable as "active" while its delegate is
    // executing and reports 0.0 seconds remaining. Consume and invalidate the
    // handle before EndLoadRocket(): release code must never mistake this callback
    // for a second pending load that it can complete again.
    TimerManager.ClearTimer(LoadTimerHandle);

    if (bCompletingLoadTimer)
    {
        UE_LOG(LogTemp, Warning, TEXT("[RocketLoadGuard] Ignored re-entrant LoadTimer for %s"),
            *Weapon->GetName());
        return;
    }

    if (!RocketLauncher || RocketLauncher != Weapon || !GetUTOwner()
        || Weapon->GetCurrentState() != this)
    {
        return;
    }

    const int32 LoadedRocketsBefore = RocketLauncher->NumLoadedRockets;
    const int32 LoadedBarrelsBefore = RocketLauncher->NumLoadedBarrels;

    if (RocketPrimaryChargedDiag(Weapon))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[RocketM1Diag] CHARGED_LOAD_CALLBACK frame=%u t=%.4f role=%d net=%d player=%s owns=%d charging=%d releaseReq=%d releaseCommit=%d loadedR=%d loadedB=%d callbackRemain=%.4f handleActiveAfterClear=%d"),
            (uint32)GFrameCounter, Weapon->GetWorld() ? Weapon->GetWorld()->GetTimeSeconds() : -1.f,
            (int32)Weapon->Role, (int32)Weapon->GetNetMode(),
            (Weapon->GetUTOwner() && Weapon->GetUTOwner()->PlayerState)
                ? *Weapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
            Weapon->GetCurrentState() == this ? 1 : 0, bCharging ? 1 : 0,
            bReleaseRequested ? 1 : 0, bReleaseCommitted ? 1 : 0,
            RocketLauncher->NumLoadedRockets, RocketLauncher->NumLoadedBarrels,
            CallbackRemaining, TimerManager.IsTimerActive(LoadTimerHandle) ? 1 : 0);
    }

    // EndLoadRocket can synchronously call back into StopFire through last-ammo
    // weapon switching or bot logic. During that narrow scope EndFiringSequence
    // may latch release intent, but CommitRelease waits until this mutation ends.
    {
        TGuardValue<bool> CompletingLoadGuard(bCompletingLoadTimer, true);
        RocketLauncher->EndLoadRocket();
    }

    if (RocketPrimaryChargedDiag(Weapon))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[RocketM1Diag] CHARGED_LOAD_COMMIT frame=%u t=%.4f role=%d net=%d player=%s owns=%d beforeR=%d afterR=%d beforeB=%d afterB=%d charging=%d releaseReq=%d releaseCommit=%d"),
            (uint32)GFrameCounter, Weapon->GetWorld() ? Weapon->GetWorld()->GetTimeSeconds() : -1.f,
            (int32)Weapon->Role, (int32)Weapon->GetNetMode(),
            (Weapon->GetUTOwner() && Weapon->GetUTOwner()->PlayerState)
                ? *Weapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
            Weapon->GetCurrentState() == this ? 1 : 0,
            LoadedRocketsBefore, RocketLauncher->NumLoadedRockets,
            LoadedBarrelsBefore, RocketLauncher->NumLoadedBarrels,
            bCharging ? 1 : 0, bReleaseRequested ? 1 : 0,
            bReleaseCommitted ? 1 : 0);
    }

    // EndLoadRocket may have switched/removed the weapon. Never schedule or fire
    // from a state that no longer owns the weapon.
    if (Weapon->GetCurrentState() != this || !RocketLauncher || !GetUTOwner())
    {
        return;
    }

    if (!bCharging || bReleaseRequested)
    {
        bCharging = false;
        bReleaseRequested = true;
        CommitRelease();
        return;
    }

    // Check if we're fully loaded.
    if (RocketLauncher->NumLoadedBarrels >= RocketLauncher->MaxLoadedRockets)
    {
        // Fully loaded - start grace timer. The grace timeout is the only
        // intentional auto-release in this state.
        if (GetUTOwner() && !GetUTOwner()->IsLocallyControlled() && GetWorld()->GetNetMode() != NM_Client)
        {
            RocketLauncher->ClientAbortLoad();
        }

        if (!TimerManager.IsTimerActive(GraceTimerHandle))
        {
            TimerManager.SetTimer(
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
        // Still held: begin exactly one next load and give its timer ownership
        // to the next callback. Release never calls this callback directly.
        RocketLauncher->BeginLoadRocket();

        const float LoadTime = RocketLauncher->GetLoadTime(RocketLauncher->NumLoadedRockets);
        TimerManager.SetTimer(
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

void UUTWeaponStateFiringChargedRocket_Transactional::ExitToActiveAndAttemptBufferedPrimary()
{
    AUTWeapon* Weapon = GetOuterAUTWeapon();
    AUTCharacter* Owner = GetUTOwner();
    if (!Weapon || !Owner)
    {
        return;
    }

    // ActiveState::BeginState checks PendingFire immediately. Hide primary across
    // the transition so it cannot auto-dispatch once here and again in our retry.
    const bool bWantsPrimary = Owner->IsPendingFire(0);
    if (bWantsPrimary)
    {
        Owner->SetPendingFire(0, false);
    }
    Weapon->GotoActiveState();

    Owner = Weapon->GetUTOwner();
    if (!Owner || !bWantsPrimary)
    {
        return;
    }

    // Restore held intent even when a weapon switch is pending; only this weapon's
    // explicit retry is suppressed in that case so the holstered weapon cannot fire.
    // A different pending mode may also have re-entered firing synchronously from
    // ActiveState; never inject primary on top of that state.
    Owner->SetPendingFire(0, true);
    if (Owner->GetWeapon() != Weapon || Owner->GetPendingWeapon() != nullptr
        || Weapon->IsFiring())
    {
        return;
    }

    TWeakObjectPtr<AUTWeapon> WeakWeapon = Weapon;
    Weapon->GetWorldTimerManager().SetTimerForNextTick(
        FTimerDelegate::CreateLambda([WeakWeapon]()
        {
            AUTCharacter* CurrentOwner = WeakWeapon.IsValid() ? WeakWeapon->GetUTOwner() : nullptr;
            if (!WeakWeapon.IsValid() || !CurrentOwner || !CurrentOwner->IsPendingFire(0)
                || CurrentOwner->GetWeapon() != WeakWeapon.Get()
                || CurrentOwner->GetPendingWeapon() != nullptr
                || WeakWeapon->IsFiring())
            {
                return;
            }

            // The charged state is finished. Release the transactional mode
            // tracker before handing a genuinely held primary back to StartFire.
            if (AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(WeakWeapon.Get()))
            {
                FixWeapon->ResetFiringModeTracker();
            }
            WeakWeapon->StartFire(0);
        }));
}

void UUTWeaponStateFiringChargedRocket_Transactional::CommitRelease()
{
    if (bReleaseCommitted || bCompletingLoadTimer)
    {
        return;
    }

    AUTWeapon* Weapon = GetOuterAUTWeapon();
    if (!Weapon || Weapon->GetCurrentState() != this || !GetUTOwner())
    {
        return;
    }

    FTimerManager& TimerManager = Weapon->GetWorldTimerManager();
    const int32 LoadedRockets = RocketLauncher ? RocketLauncher->NumLoadedRockets : 0;
    const int32 LoadedBarrels = RocketLauncher ? RocketLauncher->NumLoadedBarrels : 0;

    if (RocketLauncher && LoadedRockets <= 0 && TimerManager.IsTimerActive(LoadTimerHandle))
    {
        // A tap before rocket one completes is still guaranteed one rocket. The
        // existing timer owns that completion; no ping estimate or direct callback
        // is allowed to promote it. LoadTimer() will return here after completing it.
        if (RocketPrimaryChargedDiag(Weapon))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[RocketM1Diag] CHARGED_RELEASE_WAIT_FIRST frame=%u t=%.4f role=%d net=%d player=%s loadedR=%d loadedB=%d loadRemain=%.4f"),
                (uint32)GFrameCounter, Weapon->GetWorld() ? Weapon->GetWorld()->GetTimeSeconds() : -1.f,
                (int32)Weapon->Role, (int32)Weapon->GetNetMode(),
                (Weapon->GetUTOwner() && Weapon->GetUTOwner()->PlayerState)
                    ? *Weapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
                LoadedRockets, LoadedBarrels, TimerManager.GetTimerRemaining(LoadTimerHandle));
        }
        return;
    }

    // Commit before any call that can synchronously re-enter weapon/state logic.
    // One or more completed rockets form the release snapshot. A partly loaded
    // next rocket is cancelled, not promoted, regardless of ping.
    bReleaseCommitted = true;
    const float CancelledLoadRemaining = TimerManager.GetTimerRemaining(LoadTimerHandle);
    TimerManager.ClearTimer(LoadTimerHandle);
    TimerManager.ClearTimer(GraceTimerHandle);

    if (RocketPrimaryChargedDiag(Weapon))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[RocketM1Diag] CHARGED_RELEASE_COMMIT frame=%u t=%.4f role=%d net=%d player=%s owns=%d loadedR=%d loadedB=%d cancelledLoadRemain=%.4f burstActive=%d refireActive=%d"),
            (uint32)GFrameCounter, Weapon->GetWorld() ? Weapon->GetWorld()->GetTimeSeconds() : -1.f,
            (int32)Weapon->Role, (int32)Weapon->GetNetMode(),
            (Weapon->GetUTOwner() && Weapon->GetUTOwner()->PlayerState)
                ? *Weapon->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
            Weapon->GetCurrentState() == this ? 1 : 0, LoadedRockets, LoadedBarrels,
            CancelledLoadRemaining,
            TimerManager.IsTimerActive(FireLoadedRocketHandle) ? 1 : 0,
            TimerManager.IsTimerActive(RefireCheckHandle) ? 1 : 0);
    }

    if (!RocketLauncher)
    {
        ExitToActiveAndAttemptBufferedPrimary();
        return;
    }

    if (RocketLauncher->NumLoadedRockets <= 0)
    {
        // No load timer and no completed rocket is an invalid/abandoned charged
        // state. Exit rather than waiting forever with IsFiring() still true.
        ExitToActiveAndAttemptBufferedPrimary();
        return;
    }

    AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
    if (GameState && (GameState->HasMatchEnded() || GameState->IsMatchIntermission()))
    {
        RocketLauncher->NumLoadedRockets = 0;
        // Grace auto-release can leave secondary marked pending. ActiveState scans
        // every pending mode and can bypass StartFire's match gate, so suppress all
        // fire intent while leaving the charged state at match end/intermission.
        if (GetUTOwner())
        {
            GetUTOwner()->SetPendingFire(0, false);
            GetUTOwner()->SetPendingFire(1, false);
        }
        Weapon->GotoActiveState();
        return;
    }

    // FireLoadedRocket owns the burst and then arms this charged state's refire
    // timer. Generic Stop-RPC cleanup must not clear or race those timers.
    FireLoadedRocket();
}



void UUTWeaponStateFiringChargedRocket_Transactional::EndFiringSequence(uint8 FireModeNum)
{
	if (FireModeNum != GetFireMode())
	{
		return;
	}

	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_RELEASE_RX frame=%u t=%.4f role=%d net=%d player=%s requestedMode=%d fireMode=%d currentMode=%d owns=%d pending0=%d pending1=%d charging=%d releaseReq=%d releaseCommit=%d completingLoad=%d loadedR=%d loadedB=%d timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->PlayerState)
				? *W->GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
			FireModeNum, GetFireMode(), W->GetCurrentFireMode(),
			W->GetCurrentState() == this ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(1)) ? 1 : 0,
			bCharging ? 1 : 0, bReleaseRequested ? 1 : 0, bReleaseCommitted ? 1 : 0,
			bCompletingLoadTimer ? 1 : 0, RocketLauncher ? RocketLauncher->NumLoadedRockets : -1,
			RocketLauncher ? RocketLauncher->NumLoadedBarrels : -1,
			W->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(GraceTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(FireLoadedRocketHandle),
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}

    // 328 intentionally receives the same physical release through the stock
    // Stop RPC (needed to order against retried stock Start RPCs) and the fixed
    // Stop RPC. The first notification latches intent; CommitRelease is separately
    // idempotent so a later notification can only resume an uncommitted release.
    if (bReleaseRequested)
    {
        if (!bDuplicateReleaseLogged && RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
        {
            bDuplicateReleaseLogged = true;
            UE_LOG(LogTemp, Warning,
                TEXT("[RocketM1Diag] CHARGED_RELEASE_DUPLICATE frame=%u t=%.4f role=%d player=%s committed=%d loadedR=%d loadedB=%d"),
                (uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
                GetOuterAUTWeapon() ? (int32)GetOuterAUTWeapon()->Role : -1,
                (GetUTOwner() && GetUTOwner()->PlayerState)
                    ? *GetUTOwner()->PlayerState->PlayerName : TEXT("?"),
                bReleaseCommitted ? 1 : 0,
                RocketLauncher ? RocketLauncher->NumLoadedRockets : -1,
                RocketLauncher ? RocketLauncher->NumLoadedBarrels : -1);
        }
        if (!bReleaseCommitted && !bCompletingLoadTimer)
        {
            CommitRelease();
        }
        return;
    }

    bReleaseRequested = true;
    bCharging = false;

    // EndLoadRocket is not re-entrant-safe. If its ammo/bot callbacks produced
    // this Stop, record the intent now and let LoadTimer commit after it returns.
    if (bCompletingLoadTimer)
    {
        if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[RocketM1Diag] CHARGED_RELEASE_DEFER_LOAD frame=%u t=%.4f role=%d loadedR=%d loadedB=%d"),
                (uint32)GFrameCounter, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f,
                GetOuterAUTWeapon() ? (int32)GetOuterAUTWeapon()->Role : -1,
                RocketLauncher ? RocketLauncher->NumLoadedRockets : -1,
                RocketLauncher ? RocketLauncher->NumLoadedBarrels : -1);
        }
        return;
    }

    CommitRelease();
}

void UUTWeaponStateFiringChargedRocket_Transactional::RecoverWedgedRelease()
{
    AUTWeapon* Weapon = GetOuterAUTWeapon();
    if (!Weapon || Weapon->GetCurrentState() != this || !GetUTOwner()
        || bCompletingLoadTimer)
    {
        return;
    }

    FTimerManager& TimerManager = Weapon->GetWorldTimerManager();
    if (TimerManager.IsTimerActive(LoadTimerHandle)
        || TimerManager.IsTimerActive(GraceTimerHandle)
        || TimerManager.IsTimerActive(FireLoadedRocketHandle)
        || TimerManager.IsTimerActive(RefireCheckHandle))
    {
        return;
    }

    bCharging = false;
    bReleaseRequested = true;
    if (!bReleaseCommitted)
    {
        CommitRelease();
    }
    else if (RocketLauncher && RocketLauncher->NumLoadedRockets > 0)
    {
        // A committed timed burst lost its continuation timer. Resume only the
        // logical rockets that remain; never consult the stale barrel count.
        FireLoadedRocket();
    }
}




void UUTWeaponStateFiringChargedRocket_Transactional::FireLoadedRocket()
{
	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_FIRE_LOADED frame=%u t=%.4f role=%d net=%d fireMode=%d currentMode=%d owns=%d pending0=%d charging=%d loadedR=%d loadedB=%d"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(), GetFireMode(), W->GetCurrentFireMode(),
			W->GetCurrentState() == this ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			bCharging ? 1 : 0, RocketLauncher ? RocketLauncher->NumLoadedRockets : -1,
			RocketLauncher ? RocketLauncher->NumLoadedBarrels : -1);
	}

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
	if (RocketPrimaryChargedDiag(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] CHARGED_REFIRE frame=%u t=%.4f role=%d net=%d local=%d fireMode=%d currentMode=%d owns=%d pending0=%d pending1=%d charging=%d loadedR=%d loadedB=%d timers(load=%.4f grace=%.4f burst=%.4f refire=%.4f)"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0,
			GetFireMode(), W->GetCurrentFireMode(), W->GetCurrentState() == this ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(1)) ? 1 : 0,
			bCharging ? 1 : 0, RocketLauncher ? RocketLauncher->NumLoadedRockets : -1,
			RocketLauncher ? RocketLauncher->NumLoadedBarrels : -1,
			W->GetWorldTimerManager().GetTimerRemaining(LoadTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(GraceTimerHandle),
			W->GetWorldTimerManager().GetTimerRemaining(FireLoadedRocketHandle),
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}

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
	// UTOwner is null by the time we get back here. Any state transition would
	// trip "Attempt to send X to state Y while not owned" in AUTWeapon::GotoState
	// (only InactiveState is allowed without an owner, and it's protected with
	// no public accessor). Just bail — the weapon is about to be destroyed by
	// AUTCharacter::DiscardAllInventory anyway. Repro:
	//   [DeathRelease] Firing N loaded rockets on death
	//   Ensure failed: UTOwner != NULL || NewState == InactiveState
	if (AUTWeapon* Wpn = GetOuterAUTWeapon())
	{
		if (Wpn->GetUTOwner() == nullptr)
		{
			return;
		}
	}
	else
	{
		return;
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

	// A release before rocket one completes is no longer "charging", but the
	// load callback still owns the guaranteed first rocket. While EndLoadRocket
	// executes its handle has already been consumed, so the synchronous guard is
	// checked separately from the pending-timer case. A weapon switch must not
	// tear down the state before that callback commits the release.
	if (bCompletingLoadTimer)
	{
		return;
	}
	if (bReleaseRequested && !bReleaseCommitted)
	{
		if (GetOuterAUTWeapon()->GetWorldTimerManager().IsTimerActive(LoadTimerHandle))
		{
			return;
		}
		// Defensive recovery for an uncommitted release whose timer disappeared:
		// use the same idempotent path, never PutDown's direct-volley shortcut.
		CommitRelease();
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
