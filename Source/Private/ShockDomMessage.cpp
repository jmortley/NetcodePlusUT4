#include "ShockDomMessage.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "ShockDomControlPoint.h"


UShockDomMessage::UShockDomMessage(const FObjectInitializer& OI)
	: Super(OI)
{
	bIsStatusAnnouncement = true;
	Lifetime = 3.0f;
	MessageArea = FName(TEXT("Announcements"));
	MessageSlot = FName(TEXT("GameMessages"));
}


FText UShockDomMessage::GetText(int32 Switch, bool bTargetsPlayerState1,
	APlayerState* RelatedPlayerState_1, APlayerState* RelatedPlayerState_2,
	UObject* OptionalObject) const
{
	AShockDomControlPoint* Point = Cast<AShockDomControlPoint>(OptionalObject);
	FString PointName = Point ? Point->PointLabel : TEXT("?");

	// Get team name from RelatedPlayerState_1
	FString TeamName = TEXT("A team");
	AUTPlayerState* UTPS = Cast<AUTPlayerState>(RelatedPlayerState_1);
	if (UTPS && UTPS->Team)
	{
		TeamName = (UTPS->Team->TeamIndex == 0) ? TEXT("RED") : TEXT("BLUE");
	}

	switch (Switch)
	{
	case 0:
		// "RED captured Point A"
		return FText::FromString(FString::Printf(TEXT("%s captured Point %s"), *TeamName, *PointName));

	case 1:
		// "RED controls all points!"
		return FText::FromString(FString::Printf(TEXT("%s controls all points!"), *TeamName));

	default:
		return FText::GetEmpty();
	}
}


FLinearColor UShockDomMessage::GetMessageColor_Implementation(int32 MessageIndex) const
{
	// White — the team name itself conveys the team color context
	return FLinearColor::White;
}
