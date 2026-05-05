// NCPlusHUDWidget_Speedometer.cpp — horizontal-speed readout.
//
// Visibility is layout-gated: the widget hides itself unless the live layout
// has a "speedometer" entry. Default location is just below the crosshair so
// the eye doesn't have to leave aim center to glance at speed (matches what
// the original BP customhud mutator did before this rewrite).

#include "NCPlusHUDWidget_Speedometer.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "Engine/Canvas.h"
#include "NCPlusHUDLayout.h"

UNCPlusHUDWidget_Speedometer::UNCPlusHUDWidget_Speedometer(const FObjectInitializer& OI)
	: Super(OI)
{
	// Stock position: just below crosshair, centered. Origin pivots at top-
	// center so the value extends downward without nudging into the crosshair
	// when the user re-anchors it.
	Position           = FVector2D(0.f, 80.f);
	Size               = FVector2D(160.f, 40.f);
	ScreenPosition     = FVector2D(0.5f, 0.5f);   // Center anchor
	Origin             = FVector2D(0.5f, 0.0f);   // pivot top-center
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

bool UNCPlusHUDWidget_Speedometer::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!IsValid(PS) || PS->bOnlySpectator) return false;

	// Opt-in via nchud editor; no entry = hidden.
	const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(TEXT("speedometer"));
	if (!E) return false;
	if (E->bHidden) return false;
	return true;
}

void UNCPlusHUDWidget_Speedometer::Draw_Implementation(float DeltaTime)
{
	if (!Canvas) return;
	if (!IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner)) return;
	APawn* Pawn = UTHUDOwner->UTPlayerOwner->GetPawn();
	if (!IsValid(Pawn)) return;

	// Horizontal speed only — vertical drops out so falling/jumping doesn't
	// inflate the readout. Matches what competitive players track.
	const FVector V = Pawn->GetVelocity();
	const float Speed = FVector(V.X, V.Y, 0.f).Size();

	// Color tier: rough movement-state cues. Stock UT walk speed is ~940uu/s,
	// a single dodge is ~1100, dodge-jump chains push past 1500. Keeps the
	// number useful at a glance for both casual and movement-focused players.
	const FLinearColor SpeedColor =
		(Speed >= 1500.f) ? FLinearColor(0.40f, 1.0f, 0.55f, 1.f) :   // green: chained
		(Speed >= 1100.f) ? FLinearColor(1.0f, 0.95f, 0.35f, 1.f) :   // yellow: dodge
		(Speed >=  500.f) ? FLinearColor(1.f, 1.f, 1.f, 0.95f) :      // white: walking
		                    FLinearColor(1.f, 1.f, 1.f, 0.55f);       // dim: idle

	UFont* BigFont  = UTHUDOwner->LargeFont  ? UTHUDOwner->LargeFont  : UTHUDOwner->MediumFont;
	UFont* SmallFnt = UTHUDOwner->SmallFont  ? UTHUDOwner->SmallFont  : UTHUDOwner->TinyFont;
	if (!BigFont) return;

	const FString SpeedStr = FString::Printf(TEXT("%d"), FMath::RoundToInt(Speed));
	DrawText(FText::FromString(SpeedStr), Size.X * 0.5f, 0.f,
		BigFont, RenderScale, 1.0f, SpeedColor,
		ETextHorzPos::Center, ETextVertPos::Top);

	// Optional unit label below the number — small, dim, easy to ignore once
	// the user gets used to the value range.
	if (SmallFnt)
	{
		DrawText(FText::FromString(TEXT("uu/s")), Size.X * 0.5f, 22.f,
			SmallFnt, RenderScale, 1.0f, FLinearColor(1.f, 1.f, 1.f, 0.6f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}
}
