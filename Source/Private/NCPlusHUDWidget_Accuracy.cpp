// NCPlusHUDWidget_Accuracy.cpp — live shaft accuracy readout.

#include "NCPlusHUDWidget_Accuracy.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Engine/Canvas.h"
#include "StatNames.h"

UNCPlusHUDWidget_Accuracy::UNCPlusHUDWidget_Accuracy(const FObjectInitializer& OI)
	: Super(OI)
{
	Position           = FVector2D(0.f, 0.f);
	Size               = FVector2D(220.f, 80.f);
	ScreenPosition     = FVector2D(0.5f, 0.10f);   // top-center default
	Origin             = FVector2D(0.5f, 0.f);
	DesignedResolution = 1080.f;
	bShouldKickBack    = false;
}

bool UNCPlusHUDWidget_Accuracy::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores) return false;
	if (!UTHUDOwner || !UTHUDOwner->UTPlayerOwner) return false;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	return PS && !PS->bOnlySpectator;
}

void UNCPlusHUDWidget_Accuracy::Draw_Implementation(float DeltaTime)
{
	if (!UTHUDOwner || !UTHUDOwner->UTPlayerOwner || !Canvas) return;
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (!PS) return;

	const int32 Hits  = PS->GetStatsValue(NAME_LinkHits);
	const int32 Shots = PS->GetStatsValue(NAME_LinkShots);
	const float Pct = (Shots > 0) ? float(Hits) / float(Shots) * 100.f : 0.f;

	// Color tier: green ≥ 50, yellow ≥ 30, red below.
	const FLinearColor PctColor = (Pct >= 50.f) ? FLinearColor(0.30f, 1.0f, 0.40f, 1.f)
	                            : (Pct >= 30.f) ? FLinearColor(1.0f, 0.95f, 0.35f, 1.f)
	                            : FLinearColor(1.0f, 0.45f, 0.45f, 1.f);

	UFont* BigFont   = UTHUDOwner->LargeFont   ? UTHUDOwner->LargeFont   : UTHUDOwner->MediumFont;
	UFont* SmallFnt  = UTHUDOwner->SmallFont   ? UTHUDOwner->SmallFont   : UTHUDOwner->TinyFont;
	if (!BigFont || !SmallFnt) return;

	// Big percentage number.
	const FString PctStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct));
	DrawText(FText::FromString(PctStr), Size.X * 0.5f, 0.f,
		BigFont, RenderScale, 1.0f, PctColor,
		ETextHorzPos::Center, ETextVertPos::Top);

	// Subline: "Hits / Shots"
	const FString SubStr = FString::Printf(TEXT("%d / %d"), Hits, Shots);
	DrawText(FText::FromString(SubStr), Size.X * 0.5f, 48.f,
		SmallFnt, RenderScale, 1.0f, FLinearColor(1.f, 1.f, 1.f, 0.85f),
		ETextHorzPos::Center, ETextVertPos::Top);
}
