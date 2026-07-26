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

DEFINE_LOG_CATEGORY_STATIC(LogClientHitsounds, Log, All);

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
		Preset.Volume = 1.0f;
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

const TArray<FHitsound>& AClientHitsounds::GetCatalog()
{
	EnsureCatalog();
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
	bClientSideHitsoundsEnabled = true;
	ClientHitsoundDedupWindow = 0.25f;
	ClientHitsoundMinInterval = 0.05f;

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
	if (!MutateString.TrimStartAndEnd().Equals(TEXT("hitsounds"), ESearchCase::IgnoreCase))
	{
		return;
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

		bool bShouldReceive = (AttackerPawn != nullptr && PC->GetViewTarget() == AttackerPawn);
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
		LastClientHitsoundTime = GetWorld() ? GetWorld()->GetTimeSeconds() : LastClientHitsoundTime;
		return;
	}

	PlayResolved(this, ResolvePlayback(Preset, Damage), Preset.Volume * Config.UserMultiplier);
	if (UWorld* World = GetWorld())
	{
		LastClientHitsoundTime = World->GetTimeSeconds();
	}
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

	// Team-aware: predicting the ENEMY cue for a teammate you just pinged for
	// zero damage is worse than staying silent.
	if (bFriendly && EstimatedDamage <= 0 && !Config.bPlayZeroFriendly)
	{
		return;
	}

	const FHitsound& Preset = bFriendly ? Config.Friendly : Config.Enemy;
	PlayResolved(this, ResolvePlayback(Preset, EstimatedDamage), Preset.Volume * Config.UserMultiplier);
	LastClientHitsoundTime = Now;
}

bool AClientHitsounds::ShouldSuppressServerHitsound() const
{
	UWorld* World = GetWorld();
	if (World == nullptr || !bClientSideHitsoundsEnabled)
	{
		return false;
	}
	return (World->GetTimeSeconds() - LastClientHitsoundTime) < ClientHitsoundDedupWindow;
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
	const FString Name = DamageType->GetName();
	return Name.Contains(TEXT("Flak")) || Name.Contains(TEXT("Shard"));
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
