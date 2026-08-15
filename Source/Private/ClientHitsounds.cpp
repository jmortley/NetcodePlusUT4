// ClientHitsounds.cpp
// Native hitsounds for NetcodePlus. See ClientHitsounds.h for the architecture note.

#include "ClientHitsounds.h"
#include "HitsoundPack.h"
#include "UTHitsoundMessage.h"
#include "UnrealTournament.h"
#include "UTGameMode.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "UTCharacter.h"
#include "UTGameplayStatics.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorIterator
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "AssetRegistryModule.h"
#include "IAssetRegistry.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogClientHitsounds, Log, All);

// Client-side. When on, an authoritative hitsound inside the dedup window still
// plays if it resolves to a DIFFERENT tier than the predicted sound (rejected or
// demoted head claim, blocked headshot, FF scaling) — a wrong confirm gets
// audibly corrected at the cost of an occasional double-blip. 0 = 327 behavior:
// any predicted sound blanket-suppresses the authoritative one for the window.
static TAutoConsoleVariable<int32> CVarHitsoundCorrection(
	TEXT("ncp.HitsoundCorrection"), 1,
	TEXT("1 = authoritative hitsounds that resolve to a different tier than the predicted sound play through the dedup window (audible misprediction correction). 0 = suppress all authoritative sounds inside the window."));

const FString AClientHitsounds::ConfigSection = TEXT("ClientHitsounds");

const float FNCPHitsoundPitch::EnginePitchMin = 0.4f;
const float FNCPHitsoundPitch::EnginePitchMax = 2.0f;

// =========================================================================
// BUILT-IN PRESET MANIFEST
//
// Explicit per-cue asset names, NOT a name built from the folder: dc's naming
// is irregular in two places (the Default folder is "DefaultLow_Cue" with no
// separator, and the FAHH folder's stem is "fahhhhh"), so any algorithmic
// path builder silently produces two dead presets.
//
// Order below is dc's MakeArray pin order, i.e. the combo-box order players
// already know.
//
// Loaded with runtime LoadObject and null-checked per entry — deliberately NOT
// ConstructorHelpers::FObjectFinder, which fatally asserts at CDO construction
// when an asset is missing. A missing preset degrades to "absent from the
// list", never a crash, so a server without dc's content still runs.
// =========================================================================
struct FNCPBuiltinPreset
{
	const TCHAR* DisplayName;
	const TCHAR* Folder;
	const TCHAR* Low;
	const TCHAR* Med;
	const TCHAR* High;
};

static const TCHAR* const HITSOUND_CONTENT_ROOT = TEXT("/Game/Blueprints/Netcode/Hitsounds/dcSounds");
/** Fallback root, so the cues can later ship inside the plugin's own mount without a code change. */
static const TCHAR* const HITSOUND_CONTENT_ROOT_FALLBACK = TEXT("/NetcodePlus/Hitsounds");

static const FNCPBuiltinPreset BUILTIN_HITSOUND_PRESETS[] =
{
	{ TEXT("Default"),         TEXT("Default"),        TEXT("DefaultLow_Cue"),          TEXT("DefaultMid_Cue"),          TEXT("DefaultHigh_Cue") },
	{ TEXT("TTM / Quake"),     TEXT("TTM"),            TEXT("TTM_low_Cue"),             TEXT("TTM_mid_Cue"),             TEXT("TTM_high_Cue") },
	{ TEXT("Quake Champions"), TEXT("QuakeChampions"), TEXT("QuakeChampions_low_Cue"),  TEXT("QuakeChampions_mid_Cue"),  TEXT("QuakeChampions_high_Cue") },
	{ TEXT("Ding"),            TEXT("Ding"),           TEXT("Ding_low_Cue"),            TEXT("Ding_mid_Cue"),            TEXT("Ding_high_Cue") },
	{ TEXT("UTComp Friendly"), TEXT("Friendly"),       TEXT("Friendly_low_Cue"),        TEXT("Friendly_mid_Cue"),        TEXT("Friendly_high_Cue") },
	{ TEXT("Squash"),          TEXT("Squash"),         TEXT("Squash_low_Cue"),          TEXT("Squash_mid_Cue"),          TEXT("Squash_high_Cue") },
	{ TEXT("Fatal1ty"),        TEXT("Woo"),            TEXT("Woo_low_Cue"),             TEXT("Woo_mid_Cue"),             TEXT("Woo_high_Cue") },
	{ TEXT("Waisty"),          TEXT("Waisty"),         TEXT("Waisty_low_Cue"),          TEXT("Waisty_mid_Cue"),          TEXT("Waisty_high_Cue") },
	{ TEXT("JEPA"),            TEXT("JEPA"),           TEXT("JEPA_low_Cue"),            TEXT("JEPA_mid_Cue"),            TEXT("JEPA_high_Cue") },
	{ TEXT("FAHH"),            TEXT("FAHH"),           TEXT("fahhhhh_low_Cue"),         TEXT("fahhhhh_mid_Cue"),         TEXT("fahhhhh_high_Cue") }
};

/** Roots the loaded cues against GC — the catalog is a plain static, not a UPROPERTY. */
class FNCPHitsoundCatalogReferences : public FGCObject
{
public:
	TArray<UObject*> Assets;

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(Assets);
	}
};

static FNCPHitsoundCatalogReferences* HitsoundCatalogReferences = nullptr;
static TArray<FHitsound> HitsoundCatalog;
static bool bHitsoundCatalogBuilt = false;
static bool bHitsoundCatalogReady = false;
static double HitsoundCatalogLastBuildTime = -1.0e30;
static constexpr double EmptyCatalogRetryIntervalSeconds = 30.0;

