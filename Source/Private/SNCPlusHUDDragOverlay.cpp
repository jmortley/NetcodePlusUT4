// SNCPlusHUDDragOverlay — implementation.
#include "SNCPlusHUDDragOverlay.h"
#include "UnrealTournament.h"
#include "UTLocalPlayer.h"
#include "UTPlayerController.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "Engine/Canvas.h"
#include "Engine/GameViewportClient.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBox.h"

namespace NCDragOverlay
{
	// Frame colors
	static const FLinearColor IdleFill        (1.0f, 0.85f, 0.20f, 0.10f);
	static const FLinearColor IdleOutline     (1.0f, 0.85f, 0.20f, 0.85f);
	static const FLinearColor HoverFill       (0.4f, 0.95f, 0.50f, 0.18f);
	static const FLinearColor HoverOutline    (0.4f, 0.95f, 0.50f, 1.00f);
	static const FLinearColor DragFill        (0.4f, 0.95f, 0.50f, 0.30f);
	static const FLinearColor DragOutline     (0.4f, 0.95f, 0.50f, 1.00f);
	static const FLinearColor LabelColor      (1.0f, 1.00f, 1.00f, 0.95f);
	static const FLinearColor LabelShadowColor(0.0f, 0.00f, 0.00f, 0.85f);

	static const float OutlineThickness = 2.f;
}

void SNCPlusHUDDragOverlay::Construct(const FArguments& InArgs)
{
	PlayerOwner = InArgs._PlayerOwner;

	// Empty container — actual rendering happens in OnPaint, mouse events come
	// from the SCompoundWidget base. Visibility::Visible is critical so we
	// catch mouse events instead of letting them fall through to the game.
	ChildSlot
	[
		SNew(SBox)
	];
}

AUTHUD* SNCPlusHUDDragOverlay::GetHUD() const
{
	if (!PlayerOwner.IsValid()) return nullptr;
	if (!PlayerOwner->PlayerController) return nullptr;
	return Cast<AUTHUD>(PlayerOwner->PlayerController->MyHUD);
}

float SNCPlusHUDDragOverlay::GetRenderScale() const
{
	AUTHUD* HUD = GetHUD();
	if (HUD && HUD->Canvas && HUD->Canvas->ClipY > 0.f)
	{
		return HUD->Canvas->ClipY / 1080.f;
	}
	return 1.f;
}

void SNCPlusHUDDragOverlay::RefreshCachedElements() const
{
	CachedElements.Empty();
	AUTHUD* HUD = GetHUD();
	if (!HUD) return;

	for (UUTHUDWidget* W : HUD->HudWidgets)
	{
		if (!W || W->IsPendingKill()) continue;
		const FName Alias = NCPlusHUDAliases::GetAliasForClass(W->GetClass());
		if (Alias == NAME_None) continue;

		// Skip hidden widgets — no point dragging something the user can't see.
		if (W->IsHidden()) continue;

		FOverlayElement E;
		E.Alias      = Alias;
		E.Label      = NCPlusHUDAliases::GetDisplayName(Alias).ToString();
		E.ScreenPos  = W->GetRenderPosition();
		E.ScreenSize = W->GetRenderSize();
		// Skip degenerate rects (some widgets have ShouldDraw==false → never sized).
		if (E.ScreenSize.X < 4.f || E.ScreenSize.Y < 4.f) continue;
		CachedElements.Add(E);
	}
}

