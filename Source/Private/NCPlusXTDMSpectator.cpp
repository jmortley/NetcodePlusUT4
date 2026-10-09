#include "NCPlusXTDMSpectator.h"

#include "NCPlusXTDMHUD.h"
#include "NCPlusXTDMReplicator.h"
#include "NCPlusHUDLayout.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "Engine/Canvas.h"

UNCPlusXTDMSpectator::UNCPlusXTDMSpectator(const FObjectInitializer& OI) : Super(OI)
{
	ToggleBounds = FVector4(0, 0, 0, 0);
}

void UNCPlusXTDMSpectator::Draw_Implementation(float DeltaTime)
{
	// Do not call Super::Draw: it drops teams 2/3. Inherit ShouldDraw to manage SUTSpectatorWindow.
	PlayerHits.Reset();
	ANCPlusXTDMHUD* HUD = Cast<ANCPlusXTDMHUD>(UTHUDOwner);
	if (!HUD || !Canvas || !UTPlayerOwner || !UTGameState) return;
	const float S = FMath::Min(Canvas->ClipY / 1080.f * HUD->GetHUDWidgetScaleOverride(), Canvas->ClipY / 900.f);
	const float X = 16.f * S, Y = 150.f * S, Width = 320.f * S;
	ToggleBounds = FVector4(X, Y, X + Width, Y + 32.f * S);
	HUD->Tile(Canvas, X, Y, Width, 32.f * S, FLinearColor(.02f, .025f, .035f, .85f));
	HUD->Text(Canvas, HUD->SmallFont, UTPlayerOwner->bRequestingSlideOut ? TEXT("SPECTATORS  /  HIDE") : TEXT("SPECTATORS  /  SHOW"),
		X + 10.f * S, Y + 4.f * S, .55f * S, FLinearColor::White);
	if (!UTPlayerOwner->bRequestingSlideOut) return;
	float RowY = Y + 37.f * S;
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		FLinearColor Col = HUD->TeamColor(UTGameState, Team);
		HUD->Text(Canvas, HUD->SmallFont, FString::Printf(TEXT("ALT+%d  %s%s"), Team + 1, *HUD->TeamLabel(Team),
			Team == HUD->KeyboardSpectatorTeam ? TEXT("  [SELECTED]") : TEXT("")),
			X + 8.f * S, RowY, .55f * S, Col);
		RowY += 25.f * S;
		TArray<AUTPlayerState*> Players; HUD->GetTeamRoster(Team, Players);
		for (int32 Slot = 0; Slot < Players.Num() && Slot < 4; ++Slot)
		{
			AUTPlayerState* PS = Players[Slot];
			if (!UTGameState->CanSpectate(UTPlayerOwner, PS)) continue;
			FVector4 Bounds(X, RowY, X + Width, RowY + 28.f * S);
			PlayerHits.Add(FSelectionObject(PS, Bounds));
			const bool bHovered = bPointerInteractive && PointerPosition.X >= Bounds.X && PointerPosition.X <= Bounds.Z
				&& PointerPosition.Y >= Bounds.Y && PointerPosition.Y <= Bounds.W;
			HUD->Tile(Canvas, X, RowY, Width, 28.f * S, bHovered ? FLinearColor(.13f, .16f, .2f, .92f) : FLinearColor(.02f, .025f, .035f, .82f));
			const TCHAR* Keys[] = { TEXT("Q"), TEXT("W"), TEXT("E"), TEXT("R") };
			HUD->Text(Canvas, HUD->SmallFont, Keys[Slot], X + 7.f * S, RowY + 2.f * S, .5f * S, Col);
			FText Fitted; float W, H;
			NCPlusHUDDrawCall::ResolveFittedName(Canvas, PS, HUD->SmallFont, PS->PlayerName, Width - 42.f * S, .52f * S, Fitted, W, H);
			NCPlusHUDDrawCall::DrawResolvedText(Canvas, HUD->SmallFont, Fitted, X + 30.f * S, RowY + 2.f * S, .52f * S, .52f * S, FColor::White, true);
			RowY += 30.f * S;
		}
		RowY += 9.f * S;
	}
	HUD->Text(Canvas, HUD->SmallFont, TEXT("ALT+1-4: TEAM  |  ALT+Q/W/E/R: PLAYER"), X, RowY, .4f * S, FLinearColor(.65f, .7f, .78f));
}

bool UNCPlusXTDMSpectator::MouseClick(FVector2D P)
{
	if (!UTPlayerOwner || !UTGameState) return false;
	if (P.X >= ToggleBounds.X && P.X <= ToggleBounds.Z && P.Y >= ToggleBounds.Y && P.Y <= ToggleBounds.W)
	{
		UTPlayerOwner->bRequestingSlideOut = !UTPlayerOwner->bRequestingSlideOut;
		return true;
	}
	for (const FSelectionObject& Hit : PlayerHits)
	{
		AUTPlayerState* PS = Hit.ScoreOwner.Get();
		if (PS && PS->SpectatingIDTeam > 0 && P.X >= Hit.ScoreBounds.X && P.X <= Hit.ScoreBounds.Z && P.Y >= Hit.ScoreBounds.Y && P.Y <= Hit.ScoreBounds.W
			&& UTGameState->CanSpectate(UTPlayerOwner, PS))
		{
			if (ANCPlusXTDMHUD* HUD = Cast<ANCPlusXTDMHUD>(UTHUDOwner)) HUD->KeyboardSpectatorTeam = PS->GetTeamNum();
			UTPlayerOwner->ViewPlayerNum(PS->SpectatingIDTeam, PS->GetTeamNum());
			return true;
		}
	}
	return false;
}
