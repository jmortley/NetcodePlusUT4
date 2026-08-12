#include "NCPlusAnnouncer.h"

#include "UnrealTournament.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UTCTFGameMessage.h"
#include "UTGameState.h"
#include "UTLocalMessage.h"
#include "UTMultiKillMessage.h"
#include "UTPlayerController.h"
#include "UTSpreeMessage.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCPlusAnnouncer, Log, All);

// Temporary client-side announcer diagnostics. Pure logging: no queue, playback,
// replication, or RPC behavior changes.
// Default 0 for live (restored for the 2026-08-14 328 cut; was 1 during the 328-RC dogfood).
static TAutoConsoleVariable<int32> CVarAnnouncerTrace(
	TEXT("ncp.AnnouncerTrace"), 0,
	TEXT("Announcer diagnostics: 0=off, 1=trace install/defaults, queue admission, sound lookup/cache, and playback."),
	ECVF_Default);

// Flag-taken audibility (frenchempire 2026-08-06, "flag re-taken is silent"):
// stock UUTCTFGameMessage::CancelByAnnouncement cancels an incoming taken(4)
// whenever ANY taken(4) is current or queued — with no team comparison, so the
// OTHER flag's line (routine two-flag traffic) eats it — and also while a
// scoring/halftime/overtime line is up. Regrabs have no base alarm to mask the
// loss (that alarm is bWasHome-gated), so the eaten line IS the event.
static TAutoConsoleVariable<int32> CVarFlagTakenGuarantee(
	TEXT("ncp.FlagTakenGuarantee"), 1,
	TEXT("Flag-taken announcements are never cancelled by unrelated announcements: 0=stock cancellation rules, 1=always queue (deduped against an identical queued line)."),
	ECVF_Default);

namespace
{
	const TCHAR* ConfigSection = TEXT("NetcodePlus");
	const TCHAR* ConfigKey = TEXT("AnnouncerPack");
	const TCHAR* OriginalAnnouncerPathConfigKey = TEXT("OriginalAnnouncerPath");
	const TCHAR* PlayerControllerAnnouncerPathKey = TEXT("AnnouncerPath");
	const TCHAR* CanonicalStockAnnouncerClassPath = TEXT("/Game/RestrictedAssets/Blueprints/RewardAnnouncerMale.RewardAnnouncerMale_C");
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
	FDelegateHandle AnnouncerUpgradeTickerHandle;

	bool AnnouncerTraceEnabled()
	{
		return CVarAnnouncerTrace.GetValueOnAnyThread() > 0;
	}

	FString DescribeAnnouncement(const FAnnouncementInfo& Announcement)
	{
		return Announcement.MessageClass != nullptr
			? FString::Printf(TEXT("%s:%d"), *Announcement.MessageClass->GetName(), Announcement.Switch)
			: FString(TEXT("None"));
	}

	FString DescribeObject(const UObject* Object)
	{
		return Object != nullptr ? Object->GetPathName() : FString(TEXT("None"));
	}

	bool IsGuaranteedFlagTaken(const FAnnouncementInfo& Announcement)
	{
		return CVarFlagTakenGuarantee.GetValueOnGameThread() > 0
			&& Announcement.MessageClass != nullptr
			&& Announcement.MessageClass->IsChildOf(UUTCTFGameMessage::StaticClass())
			&& Announcement.Switch == 4;
	}

	struct FAnnouncerLookupTrace
	{
		FString CacheState = TEXT("not-applicable");
		FString ExplicitListMatch = TEXT("None");
		FString PackageName = TEXT("None");
		FString ObjectPath = TEXT("None");
		bool bPackageExists = false;
	};

	FString DescribeCacheState(UUTAnnouncer* Announcer, bool bStatus, FName SoundName)
	{
		if (Announcer == nullptr || SoundName == NAME_None || SoundName == NAME_Custom)
		{
			return TEXT("not-applicable");
		}

		USoundBase** CachePtr = bStatus
			? Announcer->StatusCachedAudio.Find(SoundName) : Announcer->RewardCachedAudio.Find(SoundName);
		if (CachePtr == nullptr)
		{
			return TEXT("absent");
		}
		if (*CachePtr == nullptr)
		{
			return TEXT("cached-null");
		}
		return FString::Printf(TEXT("hit:%s"), *DescribeObject(*CachePtr));
	}

