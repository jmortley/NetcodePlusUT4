// ElimPlusScoreboard.cpp — Portrait-row scoreboard for ElimPlus.
// Reads stats from AElimPlusStatsReplicator (Damage, PPRCurrent, Elo, LGAcc).
// Falls back to PlayerState->DamageDone when the replicator isn't available
// (listen-server / standalone). 7 columns: Name | K | D | DMG | PPR | ELO | LG_Acc | Ping.

#include "ElimPlusScoreboard.h"
#include "NCPlusScoreboardHost.h"
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "UTBot.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "ElimPlusStatsReplicator.h"
#include "EngineUtils.h"

UElimPlusScoreboard::UElimPlusScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CellHeight = 80.f;
	CellWidth = 850.f;

	// 6 stat columns after Name (K/D/DMG/PPR/ELO/LG_Acc) + Ping. Spaced for
	// readability at 1080p — wider gaps than initial 9-column layout. Available
	// width = 0.83 (between 0.10 name and 0.93 ping); 0.83/7 ≈ 0.12 per slot.
	ColumnHeaderPlayerX  = 0.10f;
	ColumnHeaderScoreX   = 0.30f; // unused but required by base class
	ColumnHeaderKillsX   = 0.35f;
	ColumnHeaderDeathsX  = 0.43f;
	ColumnHeaderDamageX  = 0.53f;
	ColumnHeaderPPRCurX  = 0.63f;
	ColumnHeaderEloX     = 0.74f; // wider neighbor gaps for "1400 +12" delta text
	ColumnHeaderLGAccX   = 0.85f;
	ColumnHeaderPingX    = 0.94f;

	CH_Kills  = NSLOCTEXT("ElimPlusScoreboard", "Kills",  "K");
	CH_Deaths = NSLOCTEXT("ElimPlusScoreboard", "Deaths", "D");
	CH_Damage = NSLOCTEXT("ElimPlusScoreboard", "Damage", "DMG");
	CH_PPRCur = NSLOCTEXT("ElimPlusScoreboard", "PPRCur", "PPR");
	CH_Elo    = NSLOCTEXT("ElimPlusScoreboard", "Elo",    "ELO");
	CH_LGAcc  = NSLOCTEXT("ElimPlusScoreboard", "LGAcc",  "LG_Acc");

	bUseRoundKills = false; // overall match stats

	// Portrait atlas UV coords — same as WipeoutScoreboard / FlagRun.
	// Texture pointer is grabbed from UTHUDOwner at draw time.
	RedTeamIcon.U = 5.f;
	RedTeamIcon.V = 5.f;
	RedTeamIcon.UL = 224.f;
	RedTeamIcon.VL = 310.f;

	BlueTeamIcon.U = 237.f;
	BlueTeamIcon.V = 5.f;
	BlueTeamIcon.UL = 224.f;
	BlueTeamIcon.VL = 310.f;

	BlueTeamOverlay.U = 237.0f;
	BlueTeamOverlay.V = 330.0f;
	BlueTeamOverlay.UL = 224.0f;
	BlueTeamOverlay.VL = 310.0f;

	RedTeamOverlay.U = 5.0f;
	RedTeamOverlay.V = 330.0f;
	RedTeamOverlay.UL = 224.0f;
	RedTeamOverlay.VL = 310.0f;
}

bool UElimPlusScoreboard::HasCustomTeamColors() const
{
	if (!UTGameState) return false;

	for (int32 i = 0; i < 2; i++)
	{
		if (!UTGameState->Teams.IsValidIndex(i) || !UTGameState->Teams[i]) continue;
		FLinearColor TC = UTGameState->Teams[i]->TeamColor;
		if (i == 0)
		{
			if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f)
				return true;
		}
		else
		{
			if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f)
				return true;
		}
	}
	return false;
}

