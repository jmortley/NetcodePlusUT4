#include "ClutchScoreboard.h"
#include "ClutchRoundState.h"
#include "UTPlayerState.h"
#include "EngineUtils.h"


UClutchScoreboard::UClutchScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CH_PlayerName = NSLOCTEXT("ClutchScoreboard", "Player", "PLAYER");
	CH_Score = NSLOCTEXT("ClutchScoreboard", "Role", "ROLE");
	CH_Deaths = NSLOCTEXT("ClutchScoreboard", "Armor", "ARMOR");
	CH_Ping = NSLOCTEXT("ClutchScoreboard", "Ping", "PING");
	ColumnHeaderPlayerX = 0.055f;
	ColumnHeaderScoreX = 0.60f;
	ColumnHeaderDeathsX = 0.78f;
	ColumnHeaderPingX = 0.965f;
}


void UClutchScoreboard::DrawTeamPanel(float RenderDelta, float& YOffset)
{
	AClutchRoundState* State = ResolveClutchState();
	if (State && State->AttackingTeamIndex <= 1 && State->IsGameplayPhase())
	{
		RedTeamText = State->AttackingTeamIndex == 0
			? NSLOCTEXT("ClutchScoreboard", "RedAttack", "RED - ATTACK")
			: NSLOCTEXT("ClutchScoreboard", "RedDefend", "RED - DEFEND");
		BlueTeamText = State->AttackingTeamIndex == 1
			? NSLOCTEXT("ClutchScoreboard", "BlueAttack", "BLUE - ATTACK")
			: NSLOCTEXT("ClutchScoreboard", "BlueDefend", "BLUE - DEFEND");
	}
	else
	{
		RedTeamText = NSLOCTEXT("ClutchScoreboard", "Red", "RED");
		BlueTeamText = NSLOCTEXT("ClutchScoreboard", "Blue", "BLUE");
	}
	Super::DrawTeamPanel(RenderDelta, YOffset);
}


void UClutchScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState,
	float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	AClutchRoundState* State = ResolveClutchState();
	const FClutchRosterEntry* Entry = State && PlayerState
		? State->FindEntry(PlayerState)
		: nullptr;

	FString RoleText(TEXT("WAIT"));
	FString ArmorText(TEXT("-"));
	if (Entry)
	{
		switch (Entry->PlayerStatus)
		{
		case EClutchStatus::Active:
			RoleText = Entry->PlayerRole == EClutchRole::Attacker
				? TEXT("ATTACK")
				: (Entry->PlayerRole == EClutchRole::Defender ? TEXT("DEFEND") : TEXT("ACTIVE"));
			break;
		case EClutchStatus::Benched: RoleText = TEXT("BENCH"); break;
		case EClutchStatus::Eliminated: RoleText = TEXT("OUT"); break;
		case EClutchStatus::Disconnected: RoleText = TEXT("OFFLINE"); break;
		default: RoleText = TEXT("NEXT"); break;
		}

		if (Entry->PlayerRole == EClutchRole::Attacker && State)
		{
			ArmorText = FString::Printf(TEXT("%d/%d"),
				State->GetEntryArmorRemaining(*Entry),
				static_cast<int32>(State->MaxAttackerHits));
		}
	}

	DrawText(FText::FromString(RoleText),
		XOffset + Width * ColumnHeaderScoreX, YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);
	DrawText(FText::FromString(ArmorText),
		XOffset + Width * ColumnHeaderDeathsX, YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);
}


FLinearColor UClutchScoreboard::GetPlayerColorFor(AUTPlayerState* PlayerState) const
{
	AClutchRoundState* State = ResolveClutchState();
	const FClutchRosterEntry* Entry = State && PlayerState
		? State->FindEntry(PlayerState)
		: nullptr;
	if (!Entry)
	{
		return Super::GetPlayerColorFor(PlayerState);
	}
	if (Entry->PlayerStatus == EClutchStatus::Active
		&& Entry->PlayerRole == EClutchRole::Attacker)
	{
		return FLinearColor(1.0f, 0.68f, 0.18f, 1.0f);
	}
	if (Entry->PlayerStatus == EClutchStatus::Benched)
	{
		return FLinearColor(0.62f, 0.62f, 0.62f, 1.0f);
	}
	if (Entry->PlayerStatus == EClutchStatus::Eliminated)
	{
		return FLinearColor(0.42f, 0.42f, 0.42f, 1.0f);
	}
	return FLinearColor::White;
}


AClutchRoundState* UClutchScoreboard::ResolveClutchState() const
{
	for (TActorIterator<AClutchRoundState> It(GetWorld()); It; ++It)
	{
		return *It;
	}
	return nullptr;
}
