// NCClutchOverlay.cpp

#include "NCClutchOverlay.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTHUD.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"

namespace
{
	static int8 GClutchOverlayEnabled = -1;
	static const float SuccessfulOutroSeconds = 2.5f;
	static const float DeniedOutroSeconds = 1.5f;

	FString GetModIniPath()
	{
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	FLinearColor GetTeamAccent(uint8 TeamIndex)
	{
		return TeamIndex == 0
			? FLinearColor(0.95f, 0.12f, 0.10f, 1.f)
			: FLinearColor(0.10f, 0.48f, 1.f, 1.f);
	}

	FString GetOutcomeLabel(const FNCClutchOverlayState& State)
	{
		if (State.bActive)
		{
			return (State.bRespawnHold && !State.bSuddenDeath)
				? TEXT("LAST ALIVE / HOLD")
				: TEXT("CLUTCH");
		}

		switch (State.Outcome)
		{
			case ENCClutchOverlayOutcome::Clutched: return TEXT("CLUTCHED");
			case ENCClutchOverlayOutcome::Held:     return TEXT("HELD");
			case ENCClutchOverlayOutcome::Denied:   return TEXT("DENIED");
			default:                                return FString();
		}
	}

	void DrawCenteredText(UCanvas* Canvas, UFont* Font, const FString& Text,
		float CenterX, float Y, float Scale, const FLinearColor& Color)
	{
		if (!Canvas || !Font || Text.IsEmpty())
		{
			return;
		}

		float Width = 0.f;
		float Height = 0.f;
		Canvas->TextSize(Font, Text, Width, Height, Scale, Scale);
		FCanvasTextItem Item(
			FVector2D(CenterX - Width * 0.5f, Y),
			FText::FromString(Text), Font, Color);
		Item.Scale = FVector2D(Scale, Scale);
		Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.9f), FVector2D(1.f, 1.f));
		Canvas->DrawItem(Item);
	}

	bool ShouldDrawState(const FNCClutchOverlayState& State, float ServerNow,
		float& OutAlpha)
	{
		OutAlpha = 1.f;
		if (State.bActive)
		{
			return true;
		}
		if (State.Outcome == ENCClutchOverlayOutcome::None
			|| State.Outcome == ENCClutchOverlayOutcome::Cancelled
			|| State.EndServerTime <= 0.f)
		{
			return false;
		}

		const float Lifetime = State.Outcome == ENCClutchOverlayOutcome::Denied
			? DeniedOutroSeconds : SuccessfulOutroSeconds;
		const float Age = FMath::Max(0.f, ServerNow - State.EndServerTime);
		if (Age >= Lifetime)
		{
			return false;
		}

		const float FadeSeconds = FMath::Min(0.6f, Lifetime);
		OutAlpha = FMath::Clamp((Lifetime - Age) / FadeSeconds, 0.f, 1.f);
		return true;
	}
}

bool NCClutchOverlay::IsEnabled()
{
	if (GClutchOverlayEnabled >= 0)
	{
		return GClutchOverlayEnabled != 0;
	}

	bool bEnabled = true;
	if (GConfig)
	{
		GConfig->GetBool(TEXT("NetcodePlus"), TEXT("ShowClutchOverlay"),
			bEnabled, GetModIniPath());
	}
	GClutchOverlayEnabled = bEnabled ? 1 : 0;
	return bEnabled;
}

void NCClutchOverlay::SetEnabled(bool bEnabled)
{
	GClutchOverlayEnabled = bEnabled ? 1 : 0;
	if (GConfig)
	{
		const FString Path = GetModIniPath();
		GConfig->SetBool(TEXT("NetcodePlus"), TEXT("ShowClutchOverlay"), bEnabled, Path);
		GConfig->Flush(false, Path);
	}
}

