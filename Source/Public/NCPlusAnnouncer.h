#pragma once

#include "CoreMinimal.h"
#include "UTAnnouncer.h"
#include "NCPlusAnnouncer.generated.h"

class AUTPlayerController;

/** An announcer choice whose optional content package is installed locally. */
struct FNCPlusAnnouncerPackOption
{
	FString Id;
	FString DisplayName;

	FNCPlusAnnouncerPackOption(const FString& InId, const FString& InDisplayName)
		: Id(InId), DisplayName(InDisplayName)
	{
	}
};

/**
 * UT announcer queue with immediate multikill/spree rewards while preserving
 * status announcements already in progress.
 */
UCLASS(Blueprintable)
class NETCODEPLUS_API UNCPlusAnnouncer : public UUTAnnouncer
{
	GENERATED_BODY()

public:
	UNCPlusAnnouncer(const FObjectInitializer& ObjectInitializer);

	virtual void PlayAnnouncement(TSubclassOf<UUTLocalMessage> MessageClass, int32 Switch,
		const APlayerState* PlayerState1, const APlayerState* PlayerState2,
		const UObject* OptionalObject) override;
	virtual void PlayNextAnnouncement() override;
	virtual void PostInitProperties() override;

	/** Copy the sound-pack data from the announcer selected by UT. */
	void CopyPackDefaultsFrom(const UUTAnnouncer* Source);

private:
	static bool IsLegacyImmediateReward(const FAnnouncementInfo& Announcement);
	static bool ShouldInterruptForLegacyReward(const FAnnouncementInfo& Incoming,
		const FAnnouncementInfo& Existing);
};

/**
 * Client-only announcer installation and optional-pack selection.
 * Optional packs are discovered by package name so NetcodePlus never hard-references them.
 */
namespace NCPlusAnnouncerPacks
{
	NETCODEPLUS_API void Install();
	NETCODEPLUS_API void Uninstall();

	NETCODEPLUS_API void EnumerateAvailable(TArray<FNCPlusAnnouncerPackOption>& OutPacks);
	NETCODEPLUS_API FString GetConfiguredPackId();

	/** Persist and apply a pack. Unknown or unavailable IDs resolve to Stock UT4. */
	NETCODEPLUS_API void SaveAndApply(const FString& PackId, AUTPlayerController* LocalPlayerController);
}
