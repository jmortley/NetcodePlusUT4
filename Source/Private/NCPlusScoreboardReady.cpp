#include "NCPlusScoreboardReady.h"

#include "NCReadyUp.h"
#include "UTPlayerState.h"

bool NCPlusScoreboardReady::TryGetText(UWorld* World, AUTPlayerState* PlayerState,
	const FText& TeamSwitchText, FText& OutText)
{
	ANCReadyUpState* ReadyUpState = ANCReadyUpState::Find(World);
	if (!ReadyUpState)
	{
		return false;
	}

	if (PlayerState && PlayerState->bPendingTeamSwitch)
	{
		OutText = TeamSwitchText;
	}
	else if (PlayerState && (PlayerState->bIsABot || ReadyUpState->IsPlayerReady(PlayerState)))
	{
		OutText = NSLOCTEXT("NCPlusScoreboardReady", "Ready", "READY");
	}
	else
	{
		OutText = NSLOCTEXT("NCPlusScoreboardReady", "NotReady", "NOT READY");
	}

	return true;
}
