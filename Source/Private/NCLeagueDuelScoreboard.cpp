// NCLeagueDuelScoreboard.cpp — accuracy column replaces BeltAmp.

#include "NCLeagueDuelScoreboard.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "StatNames.h"

UNCLeagueDuelScoreboard::UNCLeagueDuelScoreboard(const FObjectInitializer& OI)
	: Super(OI)
{
	CH_Accuracy = NSLOCTEXT("NCLeagueDuelScoreboard", "ColumnHeader_Accuracy", "Acc");
}

void UNCLeagueDuelScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
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
			// Replace "B/A" with "Acc" — duel cares about shooting precision, not pickups.
			DrawText(CH_Accuracy, XOffset + (ScaledCellWidth * ColumnHeaderBeltAmpX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Damage, XOffset + (ScaledCellWidth * ColumnHeaderDamageX), YOffset + ColumnHeaderY,
				UTHUDOwner->TinyFont, 1.0f, 1.0f, FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
			DrawText(CH_Efficiency, XOffset + (ScaledCellWidth * ColumnHeaderEfficiencyX), YOffset + ColumnHeaderY,
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

void UNCLeagueDuelScoreboard::DrawPlayerScore(AUTPlayerState* PS, float XOffset,
	float YOffset, float Width, FLinearColor DrawColor)
{
	if (!PS) return;

	// K/D (same as Wipeout)
	const int32 DisplayKills = PS->Kills + PS->KillAssists;
	const FString KDStr = FString::Printf(TEXT("%d/%d"), DisplayKills, PS->Deaths);
	DrawText(FText::FromString(KDStr), XOffset + (Width * ColumnHeaderKDX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Combined hitscan accuracy: (LG + Shock + Sniper) hits / shots.
	const int32 Hits  = PS->GetStatsValue(NAME_LinkHits)
	                  + PS->GetStatsValue(NAME_ShockRifleHits)
	                  + PS->GetStatsValue(NAME_SniperHits);
	const int32 Shots = PS->GetStatsValue(NAME_LinkShots)
	                  + PS->GetStatsValue(NAME_ShockRifleShots)
	                  + PS->GetStatsValue(NAME_SniperShots);
	const float Pct = (Shots > 0) ? float(Hits) / float(Shots) * 100.f : 0.f;
	const FLinearColor AccColor = (Pct >= 35.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
	                            : (Pct >= 20.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
	                            : FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	const FString AccStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Pct));
	DrawText(FText::FromString(AccStr), XOffset + (Width * ColumnHeaderBeltAmpX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, AccColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage
	const int32 Damage = int32(PS->DamageDone);
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PS->GetUTCharacter()) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Efficiency
	const int32 EffKills = PS->Kills;
	const int32 EffDeaths = PS->Deaths;
	const float EffPct = (EffKills + EffDeaths > 0)
		? (float(EffKills) / float(EffKills + EffDeaths)) * 100.f : 0.f;
	const FLinearColor EffColor = (EffPct >= 60.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (EffPct >= 40.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	const FString EffStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(EffPct));
	DrawText(FText::FromString(EffStr), XOffset + (Width * ColumnHeaderEfficiencyX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, EffColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage / life
	const int32 Lives = PS->Deaths + 1;
	const float DmgPerLife = float(Damage) / float(Lives);
	const FLinearColor DplColor = (DmgPerLife >= 300.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (DmgPerLife >= 150.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	const FString DplStr = FString::Printf(TEXT("%d"), FMath::RoundToInt(DmgPerLife));
	DrawText(FText::FromString(DplStr), XOffset + (Width * ColumnHeaderDmgPerLifeX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DplColor, ETextHorzPos::Center, ETextVertPos::Center);
}