static USoundBase* LoadHitsoundCue(const TCHAR* Folder, const TCHAR* AssetName)
{
	if (AssetName == nullptr || *AssetName == 0)
	{
		return nullptr;
	}

	const FString Primary = FString::Printf(TEXT("%s/%s/%s.%s"),
		HITSOUND_CONTENT_ROOT, Folder, AssetName, AssetName);
	if (USoundBase* Sound = LoadObject<USoundBase>(nullptr, *Primary))
	{
		return Sound;
	}

	const FString Fallback = FString::Printf(TEXT("%s/%s/%s.%s"),
		HITSOUND_CONTENT_ROOT_FALLBACK, Folder, AssetName, AssetName);
	return LoadObject<USoundBase>(nullptr, *Fallback);
}

void AClientHitsounds::RefreshCatalog()
{
	HitsoundCatalogLastBuildTime = FPlatformTime::Seconds();
	// Sounds are a client-side concern; a headless server never needs the cues
	// (and must not pay a synchronous asset load for them).
	if (IsRunningDedicatedServer())
	{
		bHitsoundCatalogBuilt = true;
		return;
	}

	HitsoundCatalog.Empty();
	bHitsoundCatalogReady = false;

	if (HitsoundCatalogReferences == nullptr)
	{
		HitsoundCatalogReferences = new FNCPHitsoundCatalogReferences();
	}
	HitsoundCatalogReferences->Assets.Empty();

	// --- built-in presets, in authored order ---
	int32 MissingCount = 0;
	for (int32 Index = 0; Index < ARRAY_COUNT(BUILTIN_HITSOUND_PRESETS); ++Index)
	{
		const FNCPBuiltinPreset& Entry = BUILTIN_HITSOUND_PRESETS[Index];

		FHitsound Preset;
		Preset.DisplayName = Entry.DisplayName;
		Preset.Low  = LoadHitsoundCue(Entry.Folder, Entry.Low);
		Preset.Med  = LoadHitsoundCue(Entry.Folder, Entry.Med);
		Preset.High = LoadHitsoundCue(Entry.Folder, Entry.High);
		// 2.0 is dc's CONFIG-READ default, which is what set the out-of-box
		// loudness his users know (his bank literals said 1.0 but every unset
		// slot read back as 2.0). Seeding 1.0 here would migrate dc users at
		// half volume.
		Preset.Volume = 2.0f;
		Preset.Pitch = 1.9f;
		Preset.bCustom = false;

		if (!Preset.HasAnySound())
		{
			++MissingCount;
			UE_LOG(LogClientHitsounds, Warning,
				TEXT("Hitsound preset '%s' has no loadable cues under %s/%s — skipped."),
				Entry.DisplayName, HITSOUND_CONTENT_ROOT, Entry.Folder);
			continue;
		}

		if (Preset.Low)  { HitsoundCatalogReferences->Assets.Add(Preset.Low); }
		if (Preset.Med)  { HitsoundCatalogReferences->Assets.Add(Preset.Med); }
		if (Preset.High) { HitsoundCatalogReferences->Assets.Add(Preset.High); }
		HitsoundCatalog.Add(Preset);
	}

	// --- user/custom packs: any UHitsoundPack data asset in the registry ---
	// This is the extensibility hook: drop a UHitsoundPack into a PAK and it
	// appears in the menu. Because every client resolves its own sound from its
	// own config, a custom pack needs no server support whatsoever.
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> PackAssets;
		AssetRegistryModule.Get().GetAssetsByClass(UHitsoundPack::StaticClass()->GetFName(), PackAssets, true);

		for (const FAssetData& Asset : PackAssets)
		{
			UHitsoundPack* Pack = Cast<UHitsoundPack>(Asset.GetAsset());
			if (Pack == nullptr)
			{
				continue;
			}

			FHitsound Preset;
			Preset.DisplayName = Pack->DisplayName.IsEmpty() ? Pack->GetName() : Pack->DisplayName;
			Preset.Low  = Pack->Low;
			Preset.Med  = Pack->Med;
			Preset.High = Pack->High;
			Preset.Volume = Pack->DefaultVolume;
			Preset.Pitch = Pack->DefaultPitch;
			Preset.bCustom = true;

			if (!Preset.HasAnySound())
			{
				UE_LOG(LogClientHitsounds, Warning,
					TEXT("Custom hitsound pack '%s' has no sounds assigned — skipped."),
					*Preset.DisplayName);
				continue;
			}

			// A custom pack may not fill all three tiers; fill the gaps so every
			// style has something to play at any damage.
			if (Preset.Med == nullptr)  { Preset.Med = Preset.Low ? Preset.Low : Preset.High; }
			if (Preset.Low == nullptr)  { Preset.Low = Preset.Med; }
			if (Preset.High == nullptr) { Preset.High = Preset.Med; }

			// A custom pack wins a name clash with a built-in: that is how a
			// player overrides "Default" with their own cues.
			const int32 Existing = HitsoundCatalog.IndexOfByPredicate(
				[&Preset](const FHitsound& In) { return In.DisplayName.Equals(Preset.DisplayName, ESearchCase::IgnoreCase); });

			if (Preset.Low)  { HitsoundCatalogReferences->Assets.Add(Preset.Low); }
			if (Preset.Med)  { HitsoundCatalogReferences->Assets.Add(Preset.Med); }
			if (Preset.High) { HitsoundCatalogReferences->Assets.Add(Preset.High); }

			if (Existing != INDEX_NONE)
			{
				HitsoundCatalog[Existing] = Preset;
			}
			else
			{
				HitsoundCatalog.Add(Preset);
			}
		}
	}

	bHitsoundCatalogBuilt = true;
	bHitsoundCatalogReady = HitsoundCatalog.Num() > 0;

	UE_LOG(LogClientHitsounds, Log,
		TEXT("Hitsound catalog: %d preset(s) available (%d built-in entr%s missing)."),
		HitsoundCatalog.Num(), MissingCount, (MissingCount == 1) ? TEXT("y") : TEXT("ies"));
}