void UElimPlusScoreboard::DrawTeamPanel(float RenderDelta, float& YOffset)
{
	if (!UTGameState || UTGameState->Teams.Num() < 2 || !UTGameState->Teams[0] || !UTGameState->Teams[1]) return;

	// Faction names only when custom team colors are in use AND the scorebar's
	// Team-Color toggle is on — untick it and the scoreboard reads plain RED/BLUE
	// (unifies with the top bar; kills the "am I red, blue, or phayder?" confusion).
	const bool bCustom = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));
	RedTeamText  = bCustom ? FText::FromString(TEXT("PHAYDER (R)")) : FText::FromString(TEXT("RED"));
	BlueTeamText = bCustom ? FText::FromString(TEXT("LIANDRI (B)")) : FText::FromString(TEXT("BLUE"));

	const float Width = 0.5f * (Size.X - 400.f) * RenderScale;
	const float FrontSize = 35.f * RenderScale;
	const float EndSize = 16.f * RenderScale;
	const float MiddleSize = Width - FrontSize - EndSize;
	const float BackgroundY = YOffset + 22.f * RenderScale;
	const float TeamTextY = YOffset + 40.f * RenderScale;
	const float TeamScoreY = YOffset + 36.f * RenderScale;
	const float BackgroundHeight = 65.f * RenderScale;
	const float TeamEdgeSize = 40.f * RenderScale;
	const float NamePosition = TeamEdgeSize + FrontSize + 0.25f * MiddleSize;

	// Background color follows the same toggle as the names: custom team colors when
	// faction mode is on, stock red/blue when the user wants plain RED vs BLUE — so a
	// "RED" label never sits on a magenta bar.
	const FLinearColor Team0Color = bCustom ? UTGameState->Teams[0]->TeamColor : FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	const FLinearColor Team1Color = bCustom ? UTGameState->Teams[1]->TeamColor : FLinearColor(0.05f, 0.1f, 0.9f, 1.f);

	// Team 0 (left)
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize, BackgroundY, FrontSize, BackgroundHeight, 0, 188, 36, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize, BackgroundY, MiddleSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize + MiddleSize, BackgroundY, EndSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);

	DrawText(RedTeamText, NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[0]->Score), TeamEdgeSize + FrontSize + MiddleSize - EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0, 0), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * RedScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Right, ETextVertPos::Center);

	// Team 1 (right)
	const float LeftEdge = Canvas->ClipX - TeamEdgeSize - FrontSize - MiddleSize - EndSize;

	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize + MiddleSize, BackgroundY, FrontSize, BackgroundHeight, 196, 188, 36, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize, BackgroundY, MiddleSize, BackgroundHeight, 130, 188, 64, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge, BackgroundY, EndSize, BackgroundHeight, 117, 188, 16, 65, 1.f, Team1Color);

	DrawText(BlueTeamText, Canvas->ClipX - NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Right, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[1]->Score), LeftEdge + 2.f * EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0.f, 0.f), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * BlueScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Left, ETextVertPos::Center);

	YOffset += 119.f * RenderScale;
	BlueScoreScaling = FMath::Max(BlueScoreScaling - RenderDelta, 1.f);
	RedScoreScaling = FMath::Max(RedScoreScaling - RenderDelta, 1.f);
}

void UElimPlusScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	const float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		// Header background
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height,
			149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));

		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_Kills,  XOffset + (ScaledCellWidth * ColumnHeaderKillsX),  YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Deaths, XOffset + (ScaledCellWidth * ColumnHeaderDeathsX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_PPRCur, XOffset + (ScaledCellWidth * ColumnHeaderPPRCurX), YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Elo,    XOffset + (ScaledCellWidth * ColumnHeaderEloX),    YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_LGAcc,  XOffset + (ScaledCellWidth * ColumnHeaderLGAccX),  YOffset + ColumnHeaderY, UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
		}
		DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, RenderScale, RenderScale, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4.f;
}

