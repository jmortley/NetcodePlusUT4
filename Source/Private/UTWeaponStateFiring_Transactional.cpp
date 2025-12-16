#include "UTWeaponStateFiring_Transactional.h"
#include "UTWeaponFix.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTWeapon.h"
#include "UTBot.h"

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
	FireShot();
	GetOuterAUTWeapon()->bNetDelayedShot = false;
	
	// 4. START TIMER (CLIENT ONLY)
	// The Client needs this timer to:
	// A) Trigger the next 'FireShot' (which sends the next RPC)
	// B) Detect when you release the button so it can exit the state.



}

void UUTWeaponStateFiring_Transactional::EndState()
{
	// Clean up the timer immediately when leaving this state
	// This prevents RefireCheckTimer from ticking one last time after the user released the button
	if (GetOuterAUTWeapon())
	{
		GetOuterAUTWeapon()->GetWorldTimerManager().ClearTimer(RefireCheckHandle);
	}

	Super::EndState();
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

	if (GetOuterAUTWeapon()->HandleContinuedFiring())
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
			W->StopFire(W->GetCurrentFireMode());
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
	GetOuterAUTWeapon()->bNetDelayedShot = false;

	if (GetOuterAUTWeapon()->HandleContinuedFiring())
	{
		FireShot();
	}
	else
	{
		AUTWeaponFix* W = Cast<AUTWeaponFix>(GetOuterAUTWeapon());
		if (W && W->GetCurrentState() == this)
		{
			W->StopFire(W->GetCurrentFireMode());
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