	FAnnouncerLookupTrace BuildLookupTrace(UUTAnnouncer* Announcer, bool bStatus, FName SoundName)
	{
		FAnnouncerLookupTrace Result;
		if (Announcer == nullptr || SoundName == NAME_None || SoundName == NAME_Custom)
		{
			return Result;
		}

		Result.CacheState = DescribeCacheState(Announcer, bStatus, SoundName);

		const TArray<FAnnouncerSound>& AudioList = bStatus
			? Announcer->StatusAudioList : Announcer->RewardAudioList;
		for (const FAnnouncerSound& Entry : AudioList)
		{
			if (Entry.SoundName == SoundName)
			{
				Result.ExplicitListMatch = Entry.Sound != nullptr
					? DescribeObject(Entry.Sound) : FString(TEXT("entry-null"));
				break;
			}
		}

		FString AudioPath = bStatus ? Announcer->StatusAudioPath : Announcer->RewardAudioPath;
		if (!AudioPath.EndsWith(TEXT("/")))
		{
			AudioPath += TEXT("/");
		}
		const FString AudioPrefix = bStatus
			? Announcer->StatusAudioNamePrefix : Announcer->RewardAudioNamePrefix;
		Result.PackageName = AudioPath + AudioPrefix + SoundName.ToString();
		Result.ObjectPath = Result.PackageName + TEXT(".") + AudioPrefix + SoundName.ToString();
		Result.bPackageExists = FPackageName::DoesPackageExist(Result.PackageName);
		return Result;
	}

