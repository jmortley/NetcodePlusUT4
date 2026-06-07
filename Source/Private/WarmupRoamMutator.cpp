// WarmupRoamMutator.cpp - see header.
#include "WarmupRoamMutator.h"
#include "UnrealTournament.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTGameState.h"

AWarmupRoamMutator::AWarmupRoamMutator(const FObjectInitializer& OI)
	: Super(OI)
{
	DisplayName = NSLOCTEXT("NCPlus", "WarmupRoam", "Warmup Roam");
}

bool AWarmupRoamMutator::IsWarmup() const
{
	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	return GS != nullptr && GS->GetMatchState() == MatchState::WaitingToStart;
}

bool AWarmupRoamMutator::IsRoaming(AController* C) const
{
	for (const TWeakObjectPtr<AController>& W : RoamingControllers)
	{
		if (W.Get() == C)
		{
			return true;
		}
	}
	return false;
}

void AWarmupRoamMutator::SetRoaming(AController* C, bool bRoam)
{
	// Drop stale + any existing entry for C first, then re-add if enabling.
	RoamingControllers.RemoveAll([C](const TWeakObjectPtr<AController>& W)
	{
		return !W.IsValid() || W.Get() == C;
	});
	if (bRoam && C != nullptr)
	{
		RoamingControllers.Add(C);
	}
}

void AWarmupRoamMutator::ApplyRoam(AUTCharacter* Char, bool bEnable)
{
	if (Char == nullptr)
	{
		return;
	}
	// bDamageHurtsHealth=false -> TakeDamage applies no health damage (server gate).
	// DisallowWeaponFiring(true) -> weapon fire paths bail on IsFiringDisabled().
	Char->bDamageHurtsHealth = !bEnable;
	Char->DisallowWeaponFiring(bEnable);
}

void AWarmupRoamMutator::ClearAll()
{
	for (const TWeakObjectPtr<AController>& W : RoamingControllers)
	{
		if (AController* C = W.Get())
		{
			ApplyRoam(Cast<AUTCharacter>(C->GetPawn()), false);
		}
	}
	RoamingControllers.Empty();
}

void AWarmupRoamMutator::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	if (Sender != nullptr && MutateString.Equals(TEXT("warmup"), ESearchCase::IgnoreCase))
	{
		if (!IsWarmup())
		{
			Sender->ClientMessage(TEXT("Warmup roam is only available during warmup."));
		}
		else
		{
			const bool bNowRoaming = !IsRoaming(Sender);
			SetRoaming(Sender, bNowRoaming);
			ApplyRoam(Cast<AUTCharacter>(Sender->GetPawn()), bNowRoaming);
			Sender->ClientMessage(bNowRoaming
				? TEXT("Warmup roam ON - invulnerable, firing disabled. Ends when the match starts.")
				: TEXT("Warmup roam OFF."));
		}
		// fall through to Super so the rest of the mutator chain still sees the command.
	}

	Super::Mutate_Implementation(MutateString, Sender);
}

void AWarmupRoamMutator::ModifyPlayer_Implementation(APawn* Other, bool bIsNewSpawn)
{
	Super::ModifyPlayer_Implementation(Other, bIsNewSpawn);

	// Firing-disable + invuln reset to defaults on every spawn, so re-assert for
	// roamers while we're still in warmup.
	if (IsWarmup())
	{
		AUTCharacter* Char = Cast<AUTCharacter>(Other);
		if (Char != nullptr && IsRoaming(Char->GetController()))
		{
			ApplyRoam(Char, true);
		}
	}
}

void AWarmupRoamMutator::NotifyMatchStateChange_Implementation(FName NewState)
{
	Super::NotifyMatchStateChange_Implementation(NewState);

	// Roam is warmup-only. The instant we leave WaitingToStart (countdown OR match
	// start), strip it from everyone so godmode can never carry into live play.
	if (NewState != MatchState::WaitingToStart)
	{
		ClearAll();
	}
}
