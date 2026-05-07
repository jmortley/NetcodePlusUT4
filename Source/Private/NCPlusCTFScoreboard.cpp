// NCPlusCTFScoreboard.cpp — K/D/Eff/Acc/C/G/R/Ping columns for CTF.
#include "NCPlusCTFScoreboard.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "UTTeamInfo.h"
#include "CTFStatsReplicator.h"
#include "StatNames.h"
#include "Engine/World.h"
#include "UTBot.h"
#include "UTHUD.h"
#include "Engine/Canvas.h"

UNCPlusCTFScoreboard::UNCPlusCTFScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CellWidth = 850.f; // Match Wipeout — plenty of room for 8 columns

	// Column positions (fraction of CellWidth).
	// PlayerX 0.09: engine draws the country flag at FlagX=0.01 with a ~36px
	// texture (~6-7% of CellWidth). The previous 0.02 put the clan-tag-
	// prefixed name on top of the flag, so [SV] / [U★P] / etc. crowded the
	// flag glyph. 0.09 sits one cell to the right of the flag; engine default
	// for the same situation is 0.10. Stat-column positions kept at their
	// original spots — name area is now 0.09 → 0.32 (~23% of CellWidth, plenty
	// for typical names + clan tags).
	ColumnHeaderPlayerX  = 0.09f;
	ColumnHeaderKillsX   = 0.32f;
	ColumnHeaderDeathsX  = 0.37f;
	ColumnHeaderEffX     = 0.45f;
	ColumnHeaderAccX     = 0.54f;
	ColumnHeaderCapsX2   = 0.63f;
	ColumnHeaderGrabsX   = 0.72f;
	ColumnHeaderReturnsX2 = 0.81f;
	ColumnHeaderPingX    = 0.92f;

	// Column header texts
	CH_Kills  = NSLOCTEXT("CTFScoreboard", "KillsHeader", "K");
	CH_Deaths = NSLOCTEXT("CTFScoreboard", "DeathsHeader", "D");
	CH_Eff    = NSLOCTEXT("CTFScoreboard", "EffHeader", "EFF");
	CH_Acc    = NSLOCTEXT("CTFScoreboard", "AccHeader", "ACC");
	CH_Caps   = NSLOCTEXT("CTFScoreboard", "CapsHeader", "Caps");
	CH_Grabs  = NSLOCTEXT("CTFScoreboard", "GrabsHeader", "Grabs");
	CH_Returns = NSLOCTEXT("CTFScoreboard", "ReturnsHeader", "Returns");
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

		if (UTGameState)
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
	// Copy of UUTTeamScoreboard::DrawPlayerScores with the engine's
	// "X Spectators Watching" count text replaced by a comma-separated
	// names list. Calling Super and then drawing our text is what we used
	// to do, but the engine paints its count at Y=765 — same Y as our
	// names list — and the two overlap into the "garbled" look.
	// Same pattern Elim/Wipeout scoreboards use.
	if (UTGameState == nullptr) return;

	int32 XOffset = ScaledEdgeSize;
	float MaxYOffset = 0.f;
	TArray<FString> SpectatorNames;

	for (int8 Team = 0; Team < 2; Team++)
	{
		int32 Place = 1;
		float DrawOffset = YOffset;
		const int32 NumPlayersToShow = ShouldDrawScoringStats() ? 5 : UTGameState->PlayerArray.Num();
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PlayerState = Cast<AUTPlayerState>(UTGameState->PlayerArray[i]);
			if (PlayerState)
			{
				if (!PlayerState->bOnlySpectator)
				{
					if (PlayerState->GetTeamNum() == Team)
					{
						DrawPlayer(Place, PlayerState, RenderDelta, XOffset, DrawOffset);
						Place++;
						DrawOffset += CellHeight * RenderScale;
						if (Place > NumPlayersToShow) break;
					}
				}
				else if (Team == 0 && !PlayerState->bIsDemoRecording && !PlayerState->PlayerName.IsEmpty())
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
			}
		}
		MaxYOffset = FMath::Max(DrawOffset, MaxYOffset);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset = MaxYOffset;

	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}

