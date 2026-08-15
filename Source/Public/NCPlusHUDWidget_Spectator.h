// NCPlusHUDWidget_Spectator - ready-aware spectator status message.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget_Spectator.h"
#include "NCPlusHUDWidget_Spectator.generated.h"

/**
 * Preserves the stock spectator widget everywhere except ready-up's pre-match
 * wait. Casters and true spectators cannot start an NCPlus ready-up match with
 * Enter, so that one state reports replicated player readiness instead.
 */
UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_Spectator : public UUTHUDWidget_Spectator
{
	GENERATED_UCLASS_BODY()

public:
	virtual FText GetSpectatorMessageText(FText& ShortMessage) override;
};