	void TracePackDefaults(const TCHAR* Phase, const UUTAnnouncer* Announcer)
	{
		if (!AnnouncerTraceEnabled())
		{
			return;
		}

		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] %s object=%s class=%s template=%d outer=%s type=%d rewardPath=\"%s\" rewardPrefix=\"%s\" rewardList=%d rewardCache=%d statusPath=\"%s\" statusPrefix=\"%s\" statusList=%d statusCache=%d comp=%s playing=%d sound=%s"),
			Phase,
			*DescribeObject(Announcer),
			Announcer != nullptr ? *Announcer->GetClass()->GetPathName() : TEXT("None"),
			Announcer != nullptr && Announcer->IsTemplate() ? 1 : 0,
			Announcer != nullptr ? *DescribeObject(Announcer->GetOuter()) : TEXT("None"),
			Announcer != nullptr ? int32(Announcer->Type) : -1,
			Announcer != nullptr ? *Announcer->RewardAudioPath : TEXT(""),
			Announcer != nullptr ? *Announcer->RewardAudioNamePrefix : TEXT(""),
			Announcer != nullptr ? Announcer->RewardAudioList.Num() : -1,
			Announcer != nullptr ? Announcer->RewardCachedAudio.Num() : -1,
			Announcer != nullptr ? *Announcer->StatusAudioPath : TEXT(""),
			Announcer != nullptr ? *Announcer->StatusAudioNamePrefix : TEXT(""),
			Announcer != nullptr ? Announcer->StatusAudioList.Num() : -1,
			Announcer != nullptr ? Announcer->StatusCachedAudio.Num() : -1,
			Announcer != nullptr ? *DescribeObject(Announcer->AnnouncementComp) : TEXT("None"),
			Announcer != nullptr && Announcer->AnnouncementComp != nullptr && Announcer->AnnouncementComp->IsPlaying() ? 1 : 0,
			Announcer != nullptr && Announcer->AnnouncementComp != nullptr
				? *DescribeObject(Announcer->AnnouncementComp->Sound) : TEXT("None"));
	}

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

	bool TryResolveBaseAnnouncerPath(const FString& Candidate,
		FStringClassReference& OutAnnouncerPath)
	{
		FString TrimmedCandidate = Candidate;
		TrimmedCandidate.Trim();
		TrimmedCandidate.TrimTrailing();
		if (TrimmedCandidate.IsEmpty())
		{
			return false;
		}

		const UUTAnnouncer* Defaults = LoadAnnouncerDefaults(TrimmedCandidate);
		if (Defaults == nullptr
			|| Defaults->GetClass()->IsChildOf(UNCPlusAnnouncer::StaticClass()))
		{
			return false;
		}

		OutAnnouncerPath = FStringClassReference(TrimmedCandidate);
		return true;
	}

	bool TryGetSourceAnnouncerPath(FString& OutAnnouncerPath)
	{
		if (GConfig == nullptr || GGameIni.IsEmpty())
		{
			return false;
		}

		FConfigFile* GameConfig = GConfig->FindConfigFile(GGameIni);
		if (GameConfig == nullptr || GameConfig->SourceConfigFile == nullptr)
		{
			return false;
		}

		const FString PlayerControllerSection = AUTPlayerController::StaticClass()->GetPathName();
		return GameConfig->SourceConfigFile->GetString(
			*PlayerControllerSection, PlayerControllerAnnouncerPathKey, OutAnnouncerPath);
	}

	bool ResolveOriginalAnnouncerPath(const FStringClassReference& CurrentPath,
		UClass* CurrentClass, FStringClassReference& OutAnnouncerPath, FString& OutSource)
	{
		if (TryResolveBaseAnnouncerPath(CurrentPath.ToString(), OutAnnouncerPath))
		{
			OutSource = TEXT("current-player-controller-default");
			return true;
		}

		// Do not turn an intentionally empty or invalid announcer setting back on.
		// Recovery fallbacks are reserved for the self-reference written by an earlier
		// NetcodePlus run (or for hot reload while our native class is still installed).
		if (CurrentClass == nullptr
			|| !CurrentClass->IsChildOf(UNCPlusAnnouncer::StaticClass()))
		{
			return false;
		}

		FString Candidate;
		if (GConfig != nullptr
			&& GConfig->GetString(ConfigSection, OriginalAnnouncerPathConfigKey,
				Candidate, GetConfigPath())
			&& TryResolveBaseAnnouncerPath(Candidate, OutAnnouncerPath))
		{
			OutSource = TEXT("saved-netcodeplus-original");
			return true;
		}

		Candidate.Empty();
		if (TryGetSourceAnnouncerPath(Candidate)
			&& TryResolveBaseAnnouncerPath(Candidate, OutAnnouncerPath))
		{
			OutSource = TEXT("untainted-game-default");
			return true;
		}

		if (TryResolveBaseAnnouncerPath(CanonicalStockAnnouncerClassPath, OutAnnouncerPath))
		{
			OutSource = TEXT("canonical-stock-fallback");
			return true;
		}

		return false;
	}

	void PersistOriginalAnnouncerPath()
	{
		if (GConfig == nullptr || !OriginalAnnouncerPath.IsValid())
		{
			return;
		}

		const FString ConfigPath = GetConfigPath();
		const FString OriginalPath = OriginalAnnouncerPath.ToString();
		FString SavedPath;
		if (GConfig->GetString(ConfigSection, OriginalAnnouncerPathConfigKey,
			SavedPath, ConfigPath)
			&& SavedPath == OriginalPath)
		{
			return;
		}

		GConfig->SetString(ConfigSection, OriginalAnnouncerPathConfigKey,
			*OriginalPath, ConfigPath);
		GConfig->Flush(false, ConfigPath);
	}

	void RepairPlayerControllerAnnouncerConfig(bool bOnlyIfNativePath)
	{
		if (GConfig == nullptr || GGameIni.IsEmpty() || !OriginalAnnouncerPath.IsValid())
		{
			return;
		}

		const FString PlayerControllerSection = AUTPlayerController::StaticClass()->GetPathName();
		const FString OriginalPath = OriginalAnnouncerPath.ToString();
		const FString NativePath = FStringClassReference(UNCPlusAnnouncer::StaticClass()).ToString();
		FString ConfiguredPath;
		const bool bHasConfiguredPath = GConfig->GetString(*PlayerControllerSection,
			PlayerControllerAnnouncerPathKey, ConfiguredPath, GGameIni);

		if (bOnlyIfNativePath
			&& (!bHasConfiguredPath || ConfiguredPath != NativePath))
		{
			return;
		}
		if (bHasConfiguredPath && ConfiguredPath == OriginalPath)
		{
			return;
		}

		GConfig->SetString(*PlayerControllerSection, PlayerControllerAnnouncerPathKey,
			*OriginalPath, GGameIni);
		GConfig->Flush(false, GGameIni);

		UE_LOG(LogLoad, Log,
			TEXT("netcodeplus: repaired persisted announcer path from %s to %s"),
			bHasConfiguredPath ? *ConfiguredPath : TEXT("None"), *OriginalPath);
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] CONFIG-REPAIR gameIni=%s old=%s new=%s"),
				*GGameIni, bHasConfiguredPath ? *ConfiguredPath : TEXT("None"), *OriginalPath);
		}
	}

	const UUTAnnouncer* GetStockDefaults()
	{
		if (!OriginalAnnouncerPath.IsValid())
		{
			return nullptr;
		}

		const UUTAnnouncer* Defaults = LoadAnnouncerDefaults(OriginalAnnouncerPath.ToString());
		return (Defaults != nullptr
			&& !Defaults->GetClass()->IsChildOf(UNCPlusAnnouncer::StaticClass()))
			? Defaults : nullptr;
	}

	FString ResolveAvailablePackId(const FString& RequestedId)
	{
		// 4.15 API: TrimStartAndEnd/TrimStartAndEndInline do not exist yet —
		// Trim() (leading) + TrimTrailing() are the in-place equivalents.
		FString TrimmedId = RequestedId;
		TrimmedId.Trim();
		TrimmedId.TrimTrailing();
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
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] APPLY requested=%s resolved=%s applied=%s localPC=%s source=%s native=%s"),
				*RequestedId, *ResolvedId, *AppliedId, *DescribeObject(LocalPlayerController),
				*DescribeObject(SourceDefaults), *DescribeObject(NativeDefaults));
			TracePackDefaults(TEXT("APPLY-SOURCE"), SourceDefaults);
			TracePackDefaults(TEXT("APPLY-NATIVE-BEFORE"), NativeDefaults);
		}
		if (SourceDefaults == nullptr || NativeDefaults == nullptr)
		{
			UE_LOG(LogLoad, Warning, TEXT("netcodeplus: unable to apply announcer pack %s; stock announcer was unavailable"),
				*RequestedId);
			return StockPackId;
		}

		NativeDefaults->CopyPackDefaultsFrom(SourceDefaults);
		TracePackDefaults(TEXT("APPLY-NATIVE-AFTER"), NativeDefaults);
		if (LocalPlayerController != nullptr)
		{
			if (UNCPlusAnnouncer* LiveAnnouncer = Cast<UNCPlusAnnouncer>(LocalPlayerController->Announcer))
			{
				// Do not clear CurrentAnnouncement, QueuedAnnouncements, or the audio component.
				// The current line finishes; queued and future lines resolve against the new pack.
				LiveAnnouncer->CopyPackDefaultsFrom(SourceDefaults);
				TracePackDefaults(TEXT("APPLY-LIVE-AFTER"), LiveAnnouncer);
			}
			else if (AnnouncerTraceEnabled())
			{
				UE_LOG(LogNCPlusAnnouncer, Warning,
					TEXT("[AnnouncerTrace] APPLY-LIVE-SKIP pc=%s announcer=%s class=%s"),
					*DescribeObject(LocalPlayerController),
					*DescribeObject(LocalPlayerController->Announcer),
					LocalPlayerController->Announcer != nullptr
						? *LocalPlayerController->Announcer->GetClass()->GetPathName() : TEXT("None"));
			}
		}

		UE_LOG(LogLoad, Log, TEXT("netcodeplus: announcer pack set to %s"), *AppliedId);
		return AppliedId;
	}

	/** BP PlayerController subclasses (Elim/Wipeout's ElimUTPlayerController_C)
	 *  carry their own AnnouncerPath default, so the base-CDO override written by
	 *  Install() never reaches them and InitInputSystem builds the stock
	 *  announcer — the pack silently stops working in those modes (log signature:
	 *  APPLY-LIVE-SKIP with announcer=RewardAnnouncerMale_C). This low-frequency
	 *  client ticker rebuilds any local PC's foreign announcer as ours;
	 *  PostInitProperties seeds the selected pack from the native CDO and
	 *  precaches, exactly like a normal install. A PC with NO announcer is left
	 *  alone (an intentionally disabled announcer stays disabled). */
	bool TickUpgradeLocalAnnouncers(float /*DeltaTime*/)
	{
		if (!bAnnouncerInstalled || GEngine == nullptr)
		{
			return true;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr
				|| (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE))
			{
				continue;
			}
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				AUTPlayerController* PC = Cast<AUTPlayerController>(*It);
				if (PC == nullptr || !PC->IsLocalPlayerController()
					|| PC->Announcer == nullptr
					|| PC->Announcer->IsA<UNCPlusAnnouncer>())
				{
					continue;
				}
				// Replace only a FULLY idle announcer; retry on the next tick.
				// Mid-line is obvious, but the between-lines spacing gap matters
				// too: AnnouncementFinished has cleared CurrentAnnouncement and
				// armed PlayNextAnnouncementHandle while queued lines remain —
				// swapping there orphans the queue AND the armed timer keeps
				// ticking on the orphan, which can play audio on its own until GC.
				if (PC->Announcer->CurrentAnnouncement.MessageClass != nullptr
					|| PC->Announcer->QueuedAnnouncements.Num() > 0
					|| World->GetTimerManager().IsTimerActive(PC->Announcer->PlayNextAnnouncementHandle)
					|| (PC->Announcer->AnnouncementComp != nullptr
						&& PC->Announcer->AnnouncementComp->IsPlaying()))
				{
					continue;
				}
				UUTAnnouncer* OldAnnouncer = PC->Announcer;
				PC->Announcer = NewObject<UNCPlusAnnouncer>(PC);
				if (AnnouncerTraceEnabled())
				{
					UE_LOG(LogNCPlusAnnouncer, Warning,
						TEXT("[AnnouncerTrace] UPGRADE pc=%s old=%s new=%s"),
						*DescribeObject(PC), *DescribeObject(OldAnnouncer),
						*DescribeObject(PC->Announcer));
				}
			}
		}
		return true;
	}
}

