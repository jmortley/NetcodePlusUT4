#include "NCPlusAnnouncer.h"

#include "UnrealTournament.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UTGameState.h"
#include "UTLocalMessage.h"
#include "UTMultiKillMessage.h"
#include "UTPlayerController.h"
#include "UTSpreeMessage.h"

namespace
{
	const TCHAR* ConfigSection = TEXT("NetcodePlus");
	const TCHAR* ConfigKey = TEXT("AnnouncerPack");
	const TCHAR* StockPackId = TEXT("Stock");
	const TCHAR* StockDisplayName = TEXT("Stock UT4");

	struct FPackDefinition
	{
		const TCHAR* Id;
		const TCHAR* DisplayName;
		const TCHAR* PackagePath;
		const TCHAR* ClassPath;
	};

	const FPackDefinition OptionalPacks[] =
	{
		{
			TEXT("UT2004Male"),
			TEXT("UT2004 Male"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2004Male"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2004Male.BP_NCPAnnouncer_UT2004Male_C")
		},
		{
			TEXT("UT2004Female"),
			TEXT("UT2004 Female"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2004Female"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2004Female.BP_NCPAnnouncer_UT2004Female_C")
		},
		{
			TEXT("UT2003"),
			TEXT("UT2003"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2003"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_UT2003.BP_NCPAnnouncer_UT2003_C")
		},
		{
			TEXT("ClassicUT"),
			TEXT("Classic UT"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_ClassicUT"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_ClassicUT.BP_NCPAnnouncer_ClassicUT_C")
		},
		{
			TEXT("Sexy"),
			TEXT("Sexy"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_Sexy"),
			TEXT("/Game/NetcodePlusOptional/Announcers/UT2004/BP_NCPAnnouncer_Sexy.BP_NCPAnnouncer_Sexy_C")
		}
	};

	FStringClassReference OriginalAnnouncerPath;
	bool bAnnouncerInstalled = false;

	FString GetConfigPath()
	{
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	const FPackDefinition* FindOptionalPack(const FString& PackId)
	{
		for (const FPackDefinition& Pack : OptionalPacks)
		{
			if (PackId.Equals(Pack.Id, ESearchCase::IgnoreCase))
			{
				return &Pack;
			}
		}
		return nullptr;
	}

	bool IsPackInstalled(const FPackDefinition& Pack)
	{
		return FPackageName::DoesPackageExist(Pack.PackagePath);
	}

	const UUTAnnouncer* LoadAnnouncerDefaults(const FString& ClassPath)
	{
		UClass* AnnouncerClass = LoadClass<UUTAnnouncer>(nullptr, *ClassPath, nullptr, LOAD_None, nullptr);
		return (AnnouncerClass != nullptr && AnnouncerClass->IsChildOf(UUTAnnouncer::StaticClass()))
			? Cast<UUTAnnouncer>(AnnouncerClass->GetDefaultObject())
			: nullptr;
	}

	const UUTAnnouncer* GetStockDefaults()
	{
		return OriginalAnnouncerPath.IsValid()
			? LoadAnnouncerDefaults(OriginalAnnouncerPath.ToString())
			: nullptr;
	}

	FString ResolveAvailablePackId(const FString& RequestedId)
	{
		FString TrimmedId = RequestedId;
		TrimmedId.TrimStartAndEndInline();
		if (TrimmedId.IsEmpty() || TrimmedId.Equals(StockPackId, ESearchCase::IgnoreCase))
		{
			return StockPackId;
		}

		const FPackDefinition* Pack = FindOptionalPack(TrimmedId);
		return (Pack != nullptr && IsPackInstalled(*Pack)) ? FString(Pack->Id) : FString(StockPackId);
	}

	const UUTAnnouncer* ResolvePackDefaults(const FString& ResolvedPackId, FString& OutAppliedPackId)
	{
		OutAppliedPackId = ResolvedPackId;
		const FPackDefinition* Pack = FindOptionalPack(ResolvedPackId);
		if (Pack != nullptr && IsPackInstalled(*Pack))
		{
			if (const UUTAnnouncer* PackDefaults = LoadAnnouncerDefaults(Pack->ClassPath))
			{
				return PackDefaults;
			}

			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: announcer pack %s exists but its class could not be loaded"),
				Pack->Id);
			OutAppliedPackId = StockPackId;
		}

		return GetStockDefaults();
	}

	FString ApplyPack(const FString& RequestedId, AUTPlayerController* LocalPlayerController)
	{
		const FString ResolvedId = ResolveAvailablePackId(RequestedId);
		FString AppliedId;
		const UUTAnnouncer* SourceDefaults = ResolvePackDefaults(ResolvedId, AppliedId);
		UNCPlusAnnouncer* NativeDefaults = GetMutableDefault<UNCPlusAnnouncer>();
		if (SourceDefaults == nullptr || NativeDefaults == nullptr)
		{
			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: unable to apply announcer pack %s; stock announcer was unavailable"),
				*RequestedId);
			return StockPackId;
		}

		NativeDefaults->CopyPackDefaultsFrom(SourceDefaults);
		if (LocalPlayerController != nullptr)
		{
			if (UNCPlusAnnouncer* LiveAnnouncer = Cast<UNCPlusAnnouncer>(LocalPlayerController->Announcer))
			{
				// Do not clear CurrentAnnouncement, QueuedAnnouncements, or the audio component.
				// The current line finishes; queued and future lines resolve against the new pack.
				LiveAnnouncer->CopyPackDefaultsFrom(SourceDefaults);
			}
		}

		UE_LOG(LogLoad, Log, TEXT("netcodeplus: announcer pack set to %s"), *AppliedId);
		return AppliedId;
	}
}

