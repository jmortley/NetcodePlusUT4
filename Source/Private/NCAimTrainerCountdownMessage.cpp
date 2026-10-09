#include "NCAimTrainerCountdownMessage.h"

#include "NCAimTrainerPlayerController.h"
#include "Engine/World.h"

UNCAimTrainerCountdownMessage::UNCAimTrainerCountdownMessage(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	MaxAnnouncementDelay = 0.5f;
}

FText UNCAimTrainerCountdownMessage::GetText(int32 /*Switch*/, bool /*bTargetsPlayerState1*/,
	APlayerState* /*Player1*/, APlayerState* /*Player2*/, UObject* /*OptionalObject*/) const
{
	// The trainer HUD already presents its own countdown.
	return FText::GetEmpty();
}

bool UNCAimTrainerCountdownMessage::IsOptionalSpoken(int32 /*MessageIndex*/) const
{
	// UUTCountDownMessage overrides this method, so changing bOptionalSpoken is insufficient.
	return false;
}

float UNCAimTrainerCountdownMessage::GetAnnouncementPriority(const FAnnouncementInfo /*AnnouncementInfo*/) const
{
	return 1.f;
}

bool UNCAimTrainerCountdownMessage::InterruptAnnouncement(const FAnnouncementInfo AnnouncementInfo,
	const FAnnouncementInfo /*OtherAnnouncementInfo*/) const
{
	// Counts must start on time even when an earlier match announcement is still playing.
	return AnnouncementInfo.Switch >= 1 && AnnouncementInfo.Switch <= 3;
}

bool UNCAimTrainerCountdownMessage::ShouldStillPlay(AUTGameState* GS, const FAnnouncementInfo AnnouncementInfo) const
{
	const ANCAimTrainerPlayerController* PC = Cast<ANCAimTrainerPlayerController>(AnnouncementInfo.OptionalObject);
	if (!IsValid(PC) || !PC->IsLocalController() || !PC->GetWorld()
		|| !Super::ShouldStillPlay(GS, AnnouncementInfo))
	{
		return false;
	}
	const FNCAimTrainerProgress& Progress = PC->GetTrainerProgress();
	// RemainingSeconds stays at 3 while local account verification is pending.
	// Recheck at playback so an abort or a newer count cannot leave stale speech queued.
	return Progress.Phase == 1 && FMath::IsFinite(Progress.RemainingSeconds)
		&& Progress.RemainingSeconds > 0.f && Progress.RemainingSeconds < 3.f
		&& FMath::CeilToInt(Progress.RemainingSeconds) == AnnouncementInfo.Switch
		&& PC->GetWorld()->GetTimeSeconds() - AnnouncementInfo.QueueTime <= MaxAnnouncementDelay;
}
