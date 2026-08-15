// NCPlusRoundSpectate.h
//
// Denies the free-roaming spectator camera to a player who is pawn-less in the
// MIDDLE of a live elimination round — the late joiner and the reconnecting
// player. Shared by ElimPlus and Wipeout, which both refuse to spawn a pawn
// mid-round and both leave such a player in NAME_Spectating with no view target,
// which is precisely the free-fly camera state.
//
// See NCPlusRoundSpectate.cpp for the full mechanism and why NAME_Inactive is the
// load-bearing part rather than the view target.

#pragma once

#include "CoreMinimal.h"

class AUTPlayerController;
class AUTPlayerState;

namespace NCPlusRoundSpectate
{
	/**
	 * True when this controller is a player who is pawn-less mid-round WITHOUT
	 * having died this round — i.e. someone who joined or reconnected into a live
	 * round. Dead players (bOutOfLives) are excluded: the mode's existing
	 * ForceTeamSpectate path already owns their camera.
	 *
	 * Callers must only ask while a round is actually live.
	 */
	bool ShouldLock(const AUTPlayerController* PC, const AUTPlayerState* PS);

	/**
	 * Take the free camera away from `PC` and park it on `TeammateToWatch` (may be
	 * null, in which case they simply get no camera). Idempotent — safe to call
	 * every sweep.
	 */
	void Lock(AUTPlayerController* PC, AUTPlayerState* TeammateToWatch);
}