void UElimPlusScoreboard::DrawPortraitPip(AUTPlayerState* PlayerState, float XOffset, float YOffset, float PipWidth, float PipHeight)
{
	if (!UTHUDOwner || !UTHUDOwner->CharacterPortraitAtlas) return;

	UTexture2D* Atlas = UTHUDOwner->CharacterPortraitAtlas;
	if (RedTeamIcon.Texture == nullptr)
	{
		RedTeamIcon.Texture = Atlas;
		BlueTeamIcon.Texture = Atlas;
		RedTeamOverlay.Texture = Atlas;
		BlueTeamOverlay.Texture = Atlas;
	}

	// In elim, dead means dead-until-round-end. Show portrait dimmed; no countdown.
	AUTCharacter* UTC_Pip = PlayerState->GetUTCharacter();
	const bool bIsDead = (UTC_Pip == nullptr || UTC_Pip->IsDead());
	const uint8 TeamNum = PlayerState->GetTeamNum();

	// Layer 1: Team-colored background (TeamSkins-aware)
	FLinearColor TeamBGColor = (TeamNum == 1)
		? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)
		: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);
	if (UTGameState && UTGameState->Teams.IsValidIndex(TeamNum) && UTGameState->Teams[TeamNum])
	{
		TeamBGColor = UTGameState->Teams[TeamNum]->TeamColor;
	}
	Canvas->SetLinearDrawColor(TeamBGColor);
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipWidth, PipHeight, 0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FLinearColor::White);

	// Layer 2: Character portrait (dimmed if dead)
	const FCanvasIcon& CharIcon = PlayerState->GetHUDIcon();
	if (CharIcon.Texture != nullptr)
	{
		if (bIsDead)
		{
			Canvas->SetLinearDrawColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.f));
		}
		if (TeamNum == 1)
		{
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
				CharIcon.U + CharIcon.UL, CharIcon.V, CharIcon.UL * -1.0f, CharIcon.VL);
		}
		else
		{
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
				CharIcon.U, CharIcon.V, CharIcon.UL, CharIcon.VL);
		}
	}

	// Layer 3: Full-pip dark dim if dead (no sweep — they don't respawn this round)
	if (bIsDead)
	{
		Canvas->SetLinearDrawColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipWidth, PipHeight, 0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 4: Team-colored frame overlay
	Canvas->SetLinearDrawColor(FLinearColor::White);
	const FCanvasIcon& OverlayIcon = (TeamNum == 1) ? BlueTeamOverlay : RedTeamOverlay;
	Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
		OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);

	// Layer 5: Red "X" on dead portraits (always shown — no respawn this round)
	if (bIsDead)
	{
		const float FontScale = 0.75f * RenderScale;
		FString XStr = TEXT("X");
		float XL, YL;
		Canvas->StrLen(UTHUDOwner->SmallFont, XStr, XL, YL);
		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;
		Canvas->SetLinearDrawColor(FLinearColor(1.f, 0.2f, 0.2f, 0.95f));
		Canvas->DrawText(UTHUDOwner->SmallFont, FText::FromString(XStr),
			XOffset + (PipWidth * 0.5f) - (XL * FontScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontScale * 0.5f),
			FontScale, FontScale, TextRenderInfo);
	}
}

