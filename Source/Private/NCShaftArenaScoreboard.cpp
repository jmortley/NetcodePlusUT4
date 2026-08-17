// NCShaftArenaScoreboard.cpp - 6-column layout: Player | K/D | Acc | Streak | DMG | Ping.
// Drops Eff% and DMG/Life vs the inherited parent layout - shaft arena cares
// about kills, accuracy, current spree, damage, and (since 2026-08-17, Jeremy)
// ping; the rest is noise in a 1v1 beam duel.

#include "NCShaftArenaScoreboard.h"
#include "NCPlusScoreboardHost.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTBot.h"
#include "UTHUD.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"
#include "StatNames.h"
#include "NCShaftArenaStatsReplicator.h"

namespace
{
	/** Ping column centre. Sits in the right margin the 5-column layout left
	 *  empty; shared by the header and row draws. */
	constexpr float ShaftColumnPingX = 0.94f;

	/** Cached weak-ptr lookup for the replicator. Same pattern the duel
	 *  scoreboard uses - re-iterating actors every frame is wasted work. */
	ANCShaftArenaStatsReplicator* FindNCShaftArenaStatsReplicator(UWorld* World)
	{
		if (!World) return nullptr;
		static TWeakObjectPtr<UWorld> CachedWorld;
		static TWeakObjectPtr<ANCShaftArenaStatsReplicator> CachedRep;
		if (CachedWorld.Get() == World && CachedRep.IsValid())
		{
			return CachedRep.Get();
		}
		for (TActorIterator<ANCShaftArenaStatsReplicator> It(World); It; ++It)
		{
			CachedWorld = World;
			CachedRep   = *It;
			return *It;
		}
		return nullptr;
	}
}

UNCShaftArenaScoreboard::UNCShaftArenaScoreboard(const FObjectInitializer& OI)
	: Super(OI)
{
	CH_Accuracy = NSLOCTEXT("NCShaftArenaScoreboard", "ColumnHeader_Accuracy", "Acc");
	CH_Streak   = NSLOCTEXT("NCShaftArenaScoreboard", "ColumnHeader_Streak",   "Streak");

	// 5-column layout: Player(left) | K/D | Acc | Streak | DMG
	// Spread across the row leaving the right ~6% empty (which the
	// DrawPlayer override scrubs of inherited skill/ping).
	ColumnHeaderKDX         = 0.42f;
	ColumnHeaderBeltAmpX    = 0.54f;   // Acc
	ColumnHeaderDamageX     = 0.66f;   // Streak (re-purposed: name kept for parent compat)
	ColumnHeaderEfficiencyX = 0.84f;   // DMG total (re-purposed)
}

void UNCShaftArenaScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	float XOffset = ScaledEdgeSize;
	const float Height = 23.f * RenderScale;

	for (int32 i = 0; i < 2; i++)
	{
		DrawTexture(UTHUDOwner->ScoreboardAtlas, XOffset, YOffset, ScaledCellWidth, Height,
			149, 138, 32, 32, 1.0, FLinearColor(0.72f, 0.72f, 0.72f, 0.85f));

		DrawText(CH_PlayerName, XOffset + (ScaledCellWidth * ColumnHeaderPlayerX), YOffset + ColumnHeaderY,
			UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Left, ETextVertPos::Center);

		if (UTGameState && UTGameState->HasMatchStarted())
		{
			DrawText(CH_KD, XOffset + (ScaledCellWidth * ColumnHeaderKDX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Accuracy, XOffset + (ScaledCellWidth * ColumnHeaderBeltAmpX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Streak, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderEfficiencyX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText((GetWorld()->GetNetMode() == NM_Standalone) ? CH_Skill : CH_Ping,
				XOffset + (ScaledCellWidth * ShaftColumnPingX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			// Eff% and DMG/Life intentionally removed; Ping restored 2026-08-17.
		}

		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
	YOffset += Height + 4.f;
}

void UNCShaftArenaScoreboard::DrawPlayerScore(AUTPlayerState* PS, float XOffset,
	float YOffset, float Width, FLinearColor DrawColor)
{
	if (!PS) return;

	// Kills / Deaths
	const FString KDStr = FString::Printf(TEXT("%d/%d"), PS->Kills, PS->Deaths);
	DrawText(FText::FromString(KDStr), XOffset + (Width * ColumnHeaderKDX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Build replicator key once - both accuracy and damage come from it.
	const FString PlayerId = PS->UniqueId.IsValid()
		? PS->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *PS->PlayerName);
	ANCShaftArenaStatsReplicator* Rep = FindNCShaftArenaStatsReplicator(GetWorld());
	const bool bIsAuthority = GetWorld() && GetWorld()->GetNetMode() != NM_Client;

	// Link gun accuracy. Replicator on dedicated clients; PS->GetStatsValue
	// authority fallback for listen-server / standalone where the replicator
	// might not have ticked yet. NAME_LinkBeamShots is the per-refire-tick
	// counter UTWeap_LinkGun_Plus increments on every beam-mode ConsumeAmmo
	// call — Quake-style accuracy. NAME_LinkHits ticks per damage chunk.
	float Pct = Rep ? Rep->GetAccuracyForPlayer(PlayerId) : 0.f;
	if (Pct == 0.f && bIsAuthority)
	{
		static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));
		const int32 Hits  = PS->GetStatsValue(NAME_LinkHits);
		const int32 Shots = PS->GetStatsValue(NAME_LinkBeamShots);
		Pct = (Shots > 0) ? FMath::Min(float(Hits) / float(Shots) * 100.f, 100.f) : 0.f;
	}
	const FLinearColor AccColor = (Pct >= 50.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
	                            : (Pct >= 30.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
	                            : FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	const FString AccStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct));
	DrawText(FText::FromString(AccStr), XOffset + (Width * ColumnHeaderBeltAmpX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, AccColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Current spree (engine PS->Spree IS replicated - direct read is fine).
	const FString StreakStr = FString::Printf(TEXT("%d"), PS->Spree);
	DrawText(FText::FromString(StreakStr), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor(1.f, 0.85f, 0.4f, 1.f),
		ETextHorzPos::Center, ETextVertPos::Center);

	// Damage total. AUTPlayerState::DamageDone is server-only, so replicator
	// path on dedicated clients; authority fallback for standalone.
	int32 Damage = Rep ? Rep->GetDamageForPlayer(PlayerId) : 0;
	if (Damage == 0 && bIsAuthority)
	{
		Damage = int32(PS->DamageDone);
	}
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PS->GetUTCharacter()) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderEfficiencyX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Ping (restored 2026-08-17). Stock convention: quantized PS->Ping*4 for
	// remote rows, ExactPing for the local player's own row. Bots have no ping;
	// skip the cell rather than draw a lying 0ms.
	if (!PS->bIsABot)
	{
		int32 PingMs = PS->Ping * 4;
		if (UTHUDOwner && UTHUDOwner->UTPlayerOwner
			&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PS)
		{
			PingMs = int32(PS->ExactPing);
		}
		DrawText(FText::Format(PingFormatText, FText::AsNumber(PingMs)),
			XOffset + (Width * ShaftColumnPingX), YOffset + ColumnY,
			UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);
	}

	// Eff%, DMG/Life intentionally removed.
}

void UNCShaftArenaScoreboard::DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
	float RenderDelta, float XOffset, float YOffset)
{
	// Direct copy of UUTScoreboard::DrawPlayer with the bot-skill / ping
	// right-edge draw block omitted. Same approach as NCLeagueDuelScoreboard.
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

	// Null-guard owner check. Match elim/wipeout's defensive pattern — duel's
	// matching deref crashes pre-match scoreboard render in standalone PIE
	// before UTPlayerOwner is fully wired up. Same hazard here.
	const bool bIsOwner = (UTHUDOwner && UTHUDOwner->UTPlayerOwner
		&& UTHUDOwner->UTPlayerOwner->UTPlayerState == PlayerState);
	if (bIsOwner)
	{
		BarOpacity = 0.5f;
	}

	FLinearColor BarColor = GetPlayerBackgroundColorFor(PlayerState);
	float FinalBarOpacity = BarOpacity;
	if (bIsUnderCursor)
	{
		BarColor = FLinearColor(0.0, 0.3, 0.0, 1.0);
		FinalBarOpacity = 0.75f;
	}
	if (PlayerState == SelectedPlayer)
	{
		BarColor = FLinearColor(0.0, 0.3, 0.3, 1.0);
		FinalBarOpacity = 0.75f;
	}

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

	// Match-host badge — tags the player who pressed Enter to start the match,
	// past the name (and the friend icon when present). Shared helper.
	NCPlusScoreboardHost::DrawHostMarker(this, UTHUDOwner, PlayerState, UTGameState,
		XOffset + (ScaledCellWidth * ColumnHeaderPlayerX) + NameSize.X * NameScaling
			+ (PlayerState->bIsFriend ? 40.f : 8.f) * RenderScale,
		YOffset + ColumnY, RenderScale);
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

	// >>> Parent's right-edge Skill/Ping draw still OMITTED — our own Ping
	// column (ShaftColumnPingX, drawn in DrawPlayerScore) replaces it. <<<

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

	// Mute indicator removed — elim/wipeout don't have it either, and the
	// IsPlayerGameMuted call required an unguarded UTHUDOwner->UTPlayerOwner
	// deref that risked a crash during pre-match scoreboard render.
	if (PlayerState->bIsTalking)
	{
		bool bLeft = (XOffset < Canvas->ClipX * 0.5f);
		float TalkingXOffset = bLeft ? ScaledCellWidth + (10.0f *RenderScale) : (-36.0f * RenderScale);
		FTextureUVs ChatIconUVs = bLeft
			? FTextureUVs(497.0f, 965.0f, 35.0f, 31.0f)
			: FTextureUVs(532.0f, 965.0f, -35.0f, 31.0f);
		DrawTexture(UTHUDOwner->HUDAtlas, XOffset + TalkingXOffset,
			YOffset + ((CellHeight * 0.5f - 24.0f) * RenderScale),
			(26 * RenderScale), (23 * RenderScale),
			ChatIconUVs.U, ChatIconUVs.V, ChatIconUVs.UL, ChatIconUVs.VL, 1.0f);
	}
}
