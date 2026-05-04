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
#include "Widgets/Layout/SBorder.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Framework/Application/SlateApplication.h"

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

	// Explicit Visible so the overlay's bounding box catches mouse events.
	// Default would technically be Visible too, but defensive against parent
	// containers (AddViewportWidgetContent) tweaking child visibility.
	SetVisibility(EVisibility::Visible);

	// SBorder with NoBorder brush + HAlign/VAlign Fill makes the ChildSlot
	// expand to the full viewport, so AllottedGeometry in OnPaint covers the
	// entire screen — required for hit-testing arbitrary screen positions.
	// An empty SBox would collapse to zero size and make mouse events miss us.
	ChildSlot
	.HAlign(HAlign_Fill)
	.VAlign(VAlign_Fill)
	[
		SNew(SBorder)
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("NoBorder")))
		.Visibility(EVisibility::Visible)
	];

	// Switch input mode to GameAndUI so Slate can receive mouse events. The
	// HUD's GetInputMode_Implementation forces GameOnly during InProgress
	// (camera-look capture); flipping NCPlusHUDDragMode flag makes it return
	// GameAndUI on next poll. Forcing it on the PC here too snaps the cursor
	// loose immediately instead of waiting for the next HUD tick.
	NCPlusHUDDragMode::SetActive(true);
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		APlayerController* PC = PlayerOwner->PlayerController;
		PC->bShowMouseCursor = true;

		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(SharedThis(this));
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(InputMode);
	}
}

AUTHUD* SNCPlusHUDDragOverlay::GetHUD() const
{
	if (!PlayerOwner.IsValid()) return nullptr;
	if (!PlayerOwner->PlayerController) return nullptr;
	return Cast<AUTHUD>(PlayerOwner->PlayerController->MyHUD);
}

float SNCPlusHUDDragOverlay::GetRenderScale() const
{
	// AHUD::Canvas is protected — can't read it directly from outside the class.
	// Pull viewport pixel height from the GameViewport instead; matches what
	// AUTHUD::DrawHUD uses when computing Canvas->ClipY.
	if (PlayerOwner.IsValid() && PlayerOwner->GetWorld())
	{
		if (UGameViewportClient* VC = PlayerOwner->GetWorld()->GetGameViewport())
		{
			if (VC->Viewport)
			{
				const FIntPoint Size = VC->Viewport->GetSizeXY();
				if (Size.Y > 0)
				{
					return float(Size.Y) / 1080.f;
				}
			}
		}
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

	// AbsToLocal: GetRenderPosition returns absolute screen pixels (set by
	// AUTHUD::DrawHUD). ToPaintGeometry expects LOCAL (DPI-scaled) coords. On
	// a high-DPI display the two diverge by AllottedGeometry.Scale, so naive
	// passthrough drops the frame in the wrong place. Convert once via this
	// helper at every paint site.
	const float DPIScale = (AllottedGeometry.Scale > 0.f) ? AllottedGeometry.Scale : 1.f;
	auto AbsToLocal = [DPIScale](const FVector2D& Abs) -> FVector2D { return Abs / DPIScale; };

	for (int32 i = 0; i < CachedElements.Num(); i++)
	{
		const FOverlayElement& E = CachedElements[i];
		const bool bIsDragging = (DragIdx == i);

		const FLinearColor Fill    = bIsDragging ? DragFill    : IdleFill;
		const FLinearColor Outline = bIsDragging ? DragOutline : IdleOutline;

		const FVector2D LocalPos  = AbsToLocal(E.ScreenPos);
		const FVector2D LocalSize = AbsToLocal(E.ScreenSize);

		// Fill rect (translucent so user can still see what's underneath).
		// NOTE: UE 4.15's MakeBox/MakeText require an FSlateRect ClippingRect
		// arg before the draw-effect bitmask (the 4.17+ overload that omits it
		// doesn't exist here). Pass MyClippingRect at every call site.
		FSlateDrawElement::MakeBox(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(LocalPos, LocalSize),
			WhiteBrush, MyClippingRect, ESlateDrawEffect::None, Fill);

		// Outline — 4 thin rects (top, bottom, left, right). MakeBox doesn't
		// have a stroke-only mode in 4.15, so we composite manually.
		const float T = OutlineThickness / DPIScale;  // keep visual thickness uniform across DPI
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(LocalPos, FVector2D(LocalSize.X, T)),
			WhiteBrush, MyClippingRect, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(LocalPos + FVector2D(0.f, LocalSize.Y - T), FVector2D(LocalSize.X, T)),
			WhiteBrush, MyClippingRect, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(LocalPos, FVector2D(T, LocalSize.Y)),
			WhiteBrush, MyClippingRect, ESlateDrawEffect::None, Outline);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(LocalPos + FVector2D(LocalSize.X - T, 0.f), FVector2D(T, LocalSize.Y)),
			WhiteBrush, MyClippingRect, ESlateDrawEffect::None, Outline);

		// Label with shadow — anchored top-left of the frame.
		const FVector2D LabelPos = LocalPos + FVector2D(6.f, 4.f);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId + 2,
			AllottedGeometry.ToPaintGeometry(LabelPos + FVector2D(1.f, 1.f), FVector2D(400.f, 24.f)),
			E.Label, Font, MyClippingRect, ESlateDrawEffect::None, LabelShadowColor);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId + 3,
			AllottedGeometry.ToPaintGeometry(LabelPos, FVector2D(400.f, 24.f)),
			E.Label, Font, MyClippingRect, ESlateDrawEffect::None, LabelColor);
	}

	// Footer hint — tells the user what to do. AllottedGeometry.GetLocalSize()
	// is already in local (DPI-adjusted) coords, so no conversion needed here.
	const FString Hint = FString(TEXT("Drag any frame to move that element. ESC to close."));
	const FVector2D HintPos(20.f, AllottedGeometry.GetLocalSize().Y - 32.f);
	FSlateDrawElement::MakeText(
		OutDrawElements, LayerId + 4,
		AllottedGeometry.ToPaintGeometry(HintPos + FVector2D(1.f, 1.f), FVector2D(900.f, 24.f)),
		Hint, Font, MyClippingRect, ESlateDrawEffect::None, LabelShadowColor);
	FSlateDrawElement::MakeText(
		OutDrawElements, LayerId + 5,
		AllottedGeometry.ToPaintGeometry(HintPos, FVector2D(900.f, 24.f)),
		Hint, Font, MyClippingRect, ESlateDrawEffect::None, LabelColor);

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
	// Restore input mode FIRST, before tearing down the widget — once the
	// widget is removed, SharedThis would be stale.
	NCPlusHUDDragMode::SetActive(false);
	if (PlayerOwner.IsValid() && PlayerOwner->PlayerController)
	{
		APlayerController* PC = PlayerOwner->PlayerController;
		PC->bShowMouseCursor = false;

		// FInputModeGameOnly snaps the cursor back to camera-look capture.
		// The HUD's GetInputMode poll will reaffirm this next tick (with the
		// drag-mode flag now cleared, it returns to its normal GameOnly path).
		FInputModeGameOnly InputMode;
		PC->SetInputMode(InputMode);
	}

	if (UGameViewportClient* VC = (PlayerOwner.IsValid() && PlayerOwner->GetWorld())
		? PlayerOwner->GetWorld()->GetGameViewport() : nullptr)
	{
		VC->RemoveViewportWidgetContent(SharedThis(this));
	}
}