void AClientHitsounds::EnsureCatalog()
{
	if (!bHitsoundCatalogBuilt)
	{
		RefreshCatalog();
	}
}

void AClientHitsounds::ShutdownCatalog()
{
	HitsoundCatalog.Empty();
	bHitsoundCatalogBuilt = false;
	bHitsoundCatalogReady = false;
	HitsoundCatalogLastBuildTime = -1.0e30;
	if (HitsoundCatalogReferences != nullptr)
	{
		delete HitsoundCatalogReferences;
		HitsoundCatalogReferences = nullptr;
	}
}

const TArray<FHitsound>& AClientHitsounds::GetCatalog()
{
	EnsureCatalog();
	return HitsoundCatalog;
}

const TArray<FHitsound>& AClientHitsounds::GetCatalogForMenu()
{
	// Capture the state before EnsureCatalog(): an empty first-ever build must not
	// immediately repeat the same disk/registry work, while a later menu opening
	// should retain late-mounted-PAK recovery at a bounded retry cadence.
	const bool bRetryPreviouslyEmptyCatalog = bHitsoundCatalogBuilt
		&& !bHitsoundCatalogReady
		&& FPlatformTime::Seconds() - HitsoundCatalogLastBuildTime >= EmptyCatalogRetryIntervalSeconds;
	EnsureCatalog();
	if (bRetryPreviouslyEmptyCatalog)
	{
		RefreshCatalog();
	}
	return HitsoundCatalog;
}

bool AClientHitsounds::IsCatalogReady()
{
	EnsureCatalog();
	return bHitsoundCatalogReady;
}

FHitsound AClientHitsounds::FindPreset(const FString& Name)
{
	EnsureCatalog();
	for (const FHitsound& Preset : HitsoundCatalog)
	{
		if (Preset.DisplayName.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Preset;
		}
	}
	return HitsoundCatalog.Num() > 0 ? HitsoundCatalog[0] : FHitsound();
}

// =========================================================================
// STYLE PARAMETERS (dc's per-style tuning tables)
// =========================================================================
FHitsoundStyleParams FHitsoundStyleParams::ForStyle(ENCPHitsoundStyle Style)
{
	FHitsoundStyleParams Params;
	switch (Style)
	{
	case ENCPHitsoundStyle::UTComp:
		Params.MinDamage = 20;
		Params.MaxDamage = 185;
		Params.DamageMultiplier = 30.0f;
		Params.RangeMultiplier = 1.9f;
		break;

	case ENCPHitsoundStyle::Absolute:
	case ENCPHitsoundStyle::Flat:
	default:
		Params.MinDamage = 1;
		Params.MaxDamage = 190;
		Params.MinOctave = 0.5f;
		Params.MaxOctave = 2.5f;
		break;
	}
	return Params;
}

// =========================================================================
// LIFECYCLE
// =========================================================================
AClientHitsounds::AClientHitsounds(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FlakHitMinAge = 0.035f;

	LastClientHitsoundTime = 0.0f;
	LastPredictedDamage = 0;
	bLastPredictedFriendly = false;
	bClientSideHitsoundsEnabled = true;
	ClientHitsoundDedupWindow = 0.25f;
	ClientHitsoundMinInterval = 0.05f;
	MenuRequestCooldown = 2.0f;

	// The client's own replicated copy is what resolves OptionalObject, so this
	// actor must replicate and always be relevant.
	bReplicates = true;
	bAlwaysRelevant = true;

	HitsoundMessageClass = UUTHitsoundMessage::StaticClass();

	// Mutually exclusive with dc's Blueprint mutator: without a shared group an
	// admin who adds this alongside dcHitsounds gets every hitsound twice.
	GroupNames.Add(FName(TEXT("Hitsounds")));

	DisplayName = NSLOCTEXT("NetcodePlus", "HitsoundsMutatorName", "Hitsounds");
	Author = NSLOCTEXT("NetcodePlus", "HitsoundsMutatorAuthor", "NetcodePlus (after dc, Scoob, olec, chatouille)");
	Description = NSLOCTEXT("NetcodePlus", "HitsoundsMutatorDesc", "Damage-pitched hitsounds, configured per client (F5 > Hitsounds).");
}

void AClientHitsounds::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() != NM_DedicatedServer)
	{
		EnsureCatalog();
		ReadConfig();
	}

	UE_LOG(LogClientHitsounds, Log, TEXT("ClientHitsounds initialized (netmode %d)"), (int32)GetNetMode());
}

