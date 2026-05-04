// SNCPlusHUDDragOverlay.h — full-screen Slate overlay for visually positioning
// HUD elements. Each registered widget alias gets a translucent frame drawn
// over its current render rect; click+drag a frame to move that element. ESC
// closes. Mutations write to FNCPlusHUDLayout::GetLive() through the same path
// as the panel editor, so changes persist on Save.
//
// Console command: nchud_drag (toggles).
//
// Phase 4.0a: visual frames + ESC + console command.
// Phase 4.0b: mouse-down capture + drag → live offset update + mouse-up commit.
#pragma once

#include "NetcodePlus.h"
#include "SlateBasics.h"
#include "NCPlusHUDLayout.h"

class UUTLocalPlayer;
class AUTHUD;

class SNCPlusHUDDragOverlay : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SNCPlusHUDDragOverlay) {}
	SLATE_ARGUMENT(TWeakObjectPtr<UUTLocalPlayer>, PlayerOwner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void ClosePanel();

	// SWidget overrides — paint frames, handle drag + ESC.
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyClippingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove      (const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp  (const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown        (const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool   SupportsKeyboardFocus() const override { return true; }

private:
	TWeakObjectPtr<UUTLocalPlayer> PlayerOwner;

	struct FOverlayElement
	{
		FName     Alias;
		FString   Label;
		FVector2D ScreenPos;   // top-left in absolute screen pixels
		FVector2D ScreenSize;  // pixels
	};

	// Recomputed every paint pass — widget render rects change with viewport
	// resize, so we don't bother caching across frames.
	mutable TArray<FOverlayElement> CachedElements;

	// Drag state
	int32     DragIdx = INDEX_NONE;
	FVector2D DragStartMouseAbs;
	FVector2D DragStartOffset;        // element's design-pixel offset when drag began

	AUTHUD*   GetHUD() const;
	float     GetRenderScale() const;
	void      RefreshCachedElements() const;
	int32     HitTest(const FVector2D& AbsoluteMousePos) const;

	// Close button click handler (alternative to ESC — esc focus is flaky if a
	// drag was just released).
	FReply    OnCloseButtonClicked();

	// Right-click context menu (Phase 4.0c). Per-element actions: hide/show,
	// reset to default, toggle orientation (WeaponBars only), open properties
	// in the main editor panel. Built lazily each invocation so menu entries
	// reflect current state (hide vs show, current orientation, etc.).
	void      OpenContextMenu(FName Alias, const FVector2D& ScreenPos);
	void      OnContextHideToggle(FName Alias);
	void      OnContextReset(FName Alias);
	void      OnContextToggleOrientation(FName Alias);
	void      OnContextProperties(FName Alias);
};
