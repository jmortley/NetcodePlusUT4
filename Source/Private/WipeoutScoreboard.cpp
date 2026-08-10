// WipeoutScoreboard — Portrait-row scoreboard for Wipeout game mode
#include "WipeoutScoreboard.h"
#include "NCPlusScoreboardHost.h"
#include "NCPlusScoreboardReady.h"
#include "NCPlusHUDLayout.h"
#include "NCPlusCTFGameMode.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "UTBot.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "UTDemoRecSpectator.h"
#include "WipeoutDamageReplicator.h"
#include "StatNames.h"
#include "EngineUtils.h"

UWipeoutScoreboard::UWipeoutScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	// Taller cells to accommodate portrait pip + HP/armor bars
	CellHeight = 80.f;
	CellWidth = 850.f;

	// Column positions (fraction of CellWidth) — 6 data columns after player name
	ColumnHeaderPlayerX = 0.10f;    // Name starts after portrait
	ColumnHeaderScoreX = 0.38f;     // Score/round wins (unused in Wipeout but keep for base class)
	// Even 0.09 spacing across the six stat columns. Efficiency was dropped (it is
	// just K/(K+D), already readable straight off the K/D column) and its slot went
	// to the two stats that were previously invisible: V/S and HEAL.
	ColumnHeaderKDX = 0.37f;
	ColumnHeaderBeltAmpX = 0.46f;
	ColumnHeaderVestSiphonX = 0.55f;
	ColumnHeaderDamageX = 0.64f;
	ColumnHeaderHealX = 0.73f;
	ColumnHeaderDmgPerLifeX = 0.82f;
	ColumnHeaderPingX = 0.92f;
	// Unused by Wipeout now, but inherited (and repurposed) by the ShaftArena,
	// LeagueDuel and ShockDom scoreboards — keep the default they were built on.
	ColumnHeaderEfficiencyX = 0.68f;

	CH_KD = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_KD", "K/D");
	CH_BeltAmp = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_BeltAmp", "B/A");
	CH_Damage = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_Damage", "DMG");
	CH_VestSiphon = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_VestSiphon", "V/S");
	CH_Heal = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_Heal", "HEAL");
	// Wipeout no longer draws this, but ShockDom (which inherits this scoreboard)
	// still renders it as a real efficiency column and relies on this default.
	CH_Efficiency = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_Efficiency", "Eff%");
	CH_DmgPerLife = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_DmgPerLife", "DMG/Life");

	bUseRoundKills = false;  // Show overall match stats, not per-round

	// Portrait atlas UVs — same as WipeoutHUD / FlagRun
	// Note: CharacterPortraitAtlas is on AUTHUD, so we grab it at draw time from UTHUDOwner
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

void UWipeoutScoreboard::DrawReadyText(AUTPlayerState* PlayerState,
	float XOffset, float YOffset, float Width)
{
	FText PlayerReady;
	if (!NCPlusScoreboardReady::TryGetText(GetWorld(), PlayerState,
		TeamSwapText, PlayerReady))
	{
		Super::DrawReadyText(PlayerState, XOffset, YOffset, Width);
		return;
	}

	ReadyColor = FLinearColor::White;
	ReadyScale = 1.f;
	DrawText(PlayerReady, XOffset + ScaledCellWidth * ColumnHeaderScoreX,
		YOffset + ColumnY, UTHUDOwner->SmallFont, ReadyScale * RenderScale, 1.f,
		ReadyColor, ETextHorzPos::Center, ETextVertPos::Center);
}

// ─── Custom team colors check ──────────────────────────────────────────
bool UWipeoutScoreboard::HasCustomTeamColors() const
{
	if (!UTGameState) return false;

	for (int32 i = 0; i < 2; i++)
	{
		if (!UTGameState->Teams.IsValidIndex(i) || !UTGameState->Teams[i]) continue;
		FLinearColor TC = UTGameState->Teams[i]->TeamColor;
		if (i == 0)
		{
			// Check if team 0 is non-standard red
			if (FMath::Abs(TC.R - 1.f) > 0.2f || TC.G > 0.3f || TC.B > 0.3f)
				return true;
		}
		else
		{
			// Check if team 1 is non-standard blue
			if (FMath::Abs(TC.B - 1.f) > 0.2f || TC.R > 0.3f || TC.G > 0.3f)
				return true;
		}
	}
	return false;
}

