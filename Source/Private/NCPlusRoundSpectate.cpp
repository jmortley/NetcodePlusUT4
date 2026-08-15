// NCPlusRoundSpectate.cpp
//
// THE BUG. A player who connects (or reconnects) while an elimination round is
// running gets a free-flying spectator camera and can fly the map, through
// geometry, and read every enemy position for the rest of the round. It clears
// itself at the next round start, which is what makes it usable: disconnect and
// reconnect during a clutch, read the round, come back next round knowing where
// everyone is.
//
// WHY IT HAPPENS. A PlayerController is born in NAME_Spectating with
// bPlayerIsWaiting set (Engine PlayerController.cpp, PostInitializeComponents) —
// StateName is assigned directly so the spectator pawn is merely deferred, not
// avoided. PostLogin then reaches AGameMode::HandleStartingNewPlayer, which calls
// RestartPlayer; both AElimPlusGame::RestartPlayer and AUWipeoutGame::RestartPlayer
// refuse to spawn a pawn mid-round (correct for round-based play). Nothing then
// changes the controller's state and nothing assigns it a view target, so on the
// client ReceivedPlayer -> BeginSpectatingState -> SpawnSpectatorPawn runs and
// AutoManageActiveCameraTarget points the camera at that spectator pawn. Free
// flight, immediately, without the player pressing anything.
//
// A player who DIES never hits this: AUTPlayerController::PawnPendingDestroy puts
// them in NAME_Inactive, and ~2s later the mode's own ForceTeamSpectate moves them
// to NAME_Spectating *with* an explicit view target on a living team-mate. The late
// joiner inherits the state without the camera assignment.
//
// WHY NAME_Inactive IS THE FIX, NOT A VIEW TARGET. Setting a view target alone is
// not enough, for two independent reasons:
//   1. The spectator pawn is spawned and moved entirely on the client and is
//      explicitly non-replicated, so the server cannot see or reach it. A view
//      target we set at PostLogin can be overridden a moment later by the client's
//      own BeginSpectatingState, and the server never learns.
//   2. Even after a correct assignment, AUTPlayerController::ServerViewSelf puts
//      the player straight back on the free camera. Its only guard is
//      IsInState(NAME_Spectating).
// Moving the controller to NAME_Inactive addresses both: leaving NAME_Spectating
// destroys the client's spectator pawn, and ServerViewSelf refuses outright from
// any other state. That refusal is server-side, so it holds against a modified
// client too — which a view-target assignment never would.
//
// SCOPE. This deliberately does not touch players who died this round: they keep
// today's exact behaviour. Note that they too sit in NAME_Spectating and can
// therefore still ServerViewSelf onto the free camera — a real but separate hole,
// pre-existing and shared with stock UT4, which changing here would also change
// how every dead player spectates. Worth its own change with its own testing.

#include "NCPlusRoundSpectate.h"

#include "GameFramework/Pawn.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"

bool NCPlusRoundSpectate::ShouldLock(const AUTPlayerController* PC, const AUTPlayerState* PS)
{
	if (!PC || !PS)
	{
		return false;
	}

	// Casters and dedicated spectators are supposed to fly the map.
	if (PS->bOnlySpectator)
	{
		return false;
	}

	// Holding a pawn means playing; the camera follows the pawn.
	if (PC->GetPawn())
	{
		return false;
	}

	// Died this round: ForceTeamSpectate already owns this player's camera, and
	// their view is information their own team paid for. bOutOfLives is exactly the
	// discriminator we need — the mode sets it when a player is eliminated, so a
	// pawn-less player WITHOUT it never played this round at all.
	if (PS->bOutOfLives)
	{
		return false;
	}

	return true;
}

void NCPlusRoundSpectate::Lock(AUTPlayerController* PC, AUTPlayerState* TeammateToWatch)
{
	if (!PC)
	{
		return;
	}

	// The load-bearing line (see header comment): out of NAME_Spectating, so the
	// client's free-flying spectator pawn is destroyed and ServerViewSelf will
	// refuse to hand it back. Guarded so the sweep is idempotent — re-entering the
	// same state every second would re-run BeginInactiveState's timers.
	if (!PC->IsInState(NAME_Inactive))
	{
		PC->ChangeState(NAME_Inactive);
		PC->ClientGotoState(NAME_Inactive);
	}

	// Give them the same camera a dead team-mate would have: a living team-mate in
	// first person. If nobody on their team is alive there is nothing legal left to
	// show, and they simply get no camera until the next round spawns them — that
	// case only arises when the round is already over for their team.
	if (TeammateToWatch)
	{
		if (AUTCharacter* TeammateChar = TeammateToWatch->GetUTCharacter())
		{
			PC->SetViewTarget(TeammateChar);
			PC->bSpectateBehindView = false;
			PC->BehindView(false);
		}
	}
}
