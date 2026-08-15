#pragma once

#include "CoreMinimal.h"

class AUTPlayerState;
class UWorld;

namespace NCPlusScoreboardReady
{
	/**
	 * Resolves the pre-match scoreboard label when NCPlus player ready-up is active.
	 * Returns false when no ready-up state exists so callers can preserve their
	 * stock scoreboard behavior.
	 */
	NETCODEPLUS_API bool TryGetText(UWorld* World, AUTPlayerState* PlayerState,
		const FText& TeamSwitchText, FText& OutText);
}
