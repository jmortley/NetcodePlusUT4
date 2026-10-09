#pragma once

#include "NetcodePlus.h"
#include "UTCountDownMessage.h"
#include "NCAimTrainerCountdownMessage.generated.h"

/** Trainer-only spoken countdown using the local player's selected announcer pack. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API UNCAimTrainerCountdownMessage : public UUTCountDownMessage
{
	GENERATED_BODY()

public:
	UNCAimTrainerCountdownMessage(const FObjectInitializer& ObjectInitializer);
	virtual FText GetText(int32 Switch, bool bTargetsPlayerState1, APlayerState* Player1,
		APlayerState* Player2, UObject* OptionalObject) const override;
	virtual bool IsOptionalSpoken(int32 MessageIndex) const override;
	virtual float GetAnnouncementPriority(const FAnnouncementInfo AnnouncementInfo) const override;
	virtual bool InterruptAnnouncement(const FAnnouncementInfo AnnouncementInfo,
		const FAnnouncementInfo OtherAnnouncementInfo) const override;
	virtual bool ShouldStillPlay(AUTGameState* GS, const FAnnouncementInfo AnnouncementInfo) const override;
};
