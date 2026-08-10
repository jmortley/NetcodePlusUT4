#include "ShockDomScoreboard.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTTeamInfo.h"
#include "ShockDomReplicator.h"
#include "EngineUtils.h"


UShockDomScoreboard::UShockDomScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	CH_Captures = NSLOCTEXT("ShockDomScoreboard", "ColumnHeader_Captures", "Cap");
}


AShockDomReplicator* UShockDomScoreboard::FindDomReplicator()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	if (CachedDomReplicatorWorld.Get() != World)
	{
		CachedDomReplicatorWorld = World;
		CachedDomReplicator.Reset();
		NextDomReplicatorSearchTime = 0.f;
	}
	if (CachedDomReplicator.IsValid() && CachedDomReplicator->GetWorld() == World)
	{
		return CachedDomReplicator.Get();
	}
	CachedDomReplicator.Reset();

	const float Now = World->GetTimeSeconds();
	if (Now < NextDomReplicatorSearchTime)
	{
		return nullptr;
	}
	NextDomReplicatorSearchTime = Now + 1.f;
	for (TActorIterator<AShockDomReplicator> It(World); It; ++It)
	{
		CachedDomReplicator = *It;
		NextDomReplicatorSearchTime = 0.f;
		return *It;
	}
	return nullptr;
}


void UShockDomScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
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
			// "Cap" instead of "B/A"
			DrawText(CH_Captures, XOffset + (ScaledCellWidth * ColumnHeaderBeltAmpX), YOffset + ColumnHeaderY,
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


void UShockDomScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset,
	float YOffset, float Width, FLinearColor DrawColor)
{
	// K/D
	int32 DisplayKills = PlayerState->Kills + PlayerState->KillAssists;
	FString KDStr = FString::Printf(TEXT("%d/%d"), DisplayKills, PlayerState->Deaths);
	DrawText(FText::FromString(KDStr), XOffset + (Width * ColumnHeaderKDX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DrawColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Find DOM replicator (reused for captures + damage below)
	AShockDomReplicator* DomRep = FindDomReplicator();
	const FString PlayerId = PlayerState->UniqueId.IsValid()
		? PlayerState->UniqueId.ToString()
		: FString();

	// Captures
	int32 Captures = 0;
	if (DomRep && !PlayerId.IsEmpty())
	{
		Captures = DomRep->GetCapturesForPlayer(PlayerId);
	}
	FLinearColor CapColor = FLinearColor(0.2f, 1.f, 0.4f, 1.f); // Green
	if (!PlayerState->GetUTCharacter()) CapColor *= 0.6f;
	DrawText(FText::AsNumber(Captures), XOffset + (Width * ColumnHeaderBeltAmpX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, CapColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Damage — from DOM replicator (reuse the one we already found)
	int32 Damage = 0;
	if (DomRep && !PlayerId.IsEmpty())
	{
		Damage = DomRep->GetDamageForPlayer(PlayerId);
	}
	else
	{
		Damage = int32(PlayerState->DamageDone);
	}
	FLinearColor DmgColor = FLinearColor(1.f, 0.8f, 0.25f, 1.f);
	if (!PlayerState->GetUTCharacter()) DmgColor *= 0.6f;
	DrawText(FText::AsNumber(Damage), XOffset + (Width * ColumnHeaderDamageX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DmgColor, ETextHorzPos::Center, ETextVertPos::Center);

	// Efficiency
	int32 EffKills = PlayerState->Kills;
	int32 EffDeaths = PlayerState->Deaths;
	float EffPct = (EffKills + EffDeaths > 0) ? (float(EffKills) / float(EffKills + EffDeaths)) * 100.f : 0.f;
	FLinearColor EffColor = (EffPct >= 60.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (EffPct >= 40.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	if (!PlayerState->GetUTCharacter()) EffColor *= 0.6f;
	FString EffStr = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(EffPct));
	DrawText(FText::FromString(EffStr), XOffset + (Width * ColumnHeaderEfficiencyX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, EffColor, ETextHorzPos::Center, ETextVertPos::Center);

	// DMG/Life
	int32 Lives = PlayerState->Deaths + 1;
	float DmgPerLife = float(Damage) / float(Lives);
	FLinearColor DplColor = (DmgPerLife >= 300.f) ? FLinearColor(0.25f, 1.f, 0.25f, 1.f)
		: (DmgPerLife >= 150.f) ? FLinearColor(1.f, 1.f, 0.25f, 1.f)
		: FLinearColor(1.f, 0.4f, 0.4f, 1.f);
	if (!PlayerState->GetUTCharacter()) DplColor *= 0.6f;
	FString DplStr = FString::Printf(TEXT("%d"), FMath::RoundToInt(DmgPerLife));
	DrawText(FText::FromString(DplStr), XOffset + (Width * ColumnHeaderDmgPerLifeX), YOffset + ColumnY,
		UTHUDOwner->TinyFont, 1.0f, 1.0f, DplColor, ETextHorzPos::Center, ETextVertPos::Center);
}
