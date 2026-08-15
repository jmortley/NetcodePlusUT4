// NCPlusHUDWidget_Spectator.cpp - see header.
#include "NCPlusHUDWidget_Spectator.h"

#include "NCReadyUp.h"
#include "UTHUD.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"

UNCPlusHUDWidget_Spectator::UNCPlusHUDWidget_Spectator(const FObjectInitializer& OI)
	: Super(OI)
{
}

FText UNCPlusHUDWidget_Spectator::GetSpectatorMessageText(FText& ShortMessage)
{
	AUTPlayerState* PS = UTHUDOwner && UTHUDOwner->UTPlayerOwner
		? UTHUDOwner->UTPlayerOwner->UTPlayerState
		: nullptr;
	ANCReadyUpState* ReadyState = ANCReadyUpState::Find(GetWorld());
	if (ReadyState != nullptr && UTGameState != nullptr
		&& UTGameState->GetMatchState() == MatchState::WaitingToStart
		&& PS != nullptr && (PS->bCaster || PS->bOnlySpectator))
	{
		// Do not retain stock's "Press Enter" secondary text: ready-up has no
		// caster/host bypass, and the countdown is irreversible once locked.
		ShortMessage = FText::GetEmpty();
		if (ReadyState->bCountdownLocked)
		{
			return NSLOCTEXT("NCPlusReadyUp", "SpectatorMatchStarting",
				"Match is about to start");
		}

		FFormatNamedArguments Args;
		Args.Add(TEXT("ReadyCount"), FText::AsNumber(ReadyState->ReadyCount));
		Args.Add(TEXT("EligibleCount"), FText::AsNumber(ReadyState->EligibleCount));
		return FText::Format(NSLOCTEXT("NCPlusReadyUp", "SpectatorReadyCount",
			"{ReadyCount} / {EligibleCount} players ready"), Args);
	}

	return Super::GetSpectatorMessageText(ShortMessage);
}