UNCPlusAnnouncer::UNCPlusAnnouncer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UNCPlusAnnouncer::PostInitProperties()
{
	// Runtime changes to the native CDO are not reliably propagated through UE4's
	// precomputed native-class defaults. Seed the live object explicitly before
	// UUTAnnouncer::PostInitProperties() precaches the current game mode's sounds.
	if (!IsTemplate() && bAnnouncerInstalled)
	{
		const UNCPlusAnnouncer* SelectedDefaults = Cast<UNCPlusAnnouncer>(
			UNCPlusAnnouncer::StaticClass()->GetDefaultObject(false));
		if (SelectedDefaults != nullptr && SelectedDefaults != this)
		{
			CopyPackDefaultsFrom(SelectedDefaults);
		}
	}

	Super::PostInitProperties();
	if (IsInGameThread())
	{
		TracePackDefaults(IsTemplate() ? TEXT("POSTINIT-TEMPLATE") : TEXT("POSTINIT-LIVE"), this);
	}
}

void UNCPlusAnnouncer::CopyPackDefaultsFrom(const UUTAnnouncer* Source)
{
	if (Source == nullptr)
	{
		return;
	}

	TracePackDefaults(TEXT("COPY-DEST-BEFORE"), this);
	TracePackDefaults(TEXT("COPY-SOURCE"), Source);

	Type = Source->Type;
	RewardAudioPath = Source->RewardAudioPath;
	RewardAudioNamePrefix = Source->RewardAudioNamePrefix;
	StatusAudioPath = Source->StatusAudioPath;
	StatusAudioNamePrefix = Source->StatusAudioNamePrefix;
	RewardAudioList = Source->RewardAudioList;
	StatusAudioList = Source->StatusAudioList;
	RewardCachedAudio.Empty();
	StatusCachedAudio.Empty();
	TracePackDefaults(TEXT("COPY-DEST-AFTER"), this);
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

bool UNCPlusAnnouncer::ShouldInterruptAnnouncement(const FAnnouncementInfo& Incoming,
	const FAnnouncementInfo& Existing)
{
	if (Incoming.MessageClass == nullptr || Existing.MessageClass == nullptr)
	{
		return false;
	}

	// "Immediate" controls queue timing, not which rewards are disposable. Stock
	// message classes already encode the semantic replacements they permit (for
	// example, a newer normal spree replacing an older one). The former blanket
	// non-status interruption dropped same-kill rewards that arrive before a spree:
	// Double Kill/Headshot -> Rampage should remain an audible sequence.
	const UUTLocalMessage* IncomingMessage = Incoming.MessageClass.GetDefaultObject();
	return IncomingMessage->InterruptAnnouncement(Incoming, Existing);
}

void UNCPlusAnnouncer::PlayAnnouncement(TSubclassOf<UUTLocalMessage> MessageClass, int32 Switch,
	const APlayerState* PlayerState1, const APlayerState* PlayerState2,
	const UObject* OptionalObject)
{
	if (MessageClass == nullptr)
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning, TEXT("[AnnouncerTrace] ADMIT-DROP reason=null-message"));
		}
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
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-DROP reason=should-still-play msg=%s switch=%d status=%d current=%s queue=%d"),
				*MessageClass->GetPathName(), Switch, Message->bIsStatusAnnouncement ? 1 : 0,
				*DescribeAnnouncement(CurrentAnnouncement), QueuedAnnouncements.Num());
		}
		return;
	}

	const FName SoundName = Message->GetAnnouncementName(Switch, OptionalObject, PlayerState1, PlayerState2);
	if (SoundName == NAME_None)
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-DROP reason=sound-none msg=%s switch=%d current=%s queue=%d"),
				*MessageClass->GetPathName(), Switch, *DescribeAnnouncement(CurrentAnnouncement),
				QueuedAnnouncements.Num());
		}
		return;
	}

	if (AnnouncerTraceEnabled())
	{
		AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
		const bool bTimerActive = GetWorld()->GetTimerManager().IsTimerActive(PlayNextAnnouncementHandle);
		const FAnnouncerLookupTrace LookupTrace = BuildLookupTrace(
			this, Message->bIsStatusAnnouncement, SoundName);
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] ADMIT announcer=%s outer=%s msg=%s switch=%d status=%d legacy=%d sound=%s match=%s current=%s queue=%d timer=%d cache=%s explicit=%s package=%s object=%s exists=%d compPlaying=%d compSound=%s"),
			*GetClass()->GetPathName(), *DescribeObject(GetOuter()), *MessageClass->GetPathName(), Switch,
			Message->bIsStatusAnnouncement ? 1 : 0, bLegacyImmediateReward ? 1 : 0,
			*SoundName.ToString(), GameState != nullptr ? *GameState->GetMatchState().ToString() : TEXT("None"),
			*DescribeAnnouncement(CurrentAnnouncement), QueuedAnnouncements.Num(), bTimerActive ? 1 : 0,
			*LookupTrace.CacheState, *LookupTrace.ExplicitListMatch, *LookupTrace.PackageName,
			*LookupTrace.ObjectPath, LookupTrace.bPackageExists ? 1 : 0,
			AnnouncementComp != nullptr && AnnouncementComp->IsPlaying() ? 1 : 0,
			AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->Sound) : TEXT("None"));
	}

	if (CurrentAnnouncement.MessageClass != nullptr
		&& ShouldInterruptAnnouncement(NewAnnouncement, CurrentAnnouncement))
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-INTERRUPT incoming=%s:%d current=%s queue=%d"),
				*MessageClass->GetName(), Switch, *DescribeAnnouncement(CurrentAnnouncement),
				QueuedAnnouncements.Num());
		}
		if (CurrentAnnouncement.MessageClass.GetDefaultObject()->EnableAnnouncerLogging())
		{
			UE_LOG(UT, Warning, TEXT("%s %d immediate interrupting %s %d"),
				*MessageClass->GetName(), Switch, *CurrentAnnouncement.MessageClass->GetName(),
				CurrentAnnouncement.Switch);
		}

		for (int32 Index = QueuedAnnouncements.Num() - 1; Index >= 0; --Index)
		{
			if (ShouldInterruptAnnouncement(NewAnnouncement, QueuedAnnouncements[Index]))
			{
				if (AnnouncerTraceEnabled())
				{
					UE_LOG(LogNCPlusAnnouncer, Warning,
						TEXT("[AnnouncerTrace] ADMIT-REMOVE incoming=%s:%d queued=%s reason=interrupt-current-branch"),
						*MessageClass->GetName(), Switch,
						*DescribeAnnouncement(QueuedAnnouncements[Index]));
				}
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
	if (IsGuaranteedFlagTaken(NewAnnouncement))
	{
		// Bypass every CancelByAnnouncement gate (see CVarFlagTakenGuarantee):
		// the line always queues and the queue always drains, so it always
		// plays. Dedup: an identical line already QUEUED is the guarantee —
		// drop this copy, one pending call suffices. Key includes PS1 because
		// the switch-4 sound depends on it ("YouHaveTheFlag_02" vs
		// "Red/BlueFlagTaken") — a teammate's regrab line must not be eaten by
		// the local player's own pending line. Deliberately NOT deduped against
		// CurrentAnnouncement: two audible taken lines are two real grab
		// events, and when the intervening dropped/Denied line was lost the
		// second line is the ONLY signal of the regrab (2026-08-06 review,
		// finding 4 verification).
		for (int32 Index = 0; Index < QueuedAnnouncements.Num(); ++Index)
		{
			if (QueuedAnnouncements[Index].MessageClass == NewAnnouncement.MessageClass
				&& QueuedAnnouncements[Index].Switch == NewAnnouncement.Switch
				&& QueuedAnnouncements[Index].OptionalObject == NewAnnouncement.OptionalObject
				&& QueuedAnnouncements[Index].RelatedPlayerState_1 == NewAnnouncement.RelatedPlayerState_1)
			{
				bCancelThisAnnouncement = true;
				break;
			}
		}
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] %s msg=%s:%d current=%s queue=%d"),
				bCancelThisAnnouncement ? TEXT("ADMIT-DROP reason=guaranteed-dup-queued")
					: TEXT("ADMIT-GUARANTEE"),
				*MessageClass->GetName(), Switch,
				*DescribeAnnouncement(CurrentAnnouncement), QueuedAnnouncements.Num());
		}
	}
	else if (CurrentAnnouncement.MessageClass != nullptr
		&& Message->CancelByAnnouncement(Switch, OptionalObject,
			CurrentAnnouncement.MessageClass, CurrentAnnouncement.Switch,
			CurrentAnnouncement.OptionalObject))
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-CANCEL incoming=%s:%d by-current=%s"),
				*MessageClass->GetName(), Switch, *DescribeAnnouncement(CurrentAnnouncement));
		}
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
			if (ShouldInterruptAnnouncement(NewAnnouncement, QueuedAnnouncements[Index]))
			{
				if (AnnouncerTraceEnabled())
				{
					UE_LOG(LogNCPlusAnnouncer, Warning,
						TEXT("[AnnouncerTrace] ADMIT-REMOVE incoming=%s:%d queued=%s reason=interrupt"),
						*MessageClass->GetName(), Switch,
						*DescribeAnnouncement(QueuedAnnouncements[Index]));
				}
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
				if (AnnouncerTraceEnabled())
				{
					UE_LOG(LogNCPlusAnnouncer, Warning,
						TEXT("[AnnouncerTrace] ADMIT-CANCEL incoming=%s:%d by-queued=%s"),
						*MessageClass->GetName(), Switch,
						*DescribeAnnouncement(QueuedAnnouncements[Index]));
				}
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
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-DROP reason=cancelled msg=%s:%d"),
				*MessageClass->GetName(), Switch);
		}
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

	const bool bComponentPlaying = AnnouncementComp != nullptr && AnnouncementComp->IsPlaying();
	const bool bStartNow = CurrentAnnouncement.MessageClass == nullptr && AnnouncementComp != nullptr
		&& !bComponentPlaying
		&& (bLegacyImmediateReward
			|| !GetWorld()->GetTimerManager().IsTimerActive(PlayNextAnnouncementHandle));
	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] ADMIT-QUEUED msg=%s:%d insert=%d queue=%d startNow=%d current=%s compPlaying=%d"),
			*MessageClass->GetName(), Switch, InsertIndex, QueuedAnnouncements.Num(), bStartNow ? 1 : 0,
			*DescribeAnnouncement(CurrentAnnouncement), bComponentPlaying ? 1 : 0);
	}
	if (bStartNow)
	{
		StartNextAnnouncement(false);
		if (AnnouncerTraceEnabled())
		{
			const FString CacheAfter = DescribeCacheState(
				this, Message->bIsStatusAnnouncement, SoundName);
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] ADMIT-DISPATCHED msg=%s:%d sound=%s cache=%s current=%s queue=%d compPlaying=%d compSound=%s"),
				*MessageClass->GetName(), Switch, *SoundName.ToString(), *CacheAfter,
				*DescribeAnnouncement(CurrentAnnouncement), QueuedAnnouncements.Num(),
				AnnouncementComp != nullptr && AnnouncementComp->IsPlaying() ? 1 : 0,
				AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->Sound) : TEXT("None"));
		}
	}
}