// Helper: locate the stats replicator on this client
static AElimPlusStatsReplicator* FindElimPlusStatsRep(UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AElimPlusStatsReplicator> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void UElimPlusScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset)
{
	if (PlayerState == nullptr) return;

	float BarOpacity = 0.3f;
	bool bIsUnderCursor = false;

	if (bIsInteractive)
	{
		FVector4 Bounds = FVector4(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + ScaledCellWidth, RenderPosition.Y + YOffset + CellHeight * RenderScale);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
		bIsUnderCursor = (CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z &&
			CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W);
	}

	PlayerState->ScoreCorner = FVector(RenderPosition.X + XOffset, RenderPosition.Y + YOffset + 0.25f * CellHeight * RenderScale, 0.f);
	if (!PlayerState->Team || (PlayerState->Team->TeamIndex != 1))
	{
		PlayerState->ScoreCorner.X += ScaledCellWidth;
	}

	const bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner) BarOpacity = 0.5f;

	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor) { BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0); FinalBarOpacity = 0.75f; }
	if (PlayerState == SelectedPlayer) { BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0); FinalBarOpacity = 0.75f; }

	DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, 0.95f * CellHeight * RenderScale,
		149, 138, 32, 32, FinalBarOpacity, BarColor);

	// Portrait pip on the left
	const float PipPadding = 4.f * RenderScale;
	const float PipHeight = (CellHeight * RenderScale * 0.9f) - (PipPadding * 2.f);
	const float PipWidth = PipHeight * (224.0f / 310.0f);
	const float PipX = XOffset + PipPadding;
	const float PipY = YOffset + PipPadding;
	DrawPortraitPip(PlayerState, PipX, PipY, PipWidth, PipHeight);

	// Player name
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);
	AUTCharacter* UTC_Name = PlayerState->GetUTCharacter();
	const bool bIsDead = (UTC_Name == nullptr || UTC_Name->IsDead());
	if (bIsDead) DrawColor *= 0.6f;

	FString DisplayName = PlayerState->PlayerName;
	float NameXL, NameYL;
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	const float MaxNameWidth = 0.22f * ScaledCellWidth; // tighter — 9 stat columns to fit
	const float NameScaling = FMath::Min(RenderScale, MaxNameWidth / FMath::Max(NameXL, 1.f));

	const float NameX = XOffset + (ScaledCellWidth * ColumnHeaderPlayerX);
	const float NameY = YOffset + ColumnY * 0.7f;

	if (!PlayerState->EpicAccountName.IsEmpty())
	{
		DrawText(FText::FromString(DisplayName), NameX, NameY, UTHUDOwner->SmallFont, false,
			FVector2D(0.f, 0.f), FLinearColor::Black, true, GetPlayerHighlightColorFor(PlayerState),
			NameScaling, 1.0f, DrawColor, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), ETextHorzPos::Left, ETextVertPos::Center);
	}
	else
	{
		DrawText(FText::FromString(DisplayName), NameX, NameY, UTHUDOwner->SmallFont, NameScaling, 1.0f, DrawColor, ETextHorzPos::Left, ETextVertPos::Center);
	}

	if (bIsOwner)
	{
		// U+25B6 BLACK RIGHT-POINTING TRIANGLE. Escape form is required because the
		// literal char in source bytes gets misinterpreted under Windows ANSI source
		// encoding (rendered as garbage glyphs like "ä-").
		DrawText(FText::FromString(FString::Chr(0x25B6)), NameX - 14.f * RenderScale, NameY, UTHUDOwner->TinyFont, RenderScale, 1.0f,
			FLinearColor(0.3f, 1.f, 0.3f, 1.f), ETextHorzPos::Left, ETextVertPos::Center);
	}

	// Match-host badge — tags the player who pressed Enter to start the match.
	NCPlusScoreboardHost::DrawHostMarker(this, UTHUDOwner, PlayerState, UTGameState,
		NameX + NameXL * NameScaling + 8.f * RenderScale, NameY, RenderScale);

	// HP/Armor bars for alive teammates (same pattern as Wipeout)
	AUTCharacter* UTC = PlayerState->GetUTCharacter();
	if (UTC != nullptr)
	{
		bool bShowBars = true;
		if (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState)
		{
			AUTPlayerState* LocalPS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
			bShowBars = UTGameState->OnSameTeam(PlayerState, LocalPS) || LocalPS->bOnlySpectator;
		}

		if (bShowBars)
		{
			const float HealthPct = FMath::Clamp(float(UTC->Health) / float(UTC->SuperHealthMax), 0.f, 1.f);
			const float ArmorPct = float(UTC->GetArmorAmount()) / float(FMath::Max(UTC->MaxStackedArmor, 1));
			const float BarHeight = 5.f * RenderScale;
			const float BarY = NameY + 14.f * RenderScale;
			const float BarX = NameX;
			const float HealthBarWidth = 80.f * RenderScale;
			const float ArmorBarWidth = 60.f * RenderScale;

			FLinearColor BarBG(0.15f, 0.15f, 0.15f, 0.6f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX, BarY, HealthBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			FLinearColor HealthColor(0.25f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX + 1.f, BarY + 1.f, (HealthBarWidth - 2.f) * HealthPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, HealthColor);

			const float ArmorBarX = BarX + HealthBarWidth + 6.f * RenderScale;
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX, BarY, ArmorBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			FLinearColor ArmorColor(0.8f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX + 1.f, BarY + 1.f, (ArmorBarWidth - 2.f) * ArmorPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, ArmorColor);
		}
	}

	// Stat columns
	if (UTGameState && UTGameState->HasMatchStarted())
	{
		DrawPlayerScore(PlayerState, XOffset, YOffset, ScaledCellWidth, DrawColor);
	}
	else
	{
		DrawReadyText(PlayerState, XOffset, YOffset, ScaledCellWidth);
	}

	// Ping / Bot skill (same as Wipeout)
	AUTBot* Bot = Cast<AUTBot>(PlayerState->GetOwner());
	if (Bot)
	{
		static const FNumberFormattingOptions SkillFmt = FNumberFormattingOptions()
			.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		DrawText(FText::AsNumber(Bot->Skill, &SkillFmt), XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
	else if (GetWorld()->GetNetMode() != NM_Standalone)
	{
		const int32 Ping = bIsOwner ? PlayerState->ExactPing : (PlayerState->Ping * 4);
		const FLinearColor PingColor = (Ping < 60) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
			: (Ping < 120) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
			: FLinearColor(1.f, 0.25f, 0.25f, 1.f);
		DrawText(FText::FromString(FString::Printf(TEXT("%dms"), Ping)),
			XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, PingColor, ETextHorzPos::Center, ETextVertPos::Center);
	}
}

void UElimPlusScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	// Resolve replicator + player id once. Bots have invalid UniqueIds, so use
	// the same synthetic "BOT:<name>" key the rating/replicator pair publishes
	// — so when bRandomizeBotElo is on, bots show their assigned ELO instead
	// of the default 1400 fallback.
	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());
	FString PId;
	if (PlayerState)
	{
		PId = PlayerState->UniqueId.IsValid()
			? PlayerState->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PlayerState->PlayerName);
	}
	const FElimPlusStatsEntry* Entry = (Stats && !PId.IsEmpty()) ? Stats->FindEntry(PId) : nullptr;

	const FLinearColor DimColor = (PlayerState->GetUTCharacter() == nullptr) ? FLinearColor(0.6f, 0.6f, 0.6f, 1.f) * DrawColor : DrawColor;

	// Kills
	const int32 Kills = PlayerState->Kills + PlayerState->KillAssists;
	DrawText(FText::AsNumber(Kills),
		XOffset + (Width * ColumnHeaderKillsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Deaths
	DrawText(FText::AsNumber(PlayerState->Deaths),
		XOffset + (Width * ColumnHeaderDeathsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage — replicator preferred; fall back to direct PlayerState read on listen-server
	int32 Damage = 0;
	if (Entry)
	{
		Damage = Entry->DamageDone;
	}
	else
	{
		Damage = int32(PlayerState->DamageDone);
	}
	const FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f) * (DimColor / DrawColor);
	DrawText(FText::AsNumber(Damage),
		XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// PPR (Current) — match-running mean across completed rounds (gamemode populates)
	const float PPRCur = Entry ? Entry->PPRCurrent : 0.f;
	DrawText(FText::FromString(FString::Printf(TEXT("%.1f"), PPRCur)),
		XOffset + (Width * ColumnHeaderPPRCurX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, DimColor, ETextHorzPos::Center, ETextVertPos::Center);

	// ELO + delta — source of truth is the replicator (gamemode pushes from
	// Mods.db). Don't fall back to PlayerState rank fields — TDMRank etc. are
	// defunct in this fork (see feedback_no_epic_mcp_or_tdmrank memory).
	const int32 Elo = Entry ? Entry->Elo : 1400;
	const int32 EloDelta = Entry ? Entry->EloDeltaThisMatch : 0;
	const int32 Rank = Entry ? Entry->GlobalRank : 0;
	FString EloStr = FString::Printf(TEXT("%d"), Elo);
	if (Rank > 0)
	{
		// Global leaderboard rank in parens, with English ordinal suffix:
		// 1st, 2nd, 3rd, 4th ... 11th-13th, 21st, 200th.
		const int32 M100 = Rank % 100;
		const int32 M10  = Rank % 10;
		const TCHAR* Suf = (M100 >= 11 && M100 <= 13) ? TEXT("th")
			: (M10 == 1) ? TEXT("st") : (M10 == 2) ? TEXT("nd") : (M10 == 3) ? TEXT("rd") : TEXT("th");
		EloStr += FString::Printf(TEXT(" (%d%s)"), Rank, Suf);
	}
	if (EloDelta != 0)
	{
		EloStr += (EloDelta > 0)
			? FString::Printf(TEXT(" +%d"), EloDelta)
			: FString::Printf(TEXT(" %d"), EloDelta);
	}
	FLinearColor EloColor = DimColor;
	if (EloDelta > 0)      EloColor = FLinearColor(0.4f, 1.f, 0.4f, 1.f);
	else if (EloDelta < 0) EloColor = FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	DrawText(FText::FromString(EloStr),
		XOffset + (Width * ColumnHeaderEloX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, EloColor, ETextHorzPos::Center, ETextVertPos::Center);

	// LG_Acc — Sniper / Lightning Gun hitscan accuracy, computed + replicated
	// server-side (NAME_SniperHits/Shots). -1 = no sniper shots fired -> show "-"
	// (matches NCPlusCTFScoreboard) instead of a misleading 0%.
	const int32 LGAccPacked = Entry ? Entry->LinkGunAccuracyTimes100 : -1;
	const bool bHasLGAcc = (LGAccPacked >= 0);
	const float LGAcc = bHasLGAcc ? (static_cast<float>(LGAccPacked) / 100.f) : 0.f;
	const FLinearColor LGColor = !bHasLGAcc       ? FLinearColor(0.5f, 0.5f, 0.5f, 1.f)
		: (LGAcc >= 35.f) ? FLinearColor(0.4f, 1.f, 0.4f, 1.f)
		: (LGAcc >= 20.f) ? FLinearColor(1.f, 1.f, 0.4f, 1.f)
		: (LGAcc > 0.f)   ? FLinearColor(1.f, 0.5f, 0.4f, 1.f)
		: FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
	DrawText(FText::FromString(bHasLGAcc ? FString::Printf(TEXT("%.0f%%"), LGAcc) : TEXT("-")),
		XOffset + (Width * ColumnHeaderLGAccX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, RenderScale, LGColor, ETextHorzPos::Center, ETextVertPos::Center);
}

void UElimPlusScoreboard::DrawPlayerScores(float RenderDelta, float& YOffset)
{
	if (!UTGameState) return;

	int32 XOffset = ScaledEdgeSize;
	float MaxYOffset = 0.f;
	TArray<FString> SpectatorNames;

	// PPR(Current) lookup for row ordering — same replicator + uid resolution
	// DrawPlayerScore uses (bots key on the synthetic "BOT:<name>"). 0 fallback
	// when the replicator isn't up or no round has completed yet, in which case
	// the kills/score tiebreaks below preserve a sensible order.
	AElimPlusStatsReplicator* Stats = FindElimPlusStatsRep(GetWorld());
	auto GetPPR = [Stats](AUTPlayerState* PS) -> float
	{
		if (!Stats || !PS) return 0.f;
		const FString PId = PS->UniqueId.IsValid()
			? PS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
		const FElimPlusStatsEntry* E = PId.IsEmpty() ? nullptr : Stats->FindEntry(PId);
		return E ? E->PPRCurrent : 0.f;
	};

	for (int8 Team = 0; Team < 2; Team++)
	{
		int32 Place = 1;
		float DrawOffset = YOffset;
		const int32 NumPlayersToShow = ShouldDrawScoringStats() ? 5 : UTGameState->PlayerArray.Num();

		// Collect this team's players (harvesting spectators once, on the team-0
		// pass), then sort by PPR(Current) desc so the board ranks by PPR. Kills
		// then score break ties (and carry the ordering before any round ends,
		// when every PPR is still 0).
		TArray<AUTPlayerState*> TeamPlayers;
		TMap<const AUTPlayerState*, float> PPRByPlayer;
		for (int32 i = 0; i < UTGameState->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PlayerState = Cast<AUTPlayerState>(UTGameState->PlayerArray[i]);
			if (!PlayerState) continue;
			if (PlayerState->bOnlySpectator)
			{
				if (Team == 0 && !PlayerState->bIsDemoRecording)
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
				continue;
			}
			if (PlayerState->GetTeamNum() == Team)
			{
				TeamPlayers.Add(PlayerState);
				PPRByPlayer.Add(PlayerState, GetPPR(PlayerState));
			}
		}
		TeamPlayers.Sort([&PPRByPlayer](const AUTPlayerState& A, const AUTPlayerState& B)
		{
			const float PA = PPRByPlayer.FindRef(&A);
			const float PB = PPRByPlayer.FindRef(&B);
			if (PA != PB) return PA > PB;
			const int32 KA = A.Kills + A.KillAssists;
			const int32 KB = B.Kills + B.KillAssists;
			if (KA != KB) return KA > KB;
			return A.Score > B.Score;
		});

		for (AUTPlayerState* PlayerState : TeamPlayers)
		{
			DrawPlayer(Place, PlayerState, RenderDelta, XOffset, DrawOffset);
			Place++;
			DrawOffset += CellHeight * RenderScale;
			if (Place > NumPlayersToShow) break;
		}

		MaxYOffset = FMath::Max(DrawOffset, MaxYOffset);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset = MaxYOffset;

	if (UTGameState->GoalScore > 0 && !ShouldDrawScoringStats())
	{
		FString GoalStr = FString::Printf(TEXT("First to %d"), UTGameState->GoalScore);
		DrawText(FText::FromString(GoalStr), Canvas->ClipX * 0.5f, YOffset + 4.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}
