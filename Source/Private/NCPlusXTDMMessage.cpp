#include "NCPlusXTDMMessage.h"

#include "NCPlusXTDMHUD.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTCTFGameMessage.h"

UNCPlusXTDMGameMessage::UNCPlusXTDMGameMessage(const FObjectInitializer& OI) : Super(OI) {}
UNCPlusXTDMVictoryMessage::UNCPlusXTDMVictoryMessage(const FObjectInitializer& OI) : Super(OI) {}

void UNCPlusXTDMGameMessage::GetEmphasisText(FText& Prefix, FText& Emphasis, FText& Postfix,
	FLinearColor& Color, int32 Switch, APlayerState* Player1, APlayerState* Player2, UObject* OptionalObject) const
{
	const AUTPlayerState* PS = Cast<AUTPlayerState>(Player1);
	if (Switch == 9 || Switch == 10)
	{
		Prefix = PS && PS->Team ? NSLOCTEXT("NCPlusXTDM", "YourTeam", "You are on ") : FText::GetEmpty();
		Emphasis = PS && PS->Team ? FText::FromString(ANCPlusXTDMHUD::TeamLabel(PS->GetTeamNum()))
			: NSLOCTEXT("NCPlusXTDM", "TeamChanged", "Team updated");
		Postfix = FText::GetEmpty();
		Color = PS && PS->Team ? ANCPlusXTDMHUD::TeamColor(nullptr, PS->GetTeamNum()) : FLinearColor::White;
		return;
	}
	Super::GetEmphasisText(Prefix, Emphasis, Postfix, Color, Switch, Player1, Player2, OptionalObject);
}

FName UNCPlusXTDMGameMessage::GetAnnouncementName_Implementation(int32 Switch, const UObject* Object,
	const APlayerState* Player1, const APlayerState* Player2) const
{
	return Switch == 9 || Switch == 10 ? NAME_None
		: Super::GetAnnouncementName_Implementation(Switch, Object, Player1, Player2);
}

void UNCPlusXTDMVictoryMessage::GetEmphasisText(FText& Prefix, FText& Emphasis, FText& Postfix,
	FLinearColor& Color, int32 Switch, APlayerState* Player1, APlayerState* Player2, UObject* OptionalObject) const
{
	const AUTTeamInfo* Team = Cast<AUTTeamInfo>(OptionalObject);
	Prefix = FText::GetEmpty();
	Emphasis = Team ? FText::FromString(ANCPlusXTDMHUD::TeamLabel(Team->TeamIndex))
		: NSLOCTEXT("NCPlusXTDM", "Draw", "DRAW");
	Postfix = Team ? NSLOCTEXT("NCPlusXTDM", "Wins", " wins the match") : FText::GetEmpty();
	Color = Team ? ANCPlusXTDMHUD::TeamColor(nullptr, Team->TeamIndex) : FLinearColor::White;
}

FText UNCPlusXTDMVictoryMessage::GetText(int32 Switch, bool bTargetsPlayerState1,
	APlayerState* Player1, APlayerState* Player2, UObject* OptionalObject) const
{
	return BuildEmphasisText(Switch, Player1, Player2, OptionalObject);
}

FName UNCPlusXTDMVictoryMessage::GetAnnouncementName_Implementation(int32 Switch, const UObject* Object,
	const APlayerState* Player1, const APlayerState* Player2) const
{
	// Stock has no green/yellow team recordings. The inherited generic victory stinger remains.
	return NAME_None;
}
