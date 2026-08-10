#include "ClutchScoreboard.h"
#include "ClutchRoundState.h"
#include "NCPlusScoreboardReady.h"
#include "UTPlayerState.h"
#include "EngineUtils.h"


namespace
{
	const float ClutchAttackerWinsColumnX = 0.84f;
}


UClutchScoreboard::UClutchScoreboard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bDrawMinimapInScoreboard = false;
	CH_PlayerName = NSLOCTEXT("ClutchScoreboard", "Player", "PLAYER");
	CH_Score = NSLOCTEXT("ClutchScoreboard", "Role", "ROLE");
	CH_Deaths = NSLOCTEXT("ClutchScoreboard", "DefenderHits", "D-HITS");
	CH_Ping = NSLOCTEXT("ClutchScoreboard", "Ping", "PING");
	ColumnHeaderPlayerX = 0.055f;
	ColumnHeaderScoreX = 0.54f;
	ColumnHeaderDeathsX = 0.70f;
	ColumnHeaderPingX = 0.965f;
}

void UClutchScoreboard::DrawReadyText(AUTPlayerState* PlayerState,
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


void UClutchScoreboard::DrawScoreHeaders(float RenderDelta, float& YOffset)
{
	const float HeaderY = YOffset;
	Super::DrawScoreHeaders(RenderDelta, YOffset);
	if (!UTGameState || !UTGameState->HasMatchStarted())
	{
		return;
	}

	const float TextY = HeaderY + ColumnHeaderY * RenderScale;
	float XOffset = ScaledEdgeSize;
	for (int32 Column = 0; Column < 2; ++Column)
	{
		DrawText(NSLOCTEXT("ClutchScoreboard", "AttackerWins", "A-WINS"),
			XOffset + ScaledCellWidth * ClutchAttackerWinsColumnX,
			TextY, UTHUDOwner->TinyFont, RenderScale, 1.0f,
			FLinearColor::Black, ETextHorzPos::Center, ETextVertPos::Center);
		XOffset = Canvas->ClipX - ScaledCellWidth - ScaledEdgeSize;
	}
}


void UClutchScoreboard::DrawPlayerScore(AUTPlayerState* PlayerState,
	float XOffset, float YOffset, float Width, FLinearColor DrawColor)
{
	AClutchRoundState* State = ResolveClutchState();
	const FClutchRosterEntry* Entry = State && PlayerState
		? State->FindEntry(PlayerState)
		: nullptr;

	FString RoleText(TEXT("WAIT"));
	FString DefenderHitsText(TEXT("0"));
	FString AttackerWinsText(TEXT("0"));
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

		DefenderHitsText = FString::FromInt(Entry->DefenderDirectHits);
		AttackerWinsText = FString::FromInt(Entry->AttackerRoundsWon);
	}

	DrawText(FText::FromString(RoleText),
		XOffset + Width * ColumnHeaderScoreX, YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);
	DrawText(FText::FromString(DefenderHitsText),
		XOffset + Width * ColumnHeaderDeathsX, YOffset + ColumnY,
		UTHUDOwner->TinyFont, RenderScale, 1.0f, DrawColor,
		ETextHorzPos::Center, ETextVertPos::Center);
	DrawText(FText::FromString(AttackerWinsText),
		XOffset + Width * ClutchAttackerWinsColumnX, YOffset + ColumnY,
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