// =============================================================================
// DrawPlayer override
// =============================================================================
// Same shape as the duel / shaft DrawPlayer overrides:
//   - Copy of UUTScoreboard::DrawPlayer (UTScoreboard.cpp:661)
//   - Null-guard UTHUDOwner->UTPlayerOwner derefs (avoids the pre-match
//     standalone-PIE crash documented in feedback_scoreboard_drawplayer_null_guards.md)
//   - Mute block removed (matches elim/wipeout)
//   - Ping / bot-skill draw is CENTERED at ColumnHeaderPingX rather than
//     right-aligned at 0.995, so the value sits under the column header
//     instead of jamming against the row's right border.
void UNCPlusCTFScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
	float RenderDelta, float XOffset, float YOffset)
{
	if (PlayerState == NULL) return;

	float BarOpacity = 0.3f;
	bool bIsUnderCursor = false;

	if (bIsInteractive)
	{
		FVector4 Bounds = FVector4(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + ScaledCellWidth, RenderPosition.Y + YOffset + CellHeight*RenderScale);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
		bIsUnderCursor = (CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z
			&& CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W);
	}
	PlayerState->ScoreCorner = FVector(RenderPosition.X + XOffset, RenderPosition.Y + YOffset + 0.25f*CellHeight*RenderScale, 0.f);
	if (!PlayerState->Team || (PlayerState->Team->TeamIndex != 1))
	{
		PlayerState->ScoreCorner.X += ScaledCellWidth;
	}

	float NameXL, NameYL;
	float ClanXL = 0.f;
	FString DisplayName = PlayerState->PlayerName;
	FString ClanName = PlayerState->ClanName;
	if (!PlayerState->ClanName.IsEmpty())
	{
		ClanName = "[" + ClanName + "]";
		Canvas->TextSize(UTHUDOwner->SmallFont, ClanName, ClanXL, NameYL, 1.f, 1.f);
		ClanXL += 4.f;
	}
	float MaxNameWidth = 0.42f*ScaledCellWidth - (PlayerState->bIsFriend ? 30.f*RenderScale : 0.f);
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	UFont* NameFont = UTHUDOwner->SmallFont;
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);

	// Null-guarded local-owner check (see feedback_scoreboard_drawplayer_null_guards.md).
	int32 Ping = PlayerState->Ping * 4;
	const bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner
		&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner)
	{
		Ping = PlayerState->ExactPing;
		BarOpacity = 0.5f;
	}

	// Background border.
	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor) { BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0); FinalBarOpacity = 0.75f; }
	if (PlayerState == SelectedPlayer) { BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0); FinalBarOpacity = 0.75f; }

	DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth,
		0.9f*CellHeight*RenderScale, 149, 138, 32, 32, FinalBarOpacity, BarColor);

	if (PlayerState->KickCount > 0)
	{
		float NumPlayers = 0.0f;
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			if (!UTGameState->PlayerArray[i]->bIsSpectator
				&& !UTGameState->PlayerArray[i]->bOnlySpectator
				&& !UTGameState->PlayerArray[i]->bIsABot)
			{
				if (!UTGameState->bOnlyTeamCanVoteKick
					|| UTGameState->OnSameTeam(PlayerState, UTGameState->PlayerArray[i]))
				{
					NumPlayers += 1.0f;
				}
			}
		}
		if (NumPlayers > 0.0f)
		{
			float KickPercent = float(PlayerState->KickCount) / NumPlayers;
			float XL, SmallYL;
			Canvas->TextSize(UTHUDOwner->SmallFont, "Kick", XL, SmallYL, RenderScale, RenderScale);
			DrawText(NSLOCTEXT("UTScoreboard", "Kick", "Kick"), XOffset + (ScaledCellWidth * FlagX),
				YOffset + ColumnY - 0.27f*SmallYL, UTHUDOwner->TinyFont, RenderScale, 1.0f,
				DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
			FText Kick = FText::Format(NSLOCTEXT("Common", "PercFormat", "{0}%"),
				FText::AsNumber(int32(KickPercent * 100.0)));
			DrawText(Kick, XOffset + (ScaledCellWidth * FlagX), YOffset + ColumnY + 0.33f*SmallYL,
				UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
				ETextHorzPos::Left, ETextVertPos::Center);
		}
	}
	else
	{
		FTextureUVs FlagUV;
		UTexture2D* NewFlagAtlas = UTHUDOwner->ResolveFlag(PlayerState, FlagUV);
		DrawTexture(NewFlagAtlas, XOffset + (ScaledCellWidth * FlagX), YOffset + 14.f*RenderScale,
			FlagUV.UL*RenderScale, FlagUV.VL*RenderScale, FlagUV.U, FlagUV.V, 36, 26, 1.0,
			FLinearColor::White, FVector2D(0.0f, 0.5f));
	}

	FVector2D NameSize;
	float NameScaling = FMath::Min(RenderScale, MaxNameWidth / FMath::Max(NameXL + ClanXL, 1.f));
	if (!PlayerState->EpicAccountName.IsEmpty())
	{
		NameSize = DrawText(FText::FromString(ClanName),
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
		NameSize += DrawText(FText::FromString(DisplayName),
			XOffset + NameScaling*ClanXL + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, false, FVector2D(0.f, 0.f), FLinearColor::Black, true,
			GetPlayerHighlightColorFor(PlayerState), NameScaling, 1.0f, DrawColor,
			FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), ETextHorzPos::Left, ETextVertPos::Center);
	}
	else
	{
		NameSize = DrawText(FText::FromString(DisplayName),
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnY,
			NameFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
	}

	if (PlayerState->bIsFriend)
	{
		DrawTexture(UTHUDOwner->ScoreboardAtlas,
			XOffset + (ScaledCellWidth * ColumnHeaderPlayerX) + NameSize.X*NameScaling + 5.f*RenderScale,
			YOffset + 18.f*RenderScale, 30.f*RenderScale, 24.f*RenderScale,
			236, 136, 30, 24, 1.0, FLinearColor::White, FVector2D(0.0f, 0.5f));
	}
	if (UTGameState && UTGameState->HasMatchStarted())
	{
		if (PlayerState->bPendingTeamSwitch && !PlayerState->bIsABot)
		{
			DrawText(TeamSwapText, XOffset + (ScaledCellWidth * ColumnHeaderScoreX),
				YOffset + ColumnY, UTHUDOwner->SmallFont, RenderScale, 1.0f,
				FLinearColor::White, ETextHorzPos::Center, ETextVertPos::Center);
		}
		else
		{
			DrawPlayerScore(PlayerState, XOffset, YOffset, ScaledCellWidth, DrawColor);
		}
	}
	else
	{
		DrawReadyText(PlayerState, XOffset, YOffset, ScaledCellWidth);
	}

	// Ping / bot-skill: draw CENTERED at ColumnHeaderPingX so the value sits
	// under the column header instead of right-aligning at the row's right
	// edge (engine default 0.995). Same TinyFont + 0.75 scale as engine.
	AUTBot* Bot = Cast<AUTBot>(PlayerState->GetOwner());
	if (Bot)
	{
		static const FNumberFormattingOptions SkillValueFormattingOptions = FNumberFormattingOptions()
			.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		DrawText(FText::AsNumber(Bot->Skill, &SkillValueFormattingOptions),
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, 0.75f*RenderScale, 1.f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}
	else if (GetWorld()->GetNetMode() != NM_Standalone)
	{
		FText PingText = FText::Format(PingFormatText, FText::AsNumber(Ping));
		DrawText(PingText, XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, 0.75f*RenderScale, 1.f, DrawColor,
			ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Strike out players that are out of lives.
	if (PlayerState->bOutOfLives)
	{
		float Height = 8.0f;
		float XL, YL;
		Canvas->TextSize(UTHUDOwner->SmallFont,
			(PlayerState->PlayerName + PlayerState->ClanName), XL, YL, RenderScale, RenderScale);
		float StrikeWidth = FMath::Min(0.475f*ScaledCellWidth, XL);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX),
			YOffset + ColumnY, StrikeWidth, Height, 185.f, 400.f, 4.f, 4.f, 1.0f, FLinearColor::Red);
	}

	// Mute block intentionally omitted (matches elim/wipeout DrawPlayer
	// overrides; IsPlayerGameMuted's UTPlayerOwner deref is the kind of
	// unguarded access we just spent a session debugging).
	if (PlayerState->bIsTalking)
	{
		bool bLeft = (XOffset < Canvas->ClipX * 0.5f);
		float TalkingXOffset = bLeft ? ScaledCellWidth + (10.0f * RenderScale) : (-36.0f * RenderScale);
		FTextureUVs ChatIconUVs = bLeft
			? FTextureUVs(497.0f, 965.0f, 35.0f, 31.0f)
			: FTextureUVs(532.0f, 965.0f, -35.0f, 31.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset,
			YOffset + ((CellHeight * 0.5f - 24.0f) * RenderScale),
			(26 * RenderScale), (23 * RenderScale),
			ChatIconUVs.U, ChatIconUVs.V, ChatIconUVs.UL, ChatIconUVs.VL, 1.0f);
	}
}