int32 SNCPlusHUDDragOverlay::HitTest(const FVector2D& AbsoluteMousePos) const
{
	// Iterate in reverse so visually-on-top elements (last drawn) win the hit.
	for (int32 i = CachedElements.Num() - 1; i >= 0; i--)
	{
		const FOverlayElement& E = CachedElements[i];
		if (AbsoluteMousePos.X >= E.ScreenPos.X &&
		    AbsoluteMousePos.X <= E.ScreenPos.X + E.ScreenSize.X &&
		    AbsoluteMousePos.Y >= E.ScreenPos.Y &&
		    AbsoluteMousePos.Y <= E.ScreenPos.Y + E.ScreenSize.Y)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 SNCPlusHUDDragOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyClippingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace NCDragOverlay;

	RefreshCachedElements();

	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	if (!WhiteBrush) return LayerId;

	// Use the body font for labels — small but readable.
	const FSlateFontInfo Font = FCoreStyle::Get().GetFontStyle(TEXT("NormalText"));

	for (int32 i = 0; i < CachedElements.Num(); i++)
	{
		const FOverlayElement& E = CachedElements[i];
		const bool bIsDragging = (DragIdx == i);

		const FLinearColor Fill    = bIsDragging ? DragFill    : IdleFill;
		const FLinearColor Outline = bIsDragging ? DragOutline : IdleOutline;

		// Fill rect (translucent so user can still see what's underneath)
		FSlateDrawElement::MakeBox(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(E.ScreenPos, E.ScreenSize),
			WhiteBrush, ESlateDrawEffect::None, Fill);

		// Outline — 4 thin rects (top, bottom, left, right). MakeBox doesn't have
		// a stroke-only mode in 4.15, so we composite manually.
		const float T = OutlineThickness;
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(E.ScreenPos, FVector2D(E.ScreenSize.X, T)),
			WhiteBrush, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(E.ScreenPos + FVector2D(0.f, E.ScreenSize.Y - T), FVector2D(E.ScreenSize.X, T)),
			WhiteBrush, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(E.ScreenPos, FVector2D(T, E.ScreenSize.Y)),
			WhiteBrush, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(E.ScreenPos + FVector2D(E.ScreenSize.X - T, 0.f), FVector2D(T, E.ScreenSize.Y)),
			WhiteBrush, ESlateDrawEffect::None, Outline);

		// Label with shadow — anchored top-left of the frame.
		const FVector2D LabelPos = E.ScreenPos + FVector2D(6.f, 4.f);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(LabelPos + FVector2D(1.f, 1.f), FVector2D(400.f, 24.f)),
			E.Label, Font, ESlateDrawEffect::None, LabelShadowColor);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId + 3,
			AllottedGeometry.ToPaintGeometry(LabelPos, FVector2D(400.f, 24.f)),
			E.Label, Font, ESlateDrawEffect::None, LabelColor);
	}

	// Footer hint — tells the user what to do.
	const FString Hint = FString(TEXT("Drag any frame to move that element. ESC to close."));
	const FVector2D HintPos(20.f, AllottedGeometry.GetLocalSize().Y - 32.f);
	FSlateDrawElement::MakeText(
		OutDrawElements, LayerId + 4,
		AllottedGeometry.ToPaintGeometry(HintPos + FVector2D(1.f, 1.f), FVector2D(900.f, 24.f)),
		Hint, Font, ESlateDrawEffect::None, LabelShadowColor);
	FSlateDrawElement::MakeText(
		OutDrawElements, LayerId + 5,
		AllottedGeometry.ToPaintGeometry(HintPos, FVector2D(900.f, 24.f)),
		Hint, Font, ESlateDrawEffect::None, LabelColor);

	return LayerId + 6;
}

FReply SNCPlusHUDDragOverlay::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	// Element rects are in absolute screen pixels (RenderPosition is what
	// AUTHUD::DrawHUD wrote during the last frame); use absolute mouse position
	// to hit-test against them.
	const FVector2D MouseAbs = MouseEvent.GetScreenSpacePosition();

	RefreshCachedElements();
	const int32 Hit = HitTest(MouseAbs);
	if (Hit == INDEX_NONE)
	{
		// Click on empty area — release focus so the game doesn't get stuck.
		return FReply::Unhandled();
	}

	DragIdx           = Hit;
	DragStartMouseAbs = MouseAbs;

	const FName Alias = CachedElements[Hit].Alias;
	const FNCPlusHUDElement* Existing = FNCPlusHUDLayout::GetLive().Find(Alias);
	DragStartOffset = Existing ? Existing->Offset : NCPlusHUDAliases::GetStockOffset(Alias);

	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply SNCPlusHUDDragOverlay::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (DragIdx == INDEX_NONE) return FReply::Unhandled();

	// Mouse delta in screen pixels → divide by RenderScale to get design-pixel
	// delta (matches how the layout's Offset is interpreted by the apply pass).
	const FVector2D MouseAbs   = MouseEvent.GetScreenSpacePosition();
	const FVector2D DeltaPx    = MouseAbs - DragStartMouseAbs;
	const float     RenderScale = GetRenderScale();
	if (RenderScale <= 0.f) return FReply::Handled();
	const FVector2D DesignDelta = DeltaPx / RenderScale;

	const FName Alias = CachedElements[DragIdx].Alias;
	FNCPlusHUDLayout& L = FNCPlusHUDLayout::GetLive();
	FNCPlusHUDElement* Elem = L.Elements.Find(Alias);
	if (!Elem)
	{
		// Seed with stock anchor so the widget doesn't snap to Center+0,0 on the
		// very first drag pixel. Same idiom as the panel editor's GetOrCreateElement.
		FNCPlusHUDElement Seed;
		Seed.Anchor = NCPlusHUDAliases::GetStockAnchor(Alias);
		Seed.Offset = NCPlusHUDAliases::GetStockOffset(Alias);
		Elem = &L.Elements.Add(Alias, Seed);
	}
	Elem->Offset = DragStartOffset + DesignDelta;
	FNCPlusHUDLayout::MarkLiveDirty();

	return FReply::Handled();
}

FReply SNCPlusHUDDragOverlay::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton) return FReply::Unhandled();
	if (DragIdx == INDEX_NONE) return FReply::Unhandled();

	DragIdx = INDEX_NONE;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply SNCPlusHUDDragOverlay::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// Release any in-progress drag before tearing down so we don't leave the
		// mouse captured.
		DragIdx = INDEX_NONE;
		ClosePanel();
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

void SNCPlusHUDDragOverlay::ClosePanel()
{
	if (UGameViewportClient* VC = (PlayerOwner.IsValid() && PlayerOwner->GetWorld())
		? PlayerOwner->GetWorld()->GetGameViewport() : nullptr)
	{
		VC->RemoveViewportWidgetContent(SharedThis(this));
	}
}
