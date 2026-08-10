// NCPlusHUDWidget_ReadyUp.cpp - see header.
#include "NCPlusHUDWidget_ReadyUp.h"

#include "NCReadyUp.h"
#include "UTHUD.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Engine/Canvas.h"

UNCPlusHUDWidget_ReadyUp::UNCPlusHUDWidget_ReadyUp(const FObjectInitializer& OI)
	: Super(OI)
{
	// Keep the prompt at the bottom-center like UTComp's warmup reminder, above
	// the viewport edge but below the normal NCPlus health/armor cluster.
	Position = FVector2D(0.f, -72.f);
	Size = FVector2D(900.f, 64.f);
	ScreenPosition = FVector2D(0.5f, 1.f);
	Origin = FVector2D(0.5f, 1.f);
	DesignedResolution = 1080.f;
	bShouldKickBack = false;
}

ANCReadyUpState* UNCPlusHUDWidget_ReadyUp::GetReadyUpState() const
{
	ANCReadyUpState* State = CachedReadyUpState.Get();
	if (State == nullptr || State->GetWorld() != GetWorld())
	{
		State = ANCReadyUpState::Find(GetWorld());
		CachedReadyUpState = State;
	}
	return State;
}

bool UNCPlusHUDWidget_ReadyUp::ShouldDraw_Implementation(bool bShowScores)
{
	if (bShowScores || !IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner))
	{
		return false;
	}

	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	return IsValid(PS) && !PS->bOnlySpectator
		&& GS != nullptr && GS->GetMatchState() == MatchState::WaitingToStart
		&& GetReadyUpState() != nullptr;
}

void UNCPlusHUDWidget_ReadyUp::Draw_Implementation(float DeltaTime)
{
	if (!Canvas || !IsValid(UTHUDOwner) || !IsValid(UTHUDOwner->UTPlayerOwner))
	{
		return;
	}

	ANCReadyUpState* State = GetReadyUpState();
	AUTPlayerState* PS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
	if (State == nullptr || !IsValid(PS))
	{
		return;
	}

	const bool bReady = State->IsPlayerReady(PS);
	const FString Prompt = State->bCountdownLocked
		? TEXT("READINESS LOCKED - MATCH STARTING")
		: (bReady ? TEXT("YOU ARE READY - F5 FOR READY MENU") : TEXT("PRESS F5 TO READY UP"));
	const FString Count = FString::Printf(TEXT("%d / %d PLAYERS READY"),
		State->ReadyCount, State->EligibleCount);
	const FLinearColor PromptColor = State->bCountdownLocked
		? FLinearColor(1.f, 1.f, 1.f, 1.f)
		: (bReady ? FLinearColor(0.f, 0.97f, 1.f, 1.f)
			: FLinearColor(1.f, 0.84f, 0.1f, 1.f));
	const FLinearColor Shadow(0.f, 0.f, 0.f, 0.85f);

	UFont* PromptFont = UTHUDOwner->MediumFont ? UTHUDOwner->MediumFont : UTHUDOwner->SmallFont;
	UFont* CountFont = UTHUDOwner->SmallFont ? UTHUDOwner->SmallFont : UTHUDOwner->TinyFont;
	if (PromptFont == nullptr)
	{
		return;
	}

	// Draw a one-pixel shadow so the prompt remains legible on bright maps.
	DrawText(FText::FromString(Prompt), Size.X * 0.5f + 1.f, 1.f,
		PromptFont, 1.f, 1.f, Shadow,
		ETextHorzPos::Center, ETextVertPos::Top);
	DrawText(FText::FromString(Prompt), Size.X * 0.5f, 0.f,
		PromptFont, 1.f, 1.f, PromptColor,
		ETextHorzPos::Center, ETextVertPos::Top);

	if (CountFont != nullptr)
	{
		DrawText(FText::FromString(Count), Size.X * 0.5f + 1.f, 31.f,
			CountFont, 1.f, 1.f, Shadow,
			ETextHorzPos::Center, ETextVertPos::Top);
		DrawText(FText::FromString(Count), Size.X * 0.5f, 30.f,
			CountFont, 1.f, 1.f, FLinearColor(1.f, 1.f, 1.f, 0.85f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}
}
