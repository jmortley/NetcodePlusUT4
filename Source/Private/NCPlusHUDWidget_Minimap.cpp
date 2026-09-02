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
#include "UTPlayerState.h"
#include "UTGameState.h"
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
	bHidden            = true;
	CachedLayoutRevision = MAX_uint32;
	CachedSizeDesign = 220.f;
	CachedAlpha = 192;
}

bool UNCPlusHUDWidget_Minimap::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	if (!UTHUDOwner->bDrawMinimap) return false;

	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("minimap"));
	if (!E) return false;
	if (E->bHidden) return false;
	return true;
}

void UNCPlusHUDWidget_Minimap::Draw_Implementation(float DeltaTime)
{
	if (!Canvas) return;
	if (!IsValid(UTHUDOwner)) return;

	const uint32 LayoutRevision = FNCPlusHUDLayout::GetLiveRevision();
	if (CachedLayoutRevision != LayoutRevision)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("minimap"));
		if (!E) return;
		CachedSizeDesign = FMath::Max(E->GetExtraFloat(TEXT("size"), 220.f), 1.f);
		const float AlphaValue = E->GetExtraFloat(TEXT("alpha"), 192.f);
		CachedAlpha = FMath::Clamp(FMath::RoundToInt(AlphaValue <= 1.f ? AlphaValue * 255.f : AlphaValue), 0, 255);
		CachedLayoutRevision = LayoutRevision;
	}

	// Resolve effective size + alpha from layout extras. RenderScale already
	// folds in the 1080p->actual-resolution math the widget framework does
	// for Position; apply it again to the configurable size so the minimap
	// stays the same fraction of the screen across resolutions.
	const float SizePx = CachedSizeDesign * RenderScale;

	// ShouldDraw_Implementation already honored bDrawMinimap. Keep the remaining
	// game-state permission half of AUTHUD::ShouldDrawMinimap here. The owning HUD
	// class suppresses only AUTHUD's later stock minimap call, so ToggleMinimap and
	// unrelated stock widgets continue to observe the real flag. Modes / maps
	// without permitted minimap data return false here. Calling DrawMinimap anyway
	// in that case races
	// the spawn-choice picker's scene-capture components on the render
	// thread and can produce a 0x18 access-violation in SlateRHIRenderer
	// (capture render-target resource bound to a null handle while Slate
	// composites). Cheaper to bail than to chase the engine race.
	AUTGameState* GS = UTHUDOwner->GetWorld() ? UTHUDOwner->GetWorld()->GetGameState<AUTGameState>() : nullptr;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner ? UTHUDOwner->UTPlayerOwner->UTPlayerState : nullptr;
	if (!GS || !GS->AllowMinimapFor(PS)) return;

	// The widget framework already transformed Position from design pixels to
	// real screen coords by the time Draw runs - but DrawMinimap is on the
	// HUD, not the widget, so it expects absolute canvas coords. Compute the
	// top-left corner from our anchor + position the same way the framework
	// does for our own Canvas calls.
	const FVector2D Anchor(Canvas->ClipX * ScreenPosition.X, Canvas->ClipY * ScreenPosition.Y);
	const FVector2D EffectiveSize(CachedSizeDesign, CachedSizeDesign);
	const FVector2D TL = Anchor + Position * RenderScale - EffectiveSize * Origin * RenderScale;

	UTHUDOwner->DrawMinimap(FColor(192, 192, 192, CachedAlpha), SizePx, TL);
}