UNCPlusAnnouncer::UNCPlusAnnouncer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UNCPlusAnnouncer::CopyPackDefaultsFrom(const UUTAnnouncer* Source)
{
	if (Source == nullptr)
	{
		return;
	}

	Type = Source->Type;
	RewardAudioPath = Source->RewardAudioPath;
	RewardAudioNamePrefix = Source->RewardAudioNamePrefix;
	StatusAudioPath = Source->StatusAudioPath;
	StatusAudioNamePrefix = Source->StatusAudioNamePrefix;
	RewardAudioList = Source->RewardAudioList;
	StatusAudioList = Source->StatusAudioList;
	RewardCachedAudio.Empty();
	StatusCachedAudio.Empty();
}

bool UNCPlusAnnouncer::IsLegacyImmediateReward(const FAnnouncementInfo& Announcement)
{
	if (Announcement.MessageClass == nullptr)
	{
		return false;
	}

	const UClass* IncomingClass = Announcement.MessageClass.Get();
	return IncomingClass->IsChildOf(UUTMultiKillMessage::StaticClass())
		|| IncomingClass->IsChildOf(UUTSpreeMessage::StaticClass());
}

bool UNCPlusAnnouncer::ShouldInterruptForLegacyReward(const FAnnouncementInfo& Incoming,
	const FAnnouncementInfo& Existing)
{
	if (Incoming.MessageClass == nullptr || Existing.MessageClass == nullptr)
	{
		return false;
	}

	if (IsLegacyImmediateReward(Incoming))
	{
		const UUTLocalMessage* ExistingMessage = Existing.MessageClass.GetDefaultObject();
		return !ExistingMessage->bIsStatusAnnouncement;
	}

	const UUTLocalMessage* IncomingMessage = Incoming.MessageClass.GetDefaultObject();
	return IncomingMessage->InterruptAnnouncement(Incoming, Existing);
}

