// WipeoutScoreboard — Portrait-row scoreboard for Wipeout game mode
#include "WipeoutScoreboard.h"
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

UWipeoutScoreboard::UWipeoutScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Taller cells to accommodate portrait pip + HP/armor bars
	CellHeight = 80.f;
	CellWidth = 775.f;

	// Column positions (fraction of CellWidth)
	ColumnHeaderPlayerX = 0.10f;    // Name starts after portrait
	ColumnHeaderScoreX = 0.40f;     // Score/round wins (unused in Wipeout but keep for base class)
	ColumnHeaderKillsX = 0.42f;
	ColumnHeaderDeathsX = 0.54f;
	ColumnHeaderDamageX = 0.65f;
	ColumnHeaderEfficiencyX = 0.77f;
	ColumnHeaderPingX = 0.90f;

	CH_Kills = NSLOCTEXT("UTScoreboard", "ColumnHeader_Kills", "Kills");
	CH_Deaths = NSLOCTEXT("UTScoreboard", "ColumnHeader_Deaths", "Deaths");
	CH_Damage = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_Damage", "DMG");
	CH_Efficiency = NSLOCTEXT("WipeoutScoreboard", "ColumnHeader_DmgPerLife", "DMG/Life");

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
			DrawText(CH_Kills, XOffset + (ScaledCellWidth * ColumnHeaderKillsX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Deaths, XOffset + (ScaledCellWidth * ColumnHeaderDeathsX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Efficiency, XOffset + (ScaledCellWidth * ColumnHeaderEfficiencyX), YOffset + ColumnHeaderY,
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
	const FCanvasIcon& CharIcon = PlayerState->GetHUDIcon();
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

	float BarOpacity = 0.3f;
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
	if (bIsOwner) BarOpacity = 0.5f;

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
	int32 Ping = PlayerState->Ping * 4;
	if (bIsOwner) Ping = PlayerState->ExactPing;

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
		FLinearColor PingColor = (Ping < 60) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
			: (Ping < 120) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
			: FLinearColor(1.f, 0.25f, 0.25f, 1.f);

		// For the local player, also show packet loss from the net connection
		FString PingStr;
		if (bIsOwner)
		{
			float PacketLossPct = 0.f;
			UNetDriver* NetDriver = GetWorld()->GetNetDriver();
			if (NetDriver && NetDriver->ServerConnection)
			{
				int32 InLost = NetDriver->ServerConnection->InPacketsLost;
				int32 InTotal = NetDriver->ServerConnection->InPackets;
				if (InTotal > 0)
				{
					PacketLossPct = (float(InLost) / float(InTotal)) * 100.f;
				}
			}
			if (PacketLossPct > 0.1f)
			{
				PingStr = FString::Printf(TEXT("%d (%.1f%%)"), Ping, PacketLossPct);
				// Tint towards red if significant packet loss
				if (PacketLossPct > 1.f) PingColor = FLinearColor(1.f, 0.25f, 0.25f, 1.f);
			}
			else
			{
				PingStr = FString::Printf(TEXT("%dms"), Ping);
			}
		}
		else
		{
			PingStr = FString::Printf(TEXT("%dms"), Ping);
		}

		DrawText(FText::FromString(PingStr), XOffset + ScaledCellWidth * ColumnHeaderPingX, YOffset + ColumnY,
			UTHUDOwner->SmallFont, RenderScale, 1.f, PingColor, ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Mute indicator removed — too cluttered for compact portrait layout
}

void UWipeoutScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	// Kills
	int32 DisplayKills = bUseRoundKills ? (PlayerState->RoundKills + PlayerState->RoundKillAssists) : (PlayerState->Kills + PlayerState->KillAssists);
	int32 FinalBlows = bUseRoundKills ? PlayerState->RoundKills : PlayerState->Kills;
	DrawText(FText::Format(NSLOCTEXT("Wipeout", "KillsFmt", "{0} ({1})"),
		FText::AsNumber(DisplayKills), FText::AsNumber(FinalBlows)),
		XOffset + (Width * ColumnHeaderKillsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Deaths
	DrawText(FText::AsNumber(PlayerState->Deaths), XOffset + (Width * ColumnHeaderDeathsX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage
	int32 Damage = int32(PlayerState->DamageDone);
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// DMG/Life — average damage dealt per life (total damage / (deaths + 1))
	int32 Lives = PlayerState->Deaths + 1;
	float DmgPerLife = float(Damage) / float(Lives);
	FLinearColor DplColor = (DmgPerLife >= 300.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (DmgPerLife >= 150.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	if (!PlayerState->GetUTCharacter() && !PlayerState->bOutOfLives) DplColor *= 0.6f;
	FString DplStr = FString::Printf(TEXT("%d"), FMath::RoundToInt(DmgPerLife));
	DrawText(FText::FromString(DplStr), XOffset + (Width * ColumnHeaderEfficiencyX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DplColor, ETextHorzPos::Center, ETextVertPos::Center);
}
