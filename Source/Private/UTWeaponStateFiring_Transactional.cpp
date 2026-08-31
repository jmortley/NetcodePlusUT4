#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTBot.h"
#include "UTPlusWeap_RocketLauncher.h"
#include "HAL/IConsoleManager.h"

static FORCEINLINE int32 RocketPrimaryDiagLevelTransactional()
{
	static IConsoleVariable* DiagVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ncp.RocketPrimaryDiag"));
	return DiagVar ? DiagVar->GetInt() : 0;
}

static FORCEINLINE bool RocketPrimaryDiagTransactional(AUTWeapon* Weapon)
{
	const int32 Level = RocketPrimaryDiagLevelTransactional();
	return Weapon != nullptr && Level > 0
		&& Cast<AUTPlusWeap_RocketLauncher>(Weapon) != nullptr
		&& (Weapon->GetCurrentFireMode() == 0 || Level >= 2);
}

UUTWeaponStateFiring_Transactional::UUTWeaponStateFiring_Transactional(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}




void UUTWeaponStateFiring_Transactional::BeginState(const UUTWeaponState* PrevState)
{

	// 1. Setup Standard Effects
	ToggleLoopingEffects(true);
	PendingFireSequence = -1;
	bDelayShot = false;

	// 2. Notify Weapon
	GetOuterAUTWeapon()->OnStartedFiring();

	// 3. Fire the First Shot (Immediate)
	if (GetOuterAUTWeapon()->GetNetMode() != NM_DedicatedServer &&
		GetOuterAUTWeapon()->GetUTOwner() &&
		GetOuterAUTWeapon()->GetUTOwner()->IsLocallyControlled())
	{
		float RefireTime = GetOuterAUTWeapon()->GetRefireTime(GetOuterAUTWeapon()->GetCurrentFireMode());
		GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(RefireCheckHandle, this, &UUTWeaponStateFiring_Transactional::RefireCheckTimer, RefireTime, true);
	}
	if (RocketPrimaryDiagTransactional(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		const float Now = W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f;
		const float Lft = Cast<AUTWeaponFix>(W) && Cast<AUTWeaponFix>(W)->LastFireTime.IsValidIndex(0)
			? Cast<AUTWeaponFix>(W)->LastFireTime[0] : -1.f;
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] TX_BEGIN_STATE frame=%u t=%.4f role=%d net=%d local=%d state=%p prev=%s currentMode=%d pending0=%d lft0=%.4f refire=%.4f timerRate=%.4f timerRemain=%.4f"),
			(uint32)GFrameCounter, Now, (int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0, this,
			PrevState ? *PrevState->GetClass()->GetName() : TEXT("null"), W->GetCurrentFireMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			Lft, W->GetRefireTime(0), W->GetWorldTimerManager().GetTimerRate(RefireCheckHandle),
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}
	FireShot();
	GetOuterAUTWeapon()->bNetDelayedShot = false;
	
	// 4. START TIMER (CLIENT ONLY)
	// The Client needs this timer to:
	// A) Trigger the next 'FireShot' (which sends the next RPC)
	// B) Detect when you release the button so it can exit the state.



}

void UUTWeaponStateFiring_Transactional::EndState()
{
	if (RocketPrimaryDiagTransactional(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] TX_END_STATE frame=%u t=%.4f role=%d net=%d local=%d state=%p currentMode=%d pending0=%d timerRemain=%.4f"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0, this,
			W->GetCurrentFireMode(), (W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}
	if (GetOuterAUTWeapon())
	{
		GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(RefireCheckHandle);
	}

	// Manual cleanup - same as parent but defer ClearFiringInfo
	bDelayShot = false;
	ToggleLoopingEffects(false);
	GetOuterAUTWeapon()->OnStoppedFiring();
	GetOuterAUTWeapon()->StopFiringEffects();

	// Defer ClearFiringInfo to next frame so visual can render
	AUTCharacter* Owner = GetOuterAUTWeapon()->GetUTOwner();
	if (Owner)
	{
		TWeakObjectPtr<AUTCharacter> WeakOwner = Owner;
		TWeakObjectPtr<AUTWeapon> WeakWeapon = GetOuterAUTWeapon();
		TWeakObjectPtr<UUTWeaponStateFiring_Transactional> WeakExitedState = this;
		const uint8 ExpectedFlashCount = Owner->FlashCount;
		const uint8 ExpectedFlashLocationCount = Owner->FlashLocation.Count;
		const FVector ExpectedFlashLocation = Owner->FlashLocation.Position;
		const uint8 ExpectedFlashExtra = Owner->FlashExtra;
		const uint8 ExpectedFireMode = Owner->FireMode;
		const bool bExpectedLocalFlashLocation = Owner->bLocalFlashLoc;

		Owner->GetWorldTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateLambda([WeakOwner, WeakWeapon, WeakExitedState,
				ExpectedFlashCount, ExpectedFlashLocationCount, ExpectedFlashLocation,
				ExpectedFlashExtra, ExpectedFireMode, bExpectedLocalFlashLocation]()
				{
					AUTCharacter* CurrentOwner = WeakOwner.Get();
					if (CurrentOwner == nullptr)
					{
						return;
					}

					// Pawn firing data is global across weapons. Clear only the tuple
					// observed by the state that scheduled this callback; any change
					// means a newer shot/state now owns it.
					if (CurrentOwner->FlashCount != ExpectedFlashCount
						|| CurrentOwner->FlashLocation.Count != ExpectedFlashLocationCount
						|| CurrentOwner->FlashLocation.Position != ExpectedFlashLocation
						|| CurrentOwner->FlashExtra != ExpectedFlashExtra
						|| CurrentOwner->FireMode != ExpectedFireMode
						|| CurrentOwner->bLocalFlashLoc != bExpectedLocalFlashLocation)
					{
						return;
					}

					// State objects are reused. A same-weapon re-entry before the next
					// tick owns the unchanged tuple, so leave it intact as well.
					AUTWeapon* OldWeapon = WeakWeapon.Get();
					if (OldWeapon && CurrentOwner->GetWeapon() == OldWeapon
						&& WeakExitedState.IsValid()
						&& OldWeapon->GetCurrentState() == WeakExitedState.Get())
					{
						return;
					}

					CurrentOwner->ClearFiringInfo();
				})
		);
	}

	GetOuterAUTWeapon()->GetWorldTimerManager().ClearAllTimersForObject(this);
}



void UUTWeaponStateFiring_Transactional::PutDown()
{
	// Ensure any delayed logic (like pending replicated shots) is processed first
	HandleDelayedShot();

	AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
	if (!W)
	{
		Super::PutDown();
		return;
	}

	// 1. Calculate cooldown using timestamps (Works on Server & Client)
	float TimeRemaining = 0.f;
	uint8 Mode = GetOuterAUTWeapon()->GetCurrentFireMode();

	// Check LastFireTime to determine when the weapon is actually ready
	if (W->LastFireTime.IsValidIndex(Mode) && W->LastFireTime[Mode] > 0.f)
	{
		float ReadyTime = W->LastFireTime[Mode] + GetOuterAUTWeapon()->GetRefireTime(Mode);
		TimeRemaining = FMath::Max(0.f, ReadyTime - GetWorld()->GetTimeSeconds());
	}

	// 2. Calculate the penalty overlap
	// (If the cooldown is longer than the PutDown animation, we must wait)
	float TimeTillPutDown = TimeRemaining * GetOuterAUTWeapon()->RefirePutDownTimePercent;

	if (TimeTillPutDown <= GetOuterAUTWeapon()->GetPutDownTime())
	{
		// CASE A: Cooldown is short enough to be hidden by the PutDown animation.
		// We can start unequipping immediately, but we set a flag (EarliestFireTime)
		// to prevent the *next* weapon from firing until this penalty is paid.

		GetOuterAUTWeapon()->EarliestFireTime = GetWorld()->GetTimeSeconds() + TimeTillPutDown;

		// CRITICAL: Call UUTWeaponState::PutDown (Grandparent).
		// We skip UUTWeaponStateFiring::PutDown (Parent) entirely because it relies
		// on the broken RefireCheckTimer logic.
		UUTWeaponState::PutDown();
	}
	else
	{
		// CASE B: Cooldown is too long. We must physically wait before lowering the weapon.
		float WaitDuration = TimeTillPutDown - GetOuterAUTWeapon()->GetPutDownTime();

		// Schedule this function to run again exactly when the wait is over.
		// When it runs again, TimeRemaining will be smaller, falling into Case A.
		GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(
			PutDownHandle,
			this,
			&UUTWeaponStateFiring_Transactional::PutDown, // Correct callback
			WaitDuration,
			false
		);
	}
}




void UUTWeaponStateFiring_Transactional::RefireCheckTimer()
{
	// --- CLIENT SIDE ONLY ---
	// This should ONLY run on the client to check for continued firing input
	// and send RPCs to the server

	if (GetOuterAUTWeapon()->GetNetMode() == NM_DedicatedServer)
	{
		// Server should never run this timer in transactional mode
		GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(RefireCheckHandle);
		return;
	}

	// Clear cached aim data for fresh targeting
	if (GetOuterAUTWeapon()->GetUTOwner() == nullptr)
	{
		GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(RefireCheckHandle);
		return;
	}

	AUTWeapon* DiagWeapon = GetOuterAUTWeapon();
	const bool bRocketDiag = RocketPrimaryDiagTransactional(DiagWeapon);
	AUTCharacter* PreOwner = nullptr;
	uint8 PreMode = 0;
	UUTWeaponState* PreState = nullptr;
	bool bPrePending0 = false;
	bool bPrePendingCurrent = false;
	bool bPrePendingWeapon = false;
	bool bPreHasAmmo = false;
	bool bPreRootBlocked = false;
	if (bRocketDiag)
	{
		PreOwner = DiagWeapon->GetUTOwner();
		PreMode = DiagWeapon->GetCurrentFireMode();
		PreState = DiagWeapon->GetCurrentState();
		bPrePending0 = PreOwner && PreOwner->IsPendingFire(0);
		bPrePendingCurrent = PreOwner && PreOwner->IsPendingFire(PreMode);
		bPrePendingWeapon = PreOwner && PreOwner->GetPendingWeapon() != nullptr;
		bPreHasAmmo = DiagWeapon->AmmoCost.IsValidIndex(PreMode)
			&& DiagWeapon->Ammo >= FMath::Min(1, DiagWeapon->AmmoCost[PreMode]);
		bPreRootBlocked = DiagWeapon->bRootWhileFiring && PreOwner
			&& PreOwner->GetCharacterMovement() != nullptr
			&& PreOwner->GetCharacterMovement()->MovementMode != MOVE_Falling;
	}

	const bool bContinued = DiagWeapon->HandleContinuedFiring();
	if (bRocketDiag)
	{
		AUTWeapon* W = DiagWeapon;
		AUTWeaponFix* Fix = Cast<AUTWeaponFix>(W);
		const float Now = W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f;
		const float Lft = Fix && Fix->LastFireTime.IsValidIndex(0) ? Fix->LastFireTime[0] : -1.f;
		const float Ready = Lft > 0.f ? Lft + W->GetRefireTime(0) : -1.f;
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] TX_REFIRE frame=%u t=%.4f wep=%p player=%s role=%d net=%d local=%d continued=%d preMode=%d preState=%s postState=%s prePending0=%d prePendingCurrent=%d prePendingWpn=%d preAmmoOK=%d preRootBlocked=%d postPending0=%d lft0=%.4f ready=%.4f lateBy=%.4f timerRate=%.4f timerRemain=%.4f"),
			(uint32)GFrameCounter, Now, W,
			(PreOwner && PreOwner->PlayerState) ? *PreOwner->PlayerState->PlayerName : TEXT("?"),
			(int32)W->Role, (int32)W->GetNetMode(), (PreOwner && PreOwner->IsLocallyControlled()) ? 1 : 0,
			bContinued ? 1 : 0, PreMode,
			PreState ? *PreState->GetClass()->GetName() : TEXT("null"),
			W->GetCurrentState() ? *W->GetCurrentState()->GetClass()->GetName() : TEXT("null"),
			bPrePending0 ? 1 : 0, bPrePendingCurrent ? 1 : 0, bPrePendingWeapon ? 1 : 0,
			bPreHasAmmo ? 1 : 0, bPreRootBlocked ? 1 : 0,
			(W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			Lft, Ready, Ready >= 0.f ? Now - Ready : -1.f,
			W->GetWorldTimerManager().GetTimerRate(RefireCheckHandle),
			W->GetWorldTimerManager().GetTimerRemaining(RefireCheckHandle));
	}

	if (bContinued)
	{
		FireShot();
	}
	else
	{
		// 2. JAM PROTECTION (Sniper Only)
		// We use a string check so we don't need to mess with header includes/dependencies
		bool bIsSniper = GetOuterAUTWeapon()->GetClass()->GetName().Contains(TEXT("Sniper"));

		if (bIsSniper)
		{
			// Check if player is holding the button
			uint8 CurrentMode = GetOuterAUTWeapon()->GetCurrentFireMode();
			bool bIsHoldingFire = false;
			if (GetOuterAUTWeapon()->GetUTOwner())
			{
				bIsHoldingFire = GetOuterAUTWeapon()->GetUTOwner()->IsPendingFire(CurrentMode);
			}

			if (bIsHoldingFire)
			{
				// RETRY LOGIC:
				// Sniper logic: If holding button but blocked by cooldown (Timer Jitter), wait 1 frame.
				GetOuterAUTWeapon()->GetWorldTimerManager().SetTimer(RefireCheckHandle, this, &UUTWeaponStateFiring_Transactional::RefireCheckTimer, 0.01f, true);
				return; // Exit here so we don't call StopFire
			}
		}

		// 3. STANDARD STOP (Shock Rifle, Link Gun, etc.)
		// If it's not a sniper, OR if the player released the button, we stop naturally.
		AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
		if (W)
		{
			W->StopFireInternal(W->GetCurrentFireMode());
		}
	}

	/* Check if we should continue firing(button still held, has ammo, etc.)
	if (GetOuterAUTWeapon()->HandleContinuedFiring())
	{
		// Send the next shot RPC to server - don't fire locally
		FireShot(); // This will send the RPC but won't do damage on client
	}
	else
	{
		// Stop firing - go to active state
		AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
		if (W)
		{
			W->StopFire(W->GetCurrentFireMode());
		}
		//GetOuterAUTWeapon()->GotoActiveState();
	}
	*/
}