void UNCPlusAnnouncer::PlayAnnouncement(TSubclassOf<UUTLocalMessage> MessageClass, int32 Switch,
	const APlayerState* PlayerState1, const APlayerState* PlayerState2,
	const UObject* OptionalObject)
{
	if (MessageClass == nullptr)
	{
		return;
	}

	const UUTLocalMessage* Message = MessageClass.GetDefaultObject();
	if (Message->EnableAnnouncerLogging())
	{
		UE_LOG(UT, Warning, TEXT("Play announcement %s %d"), *MessageClass->GetName(), Switch);
	}

	FAnnouncementInfo NewAnnouncement(MessageClass, Switch, PlayerState1, PlayerState2,
		OptionalObject, GetWorld()->GetTimeSeconds());
	const bool bLegacyImmediateReward = IsLegacyImmediateReward(NewAnnouncement);
	if (!Message->ShouldStillPlay(GetWorld()->GetGameState<AUTGameState>(), NewAnnouncement))
	{
		return;
	}

	const FName SoundName = Message->GetAnnouncementName(Switch, OptionalObject, PlayerState1, PlayerState2);
	if (SoundName == NAME_None)
	{
		return;
	}

	if (CurrentAnnouncement.MessageClass != nullptr
		&& ShouldInterruptForLegacyReward(NewAnnouncement, CurrentAnnouncement))
	{
		if (CurrentAnnouncement.MessageClass.GetDefaultObject()->EnableAnnouncerLogging())
		{
			UE_LOG(UT, Warning, TEXT("%s %d immediate interrupting %s %d"),
				*MessageClass->GetName(), Switch, *CurrentAnnouncement.MessageClass->GetName(),
				CurrentAnnouncement.Switch);
		}

		for (int32 Index = QueuedAnnouncements.Num() - 1; Index >= 0; --Index)
		{
			if (ShouldInterruptForLegacyReward(NewAnnouncement, QueuedAnnouncements[Index]))
			{
				if (QueuedAnnouncements[Index].MessageClass.GetDefaultObject()->EnableAnnouncerLogging())
				{
					UE_LOG(UT, Warning, TEXT("%s %d also interrupting %s %d"),
						*MessageClass->GetName(), Switch,
						*QueuedAnnouncements[Index].MessageClass->GetName(),
						QueuedAnnouncements[Index].Switch);
				}
				QueuedAnnouncements.RemoveAt(Index);
			}
		}

		if (bLegacyImmediateReward)
		{
			// Every non-status item was removed above. Keep any protected status calls
			// ahead of the reward; StartNextAnnouncement() will play that status now.
			QueuedAnnouncements.Add(NewAnnouncement);
		}
		else
		{
			QueuedAnnouncements.Insert(NewAnnouncement, 0);
		}
		StartNextAnnouncement(false);
		return;
	}

	bool bCancelThisAnnouncement = false;
	const float AnnouncementPriority = Message->GetAnnouncementPriority(NewAnnouncement);
	if (CurrentAnnouncement.MessageClass != nullptr
		&& Message->CancelByAnnouncement(Switch, OptionalObject,
			CurrentAnnouncement.MessageClass, CurrentAnnouncement.Switch,
			CurrentAnnouncement.OptionalObject))
	{
		if (Message->EnableAnnouncerLogging())
		{
			UE_LOG(UT, Warning, TEXT("%s %d cancelled by %s %d"), *MessageClass->GetName(),
				Switch, *CurrentAnnouncement.MessageClass->GetName(), CurrentAnnouncement.Switch);
		}
		bCancelThisAnnouncement = true;
	}
	else
	{
		for (int32 Index = QueuedAnnouncements.Num() - 1; Index >= 0; --Index)
		{
			if (ShouldInterruptForLegacyReward(NewAnnouncement, QueuedAnnouncements[Index]))
			{
				if (QueuedAnnouncements[Index].MessageClass.GetDefaultObject()->EnableAnnouncerLogging())
				{
					UE_LOG(UT, Warning, TEXT("%s %d interrupting %s %d"),
						*MessageClass->GetName(), Switch,
						*QueuedAnnouncements[Index].MessageClass->GetName(),
						QueuedAnnouncements[Index].Switch);
				}
				QueuedAnnouncements.RemoveAt(Index);
			}
			else if (Message->CancelByAnnouncement(Switch, OptionalObject,
				QueuedAnnouncements[Index].MessageClass, QueuedAnnouncements[Index].Switch,
				QueuedAnnouncements[Index].OptionalObject))
			{
				if (Message->EnableAnnouncerLogging())
				{
					UE_LOG(UT, Warning, TEXT("%s %d cancelled by %s %d"),
						*MessageClass->GetName(), Switch,
						*QueuedAnnouncements[Index].MessageClass->GetName(),
						QueuedAnnouncements[Index].Switch);
				}
				bCancelThisAnnouncement = true;
			}
		}
	}

	if (bCancelThisAnnouncement)
	{
		return;
	}

	int32 InsertIndex = INDEX_NONE;
	if (!bLegacyImmediateReward)
	{
		for (int32 Index = 0; Index < QueuedAnnouncements.Num(); ++Index)
		{
			const float QueuedPriority = QueuedAnnouncements[Index].MessageClass.GetDefaultObject()
				->GetAnnouncementPriority(QueuedAnnouncements[Index]);
			if (AnnouncementPriority > QueuedPriority)
			{
				InsertIndex = Index;
				break;
			}
		}
	}

	if (InsertIndex != INDEX_NONE)
	{
		QueuedAnnouncements.Insert(NewAnnouncement, InsertIndex);
	}
	else
	{
		QueuedAnnouncements.Add(NewAnnouncement);
	}

	if (CurrentAnnouncement.MessageClass == nullptr && !AnnouncementComp->IsPlaying()
		&& (bLegacyImmediateReward
			|| !GetWorld()->GetTimerManager().IsTimerActive(PlayNextAnnouncementHandle)))
	{
		StartNextAnnouncement(false);
	}
}