void AClientHitsounds::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (FlakTimer.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(FlakTimer);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AClientHitsounds::Init_Implementation(const FString& Options)
{
	Super::Init_Implementation(Options);
}

// =========================================================================
// MUTATE — owner-only menu open
// =========================================================================
void AClientHitsounds::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	Super::Mutate_Implementation(MutateString, Sender);

	// Only react to our own command, and answer the SENDER directly. dc had to
	// reliable-multicast every mutate string to every client and filter it
	// locally; an owner-only client RPC is the same result without the fan-out.
	if (!HasAuthority() || Sender == nullptr)
	{
		return;
	}
	// 4.15 API: TrimStartAndEnd does not exist yet; Trim()/TrimTrailing() are
	// non-const, so work on a copy (multi-word mutate commands can arrive with
	// a leading space — see the setname/ss_detail precedent).
	const FString Command = FString(MutateString).Trim().TrimTrailing();
	if (!Command.Equals(TEXT("hitsounds"), ESearchCase::IgnoreCase))
	{
		return;
	}

	// Throttle per player. The multicast reaches EVERY client, so an unthrottled
	// request is one player's spam amplified across the whole server.
	if (MenuRequestCooldown > 0.f)
	{
		UWorld* World = GetWorld();
		const float Now = World ? World->GetTimeSeconds() : 0.f;
		const TWeakObjectPtr<APlayerController> Key(Sender);

		if (const float* Last = LastMenuRequestTimes.Find(Key))
		{
			if ((Now - *Last) < MenuRequestCooldown)
			{
				UE_LOG(LogClientHitsounds, Verbose,
					TEXT("Throttled 'mutate hitsounds' from %s (%.2fs since last, cooldown %.2fs)"),
					*GetNameSafe(Sender), Now - *Last, MenuRequestCooldown);
				return;
			}
		}

		// Drop entries for players who have since left, so the map stays bounded
		// by the current player count rather than by churn over the match.
		for (auto It = LastMenuRequestTimes.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}

		LastMenuRequestTimes.Add(Key, Now);
	}

	MulticastShowHitsoundMenu(Sender);
}

void AClientHitsounds::MulticastShowHitsoundMenu_Implementation(APlayerController* Sender)
{
	// Only the machine whose local PC IS the sender opens anything. On every
	// other client Sender arrives null (their connection has no reference to
	// another player's controller), so this is self-addressing.
	APlayerController* LocalPC = UGameplayStatics::GetPlayerController(this, 0);
	if (LocalPC != nullptr && LocalPC == Sender)
	{
		ShowMenu(TEXT("hitsounds"), LocalPC);
	}
}

void AClientHitsounds::ShowMenu(const FString& Command, APlayerController* InPlayerOwner)
{
	APlayerController* PC = InPlayerOwner ? InPlayerOwner : UGameplayStatics::GetPlayerController(this, 0);
	if (PC == nullptr)
	{
		return;
	}

	// The panel is a tab of the NetcodePlus menu (F5); ncpmenu's optional first
	// arg picks the opening tab, and it owns the widget lifetime.
	PC->ConsoleCommand(TEXT("ncpmenu hitsounds"), false);
}

// =========================================================================
// SERVER DAMAGE PATH
// =========================================================================
bool AClientHitsounds::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
	AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser,
	TSubclassOf<UDamageType> DamageType)
{
	const bool bResult = Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
	if (!HasAuthority())
	{
		return bResult;
	}

	// Pure observer: Damage and Momentum are passed through untouched.
	HandleDamage(InstigatedBy, Injured, GetScaledDamage(InstigatedBy, Damage), DamageType);
	return bResult;
}

int32 AClientHitsounds::GetScaledDamage(AController* InstigatorController, int32 BaseDamage)
{
	// The mutator chain runs BEFORE AUTCharacter::ModifyDamageCaused, which
	// applies the attacker's DamageScaling (Damage Amp). Without this the
	// hitsound reports un-amped damage and an amped hit sounds identical to a
	// normal one.
	if (InstigatorController != nullptr)
	{
		if (AUTCharacter* AttackerChar = Cast<AUTCharacter>(InstigatorController->GetPawn()))
		{
			return FMath::TruncToInt(AttackerChar->DamageScaling * (float)BaseDamage);
		}
	}
	return BaseDamage;
}

void AClientHitsounds::HandleDamage(AController* CausedBy, APawn* Victim, int32 Damage, TSubclassOf<UDamageType> Type)
{
	if (CausedBy == nullptr || Victim == nullptr)
	{
		return;
	}

	// Self damage (rocket jump, bio splash, own combo) is not a hit on someone
	// else. Without this it plays the ENEMY hitsound in every FFA mode, where
	// both parties are team 255 and the friendly check cannot catch it.
	if (IsSelfDamage(CausedBy, Victim))
	{
		return;
	}

	if (IsIgnoredDamage(Type))
	{
		return;
	}

	const bool bFriendly = IsFriendlyFire(CausedBy, Victim);

	// Zero-damage events only matter for the "you hit a teammate" cue.
	if (Damage <= 0 && !bFriendly)
	{
		return;
	}

	if (IsPelletDamage(Type))
	{
		AppendFlakQueue(Damage, CausedBy, Victim, bFriendly);
		return;
	}

	NotifyDamage(Damage, CausedBy, Victim, bFriendly);
}

void AClientHitsounds::NotifyDamage(int32 Damage, AController* CausedBy, APawn* Victim, bool bFriendly)
{
	if (!HasAuthority() || CausedBy == nullptr || !HitsoundMessageClass)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	APlayerState* AttackerPS = CausedBy->PlayerState;
	APawn* AttackerPawn = CausedBy->GetPawn();
	const int32 Packed = UUTHitsoundMessage::PackSwitch(Damage, bFriendly);

	// Deliver to everyone who is WATCHING the attacker: the attacker themself,
	// anyone spectating them, and demo recorders. dc's version fell back to
	// broadcasting to the whole PlayerArray whenever the attacker's pawn was an
	// unexpected class; this predicate has no such hole.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(*It);
		if (PC == nullptr)
		{
			continue;
		}

		// The attacker's own controller ALWAYS receives — a posthumous hit
		// confirm (your rocket lands after you traded) has AttackerPawn == null
		// and a death-cam view target, and losing that sound breaks trade-kill
		// feedback. Everyone else qualifies by watching the attacker's pawn,
		// or by being a demo recorder.
		bool bShouldReceive = (PC == CausedBy) ||
			(AttackerPawn != nullptr && PC->GetViewTarget() == AttackerPawn);
		if (!bShouldReceive)
		{
			if (AUTPlayerState* WatcherPS = Cast<AUTPlayerState>(PC->PlayerState))
			{
				bShouldReceive = WatcherPS->bIsDemoRecording;
			}
		}

		if (bShouldReceive)
		{
			PC->ClientReceiveLocalizedMessage(HitsoundMessageClass, Packed, AttackerPS, nullptr, this);
		}
	}
}

