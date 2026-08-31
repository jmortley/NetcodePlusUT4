// NCPlusBetaTopBar - shared code-native tactical ribbon for Wipeout/ElimPlus.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

class AUTHUD;
class UCanvas;

enum class ENCPlusBetaTopBarSide : uint8
{
	Left = 0,
	Right = 1,
};

/** Mode-owned values rendered inside the shared score / clock / score core.
 *  Team colors and scores are already ordered by presentation side: callers
 *  remain responsible for viewer-relative red/blue swapping. */
struct NETCODEPLUS_API FNCPlusBetaTopBarCore
{
	int32 LeftScore = 0;
	int32 RightScore = 0;
	int32 ClockSeconds = -1;
	FString CenterFallback = TEXT("VS");
	FLinearColor LeftTeamColor = FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	FLinearColor RightTeamColor = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);
};

/** Resolved screen-space geometry for one complete Beta ribbon. The nearest
 *  left portrait ends at LeftPortraitAnchorX; the nearest right portrait starts
 *  at RightPortraitAnchorX. Portrait ordinals then grow away from the score core. */
struct NETCODEPLUS_API FNCPlusBetaTopBarGeometry
{
	FVector2D Center = FVector2D::ZeroVector;
	float TopY = 0.f;
	float UnitScale = 1.f;

	float CoreLeft = 0.f;
	float CoreRight = 0.f;
	float CoreHeight = 0.f;

	float PortraitWidth = 0.f;
	float PortraitHeight = 0.f;
	float PortraitPitch = 0.f;
	float PortraitY = 0.f;
	float LeftPortraitAnchorX = 0.f;
	float RightPortraitAnchorX = 0.f;
	int32 PortraitCount[2] = { 0, 0 };

	FVector2D GetPortraitPosition(ENCPlusBetaTopBarSide Side, int32 Ordinal) const;
};

namespace NCPlusBetaTopBar
{
	/** True only when the opt-in ribbon is enabled and the active HUD actually
	 *  renders it. Keep editor/drag behavior on the same mode gate as DrawHUD so
	 *  a Wipeout -> CTF/Duel travel cannot hide the legacy portrait handles. */
	NETCODEPLUS_API bool IsActiveForHUD(const class AUTHUD* HUD);

	/** Resolve the scorebar alias and derive a single connected 1080p-design-space
	 *  composition. The scorebar Scale and global HUD scale resize core and cards
	 *  together. Hidden is deliberately not consulted: portrait placement remains
	 *  stable when a user hides only the score/clock core. */
	NETCODEPLUS_API bool BuildGeometry(class AUTHUD* HUD, class UCanvas* Canvas,
		int32 LeftPortraitCount, int32 RightPortraitCount,
		FNCPlusBetaTopBarGeometry& OutGeometry);

	/** Draw the connected chamfered chassis, portrait-slot backplates, accent rails,
	 *  and compact score/clock core. Portrait images and mode-specific overlays are
	 *  drawn later by the owning HUD at the positions returned by BuildGeometry. */
	NETCODEPLUS_API void DrawChassisAndScoreCore(class AUTHUD* HUD, class UCanvas* Canvas,
		const FNCPlusBetaTopBarGeometry& Geometry,
		const FNCPlusBetaTopBarCore& Core);
}
