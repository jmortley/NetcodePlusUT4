// NCPlusScoreboardHost.h — shared "match host" badge for all NetcodePlus
// scoreboards. The host is the player who presses Enter to start the match:
// AUTPlayerState::bIsMatchHost (assigned in AUTGameMode::ReadyToStartMatch),
// with the replicated AUTGameState::HostIdString as a fallback for the warmup
// window before bIsMatchHost is set. Each scoreboard's DrawPlayer override calls
// DrawHostMarker just past the player's name so the host's own row is tagged.
// The badge is suppressed when the server uses player ready-up.
#pragma once

#include "CoreMinimal.h"

class UUTHUDWidget;
class AUTHUD;
class AUTPlayerState;
class AUTGameState;

namespace NCPlusScoreboardHost
{
	/** True when PS is the match host (the Enter-to-start player). Null-safe. */
	NETCODEPLUS_API bool IsHost(AUTPlayerState* PS, AUTGameState* GS);

	/** Draws the gold "HOST" tag at (X,Y) when PS is the host; no-op otherwise.
	 *  X should sit just right of the player's name, Y on the name baseline.
	 *  Scoreboard supplies the DrawText context (RenderPosition / scale). */
	NETCODEPLUS_API void DrawHostMarker(UUTHUDWidget* Scoreboard, AUTHUD* HUD,
		AUTPlayerState* PS, AUTGameState* GS, float X, float Y, float RenderScale);
}
