#include "UTPlusShockRifle.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTWeaponStateEquipping.h"
#include "Components/InputComponent.h"
#include "Engine.h"
#include "Engine/DemoNetDriver.h"

namespace
{
	FDelegateHandle GetInstagibEquipActionHandle(const FInputActionBinding& Binding)
	{
		// UE4.15 only exposes a mutable accessor. Inspect a copy so looking for
		// our no-argument observer never unbinds another action's key delegate.
		FInputActionUnifiedDelegate DelegateCopy = Binding.ActionDelegate;
		return DelegateCopy.GetDelegateForManualSet().GetHandle();
	}
}

void AUTPlusShockRifle::RefreshInstagibEquipInput()
{
	UWorld* World = GetWorld();
	const bool bEligibleOwner = World != nullptr && GetNetMode() != NM_DedicatedServer
		&& !IsPendingKillPending() && UTOwner != nullptr
		&& !UTOwner->IsPendingKillPending() && !UTOwner->IsDead()
		&& UTOwner->IsLocallyControlled() && UTOwner->IsPlayerControlled()
		&& UTOwner->GetWeapon() == this && HasSharedInstagibFireModes()
		&& !(World->DemoNetDriver && World->DemoNetDriver->IsPlaying());
	AUTPlayerController* PC = bEligibleOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr;
	UInputComponent* Component = PC != nullptr ? PC->InputComponent : nullptr;
	if (PC == nullptr || PC->GetPawn() != UTOwner || Component == nullptr)
	{
		StopInstagibEquipInput();
		return;
	}
	if (InstagibEquipInputController.Get() == PC
		&& InstagibEquipInputComponent.Get() == Component
		&& InstagibEquipPrimaryBindingHandle.IsValid()
		&& InstagibEquipAlternateBindingHandle.IsValid()
		&& InstagibEquipPrimaryReleaseHandle.IsValid()
		&& InstagibEquipAlternateReleaseHandle.IsValid())
	{
		return;
	}

	StopInstagibEquipInput();
	bool bHasPrimary = false;
	bool bHasAlternate = false;
	bool bHasPrimaryRelease = false;
	bool bHasAlternateRelease = false;
	for (int32 Index = 0; Index < Component->GetNumActionBindings(); ++Index)
	{
		const FInputActionBinding& Binding = Component->GetActionBinding(Index);
		if (!Binding.ActionDelegate.IsBoundToObject(PC))
		{
			continue;
		}
		bHasPrimary |= Binding.KeyEvent == IE_Pressed && Binding.ActionName == FName(TEXT("StartFire"));
		bHasAlternate |= Binding.KeyEvent == IE_Pressed && Binding.ActionName == FName(TEXT("StartAltFire"));
		bHasPrimaryRelease |= Binding.KeyEvent == IE_Released && Binding.ActionName == FName(TEXT("StopFire"));
		bHasAlternateRelease |= Binding.KeyEvent == IE_Released && Binding.ActionName == FName(TEXT("StopAltFire"));
	}
	if (!bHasPrimary || !bHasAlternate || !bHasPrimaryRelease || !bHasAlternateRelease)
	{
		return;
	}

	// Append to the existing component after its controller handlers. Observe
	// gameplay actions, not mouse keys, so rebound keys and gamepads work too.
	InstagibEquipInputController = PC;
	InstagibEquipInputComponent = Component;
	FInputActionBinding& Primary = Component->BindAction(TEXT("StartFire"), IE_Pressed,
		this, &AUTPlusShockRifle::InstagibEquipPrimaryPressed);
	Primary.bConsumeInput = false;
	Primary.bExecuteWhenPaused = false;
	InstagibEquipPrimaryBindingHandle = Primary.ActionDelegate.GetDelegateForManualSet().GetHandle();
	FInputActionBinding& Alternate = Component->BindAction(TEXT("StartAltFire"), IE_Pressed,
		this, &AUTPlusShockRifle::InstagibEquipAlternatePressed);
	Alternate.bConsumeInput = false;
	Alternate.bExecuteWhenPaused = false;
	InstagibEquipAlternateBindingHandle = Alternate.ActionDelegate.GetDelegateForManualSet().GetHandle();
	FInputActionBinding& PrimaryRelease = Component->BindAction(TEXT("StopFire"), IE_Released,
		this, &AUTPlusShockRifle::InstagibEquipPrimaryReleased);
	PrimaryRelease.bConsumeInput = false;
	PrimaryRelease.bExecuteWhenPaused = false;
	InstagibEquipPrimaryReleaseHandle = PrimaryRelease.ActionDelegate.GetDelegateForManualSet().GetHandle();
	FInputActionBinding& AlternateRelease = Component->BindAction(TEXT("StopAltFire"), IE_Released,
		this, &AUTPlusShockRifle::InstagibEquipAlternateReleased);
	AlternateRelease.bConsumeInput = false;
	AlternateRelease.bExecuteWhenPaused = false;
	InstagibEquipAlternateReleaseHandle = AlternateRelease.ActionDelegate.GetDelegateForManualSet().GetHandle();
}

