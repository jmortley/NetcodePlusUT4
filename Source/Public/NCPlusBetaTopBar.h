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
	// Portrait aliases can opt out of the scorebar's dynamic team palette. Keep
	// their rails/card frames independent from the score/clock panels so every
	// nchud Team Color toggle has one unambiguous owner.
	FLinearColor LeftPortraitColor = FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	FLinearColor RightPortraitColor = FLinearColor(0.05f, 0.1f, 0.9f, 1.f);
};

/** One independently configurable portrait bank. Scorebar position/scale remains
 *  the parent transform; the portrait alias contributes a relative X/Y adjustment,
 *  size multiplier, opacity, font and team-color choice. */
struct NETCODEPLUS_API FNCPlusBetaTopBarPortraitGeometry
{
	FName Alias = NAME_None;
	float UnitScale = 1.f;
	float Width = 0.f;
	float Height = 0.f;
	float Pitch = 0.f;
	float Y = 0.f;
	float AnchorX = 0.f;
	float Opacity = 1.f;
	int32 Count = 0;
};

/** Resolved screen-space geometry for one complete Beta ribbon. Each bank stores
 *  the inside-edge anchor nearest the score core; portrait ordinals grow outward. */
struct NETCODEPLUS_API FNCPlusBetaTopBarGeometry
{
	FVector2D Center = FVector2D::ZeroVector;
	float TopY = 0.f;
	float UnitScale = 1.f;

	float CoreLeft = 0.f;
	float CoreRight = 0.f;
	float CoreHeight = 0.f;

	FNCPlusBetaTopBarPortraitGeometry Portrait[2];

	FVector2D GetPortraitPosition(ENCPlusBetaTopBarSide Side, int32 Ordinal) const;
	const FNCPlusBetaTopBarPortraitGeometry& GetPortraitGeometry(
		ENCPlusBetaTopBarSide Side) const;
};

namespace NCPlusBetaTopBar
{
	/** True only when the opt-in ribbon is enabled and the active HUD actually
	 *  renders it. Keep editor/drag behavior on the same mode gate as DrawHUD so
	 *  a Wipeout -> CTF/Duel travel cannot hide the legacy portrait handles. */
	NETCODEPLUS_API bool IsActiveForHUD(const class AUTHUD* HUD);

	/** Resolve the scorebar parent transform plus per-side portrait alias controls
	 *  into one connected 1080p-design-space composition. Hidden is deliberately
	 *  not consulted: portrait placement remains stable when a user hides only the
	 *  score/clock core. */
	NETCODEPLUS_API bool BuildGeometry(class AUTHUD* HUD, class UCanvas* Canvas,
		int32 LeftPortraitCount, int32 RightPortraitCount,
		FName LeftPortraitAlias, FName RightPortraitAlias,
		FNCPlusBetaTopBarGeometry& OutGeometry);

	/** Compatibility overload for callers that use fixed red/blue presentation. */
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
