// NCPlusScoreboardHost.cpp — see header.
#include "NCPlusScoreboardHost.h"
#include "NCReadyUp.h"
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "UTPlayerState.h"
#include "UTGameState.h"

namespace NCPlusScoreboardHost
{
	bool IsHost(AUTPlayerState* PS, AUTGameState* GS)
	{
		if (PS == nullptr || (GS != nullptr && ANCReadyUpState::Find(GS->GetWorld()) != nullptr))
		{
			return false;
		}
		// Warmup-only by design: the badge is most useful before the match starts
		// (so latecomers know who's about to press Enter); once InProgress hits, it
		// just clutters the scoreboard. Drop after WaitingToStart so it disappears
		// for the live match, halves, OT, and end-of-match.
		if (GS == nullptr || GS->GetMatchState() != MatchState::WaitingToStart)
		{
			return false;
		}
		// Primary: engine flag set when the host (HostId player) is present.
		if (PS->bIsMatchHost)
		{
			return true;
		}
		// Fallback: the replicated HostIdString covers the warmup window before
		// ReadyToStartMatch assigns bIsMatchHost, and any mode that doesn't run
		// that path. Bots have no UniqueId so they never match.
		if (!GS->HostIdString.IsEmpty() && PS->UniqueId.IsValid())
		{
			return GS->HostIdString.Equals(PS->UniqueId.ToString(), ESearchCase::IgnoreCase);
		}
		return false;
	}

	void DrawHostMarker(UUTHUDWidget* Scoreboard, AUTHUD* HUD, AUTPlayerState* PS,
		AUTGameState* GS, float X, float Y, float RenderScale)
	{
		if (Scoreboard == nullptr || HUD == nullptr || !IsHost(PS, GS))
		{
			return;
		}
		static const FText HostText = NSLOCTEXT("NCPlusScoreboard", "HostTag", "HOST");
		// Gold badge so it reads as special — identical on every NetcodePlus scoreboard.
		Scoreboard->DrawText(HostText, X, Y, HUD->TinyFont, RenderScale, 1.0f,
			FLinearColor(1.f, 0.84f, 0.1f, 1.f), ETextHorzPos::Left, ETextVertPos::Center);
	}
}