void NCClutchOverlay::Draw(AUTHUD* HUD, UCanvas* Canvas,
	const FNCClutchOverlayState& Team0State,
	const FNCClutchOverlayState& Team1State)
{
	if (!HUD || !Canvas || !IsEnabled()
		|| NCPlusHUDDrawCall::IsHidden(TEXT("clutch_overlay"))
		|| !HUD->UTPlayerOwner || !HUD->UTPlayerOwner->UTPlayerState
		|| !HUD->UTPlayerOwner->UTPlayerState->bOnlySpectator)
	{
		return;
	}

	AUTGameState* GS = HUD->GetWorld() ? HUD->GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS || GS->HasMatchEnded())
	{
		return;
	}

	const float ServerNow = GS->GetServerWorldTimeSeconds();
	struct FVisibleState
	{
		const FNCClutchOverlayState* State = nullptr;
		float Alpha = 1.f;
	};
	FVisibleState Visible[2];
	int32 VisibleCount = 0;
	const FNCClutchOverlayState* TeamStates[2] = { &Team0State, &Team1State };
	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		float Alpha = 1.f;
		if (ShouldDrawState(*TeamStates[TeamIndex], ServerNow, Alpha))
		{
			Visible[VisibleCount].State = TeamStates[TeamIndex];
			Visible[VisibleCount].Alpha = Alpha;
			++VisibleCount;
		}
	}
	if (VisibleCount == 0)
	{
		return;
	}

	const float ResolutionScale = FMath::Max(Canvas->ClipY, 1.f) / 1080.f;
	const float LayoutScale = NCPlusHUDDrawCall::GetScale(TEXT("clutch_overlay"));
	const float Opacity = FMath::Clamp(
		NCPlusHUDDrawCall::GetOpacity(TEXT("clutch_overlay")), 0.f, 1.f);
	const float S = FMath::Max(0.25f, ResolutionScale * LayoutScale);
	const float CardWidth = 360.f * S;
	const float CardHeight = 78.f * S;
	const float CardGap = 8.f * S;
	const FVector2D Anchor = NCPlusHUDDrawCall::ResolveScreenPos(
		TEXT("clutch_overlay"), Canvas,
		FVector2D(Canvas->ClipX * 0.5f, 138.f * ResolutionScale));
	// Anchor the FIRST card and grow the stack downward, rather than centring the
	// whole stack on the anchor. Centring pushed the top card up by half the stack
	// height as soon as a second one appeared, and the two-card case is not an edge
	// case — it is the design case (1v1: both teams hold simultaneously), which is
	// exactly when the top card landed on the score bar and round clock. Growing
	// down keeps the single-card position pixel-identical and puts the second card
	// in empty space below.
	float CardY = Anchor.Y - CardHeight * 0.5f;

	for (int32 Index = 0; Index < VisibleCount; ++Index)
	{
		const FNCClutchOverlayState& State = *Visible[Index].State;
		const float Alpha = Visible[Index].Alpha * Opacity * HUD->GetHUDWidgetOpacity();
		const float CardX = Anchor.X - CardWidth * 0.5f;
		const FLinearColor Accent = GetTeamAccent(State.TeamIndex);
		const FString Header = GetOutcomeLabel(State);
		const FString CandidateName = !State.CandidateName.IsEmpty()
			? State.CandidateName
			: (State.Candidate ? State.Candidate->PlayerName : TEXT("PLAYER"));
		const int32 VersusCount = State.bActive
			? FMath::Max(1, State.EnemiesRemaining)
			: FMath::Max(1, State.EnemiesAtStart);
		const FString Matchup = FString::Printf(TEXT("%s  -  1v%d"),
			*CandidateName, VersusCount);
		const float EndOrNow = State.bActive ? ServerNow : State.EndServerTime;
		const float Elapsed = FMath::Max(0.f, EndOrNow - State.StartServerTime);
		const FString Detail = State.Outcome == ENCClutchOverlayOutcome::Held
			&& State.TeammatesRespawned > 0
			? FString::Printf(TEXT("REINFORCED +%d  |  %.1fs"),
				State.TeammatesRespawned, Elapsed)
			: FString::Printf(TEXT("%d %s  |  %.1fs"),
				State.DirectKills, State.DirectKills == 1 ? TEXT("KILL") : TEXT("KILLS"), Elapsed);

		Canvas->SetLinearDrawColor(FLinearColor(0.015f, 0.02f, 0.03f, 0.82f * Alpha));
		Canvas->DrawTile(Canvas->DefaultTexture, CardX, CardY,
			CardWidth, CardHeight, 0.f, 0.f, 1.f, 1.f, BLEND_Translucent);
		Canvas->SetLinearDrawColor(FLinearColor(Accent.R, Accent.G, Accent.B, Alpha));
		Canvas->DrawTile(Canvas->DefaultTexture, CardX, CardY,
			4.f * S, CardHeight, 0.f, 0.f, 1.f, 1.f, BLEND_Translucent);

		DrawCenteredText(Canvas, HUD->SmallFont, Header, Anchor.X,
			CardY + 6.f * S, 0.62f * S,
			FLinearColor(Accent.R, Accent.G, Accent.B, Alpha));
		DrawCenteredText(Canvas, HUD->MediumFont, Matchup, Anchor.X,
			CardY + 25.f * S, 0.62f * S,
			FLinearColor(1.f, 1.f, 1.f, Alpha));
		DrawCenteredText(Canvas, HUD->SmallFont, Detail, Anchor.X,
			CardY + 55.f * S, 0.48f * S,
			FLinearColor(0.72f, 0.76f, 0.82f, Alpha));

		CardY += CardHeight + CardGap;
	}

	Canvas->SetLinearDrawColor(FLinearColor::White);
}