// =========================================================================
// PELLET COALESCING
// =========================================================================
void AClientHitsounds::AppendFlakQueue(int32 Damage, AController* CausedBy, APawn* Victim, bool bFriendly)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Merge into the burst already open for this (attacker, victim) pair. The
	// timestamp is deliberately NOT refreshed, so a sustained stream still
	// resolves on schedule instead of being held open indefinitely.
	for (FFlakHitEvent& Pending : FlakHitQueue)
	{
		if (Pending.CausedBy == CausedBy && Pending.Victim == Victim)
		{
			Pending.Damage += Damage;
			Pending.bFriendly = bFriendly;
			return;
		}
	}

	FlakHitQueue.Add(FFlakHitEvent(Damage, World->GetTimeSeconds(), CausedBy, Victim, bFriendly));

	if (!FlakTimer.IsValid())
	{
		World->GetTimerManager().SetTimer(FlakTimer, this, &AClientHitsounds::ProcessFlakQueue, 0.01f, true);
	}
}

void AClientHitsounds::ProcessFlakQueue()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();

	// Dispatch every burst that has matured — not one per tick, which would
	// stretch a backlog of N bursts over N*10ms of audible lag.
	for (int32 Index = FlakHitQueue.Num() - 1; Index >= 0; --Index)
	{
		const FFlakHitEvent& Event = FlakHitQueue[Index];
		if ((Now - Event.WorldTime) >= FlakHitMinAge)
		{
			if (Event.CausedBy != nullptr && Event.Victim != nullptr)
			{
				NotifyDamage(Event.Damage, Event.CausedBy, Event.Victim, Event.bFriendly);
			}
			FlakHitQueue.RemoveAt(Index);
		}
	}

	if (FlakHitQueue.Num() == 0)
	{
		World->GetTimerManager().ClearTimer(FlakTimer);
	}
}

// =========================================================================
// PLAYBACK
// =========================================================================
FNCPHitsoundPitch AClientHitsounds::ResolvePlaybackFor(const FHitsound& Hitsound, int32 Damage, ENCPHitsoundStyle Style)
{
	const FHitsoundStyleParams Params = FHitsoundStyleParams::ForStyle(Style);
	FNCPHitsoundPitch Out;

	switch (Style)
	{
	case ENCPHitsoundStyle::Absolute:
	{
		// dc's continuous model. The three cues are pre-rendered TWO OCTAVES
		// apart, so the integer part of N selects the cue and the fractional
		// part sweeps +/-1 octave around it: ~6 octaves of continuous pitch
		// from 3 samples, and every emitted multiplier lands in [0.5, 2.0] —
		// inside the engine clamp without needing to clamp.
		//
		// The (1-t)^4 easing is the part that makes it feel right: pitch moves
		// fast across low damage, where telling 5 from 20 matters, and
		// compresses at the top. More damage = lower pitch.
		const int32 Span = FMath::Max(1, Params.MaxDamage - Params.MinDamage);
		const int32 Clamped = FMath::Clamp(Damage, Params.MinDamage, Params.MaxDamage);
		const float T = (float)(Clamped - Params.MinDamage) / (float)Span;
		const float U = 1.0f - T;
		const float N = Params.MinOctave + (Params.MaxOctave - Params.MinOctave) * (U * U * U * U);

		const int32 CueIndex = FMath::Clamp(FMath::FloorToInt(N), 0, 2);
		const float Frac = N - (float)FMath::FloorToInt(N);

		Out.Sound = (CueIndex == 0) ? Hitsound.Low : (CueIndex == 1) ? Hitsound.Med : Hitsound.High;
		if (Out.Sound == nullptr) { Out.Sound = Hitsound.Med; }

		Out.Pitch = (Frac > 0.5f) ? (Frac * 2.0f) : (Frac + 0.5f);
		break;
	}

	case ENCPHitsoundStyle::UTComp:
	{
		// Hyperbolic damage->pitch on a single cue. The raw curve is then
		// linearly remapped from its own theoretical range into the engine's
		// [0.4, 2.0] window — without that remap the loud end silently
		// flat-lines against the clamp.
		const float D = FMath::Clamp((float)Damage, (float)Params.MinDamage, (float)Params.MaxDamage);
		const float Raw = (Params.DamageMultiplier / FMath::Max(1.0f, D)) * Hitsound.Pitch;

		const float InMin = (Params.DamageMultiplier / FMath::Max(1.0f, (float)Params.MaxDamage)) * Params.RangeMultiplier;
		const float InMax = (Params.DamageMultiplier / FMath::Max(1.0f, (float)Params.MinDamage)) * Params.RangeMultiplier;
		const float Denom = FMath::Max(KINDA_SMALL_NUMBER, InMax - InMin);

		Out.Pitch = ((Raw - InMin) / Denom) * (FNCPHitsoundPitch::EnginePitchMax - FNCPHitsoundPitch::EnginePitchMin)
			+ FNCPHitsoundPitch::EnginePitchMin;
		Out.Sound = Hitsound.Med ? Hitsound.Med : (Hitsound.Low ? Hitsound.Low : Hitsound.High);
		break;
	}

	case ENCPHitsoundStyle::Flat:
	default:
		Out.Sound = Hitsound.Med ? Hitsound.Med : (Hitsound.Low ? Hitsound.Low : Hitsound.High);
		Out.Pitch = 1.0f;
		break;
	}

	Out.Pitch = FMath::Clamp(Out.Pitch, FNCPHitsoundPitch::EnginePitchMin, FNCPHitsoundPitch::EnginePitchMax);
	return Out;
}