void NCPlusAnnouncerPacks::Install()
{
	if (bAnnouncerInstalled)
	{
		return;
	}

	AUTPlayerController* PlayerControllerCDO = GetMutableDefault<AUTPlayerController>();
	if (PlayerControllerCDO == nullptr || !PlayerControllerCDO->AnnouncerPath.IsValid())
	{
		return;
	}

	UClass* CurrentAnnouncerClass = PlayerControllerCDO->AnnouncerPath.TryLoadClass<UUTAnnouncer>();
	if (CurrentAnnouncerClass != nullptr
		&& CurrentAnnouncerClass->IsChildOf(UNCPlusAnnouncer::StaticClass()))
	{
		return;
	}

	OriginalAnnouncerPath = PlayerControllerCDO->AnnouncerPath;
	if (GetStockDefaults() == nullptr)
	{
		UE_LOG(LogLoad, Warning, TEXT("netcodeplus: could not load configured stock announcer %s"),
			*OriginalAnnouncerPath.ToString());
		OriginalAnnouncerPath = FStringClassReference();
		return;
	}

	FString ConfiguredId;
	if (!GConfig->GetString(ConfigSection, ConfigKey, ConfiguredId, GetConfigPath()))
	{
		ConfiguredId = StockPackId;
	}
	ApplyPack(ConfiguredId, nullptr);

	PlayerControllerCDO->AnnouncerPath = FStringClassReference(UNCPlusAnnouncer::StaticClass());
	bAnnouncerInstalled = true;
}

void NCPlusAnnouncerPacks::Uninstall()
{
	if (!bAnnouncerInstalled)
	{
		return;
	}

	AUTPlayerController* PlayerControllerCDO = GetMutableDefault<AUTPlayerController>();
	const FStringClassReference NativeAnnouncerPath(UNCPlusAnnouncer::StaticClass());
	if (PlayerControllerCDO != nullptr
		&& PlayerControllerCDO->AnnouncerPath.ToString() == NativeAnnouncerPath.ToString())
	{
		PlayerControllerCDO->AnnouncerPath = OriginalAnnouncerPath;
	}

	OriginalAnnouncerPath = FStringClassReference();
	bAnnouncerInstalled = false;
}

void NCPlusAnnouncerPacks::EnumerateAvailable(TArray<FNCPlusAnnouncerPackOption>& OutPacks)
{
	OutPacks.Reset();
	OutPacks.Emplace(StockPackId, StockDisplayName);
	for (const FPackDefinition& Pack : OptionalPacks)
	{
		if (IsPackInstalled(Pack))
		{
			OutPacks.Emplace(Pack.Id, Pack.DisplayName);
		}
	}
}

FString NCPlusAnnouncerPacks::GetConfiguredPackId()
{
	FString ConfiguredId;
	if (!GConfig->GetString(ConfigSection, ConfigKey, ConfiguredId, GetConfigPath()))
	{
		return StockPackId;
	}
	return ResolveAvailablePackId(ConfiguredId);
}

void NCPlusAnnouncerPacks::SaveAndApply(const FString& PackId, AUTPlayerController* LocalPlayerController)
{
	const FString AppliedId = ApplyPack(PackId, LocalPlayerController);
	GConfig->SetString(ConfigSection, ConfigKey, *AppliedId, GetConfigPath());
	GConfig->Flush(false, GetConfigPath());
}
