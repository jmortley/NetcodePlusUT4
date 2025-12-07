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


/*
void UUTWeaponStateFiring_Transactional::RefireCheckTimer()
{
	// --- CLIENT SIDE ---
	// We MUST keep the timer running here.
	// The Client uses this timer to trigger 'FireShot()', which generates 
	// the next 'ServerStartFireFixed' RPC.
	//if (GetOuterAUTWeapon()->Role < ROLE_Authority)
	//{
		// Call the parent logic to check ammo, input, and trigger FireShot()
	if (GetOuterAUTWeapon()->GetUTOwner())
	{
		AUTCharacter* UTC = GetOuterAUTWeapon()->GetUTOwner();
		AUTPlayerController* PC = UTC ? Cast<AUTPlayerController>(UTC->Controller) : nullptr;
		if (PC)
		{
			PC->LastShotTargetGuess = nullptr;
		}
		GetOuterAUTWeapon()->TargetedCharacter = nullptr;
	}
	Super::RefireCheckTimer();
	return;
	//}


}
*/

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


	// Check if we should continue firing (button still held, has ammo, etc.)
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
