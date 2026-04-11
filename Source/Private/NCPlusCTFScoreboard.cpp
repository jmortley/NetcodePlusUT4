// NCPlusCTFScoreboard.cpp — K/D/Eff/Acc/C/G/R/Ping columns for CTF.
#include "NCPlusCTFScoreboard.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "UTTeamInfo.h"
#include "CTFStatsReplicator.h"
#include "StatNames.h"
#include "Engine/World.h"

UNCPlusCTFScoreboard::UNCPlusCTFScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;

	// Column positions (fraction of CellWidth)
	ColumnHeaderKillsX   = 0.35f;
	ColumnHeaderDeathsX  = 0.42f;
	ColumnHeaderEffX     = 0.50f;
	ColumnHeaderAccX     = 0.58f;
	ColumnHeaderCapsX2   = 0.66f;
	ColumnHeaderGrabsX   = 0.73f;
	ColumnHeaderReturnsX2 = 0.80f;
	ColumnHeaderPingX    = 0.91f;

	// Column header texts
	CH_Kills  = NSLOCTEXT("CTFScoreboard", "KillsHeader", "K");
	CH_Deaths = NSLOCTEXT("CTFScoreboard", "DeathsHeader", "D");
	CH_Eff    = NSLOCTEXT("CTFScoreboard", "EffHeader", "EFF");
	CH_Acc    = NSLOCTEXT("CTFScoreboard", "AccHeader", "ACC");
	CH_Grabs  = NSLOCTEXT("CTFScoreboard", "GrabsHeader", "G");
	// CH_Caps and CH_Returns inherited from parent UUTCTFScoreboard
}

void UNCPlusCTFScoreboard::PreDraw(float DeltaTime, AUTHUD* InUTHUDOwner, UCanvas* InCanvas, FVector2D InCanvasCenter)
{
	Super::PreDraw(DeltaTime, InUTHUDOwner, InCanvas, InCanvasCenter);
	// Parent CTFScoreboard::PreDraw sets bDrawMinimapInScoreboard = true every frame.
	// Force it off — we don't want the minimap on the scoreboard.
	bDrawMinimapInScoreboard = false;
}

ACTFStatsReplicator* UNCPlusCTFScoreboard::FindStatsReplicator()
{
	if (CachedStatsRep) return CachedStatsRep;

	for (TActorIterator<ACTFStatsReplicator> It(UTHUDOwner->GetWorld()); It; ++It)
	{
		CachedStatsRep = *It;
		return CachedStatsRep;
	}
	return nullptr;
}

void UNCPlusCTFScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		// Background
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height, 149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));
		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_Kills,  XOffset + (ScaledCellWidth * ColumnHeaderKillsX),   YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Deaths, XOffset + (ScaledCellWidth * ColumnHeaderDeathsX),  YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Eff,    XOffset + (ScaledCellWidth * ColumnHeaderEffX),     YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Acc,    XOffset + (ScaledCellWidth * ColumnHeaderAccX),     YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Caps,   XOffset + (ScaledCellWidth * ColumnHeaderCapsX2),   YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Grabs,  XOffset + (ScaledCellWidth * ColumnHeaderGrabsX),   YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Returns,XOffset + (ScaledCellWidth * ColumnHeaderReturnsX2),YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
		}
		DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4;
}

void UNCPlusCTFScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	if (!PlayerState) return;

	// Kills
	DrawText(FText::AsNumber(PlayerState->Kills),
		XOffset + (Width * ColumnHeaderKillsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);

	// Deaths
	DrawText(FText::AsNumber(PlayerState->Deaths),
		XOffset + (Width * ColumnHeaderDeathsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);

	// Efficiency
	{
		float EffKills = (float)PlayerState->Kills;
		float EffDeaths = (float)PlayerState->Deaths;
		float EffPct = (EffKills + EffDeaths > 0.f) ? (EffKills / (EffKills + EffDeaths) * 100.f) : 0.f;
		FLinearColor EffColor;
		if (EffPct >= 60.f)
			EffColor = FLinearColor(0.25f, 0.8f, 0.25f, 1.f); // green
		else if (EffPct >= 40.f)
			EffColor = FLinearColor(0.8f, 0.8f, 0.25f, 1.f);  // yellow
		else
			EffColor = FLinearColor(0.8f, 0.25f, 0.25f, 1.f);  // red

		FString EffStr = FString::Printf(TEXT("%.0f%%"), EffPct);
		DrawText(FText::FromString(EffStr),
			XOffset + (Width * ColumnHeaderEffX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, EffColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Accuracy (from replicator)
	{
		int32 Hits = 0, Shots = 0;
		ACTFStatsReplicator* Rep = FindStatsReplicator();
		if (Rep && PlayerState->UniqueId.IsValid())
		{
			Rep->GetAccuracyForPlayer(PlayerState->UniqueId.ToString(), Hits, Shots);
		}
		float AccPct = (Shots > 0) ? (float(Hits) / float(Shots) * 100.f) : 0.f;
		FLinearColor AccColor;
		if (AccPct >= 40.f)
			AccColor = FLinearColor(0.25f, 0.8f, 0.25f, 1.f);
		else if (AccPct >= 25.f)
			AccColor = FLinearColor(0.8f, 0.8f, 0.25f, 1.f);
		else
			AccColor = FLinearColor(0.8f, 0.25f, 0.25f, 1.f);

		FString AccStr = (Shots > 0) ? FString::Printf(TEXT("%.0f%%"), AccPct) : TEXT("-");
		DrawText(FText::FromString(AccStr),
			XOffset + (Width * ColumnHeaderAccX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, AccColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Caps (replicated on PlayerState)
	DrawText(FText::AsNumber(PlayerState->FlagCaptures),
		XOffset + (Width * ColumnHeaderCapsX2), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);

	// Grabs (from replicator)
	{
		int32 Grabs = 0;
		ACTFStatsReplicator* Rep = FindStatsReplicator();
		if (Rep && PlayerState->UniqueId.IsValid())
		{
			Grabs = Rep->GetGrabsForPlayer(PlayerState->UniqueId.ToString());
		}
		DrawText(FText::AsNumber(Grabs),
			XOffset + (Width * ColumnHeaderGrabsX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Returns (replicated on PlayerState)
	DrawText(FText::AsNumber(PlayerState->FlagReturns),
		XOffset + (Width * ColumnHeaderReturnsX2), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);
}

void UNCPlusCTFScoreboard::DrawPlayerScores(float RenderDelta, float& YOffset)
{
	// Let the parent team scoreboard draw all player rows
	Super::DrawPlayerScores(RenderDelta, YOffset);

	// Spectator list at bottom
	if (!UTGameState) return;

	TArray<FString> SpectatorNames;
	for (APlayerState* PS : UTGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (UTPS && UTPS->bOnlySpectator && !UTPS->PlayerName.IsEmpty())
		{
			SpectatorNames.Add(UTPS->PlayerName);
		}
	}

	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}
