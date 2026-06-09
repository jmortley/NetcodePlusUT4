// NCPlusScoreboardHost.h — shared "match host" badge for all NetcodePlus
// scoreboards. The host is the player who presses Enter to start the match.
// Identity source, in order: (1) ANCHostInfo->HostPS — plugin-replicated,
// layout-skew-proof (see NCPlusHostInfo.h for why the engine fields can't be
// trusted across server-binary vintages); (2) AUTPlayerState::bIsMatchHost +
// AUTGameState::HostIdString as engine fallbacks for old servers that don't
// spawn the info actor. Each scoreboard's DrawPlayer override calls
// DrawHostMarker just past the player's name so the host's own row is tagged.
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