// ─── DrawTeamPanel override — use dynamic team colors and custom names ─
void UWipeoutScoreboard::DrawTeamPanel(float RenderDelta, float& YOffset)
{
	if (!UTGameState || UTGameState->Teams.Num() < 2 || !UTGameState->Teams[0] || !UTGameState->Teams[1]) return;

	// Faction names only when custom team colors are in use AND the scorebar's
	// Team-Color toggle is on — untick it and the scoreboard reads plain RED/BLUE.
	bool bCustom = HasCustomTeamColors() && NCPlusHUDDrawCall::GetUseTeamColor(TEXT("scorebar"));

	// Set team names based on whether custom colors are active
	RedTeamText = bCustom ? FText::FromString(TEXT("PHAYDER (R)")) : FText::FromString(TEXT("RED"));
	BlueTeamText = bCustom ? FText::FromString(TEXT("LIANDRI (B)")) : FText::FromString(TEXT("BLUE"));

	// Call base — it uses RedTeamText/BlueTeamText and draws with FLinearColor::Red/Blue
	// We override the draw colors by using the team colors directly
	float Width = 0.5f * (Size.X - 400.f) * RenderScale;
	float FrontSize = 35.f * RenderScale;
	float EndSize = 16.f * RenderScale;
	float MiddleSize = Width - FrontSize - EndSize;
	float BackgroundY = YOffset + 22.f * RenderScale;
	float TeamTextY = YOffset + 40.f * RenderScale;
	float TeamScoreY = YOffset + 36.f * RenderScale;
	float BackgroundHeight = 65.f * RenderScale;
	float TeamEdgeSize = 40.f * RenderScale;
	float NamePosition = TeamEdgeSize + FrontSize + 0.25f * MiddleSize;

	// Background follows the same toggle as the names (parity with ElimPlusScoreboard):
	// custom team colors when faction mode is on, stock red/blue when the user wants plain
	// RED vs BLUE — so a "RED" label never sits on a magenta bar.
	FLinearColor Team0Color = bCustom ? UTGameState->Teams[0]->TeamColor : FLinearColor(0.8f, 0.05f, 0.05f, 1.f);
	FLinearColor Team1Color = bCustom ? UTGameState->Teams[1]->TeamColor : FLinearColor(0.05f, 0.1f, 0.9f, 1.f);

	// Team 0 (left)
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize, BackgroundY, FrontSize, BackgroundHeight, 0, 188, 36, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize, BackgroundY, MiddleSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, TeamEdgeSize + FrontSize + MiddleSize, BackgroundY, EndSize, BackgroundHeight, 39, 188, 64, 65, 1.0f, Team0Color);

	DrawText(RedTeamText, NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Left, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[0]->Score), TeamEdgeSize + FrontSize + MiddleSize - EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0, 0), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * RedScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Right, ETextVertPos::Center);

	// Team 1 (right)
	float LeftEdge = Canvas->ClipX - TeamEdgeSize - FrontSize - MiddleSize - EndSize;

	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize + MiddleSize, BackgroundY, FrontSize, BackgroundHeight, 196, 188, 36, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge + EndSize, BackgroundY, MiddleSize, BackgroundHeight, 130, 188, 64, 65, 1.f, Team1Color);
	DrawTexture(UTHUDOwner->ScoreboardAtlas, LeftEdge, BackgroundY, EndSize, BackgroundHeight, 117, 188, 16, 65, 1.f, Team1Color);

	DrawText(BlueTeamText, Canvas->ClipX - NamePosition, TeamTextY, UTHUDOwner->HugeFont, RenderScale, 1.f, FLinearColor::White, ETextHorzPos::Right, ETextVertPos::Center);
	DrawText(FText::AsNumber(UTGameState->Teams[1]->Score), LeftEdge + 2.f * EndSize, TeamScoreY, UTHUDOwner->HugeFont, false, FVector2D(0.f, 0.f), FLinearColor::Black, true, FLinearColor::Black, 1.5f * RenderScale * BlueScoreScaling, 1.f, FLinearColor::White, FLinearColor(0.f, 0.f, 0.f, 0.f), ETextHorzPos::Left, ETextVertPos::Center);

	YOffset += 119.f * RenderScale;
	BlueScoreScaling = FMath::Max(BlueScoreScaling - RenderDelta, 1.f);
	RedScoreScaling = FMath::Max(RedScoreScaling - RenderDelta, 1.f);
}

void UWipeoutScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		// Header background
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height,
			149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));

		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_KD, XOffset + (ScaledCellWidth * ColumnHeaderKDX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_BeltAmp, XOffset + (ScaledCellWidth * ColumnHeaderBeltAmpX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_VestSiphon, XOffset + (ScaledCellWidth * ColumnHeaderVestSiphonX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Heal, XOffset + (ScaledCellWidth * ColumnHeaderHealX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_DmgPerLife, XOffset + (ScaledCellWidth * ColumnHeaderDmgPerLifeX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
		}
		DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
			XOffset + (ScaledCellWidth * ColumnHeaderPingX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4.f;
}

void UWipeoutScoreboard::DrawPortraitPip(AUTPlayerState* PlayerState, float XOffset, float YOffset, float PipWidth, float PipHeight)
{
	if (!UTHUDOwner || !UTHUDOwner->CharacterPortraitAtlas) return;

	UTexture2D* Atlas = UTHUDOwner->CharacterPortraitAtlas;

	// Assign textures if not yet set (we can't do ConstructorHelpers in a UObject scoreboard easily)
	if (RedTeamIcon.Texture == nullptr)
	{
		RedTeamIcon.Texture = Atlas;
		BlueTeamIcon.Texture = Atlas;
		RedTeamOverlay.Texture = Atlas;
		BlueTeamOverlay.Texture = Atlas;
	}

	AUTCharacter* UTC_Pip = PlayerState->GetUTCharacter();
	bool bIsDead = (UTC_Pip == nullptr || UTC_Pip->IsDead()) && !PlayerState->bOutOfLives;
	uint8 TeamNum = PlayerState->GetTeamNum();

	// Layer 1: Team background — use dynamic team color (respects TeamSkins)
	FLinearColor TeamBGColor = (TeamNum == 1)
		? FLinearColor(0.1f, 0.2f, 0.8f, 1.f)
		: FLinearColor(0.8f, 0.1f, 0.1f, 1.f);
	if (UTGameState && UTGameState->Teams.IsValidIndex(TeamNum) && UTGameState->Teams[TeamNum])
	{
		TeamBGColor = UTGameState->Teams[TeamNum]->TeamColor;
	}
	Canvas->SetLinearDrawColor(TeamBGColor);
	Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipWidth, PipHeight,
		0, 0, 1, 1);
	Canvas->SetLinearDrawColor(FLinearColor::White);

	// Layer 2: Character portrait
	const FCanvasIcon CharIcon = NCPlusHUDPortraits::Resolve(PlayerState);
	if (CharIcon.Texture != nullptr)
	{
		if (bIsDead)
		{
			Canvas->SetLinearDrawColor(FLinearColor(0.2f, 0.2f, 0.2f, 1.f));
		}

		if (TeamNum == 1)
		{
			// Blue team: flip horizontally
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
				CharIcon.U + CharIcon.UL, CharIcon.V, CharIcon.UL * -1.0f, CharIcon.VL);
		}
		else
		{
			Canvas->DrawTile(CharIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
				CharIcon.U, CharIcon.V, CharIcon.UL, CharIcon.VL);
		}
	}

	// Respawn dark overlay
	if (bIsDead && PlayerState->RespawnTime > 0.f && PlayerState->RespawnWaitTime > 0.f)
	{
		float LiveScaling = FMath::Clamp(1.f - PlayerState->RespawnTime / PlayerState->RespawnWaitTime, 0.f, 1.f);
		Canvas->SetLinearDrawColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		Canvas->DrawTile(Canvas->DefaultTexture,
			XOffset + LiveScaling * PipWidth, YOffset,
			PipWidth - LiveScaling * PipWidth, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}
	else if (bIsDead)
	{
		Canvas->SetLinearDrawColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f));
		Canvas->DrawTile(Canvas->DefaultTexture, XOffset, YOffset, PipWidth, PipHeight,
			0, 0, 1, 1, BLEND_Translucent);
	}

	// Layer 3: Team frame overlay
	Canvas->SetLinearDrawColor(FLinearColor::White);
	const FCanvasIcon& OverlayIcon = (TeamNum == 1) ? BlueTeamOverlay : RedTeamOverlay;
	Canvas->DrawTile(OverlayIcon.Texture, XOffset, YOffset, PipWidth, PipHeight,
		OverlayIcon.U, OverlayIcon.V, OverlayIcon.UL, OverlayIcon.VL);

	// Respawn countdown on dead portraits
	if (bIsDead && PlayerState->RespawnTime > 0.f)
	{
		int32 SecondsRemaining = FMath::CeilToInt(PlayerState->RespawnTime);
		FString CountdownStr = FString::Printf(TEXT("%i"), SecondsRemaining);
		float XL, YL;
		float FontScale = 0.75f * RenderScale;
		Canvas->StrLen(UTHUDOwner->SmallFont, CountdownStr, XL, YL);

		FLinearColor CountdownColor = (TeamNum == 0)
			? FLinearColor(1.f, 0.4f, 0.4f, 1.f)
			: FLinearColor(0.4f, 0.6f, 1.f, 1.f);

		FFontRenderInfo TextRenderInfo;
		TextRenderInfo.bEnableShadow = true;
		Canvas->SetLinearDrawColor(CountdownColor);
		Canvas->DrawText(UTHUDOwner->SmallFont, FText::FromString(CountdownStr),
			XOffset + (PipWidth * 0.5f) - (XL * FontScale * 0.5f),
			YOffset + (PipHeight * 0.5f) - (YL * FontScale * 0.5f),
			FontScale, FontScale, TextRenderInfo);
	}
}

void UWipeoutScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState, float RenderDelta, float XOffset, float YOffset)
{
	if (PlayerState == nullptr) return;

	float BarOpacity = FNCPlusHUDLayout::GetScoreboardOpacity();
	bool bIsUnderCursor = false;

	// Interactive scoreboard hit-test tracking
	if (bIsInteractive)
	{
		FVector4 Bounds = FVector4(RenderPosition.X + XOffset, RenderPosition.Y + YOffset,
			RenderPosition.X + XOffset + ScaledCellWidth, RenderPosition.Y + YOffset + CellHeight * RenderScale);
		SelectionStack.Add(FSelectionObject(PlayerState, Bounds));
		bIsUnderCursor = (CursorPosition.X >= Bounds.X && CursorPosition.X <= Bounds.Z &&
			CursorPosition.Y >= Bounds.Y && CursorPosition.Y <= Bounds.W);
	}

	// Score corner for HUD widget system
	PlayerState->ScoreCorner = FVector(RenderPosition.X + XOffset, RenderPosition.Y + YOffset + 0.25f * CellHeight * RenderScale, 0.f);
	if (!PlayerState->Team || (PlayerState->Team->TeamIndex != 1))
	{
		PlayerState->ScoreCorner.X += ScaledCellWidth;
	}

	// Determine if this is the local player
	bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner) BarOpacity = FMath::Min(1.f, FNCPlusHUDLayout::GetScoreboardOpacity() + 0.2f);

	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor) { BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0); FinalBarOpacity = 0.75f; }
	if (PlayerState == SelectedPlayer) { BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0); FinalBarOpacity = 0.75f; }

	// Row background
	DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, 0.95f * CellHeight * RenderScale,
		149, 138, 32, 32, FinalBarOpacity, BarColor);

	// ---- Portrait pip on the left ----
	float PipPadding = 4.f * RenderScale;
	float PipHeight = (CellHeight * RenderScale * 0.9f) - (PipPadding * 2.f);
	float PipWidth = PipHeight * (224.0f / 310.0f);  // Inverse of portrait aspect ratio
	float PipX = XOffset + PipPadding;
	float PipY = YOffset + PipPadding;

	DrawPortraitPip(PlayerState, PipX, PipY, PipWidth, PipHeight);

	// ---- Player name ----
	FLinearColor DrawColor = GetPlayerColorFor(PlayerState);
	AUTCharacter* UTC_Name = PlayerState->GetUTCharacter();
	bool bIsDead = (UTC_Name == nullptr || UTC_Name->IsDead()) && !PlayerState->bOutOfLives;
	if (bIsDead) DrawColor *= 0.6f;

	FString DisplayName = PlayerState->PlayerName;
	float NameXL, NameYL;
	Canvas->TextSize(UTHUDOwner->SmallFont, DisplayName, NameXL, NameYL, 1.f, 1.f);
	float MaxNameWidth = 0.40f * ScaledCellWidth;
	float NameScaling = FMath::Min(RenderScale, MaxNameWidth / FMath::Max(NameXL, 1.f));

	float NameX = XOffset + (ScaledCellWidth * ColumnHeaderPlayerX);
	float NameY = YOffset + ColumnY * 0.7f;  // Slightly higher to leave room for bars

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

	// Owner indicator
	if (bIsOwner)
	{
		DrawText(FText::FromString(TEXT("\u25B6")), NameX - 14.f * RenderScale, NameY, UTHUDOwner->TinyFont, RenderScale, 1.0f,
			FLinearColor(0.3f, 1.f, 0.3f, 1.f), ETextHorzPos::Left, ETextVertPos::Center);
	}

	// Match-host badge \u2014 tags the player who pressed Enter to start the match,
	// just right of their name. Shared across every NetcodePlus scoreboard.
	NCPlusScoreboardHost::DrawHostMarker(this, UTHUDOwner, PlayerState, UTGameState,
		NameX + NameXL * NameScaling + 8.f * RenderScale, NameY, RenderScale);

	// ---- HP / Armor bars (for alive teammates, like Showdown) ----
	AUTCharacter* UTC = PlayerState->GetUTCharacter();
	if (UTC != nullptr)
	{
		bool bShowBars = true;
		// Only show bars for teammates or if spectating
		if (UTHUDOwner && UTHUDOwner->UTPlayerOwner && UTHUDOwner->UTPlayerOwner->UTPlayerState)
		{
			AUTPlayerState* LocalPS = UTHUDOwner->UTPlayerOwner->UTPlayerState;
			bShowBars = UTGameState->OnSameTeam(PlayerState, LocalPS) || LocalPS->bOnlySpectator;
		}

		if (bShowBars)
		{
			float HealthPct = FMath::Clamp(float(UTC->Health) / float(UTC->SuperHealthMax), 0.f, 1.f);
			float ArmorPct = float(UTC->GetArmorAmount()) / float(FMath::Max(UTC->MaxStackedArmor, 1));
			float BarHeight = 5.f * RenderScale;
			float BarY = NameY + 14.f * RenderScale;
			float BarX = NameX;
			float HealthBarWidth = 120.f * RenderScale;
			float ArmorBarWidth = 80.f * RenderScale;

			// Health bar background
			FLinearColor BarBG(0.15f, 0.15f, 0.15f, 0.6f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX, BarY, HealthBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			// Health bar fill
			FLinearColor HealthColor(0.25f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, BarX + 1.f, BarY + 1.f, (HealthBarWidth - 2.f) * HealthPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, HealthColor);

			// Armor bar background
			float ArmorBarX = BarX + HealthBarWidth + 6.f * RenderScale;
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX, BarY, ArmorBarWidth, BarHeight, 185.f, 400.f, 4.f, 4.f, 1.f, BarBG);
			// Armor bar fill
			FLinearColor ArmorColor(0.8f, 0.8f, 0.25f, 0.7f);
			DrawTexture(UTHUDOwner->HUDAtlas, ArmorBarX + 1.f, BarY + 1.f, (ArmorBarWidth - 2.f) * ArmorPct, BarHeight - 2.f, 185.f, 400.f, 4.f, 4.f, 1.f, ArmorColor);
		}
	}
	else if (bIsDead && PlayerState->RespawnTime > 0.f)
	{
		// Show respawn timer text next to name
		int32 Seconds = FMath::CeilToInt(PlayerState->RespawnTime);
		FLinearColor TimerColor = (PlayerState->GetTeamNum() == 0)
			? FLinearColor(1.f, 0.4f, 0.4f, 1.f)
			: FLinearColor(0.4f, 0.6f, 1.f, 1.f);
		DrawText(FText::FromString(FString::Printf(TEXT("%is"), Seconds)),
			NameX, NameY + 14.f * RenderScale, UTHUDOwner->TinyFont, 0.75f * RenderScale, 1.0f,
			TimerColor, ETextHorzPos::Left, ETextVertPos::Center);
	}

	// ---- Stats columns ----
	if (UTGameState && UTGameState->HasMatchStarted())
	{
		DrawPlayerScore(PlayerState, XOffset, YOffset, ScaledCellWidth, DrawColor);
	}
	else
	{
		DrawReadyText(PlayerState, XOffset, YOffset, ScaledCellWidth);
	}

	// ---- Ping / Bot skill ----
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
		int32 Ping = bIsOwner ? PlayerState->ExactPing : (PlayerState->Ping * 4);
		FLinearColor PingColor = (Ping < 60) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
			: (Ping < 120) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
			: FLinearColor(1.f, 0.25f, 0.25f, 1.f);
		DrawText(FText::FromString(FString::Printf(TEXT("%dms"), Ping)),
			XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, PingColor, ETextHorzPos::Center, ETextVertPos::Center);

		// Packet loss — local player only
		if (bIsOwner && UTPlayerOwner && UTPlayerOwner->Player)
		{
			UNetConnection* Conn = Cast<UNetDriver>(GetWorld()->GetNetDriver()) ? Cast<UNetConnection>(UTPlayerOwner->Player) : nullptr;
			if (!Conn)
			{
				// Try via NetDriver
				UNetDriver* Driver = GetWorld()->GetNetDriver();
				if (Driver && Driver->ServerConnection)
				{
					Conn = Driver->ServerConnection;
				}
			}
			if (Conn && Conn->InPackets > 0)
			{
				float LossPct = (float(Conn->InPacketsLost) / float(Conn->InPackets + Conn->InPacketsLost)) * 100.f;
				if (LossPct > 0.f)
				{
					FLinearColor PLColor = (LossPct < 2.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f) : FLinearColor(1.f, 0.25f, 0.25f, 1.f);
					DrawText(FText::FromString(FString::Printf(TEXT("PL:%.1f%%"), LossPct)),
						XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY + 14.f * RenderScale,
						UTHUDOwner->TinyFont, 0.75f * RenderScale, 1.f, PLColor, ETextHorzPos::Center, ETextVertPos::Center);
				}
			}
		}
	}
}

void UWipeoutScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	// K/D — merged kills and deaths
	int32 DisplayKills = bUseRoundKills ? (PlayerState->RoundKills + PlayerState->RoundKillAssists) : (PlayerState->Kills + PlayerState->KillAssists);
	FString KDStr = FString::Printf(TEXT("%d/%d"), DisplayKills, PlayerState->Deaths);
	DrawText(FText::FromString(KDStr), XOffset + (Width * ColumnHeaderKDX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// B/A — belt and amp pickup counts (from replicated damage replicator)
	int32 Belts = 0;
	int32 Amps = 0;
	{
		AWipeoutDamageReplicator* DmgRep = nullptr;
		UWorld* BAWorld = GetWorld();
		if (BAWorld)
		{
			for (TActorIterator<AWipeoutDamageReplicator> It(BAWorld); It; ++It)
			{
				DmgRep = *It;
				break;
			}
		}
		if (DmgRep && PlayerState->UniqueId.IsValid())
		{
			FString PId = PlayerState->UniqueId.ToString();
			Belts = DmgRep->GetBeltsForPlayer(PId);
			Amps = DmgRep->GetAmpsForPlayer(PId);
		}
	}
	FString BAStr = FString::Printf(TEXT("%d/%d"), Belts, Amps);
	FLinearColor BAColor = FLinearColor(0.9f, 0.8f, 0.2f, 1.f); // gold
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) BAColor *= 0.6f;
	DrawText(FText::FromString(BAStr), XOffset + (Width * ColumnHeaderBeltAmpX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, BAColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage — read from the replicated damage replicator if available,
	// otherwise fall back to PlayerState (works on listen server / standalone)
	int32 Damage = 0;
	{
		AWipeoutDamageReplicator* DmgRep = nullptr;
		UWorld* World = GetWorld();
		if (World)
		{
			for (TActorIterator<AWipeoutDamageReplicator> It(World); It; ++It)
			{
				DmgRep = *It;
				break;
			}
		}
		if (DmgRep && PlayerState->UniqueId.IsValid())
		{
			Damage = DmgRep->GetDamageForPlayer(PlayerState->UniqueId.ToString());
		}
		else
		{
			// Fallback: direct read (works on server/listen server)
			Damage = int32(PlayerState->DamageDone);
		}
	}
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Vest / Siphon pickups and healing — all three live on the same replicator as
	// Damage (vest + siphon are PlayerState stats the client never sees; healing is
	// GameMode state). Reuse the DmgRep resolved above rather than iterating actors
	// again per column. No listen-server fallback: unlike DamageDone these have no
	// client-readable equivalent, so they simply read 0 if the replicator is absent.
	int32 Vests = 0;
	int32 Siphons = 0;
	int32 Healing = 0;
	{
		AWipeoutDamageReplicator* StatRep = nullptr;
		UWorld* World = GetWorld();
		if (World)
		{
			for (TActorIterator<AWipeoutDamageReplicator> It(World); It; ++It)
			{
				StatRep = *It;
				break;
			}
		}
		if (StatRep && PlayerState->UniqueId.IsValid())
		{
			const FString PlayerIdStr = PlayerState->UniqueId.ToString();
			Vests = StatRep->GetVestsForPlayer(PlayerIdStr);
			Siphons = StatRep->GetSiphonsForPlayer(PlayerIdStr);
			Healing = StatRep->GetHealingForPlayer(PlayerIdStr);
		}
	}

	FString VSStr = FString::Printf(TEXT("%d/%d"), Vests, Siphons);
	FLinearColor VSColor = FLinearColor(0.6f, 0.85f, 1.f, 1.f); // pale blue — pickup family
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) VSColor *= 0.6f;
	DrawText(FText::FromString(VSStr), XOffset + (Width * ColumnHeaderVestSiphonX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, VSColor, ETextHorzPos::Center, ETextVertPos::Center);

	FLinearColor HealColor = (Healing > 0)
		? FLinearColor(0.3f, 1.f, 0.5f, 1.f)          // green — support contribution
		: FLinearColor(0.55f, 0.55f, 0.55f, 1.f);     // grey zero, so it never competes with DMG
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) HealColor *= 0.6f;
	DrawText(FText::AsNumber(Healing), XOffset + (Width * ColumnHeaderHealX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, HealColor, ETextHorzPos::Center, ETextVertPos::Center);

	// DMG/Life — average damage dealt per life
	int32 Lives = PlayerState->Deaths + 1;
	float DmgPerLife = float(Damage) / float(Lives);
	FLinearColor DplColor = (DmgPerLife >= 300.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (DmgPerLife >= 150.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) DplColor *= 0.6f;
	FString DplStr = FString::Printf(TEXT("%d"), FMath::RoundToInt(DmgPerLife));
	DrawText(FText::FromString(DplStr), XOffset + (Width * ColumnHeaderDmgPerLifeX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DplColor, ETextHorzPos::Center, ETextVertPos::Center);
}

void UWipeoutScoreboard::DrawPlayerScores(float RenderDelta, float& YOffset)
{
	if (!UTGameState) return;

	int32 XOffset = ScaledEdgeSize;
	float MaxYOffset = 0.f;
	TArray<FString> SpectatorNames;

	for (int8 Team = 0; Team < 2; Team++)
	{
		int32 Place = 1;
		float DrawOffset = YOffset;
		int32 NumPlayersToShow = ShouldDrawScoringStats() ? 5 : UTGameState->PlayerArray.Num();
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
				else if (Team == 0 && !PlayerState->bIsDemoRecording)
				{
					SpectatorNames.Add(PlayerState->PlayerName);
				}
			}
		}
		MaxYOffset = FMath::Max(DrawOffset, MaxYOffset);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset = MaxYOffset;

	// GoalScore centered below player rows
	if (UTGameState->GoalScore > 0 && !ShouldDrawScoringStats())
	{
		FString GoalStr = FString::Printf(TEXT("First to %d"), UTGameState->GoalScore);
		DrawText(FText::FromString(GoalStr), Canvas->ClipX * 0.5f, YOffset + 4.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Top);
	}

	// Named spectator list instead of generic count
	if (SpectatorNames.Num() > 0 && !ShouldDrawScoringStats())
	{
		FString SpecStr = TEXT("Spectators: ") + FString::Join(SpectatorNames, TEXT(", "));
		DrawText(FText::FromString(SpecStr), Size.X * 0.5f, 765.f * RenderScale,
			UTHUDOwner->SmallFont, 1.0f, 1.0f, FLinearColor(0.75f, 0.75f, 0.75f, 1.f),
			ETextHorzPos::Center, ETextVertPos::Bottom);
	}
}