void UNCPlusAnnouncer::PlayNextAnnouncement()
{
	if (!AnnouncerTraceEnabled())
	{
		Super::PlayNextAnnouncement();
		return;
	}

	const bool bHadQueued = QueuedAnnouncements.Num() > 0;
	const FAnnouncementInfo TracedAnnouncement = bHadQueued
		? QueuedAnnouncements[0] : FAnnouncementInfo();

	const float QueueAge = bHadQueued && GetWorld() != nullptr
		? GetWorld()->GetTimeSeconds() - TracedAnnouncement.QueueTime : -1.f;
	UE_LOG(LogNCPlusAnnouncer, Warning,
		TEXT("[AnnouncerTrace] PLAYNEXT-BEFORE front=%s age=%.3f queue=%d current=%s timer=%d rewardCache=%d statusCache=%d saving=%d gc=%d compPlaying=%d compSound=%s volume=%.3f soundClass=%s"),
		*DescribeAnnouncement(TracedAnnouncement), QueueAge, QueuedAnnouncements.Num(),
		*DescribeAnnouncement(CurrentAnnouncement),
		GetWorld() != nullptr && GetWorld()->GetTimerManager().IsTimerActive(PlayNextAnnouncementHandle) ? 1 : 0,
		RewardCachedAudio.Num(), StatusCachedAudio.Num(),
		GIsSavingPackage ? 1 : 0, IsGarbageCollecting() ? 1 : 0,
		AnnouncementComp != nullptr && AnnouncementComp->IsPlaying() ? 1 : 0,
		AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->Sound) : TEXT("None"),
		AnnouncementComp != nullptr ? AnnouncementComp->VolumeMultiplier : -1.f,
		AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->SoundClassOverride) : TEXT("None"));

	Super::PlayNextAnnouncement();

	UE_LOG(LogNCPlusAnnouncer, Warning,
		TEXT("[AnnouncerTrace] PLAYNEXT-AFTER traced=%s current=%s queue=%d timer=%d rewardCache=%d statusCache=%d compPlaying=%d compSound=%s volume=%.3f soundClass=%s"),
		*DescribeAnnouncement(TracedAnnouncement), *DescribeAnnouncement(CurrentAnnouncement),
		QueuedAnnouncements.Num(),
		GetWorld() != nullptr && GetWorld()->GetTimerManager().IsTimerActive(PlayNextAnnouncementHandle) ? 1 : 0,
		RewardCachedAudio.Num(), StatusCachedAudio.Num(),
		AnnouncementComp != nullptr && AnnouncementComp->IsPlaying() ? 1 : 0,
		AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->Sound) : TEXT("None"),
		AnnouncementComp != nullptr ? AnnouncementComp->VolumeMultiplier : -1.f,
		AnnouncementComp != nullptr ? *DescribeObject(AnnouncementComp->SoundClassOverride) : TEXT("None"));
}