void UUTWeaponStateFiring_Transactional::TransactionalFire()
{
	const bool bWasNetDelayedShot = GetOuterAUTWeapon()->bNetDelayedShot;
	GetOuterAUTWeapon()->bNetDelayedShot = false;

	const bool bContinued = GetOuterAUTWeapon()->HandleContinuedFiring();
	if (RocketPrimaryDiagTransactional(GetOuterAUTWeapon()))
	{
		AUTWeapon* W = GetOuterAUTWeapon();
		UE_LOG(LogTemp, Warning,
			TEXT("[RocketM1Diag] TX_SERVER_DISPATCH frame=%u t=%.4f role=%d net=%d local=%d continued=%d pending0=%d currentMode=%d state=%p wasNetDelayed=%d"),
			(uint32)GFrameCounter, W->GetWorld() ? W->GetWorld()->GetTimeSeconds() : -1.f,
			(int32)W->Role, (int32)W->GetNetMode(),
			(W->GetUTOwner() && W->GetUTOwner()->IsLocallyControlled()) ? 1 : 0,
			bContinued ? 1 : 0, (W->GetUTOwner() && W->GetUTOwner()->IsPendingFire(0)) ? 1 : 0,
			W->GetCurrentFireMode(), this, bWasNetDelayedShot ? 1 : 0);
	}

	if (bContinued)
	{
		FireShot();
	}
	else
	{
		AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
		if (W && W->GetCurrentState() == this)
		{
			W->StopFireInternal(W->GetCurrentFireMode());
		}
		//GetOuterAUTWeapon()->GotoActiveState();
	}

	// Reset flag
	GetOuterAUTWeapon()->bNetDelayedShot = false;
}


void UUTWeaponStateFiring_Transactional::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);  // IMPORTANT: allows parent HandleDelayedShot()

	//if (GetOuterAUTWeapon()->GetNetMode() == NM_DedicatedServer)
	//{
	//	if (!GetOuterAUTWeapon()->HandleContinuedFiring())
	//	{
	//		GetOuterAUTWeapon()->GotoActiveState();
	//	}
	//}
}
