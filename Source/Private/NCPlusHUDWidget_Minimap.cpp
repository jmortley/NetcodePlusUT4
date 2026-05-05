// NCPlusHUDWidget_Minimap.cpp — calls into UTHUD::DrawMinimap with
// user-configured position + size from the nchud layout.
//
// Layout extras honoured:
//   "size"   (float, design pixels) — square minimap edge length, default 220.
//   "alpha"  (0..255 int OR 0..1 float) — image alpha, default 192.
//
// Hidden by default (no seeded layout entry). Enable + reposition via the
// nchud editor.

#include "NCPlusHUDWidget_Minimap.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "Engine/Canvas.h"
#include "NCPlusHUDLayout.h"

UNCPlusHUDWidget_Minimap::UNCPlusHUDWidget_Minimap(const FObjectInitializer& OI)
	: Super(OI)
{
	// Stock position: top-left, slight inset. Matches the customhud BP
	// reference layout the user is migrating from. Origin pivots top-left so
	// "size" extras grow the minimap downward and rightward into the screen.
	Position           = FVector2D(20.f, 20.f);
	Size               = FVector2D(220.f, 220.f);
	ScreenPosition     = FVector2D(0.f, 0.f);   // TopLeft anchor
	Origin             = FVector2D(0.f, 0.f);   // pivot top-left
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

bool UNCPlusHUDWidget_Minimap::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("minimap"));
	if (!E) return false;
	if (E->bHidden) return false;
	return true;
}

void UNCPlusHUDWidget_Minimap::Draw_Implementation(float DeltaTime)
{
	if (!Canvas) return;
	if (!IsValid(UTHUDOwner)) return;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("minimap"));
	if (!E) return;

	// Resolve effective size + alpha from layout extras. RenderScale already
	// folds in the 1080p->actual-resolution math the widget framework does
	// for Position; apply it again to the configurable size so the minimap
	// stays the same fraction of the screen across resolutions.
	const float SizePx = E->GetExtraFloat(TEXT("size"), 220.f) * RenderScale;
	const int32 Alpha  = FMath::Clamp(FMath::RoundToInt(E->GetExtraFloat(TEXT("alpha"), 192.f)), 0, 255);

	// Defer to the HUD's own minimap-availability check. Modes / maps without
	// minimap world data (DM testbeds, levels with no level-bounds set up)
	// will return false here. Calling DrawMinimap anyway in that case races
	// the spawn-choice picker's scene-capture components on the render
	// thread and can produce a 0x18 access-violation in SlateRHIRenderer
	// (capture render-target resource bound to a null handle while Slate
	// composites). Cheaper to bail than to chase the engine race.
	if (!UTHUDOwner->ShouldDrawMinimap()) return;

	// The widget framework already transformed Position from design pixels to
	// real screen coords by the time Draw runs - but DrawMinimap is on the
	// HUD, not the widget, so it expects absolute canvas coords. Compute the
	// top-left corner from our anchor + position the same way the framework
	// does for our own Canvas calls.
	const FVector2D Anchor(Canvas->ClipX * ScreenPosition.X, Canvas->ClipY * ScreenPosition.Y);
	const FVector2D TL = Anchor + Position * RenderScale - Size * Origin * RenderScale;

	UTHUDOwner->DrawMinimap(FColor(192, 192, 192, Alpha), SizePx, TL);
}