void NCPlusAnnouncerPacks::Install()
{
	if (bAnnouncerInstalled)
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning, TEXT("[AnnouncerTrace] INSTALL-SKIP reason=already-installed"));
		}
		return;
	}

	AUTPlayerController* PlayerControllerCDO = GetMutableDefault<AUTPlayerController>();
	if (PlayerControllerCDO == nullptr)
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] INSTALL-DROP reason=null-pc-cdo"));
		}
		return;
	}

	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] INSTALL-BEGIN pcCDO=%s path=%s"),
			*DescribeObject(PlayerControllerCDO), *PlayerControllerCDO->AnnouncerPath.ToString());
	}
	UClass* CurrentAnnouncerClass = PlayerControllerCDO->AnnouncerPath.IsValid()
		? PlayerControllerCDO->AnnouncerPath.TryLoadClass<UUTAnnouncer>() : nullptr;
	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] INSTALL-CURRENT class=%s"), *DescribeObject(CurrentAnnouncerClass));
	}

	FString RecoverySource;
	FStringClassReference ResolvedOriginalPath;
	if (!ResolveOriginalAnnouncerPath(PlayerControllerCDO->AnnouncerPath,
		CurrentAnnouncerClass, ResolvedOriginalPath, RecoverySource))
	{
		if (AnnouncerTraceEnabled())
		{
			UE_LOG(LogNCPlusAnnouncer, Warning,
				TEXT("[AnnouncerTrace] INSTALL-DROP reason=no-valid-base path=%s class=%s"),
				*PlayerControllerCDO->AnnouncerPath.ToString(),
				*DescribeObject(CurrentAnnouncerClass));
		}
		UE_LOG(LogLoad, Warning,
			TEXT("netcodeplus: could not resolve a valid non-NetcodePlus announcer from %s; leaving announcer configuration unchanged"),
			*PlayerControllerCDO->AnnouncerPath.ToString());
		return;
	}

	OriginalAnnouncerPath = ResolvedOriginalPath;
	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] INSTALL-ORIGINAL path=%s source=%s"),
			*OriginalAnnouncerPath.ToString(), *RecoverySource);
	}
	if (GetStockDefaults() == nullptr)
	{
		UE_LOG(LogLoad, Warning, TEXT("netcodeplus: could not load configured stock announcer %s"),
			*OriginalAnnouncerPath.ToString());
		OriginalAnnouncerPath = FStringClassReference();
		return;
	}

	// Keep the recovery record and the platform Game.ini clean before installing
	// the runtime-only CDO override. Do not call SaveConfig() or ReloadConfig().
	PersistOriginalAnnouncerPath();
	RepairPlayerControllerAnnouncerConfig(true);

	FString ConfiguredId;
	if (GConfig == nullptr
		|| !GConfig->GetString(ConfigSection, ConfigKey, ConfiguredId, GetConfigPath()))
	{
		ConfiguredId = StockPackId;
	}
	ApplyPack(ConfiguredId, nullptr);

	PlayerControllerCDO->AnnouncerPath = FStringClassReference(UNCPlusAnnouncer::StaticClass());
	bAnnouncerInstalled = true;

	// Catch PlayerController SUBCLASSES whose own AnnouncerPath default bypasses
	// the base-CDO override above (Elim/Wipeout BP controllers). ~2Hz, client-only.
	if (!AnnouncerUpgradeTickerHandle.IsValid())
	{
		AnnouncerUpgradeTickerHandle = FTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&TickUpgradeLocalAnnouncers), 0.5f);
	}
	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] INSTALL-DONE configured=%s path=%s"),
			*ConfiguredId, *PlayerControllerCDO->AnnouncerPath.ToString());
		TracePackDefaults(TEXT("INSTALL-FINAL-NATIVE-DEFAULTS"), GetDefault<UNCPlusAnnouncer>());
	}
}