FNCPHitsoundPitch AClientHitsounds::ResolvePlayback(const FHitsound& Hitsound, int32 Damage) const
{
	return ResolvePlaybackFor(Hitsound, Damage, Config.Style);
}

void AClientHitsounds::PlayResolved(UObject* WorldContext, const FNCPHitsoundPitch& Resolved, float Volume)
{
	if (Resolved.Sound == nullptr || WorldContext == nullptr)
	{
		return;
	}
	// 2D, non-spatialised, fire-and-forget — the same call dc's player actors make.
	UGameplayStatics::PlaySound2D(WorldContext, Resolved.Sound, Volume, Resolved.Pitch);
}

void AClientHitsounds::PlayHitsound(int32 Damage, bool bIsFriendly)
{
	const FHitsound& Preset = bIsFriendly ? Config.Friendly : Config.Enemy;

	if (Damage <= 0)
	{
		// "You hit a teammate but friendly fire is off." Flat cue on purpose:
		// there is no damage to encode, so pitching it would be meaningless.
		if (!bIsFriendly || !Config.bPlayZeroFriendly)
		{
			return;
		}
		FNCPHitsoundPitch Zero = ResolvePlaybackFor(Preset, 0, ENCPHitsoundStyle::Flat);
		PlayResolved(this, Zero, Preset.Volume * Config.UserMultiplier);
		return;
	}

	// Deliberately does NOT touch LastClientHitsoundTime: that timestamp is
	// armed only by PREDICTED sounds. If authoritative playback armed it too,
	// authoritative sounds would suppress each other through the dedup window —
	// a spectator (who predicts nothing) would hear at most ~4 sounds/second of
	// a minigun stream, and a two-victim flak hit would play once.
	PlayResolved(this, ResolvePlayback(Preset, Damage), Preset.Volume * Config.UserMultiplier);
}

void AClientHitsounds::PlayPreview(UObject* WorldContext, const FHitsoundsConfig& InConfig, bool bFriendly, int32 Damage)
{
	const FHitsound& Preset = bFriendly ? InConfig.Friendly : InConfig.Enemy;
	PlayResolved(WorldContext, ResolvePlaybackFor(Preset, Damage, InConfig.Style), Preset.Volume * InConfig.UserMultiplier);
}

void AClientHitsounds::PlaySampleHitsound()
{
	PlayPreview(this, Config, false, 45);
}