void AUTPlusShockRifle::StopInstagibEquipInput()
{
	UInputComponent* Component = InstagibEquipInputComponent.Get();
	if (Component != nullptr)
	{
		for (int32 Index = Component->GetNumActionBindings() - 1; Index >= 0; --Index)
		{
			const FInputActionBinding& Binding = Component->GetActionBinding(Index);
			if (!Binding.ActionDelegate.IsBoundToObject(this))
			{
				continue;
			}
			const FDelegateHandle Handle = GetInstagibEquipActionHandle(Binding);
			const bool bOurPrimary = Binding.KeyEvent == IE_Pressed && Binding.ActionName == FName(TEXT("StartFire"))
				&& InstagibEquipPrimaryBindingHandle.IsValid() && Handle == InstagibEquipPrimaryBindingHandle;
			const bool bOurAlternate = Binding.KeyEvent == IE_Pressed && Binding.ActionName == FName(TEXT("StartAltFire"))
				&& InstagibEquipAlternateBindingHandle.IsValid() && Handle == InstagibEquipAlternateBindingHandle;
			const bool bOurPrimaryRelease = Binding.KeyEvent == IE_Released && Binding.ActionName == FName(TEXT("StopFire"))
				&& InstagibEquipPrimaryReleaseHandle.IsValid() && Handle == InstagibEquipPrimaryReleaseHandle;
			const bool bOurAlternateRelease = Binding.KeyEvent == IE_Released && Binding.ActionName == FName(TEXT("StopAltFire"))
				&& InstagibEquipAlternateReleaseHandle.IsValid() && Handle == InstagibEquipAlternateReleaseHandle;
			if (bOurPrimary || bOurAlternate || bOurPrimaryRelease || bOurAlternateRelease)
			{
				Component->RemoveActionBinding(Index);
			}
		}
	}
	InstagibEquipInputController.Reset();
	InstagibEquipInputComponent.Reset();
	InstagibEquipPrimaryBindingHandle.Reset();
	InstagibEquipAlternateBindingHandle.Reset();
	InstagibEquipPrimaryReleaseHandle.Reset();
	InstagibEquipAlternateReleaseHandle.Reset();
	InstagibEquipPressOwner.Reset();
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		bInstagibEquipPress[Mode] = false;
		InstagibEquipPressFrame[Mode] = 0;
	}
	++InstagibEquipInputSerial;
	ClearInstagibEquipTap();
}

void AUTPlusShockRifle::InstagibEquipPrimaryPressed()
{
	NoteInstagibEquipPress(0);
}

void AUTPlusShockRifle::InstagibEquipAlternatePressed()
{
	NoteInstagibEquipPress(1);
}

void AUTPlusShockRifle::InstagibEquipPrimaryReleased()
{
	NoteInstagibEquipRelease(0);
}

void AUTPlusShockRifle::InstagibEquipAlternateReleased()
{
	NoteInstagibEquipRelease(1);
}

void AUTPlusShockRifle::NoteInstagibEquipRelease(uint8 FireMode)
{
	// Observe action order, not deferred StopFire order: an older release in
	// the same batch must not turn a later repress-and-hold into a released tap.
	if (bInstagibTapAwaitingPossession && PendingInstagibEquipTapMode == FireMode)
	{
		bInstagibPossessionTapReleased = true;
	}
}

void AUTPlusShockRifle::NoteInstagibEquipPress(uint8 FireMode)
{
	// A fresh action supersedes a released tap even outside Equipping. Never
	// allow two actions in one frame to leave two independent buffered shots.
	++InstagibEquipInputSerial;
	ClearInstagibEquipTap();
	InstagibEquipPressOwner.Reset();
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		bInstagibEquipPress[Mode] = false;
		InstagibEquipPressFrame[Mode] = 0;
	}
	if (FireMode >= 2 || !CanRetainInstagibEquipTap(FireMode, true) || CurrentState != EquippingState)
	{
		return;
	}
	AUTPlayerController* PC = InstagibEquipInputController.Get();
	if (PC == nullptr || UTOwner->Controller != PC
		|| InstagibEquipInputComponent.Get() == nullptr
		|| InstagibEquipInputComponent.Get() != PC->InputComponent)
	{
		return;
	}
	if (PC->IsInState(NAME_Inactive))
	{
		// OnRep_Controller can expose the new living pawn and its rifle before
		// ClientRestart completes. Stock drops StartFire in this state, so a
		// same-frame provenance token alone cannot retain this physical press.
		// CanRetainInstagibEquipTap already requires this controller's current
		// pawn. Dispatch waits for Playing without reading acknowledgment state.
		PendingInstagibEquipTapMode = FireMode;
		PendingInstagibEquipTapOwner = UTOwner;
		PendingInstagibEquipTapController = PC;
		bInstagibTapAwaitingPossession = true;
		InstagibPossessionTapDeadline = GetWorld()->GetRealTimeSeconds() + 0.5f;
		return;
	}
	bInstagibEquipPress[FireMode] = true;
	InstagibEquipPressFrame[FireMode] = GFrameCounter;
	InstagibEquipPressOwner = UTOwner;
}

bool AUTPlusShockRifle::ConsumeInstagibEquipPress(uint8 FireMode)
{
	if (FireMode >= 2)
	{
		return false;
	}
	AUTPlayerController* PC = InstagibEquipInputController.Get();
	const bool bOwnsPress = bInstagibEquipPress[FireMode]
		&& InstagibEquipPressFrame[FireMode] == GFrameCounter
		&& InstagibEquipPressOwner.Get() == UTOwner
		&& PC != nullptr && UTOwner != nullptr && UTOwner->Controller == PC
		&& InstagibEquipInputComponent.Get() != nullptr
		&& InstagibEquipInputComponent.Get() == PC->InputComponent
		&& CurrentState == EquippingState && CanRetainInstagibEquipTap(FireMode);
	// A deferred/synthetic Start cannot reuse a previously accepted action or
	// revive one that arrived in another frame or equip/possession lifetime.
	bInstagibEquipPress[FireMode] = false;
	InstagibEquipPressFrame[FireMode] = 0;
	if (!bInstagibEquipPress[0] && !bInstagibEquipPress[1])
	{
		InstagibEquipPressOwner.Reset();
	}
	return bOwnsPress;
}