void NCPlusAnnouncerPacks::Uninstall()
{
	if (!bAnnouncerInstalled)
	{
		return;
	}

	if (AnnouncerUpgradeTickerHandle.IsValid())
	{
		FTicker::GetCoreTicker().RemoveTicker(AnnouncerUpgradeTickerHandle);
		AnnouncerUpgradeTickerHandle.Reset();
	}

	AUTPlayerController* PlayerControllerCDO = GetMutableDefault<AUTPlayerController>();
	const FStringClassReference NativeAnnouncerPath(UNCPlusAnnouncer::StaticClass());
	bool bRestoredPlayerControllerDefault = false;
	if (PlayerControllerCDO != nullptr
		&& PlayerControllerCDO->AnnouncerPath.ToString() == NativeAnnouncerPath.ToString())
	{
		PlayerControllerCDO->AnnouncerPath = OriginalAnnouncerPath;
		bRestoredPlayerControllerDefault = true;
	}

	if (bRestoredPlayerControllerDefault)
	{
		// A controller SaveConfig() may have written the runtime class during play.
		// Only repair it while we still own the matching CDO override so a later
		// legitimate announcer installer wins.
		RepairPlayerControllerAnnouncerConfig(true);
	}
	if (AnnouncerTraceEnabled())
	{
		UE_LOG(LogNCPlusAnnouncer, Warning,
			TEXT("[AnnouncerTrace] UNINSTALL restoredCDO=%d original=%s finalCDO=%s"),
			bRestoredPlayerControllerDefault ? 1 : 0,
			*OriginalAnnouncerPath.ToString(), PlayerControllerCDO != nullptr
				? *PlayerControllerCDO->AnnouncerPath.ToString() : TEXT("None"));
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