// =========================================================================
// CLIENT-SIDE PREDICTION
// =========================================================================
void AClientHitsounds::PlayClientPredictedHitsound(int32 EstimatedDamage, bool bFriendly)
{
	if (!bClientSideHitsoundsEnabled || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if ((Now - LastClientHitsoundTime) < ClientHitsoundMinInterval)
	{
		return;
	}

	// Friendly targets: never predict a PITCHED sound. EstimatedDamage here is
	// the weapon's configured base damage — but the server applies TeamDamagePct
	// BEFORE the mutator chain, so on a no-FF server the real damage is 0 and a
	// pitched "125 damage" cue would be loud false feedback. The client cannot
	// know the server's FF scaling, so the only honest prediction for a teammate
	// is the flat zero cue (if the user opted into it) or silence — the
	// authoritative message carries the truth either way.
	if (bFriendly)
	{
		if (!Config.bPlayZeroFriendly)
		{
			return;
		}
		FNCPHitsoundPitch Zero = ResolvePlaybackFor(Config.Friendly, 0, ENCPHitsoundStyle::Flat);
		PlayResolved(this, Zero, Config.Friendly.Volume * Config.UserMultiplier);
		LastClientHitsoundTime = Now;
		LastPredictedDamage = 0;
		bLastPredictedFriendly = true;
		return;
	}

	const FHitsound& Preset = Config.Enemy;
	PlayResolved(this, ResolvePlayback(Preset, EstimatedDamage), Preset.Volume * Config.UserMultiplier);
	LastClientHitsoundTime = Now;
	LastPredictedDamage = EstimatedDamage;
	bLastPredictedFriendly = false;
}

bool AClientHitsounds::ShouldSuppressServerHitsound(int32 AuthoritativeDamage, bool bIsFriendly) const
{
	UWorld* World = GetWorld();
	if (World == nullptr || !bClientSideHitsoundsEnabled)
	{
		return false;
	}
	if ((World->GetTimeSeconds() - LastClientHitsoundTime) >= ClientHitsoundDedupWindow)
	{
		return false;
	}
	if (CVarHitsoundCorrection.GetValueOnGameThread() == 0)
	{
		return true;
	}
	// Inside the window: suppress only a true duplicate. If the authoritative
	// pair resolves to a different cue or a clearly different pitch than what
	// was predicted, it carries information the prediction got wrong — play it.
	return PredictionResolvesSameTier(AuthoritativeDamage, bIsFriendly);
}

bool AClientHitsounds::PredictionResolvesSameTier(int32 AuthoritativeDamage, bool bAuthoritativeFriendly) const
{
	// Different preset (enemy vs friendly) is always a different tier. This also
	// covers the cross-victim case: a friendly prediction arming the window must
	// not swallow an authoritative ENEMY confirm from the same volley.
	if (bAuthoritativeFriendly != bLastPredictedFriendly)
	{
		return false;
	}

	const FHitsound& Preset = bAuthoritativeFriendly ? Config.Friendly : Config.Enemy;

	// Mirror EXACTLY what PlayClientPredictedHitsound emitted: friendly
	// predictions are always the flat zero cue, enemy predictions are pitched.
	const FNCPHitsoundPitch Predicted = bLastPredictedFriendly
		? ResolvePlaybackFor(Preset, 0, ENCPHitsoundStyle::Flat)
		: ResolvePlayback(Preset, LastPredictedDamage);
	const FNCPHitsoundPitch Authoritative = ResolvePlayback(Preset, AuthoritativeDamage);

	if (Predicted.Sound != Authoritative.Sound)
	{
		return false;
	}

	// Same cue: call it the same tier while the pitch ratio stays under about
	// a whole tone. Small damage-estimate error (70 predicted vs 65 real) stays
	// suppressed; a demoted head claim (125 -> 70 resolves ~1.5x apart in the
	// Absolute style) plays. Styles that themselves compress the difference
	// (UTComp bottoms out all big hits, Flat encodes nothing) naturally
	// resolve equal here — no correction, because none would be audible.
	const float Hi = FMath::Max(Predicted.Pitch, Authoritative.Pitch);
	const float Lo = FMath::Max(KINDA_SMALL_NUMBER, FMath::Min(Predicted.Pitch, Authoritative.Pitch));
	const float SameTierMaxPitchRatio = 1.10f;
	if ((Hi / Lo) > SameTierMaxPitchRatio)
	{
		return false;
	}

	// Pitch alone cannot separate LARGE hits. The Absolute curve compresses hard at the top:
	// 115 -> 1.0992, 125 -> 1.0560, 190+ -> 1.0000 all sit inside the ratio above, so a flak
	// shell's prediction reads as "the same tier" as an authoritative shock combo or sniper
	// headshot and silently swallows it — two hits, one sound. Where the style actually
	// encodes damage, require the damages to agree as well.
	//
	// This only runs once the pitch test has ALREADY passed, so it is self-limiting to the
	// saturated band. Hits small enough for the curve to still be expressive differ in pitch
	// first and never reach here — which is why server-summed pellets (predicted one, confirmed
	// as a sum) are unaffected: they diverge low on the curve, where pitch is steep.
	//
	// Thresholded, not exact: a few points of estimate error must still count as the same hit
	// (the 70-predicted / 65-real case the ratio comment above describes). 10% clears that and
	// the 115-vs-125 case at 8%, while catching 115-vs-100 at 13%.
	//
	// Only correct when the correction would be AUDIBLE. If the two pitches are already within a
	// hair of each other, playing the authoritative sound adds a second blip that sounds identical
	// to the first and tells the player nothing. Gating on audibility rather than on the style enum
	// covers three cases at once: Flat (encodes no damage, so every pair lands here), Absolute's
	// plateau at D >= MaxDamage (an amped 200 predicted vs a 170 confirm resolves 1.00000 vs
	// 1.00050 — 15% apart in damage, inaudible in pitch), and UTComp's clamps at both ends.
	const float AudiblePitchRatio = 1.01f;
	if ((Hi / Lo) <= AudiblePitchRatio)
	{
		return true;
	}

	// The friendly path is exempt separately: its prediction is always the flat zero cue rather
	// than a damage estimate, so LastPredictedDamage is 0 and a damage comparison is meaningless.
	if (bAuthoritativeFriendly)
	{
		return true;
	}

	const int32 HiDamage = FMath::Max(LastPredictedDamage, AuthoritativeDamage);
	const int32 LoDamage = FMath::Min(LastPredictedDamage, AuthoritativeDamage);
	if (HiDamage <= 0)
	{
		return true;
	}

	const float SameTierMaxDamagePct = 0.10f;
	return (float)(HiDamage - LoDamage) <= SameTierMaxDamagePct * (float)HiDamage;
}

// =========================================================================
// CONFIG  (Mod.ini; key names match dc's BP so existing settings carry over)
// =========================================================================
FHitsound AClientHitsounds::ReadConfigSection(const FString& Section, const FHitsound& Default)
{
	FHitsound Result = Default;

	FString SoundID;
	if (UUTGameplayStatics::GetModConfigString(Section, TEXT("SoundID"), SoundID) && !SoundID.IsEmpty())
	{
		Result = FindPreset(SoundID);
	}

	float Pitch = Result.Pitch;
	float Volume = Result.Volume;
	UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Pitch"), Pitch);
	UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Volume"), Volume);
	Result.Pitch = Pitch;
	Result.Volume = Volume;
	return Result;
}

void AClientHitsounds::WriteConfigSection(const FString& Section, const FHitsound& Hitsound)
{
	UUTGameplayStatics::SetModConfigString(Section, TEXT("SoundID"), Hitsound.DisplayName);
	UUTGameplayStatics::SetModConfigFloat(Section, TEXT("Pitch"), Hitsound.Pitch);
	UUTGameplayStatics::SetModConfigFloat(Section, TEXT("Volume"), Hitsound.Volume);
}

FHitsoundsConfig AClientHitsounds::LoadConfigFromIni()
{
	EnsureCatalog();

	FHitsoundsConfig Out;

	// Seed from the catalog so a fresh install has working sounds: dc's own
	// defaults were "Default" for enemies and "UTComp Friendly" for teammates.
	Out.Enemy = FindPreset(TEXT("Default"));
	Out.Friendly = FindPreset(TEXT("UTComp Friendly"));

	auto ReadSection = [](const FString& Section, FHitsound& InOut)
	{
		FString SoundID;
		if (UUTGameplayStatics::GetModConfigString(Section, TEXT("SoundID"), SoundID) && !SoundID.IsEmpty())
		{
			InOut = AClientHitsounds::FindPreset(SoundID);
		}
		float Pitch = InOut.Pitch;
		float Volume = InOut.Volume;
		UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Pitch"), Pitch);
		UUTGameplayStatics::GetModConfigFloat(Section, TEXT("Volume"), Volume);
		InOut.Pitch = Pitch;
		InOut.Volume = Volume;
	};

	ReadSection(TEXT("Hitsounds.Enemy"), Out.Enemy);
	ReadSection(TEXT("Hitsounds.Friendly"), Out.Friendly);

	// Style lives under OUR section, not dc's [Hitsounds.Enable] Style. dc's
	// enumerator order could not be established with confidence from the asset,
	// and silently applying the wrong pitch model is worse than one re-pick.
	int32 StyleInt = (int32)ENCPHitsoundStyle::Absolute;
	UUTGameplayStatics::GetModConfigInt(ConfigSection, TEXT("Style"), StyleInt);
	Out.Style = (ENCPHitsoundStyle)FMath::Clamp(StyleInt, 0, 2);

	int32 PlayZeroFriendly = 0;
	UUTGameplayStatics::GetModConfigInt(ConfigSection, TEXT("PlayZeroFriendly"), PlayZeroFriendly);
	Out.bPlayZeroFriendly = (PlayZeroFriendly != 0);

	float UserMult = 1.0f;
	UUTGameplayStatics::GetModConfigFloat(ConfigSection, TEXT("UserMultiplier"), UserMult);
	Out.UserMultiplier = UserMult;

	return Out;
}

void AClientHitsounds::SaveConfigToIni(const FHitsoundsConfig& InConfig, UObject* WorldContext)
{
	UUTGameplayStatics::SetModConfigString(TEXT("Hitsounds.Enemy"), TEXT("SoundID"), InConfig.Enemy.DisplayName);
	UUTGameplayStatics::SetModConfigFloat(TEXT("Hitsounds.Enemy"), TEXT("Pitch"), InConfig.Enemy.Pitch);
	UUTGameplayStatics::SetModConfigFloat(TEXT("Hitsounds.Enemy"), TEXT("Volume"), InConfig.Enemy.Volume);

	UUTGameplayStatics::SetModConfigString(TEXT("Hitsounds.Friendly"), TEXT("SoundID"), InConfig.Friendly.DisplayName);
	UUTGameplayStatics::SetModConfigFloat(TEXT("Hitsounds.Friendly"), TEXT("Pitch"), InConfig.Friendly.Pitch);
	UUTGameplayStatics::SetModConfigFloat(TEXT("Hitsounds.Friendly"), TEXT("Volume"), InConfig.Friendly.Volume);

	UUTGameplayStatics::SetModConfigInt(ConfigSection, TEXT("Style"), (int32)InConfig.Style);
	UUTGameplayStatics::SetModConfigInt(ConfigSection, TEXT("PlayZeroFriendly"), InConfig.bPlayZeroFriendly ? 1 : 0);
	UUTGameplayStatics::SetModConfigFloat(ConfigSection, TEXT("UserMultiplier"), InConfig.UserMultiplier);
	UUTGameplayStatics::SaveModConfig();

	// Push to the live mutator instance, if this client is in a match running it.
	if (WorldContext != nullptr)
	{
		if (UWorld* World = WorldContext->GetWorld())
		{
			for (TActorIterator<AClientHitsounds> It(World); It; ++It)
			{
				It->SetConfig(InConfig);
			}
		}
	}
}

void AClientHitsounds::ReadConfig()
{
	Config = LoadConfigFromIni();

	int32 ClientSideHitsounds = 1;
	UUTGameplayStatics::GetModConfigInt(ConfigSection, TEXT("ClientSideHitsounds"), ClientSideHitsounds);
	bClientSideHitsoundsEnabled = (ClientSideHitsounds != 0);
}

void AClientHitsounds::WriteConfig()
{
	SaveConfigToIni(Config, this);
	UUTGameplayStatics::SetModConfigInt(ConfigSection, TEXT("ClientSideHitsounds"), bClientSideHitsoundsEnabled ? 1 : 0);
	UUTGameplayStatics::SaveModConfig();
}

// =========================================================================
// HELPERS
// =========================================================================
bool AClientHitsounds::IsPelletDamage(TSubclassOf<UDamageType> DamageType) const
{
	if (!DamageType)
	{
		return false;
	}
	// Shard damage only (UTDmg_FlakShard, stinger shards). Matching bare "Flak"
	// would also coalesce the flak ALT shell (UTDmg_FlakShell) — a single-hit
	// damage type dc deliberately excluded — adding 35ms of pointless latency
	// to every shell hit.
	return DamageType->GetName().Contains(TEXT("Shard"));
}

bool AClientHitsounds::IsIgnoredDamage(TSubclassOf<UDamageType> DamageType) const
{
	if (!DamageType)
	{
		return false;
	}
	// The impact hammer's block/charge damage is a shield interaction, not a hit.
	return DamageType->GetName().Contains(TEXT("ImpactHammerBlock"));
}

bool AClientHitsounds::IsSelfDamage(AController* Attacker, APawn* Victim)
{
	if (Attacker == nullptr || Victim == nullptr)
	{
		return false;
	}
	if (Attacker->GetPawn() == Victim)
	{
		return true;
	}
	return Victim->GetController() == Attacker;
}

bool AClientHitsounds::IsFriendlyFire(AController* Attacker, APawn* Victim)
{
	if (Attacker == nullptr || Victim == nullptr)
	{
		return false;
	}
	AUTPlayerState* AttackerPS = Cast<AUTPlayerState>(Attacker->PlayerState);
	AController* VictimController = Victim->GetController();
	AUTPlayerState* VictimPS = VictimController ? Cast<AUTPlayerState>(VictimController->PlayerState) : nullptr;
	if (AttackerPS == nullptr || VictimPS == nullptr)
	{
		return false;
	}
	// Team 255 is "no team" (FFA) — everyone would otherwise read as a teammate.
	return AttackerPS->GetTeamNum() == VictimPS->GetTeamNum() && AttackerPS->GetTeamNum() != 255;
}
