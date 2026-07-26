// ClientHitsounds.h
// Native hitsounds for NetcodePlus — a full C++ replacement for dc's
// "Hitsounds v0.6" Blueprint mutator (dc, Scoob, olec, chatouille).
//
// ARCHITECTURE (inherited from dc's BP, which got this right):
//   The mutator replicates (bReplicates + bAlwaysRelevant) and is passed as the
//   localized message's OptionalObject. The server therefore sends only
//   (damage, friendly-flag) and the CLIENT's own replicated copy supplies the
//   cue, volume, pitch and style from that client's own Mod.ini. No preference
//   ever crosses the wire, and the server keeps no per-player sound state —
//   which is also what makes user-supplied custom sound packs work for free.
//
// The preset CATALOG is static and client-side: built-in presets come from a
// hardcoded manifest (cook-safe — the cooker can follow explicit object paths,
// which a bare AssetRegistry class scan cannot), and any UHitsoundPack data
// assets found in the registry are merged on top as user/custom packs.

#pragma once

#include "NetcodePlus.h"   // PCH preamble — required for unity-build PCH bootstrap.
#include "CoreMinimal.h"
#include "UTMutator.h"
#include "Sound/SoundBase.h"
#include "ClientHitsounds.generated.h"

// Forward declarations
class AUTPlayerController;
class AUTPlayerState;
class AUTCharacter;
class UUTLocalMessage;

/**
 * Damage-to-pitch model. Ported from dc's three HitsoundPlayer Blueprints,
 * which implemented this as a strategy pattern behind a BP interface.
 */
UENUM(BlueprintType)
enum class ENCPHitsoundStyle : uint8
{
	/** Continuous octave sweep across the Low/Med/High cues. dc's "Absolute". */
	Absolute	UMETA(DisplayName = "Absolute (continuous pitch)"),
	/** Hyperbolic damage->pitch on a single cue, remapped into the engine clamp. dc's "UTComp". */
	UTComp		UMETA(DisplayName = "UTComp"),
	/** One cue, pitch 1.0, no damage feedback. dc's "Flat Pitch". */
	Flat		UMETA(DisplayName = "Flat pitch")
};

/**
 * Per-style tuning table. dc kept this in a struct returned by his
 * CreateHitsoundPlayer factory; the numbers below are his, except MaxOctave —
 * see FNCPHitsoundPitch::EnginePitchMax.
 */
USTRUCT(BlueprintType)
struct FHitsoundStyleParams
{
	GENERATED_BODY()

	/** Damage at or below which the pitch bottoms out (highest pitch). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	int32 MinDamage;

	/** Damage at or above which the pitch tops out (lowest pitch). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	int32 MaxDamage;

	/** Absolute style: bottom of the octave sweep (cue index + fraction). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	float MinOctave;

	/** Absolute style: top of the octave sweep. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	float MaxOctave;

	/** UTComp style: numerator of the damage hyperbola. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	float DamageMultiplier;

	/** UTComp style: scales the theoretical range used for the engine remap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound|Style")
	float RangeMultiplier;

	FHitsoundStyleParams()
		: MinDamage(1)
		, MaxDamage(190)
		, MinOctave(0.5f)
		, MaxOctave(2.5f)
		, DamageMultiplier(30.0f)
		, RangeMultiplier(1.9f)
	{
	}

	/** dc's per-style defaults. */
	static FHitsoundStyleParams ForStyle(ENCPHitsoundStyle Style);
};

/**
 * Individual hitsound preset: one cue triple plus its user-tunable gain/pitch.
 *
 * The three cues are expected to be pre-rendered TWO OCTAVES apart (dc's are).
 * That is what lets the Absolute style sweep ~6 octaves continuously while
 * every PitchMultiplier it emits stays inside the engine's [0.4, 2.0] clamp.
 */
USTRUCT(BlueprintType)
struct FHitsound
{
	GENERATED_BODY()

	/** Volume multiplier (default 1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	float Volume;

	/** User pitch multiplier. Only the UTComp style reads this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	float Pitch;

	/** Sound for the low end of the damage range (highest damage). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* Low;

	/** Middle cue. The only cue the UTComp and Flat styles use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* Med;

	/** Sound for the high end of the damage range (lowest damage). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* High;

	/** Display name — this is the preset's persistent identity (written to Mod.ini as SoundID). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	FString DisplayName;

	/** True for presets contributed by a user-supplied UHitsoundPack rather than the built-in manifest. */
	UPROPERTY(BlueprintReadOnly, Category = "Hitsound")
	bool bCustom;

	FHitsound()
		: Volume(1.0f)
		, Pitch(1.9f)
		, Low(nullptr)
		, Med(nullptr)
		, High(nullptr)
		, DisplayName(TEXT("Default"))
		, bCustom(false)
	{
	}

	bool HasAnySound() const { return Low != nullptr || Med != nullptr || High != nullptr; }
};

/**
 * Hitsound configuration — one client's personal settings, read from Mod.ini.
 */
USTRUCT(BlueprintType)
struct FHitsoundsConfig
{
	GENERATED_BODY()

	/** Hitsound preset for enemy hits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	FHitsound Enemy;

	/** Hitsound preset for friendly fire hits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	FHitsound Friendly;

	/** Damage-to-pitch model. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	ENCPHitsoundStyle Style;

	/** Play a flat cue when you hit a teammate for ZERO damage (friendly fire off). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	bool bPlayZeroFriendly;

	/** Master volume multiplier applied on top of the preset volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	float UserMultiplier;

	FHitsoundsConfig()
		: Style(ENCPHitsoundStyle::Absolute)
		, bPlayZeroFriendly(false)
		, UserMultiplier(1.0f)
	{
	}
};

/**
 * Pending hitsound, coalesced per (attacker, victim) pair.
 *
 * Shotgun-family damage arrives as one event per pellet; without coalescing a
 * single flak hit fires up to 9 overlapping sounds. Damage SUMS and the
 * timestamp is NOT refreshed, so a burst produces exactly one hitsound whose
 * pitch reflects the whole burst and whose latency is bounded by FlakHitMinAge.
 */
USTRUCT()
struct FFlakHitEvent
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Damage;

	/** World time of the FIRST hit in this burst — deliberately not refreshed on merge. */
	UPROPERTY()
	float WorldTime;

	UPROPERTY()
	AController* CausedBy;

	UPROPERTY()
	APawn* Victim;

	UPROPERTY()
	bool bFriendly;

	FFlakHitEvent()
		: Damage(0)
		, WorldTime(0.0f)
		, CausedBy(nullptr)
		, Victim(nullptr)
		, bFriendly(false)
	{
	}

	FFlakHitEvent(int32 InDamage, float InTime, AController* InCausedBy, APawn* InVictim, bool bInFriendly)
		: Damage(InDamage)
		, WorldTime(InTime)
		, CausedBy(InCausedBy)
		, Victim(InVictim)
		, bFriendly(bInFriendly)
	{
	}
};

/**
 * Resolved playback parameters for one hit.
 */
struct FNCPHitsoundPitch
{
	/** UE4 clamps PlaySound2D's PitchMultiplier to this range (FSoundSource::Update).
	 *  dc's MaxOctave of 2.5 is silently truncated by the engine — we clamp
	 *  explicitly instead so the model's top end is honest. */
	static const float EnginePitchMin;
	static const float EnginePitchMax;

	USoundBase* Sound;
	float Pitch;

	FNCPHitsoundPitch()
		: Sound(nullptr)
		, Pitch(1.0f)
	{
	}
};

/**
 * AClientHitsounds — native hitsounds mutator.
 *
 * Server: observes the damage chain, coalesces pellet spam, and sends one
 * unreliable localized message per hit to every viewer of the attacker
 * (attacker, live spectators, demo recorders).
 * Client: resolves cue + pitch from its OWN config and plays it 2D.
 *
 * Also provides client-side hitsound PREDICTION (no equivalent in dc's BP),
 * driven from AUTWeaponFix so the sound lands at fire time instead of one
 * round-trip later, with the authoritative message deduplicated behind it.
 */
UCLASS(Blueprintable, Meta = (ChildCanTick))
class NETCODEPLUS_API AClientHitsounds : public AUTMutator
{
	GENERATED_BODY()

public:
	AClientHitsounds(const FObjectInitializer& ObjectInitializer);

	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	//~ Begin AUTMutator Interface
	virtual void Init_Implementation(const FString& Options) override;
	virtual void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;
	virtual bool ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
		AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser,
		TSubclassOf<UDamageType> DamageType) override;
	//~ End AUTMutator Interface

	// ============================================================
	// Preset catalog (STATIC — client-side, shared, menu-accessible
	// without a mutator instance)
	// ============================================================

	/** Build the catalog if it has not been built yet. Safe to call repeatedly; no-op on dedicated servers. */
	static void EnsureCatalog();

	/** Force a rebuild (used after a late PAK mount adds custom packs). */
	static void RefreshCatalog();

	/** All presets: built-in manifest entries first (in authored order), then custom packs. */
	static const TArray<FHitsound>& GetCatalog();

	/** True once the built-in manifest has loaded at least one usable preset. */
	static bool IsCatalogReady();

	/** Look a preset up by DisplayName (case-insensitive); falls back to the first catalog entry. */
	static FHitsound FindPreset(const FString& Name);

	// ============================================================
	// Configuration
	// ============================================================

	/** Read this client's configuration from Mod.ini. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	void ReadConfig();

	/** Write this client's configuration to Mod.ini. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	void WriteConfig();

	/** Read one preset section (SoundID / Pitch / Volume). */
	FHitsound ReadConfigSection(const FString& Section, const FHitsound& Default);

	/** Write one preset section. */
	void WriteConfigSection(const FString& Section, const FHitsound& Hitsound);

	/** Read the whole config straight from Mod.ini without needing a mutator instance (menu path). */
	static FHitsoundsConfig LoadConfigFromIni();

	/** Write a config to Mod.ini and push it to a live mutator instance if one exists (menu path). */
	static void SaveConfigToIni(const FHitsoundsConfig& InConfig, UObject* WorldContext);

	/** Current config (read by the message class and the menu). */
	UFUNCTION(BlueprintPure, Category = "Hitsounds")
	const FHitsoundsConfig& GetConfig() const { return Config; }

	/** Replace the live config (menu path — does not write to disk). */
	void SetConfig(const FHitsoundsConfig& InConfig) { Config = InConfig; }

	// ============================================================
	// Menu
	// ============================================================

	/** Open the native hitsounds panel for this client. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Menu")
	void ShowMenu(const FString& Command, APlayerController* InPlayerOwner);

	// ============================================================
	// Damage handling (server)
	// ============================================================

	/** Compensate for damage modifiers that run AFTER the mutator chain (Damage Amp). */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	int32 GetScaledDamage(AController* InstigatorController, int32 BaseDamage);

	/** Filter a damage event and either queue or dispatch it. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void HandleDamage(AController* CausedBy, APawn* Victim, int32 Damage, TSubclassOf<UDamageType> Type);

	/** Send the hit to everyone currently viewing the attacker (attacker, spectators, demo recorders). */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void NotifyDamage(int32 Damage, AController* CausedBy, APawn* Victim, bool bFriendly);

	// ============================================================
	// Pellet coalescing
	// ============================================================

	/** Merge a pellet hit into the pending burst for this (attacker, victim) pair. */
	void AppendFlakQueue(int32 Damage, AController* CausedBy, APawn* Victim, bool bFriendly);

	/** Dispatch every burst that has matured past FlakHitMinAge. */
	void ProcessFlakQueue();

	// ============================================================
	// Playback
	// ============================================================

	/** Resolve cue + pitch for a damage amount under the configured style. */
	FNCPHitsoundPitch ResolvePlayback(const FHitsound& Hitsound, int32 Damage) const;

	/** Static form — used by the menu's live preview, which has no mutator instance. */
	static FNCPHitsoundPitch ResolvePlaybackFor(const FHitsound& Hitsound, int32 Damage, ENCPHitsoundStyle Style);

	/** Play a hit for the local client. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds")
	void PlayHitsound(int32 Damage, bool bIsFriendly);

	/** Menu preview: play one hit at a representative damage with an explicit config. */
	static void PlayPreview(UObject* WorldContext, const FHitsoundsConfig& InConfig, bool bFriendly, int32 Damage);

	/** Legacy preview entry point. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds")
	void PlaySampleHitsound();

	// ============================================================
	// Client-side prediction
	// ============================================================

	/** Play a predicted hitsound at fire time. bFriendly must reflect the target's team. */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Prediction")
	void PlayClientPredictedHitsound(int32 EstimatedDamage, bool bFriendly);

	/** True if the authoritative message should be suppressed because we just predicted one. */
	UFUNCTION(BlueprintPure, Category = "Hitsounds|Prediction")
	bool ShouldSuppressServerHitsound() const;

	// ============================================================
	// Client RPCs
	// ============================================================

	/** Open the menu on the client that typed the mutate command.
	 *  Multicast rather than a Client RPC on purpose: a mutator actor has no
	 *  owning connection, so Client RPCs on it do not route. Each machine
	 *  compares Sender against its own local PC and only the issuer acts —
	 *  and since a client only ever has its OWN PlayerController replicated to
	 *  it, everyone else sees null and drops it. */
	UFUNCTION(NetMulticast, Reliable)
	void MulticastShowHitsoundMenu(APlayerController* Sender);

protected:
	/** LocalMessage class used to deliver hits to clients. */
	UPROPERTY(EditDefaultsOnly, Category = "Hitsounds")
	TSubclassOf<class UUTLocalMessage> HitsoundMessageClass;

	/** This client's configuration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	FHitsoundsConfig Config;

	/** Pending coalesced bursts. */
	UPROPERTY()
	TArray<FFlakHitEvent> FlakHitQueue;

	UPROPERTY()
	FTimerHandle FlakTimer;

	/** How long a burst is held open before it is dispatched. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak")
	float FlakHitMinAge;

	/** Timestamp of the last locally played hitsound (predicted or authoritative). */
	float LastClientHitsoundTime;

	/** Client-side prediction master switch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prediction")
	bool bClientSideHitsoundsEnabled;

	/** How long after a predicted hitsound the authoritative one is suppressed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prediction")
	float ClientHitsoundDedupWindow;

	/** Minimum gap between predicted hitsounds (pellet coalescing on the client). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prediction")
	float ClientHitsoundMinInterval;

	/** True if this damage type arrives one event per pellet and should be coalesced. */
	bool IsPelletDamage(TSubclassOf<UDamageType> DamageType) const;

	/** True if this damage type should never produce a hitsound (hammer block, etc). */
	bool IsIgnoredDamage(TSubclassOf<UDamageType> DamageType) const;

private:
	/** Mod.ini section for the mutator's own (non-preset) keys. */
	static const FString ConfigSection;

	/** Are attacker and victim on the same team? False in FFA (team 255). */
	static bool IsFriendlyFire(AController* Attacker, APawn* Victim);

	/** Is this damage the attacker hurting themselves? */
	static bool IsSelfDamage(AController* Attacker, APawn* Victim);

	/** Play a resolved hit locally. */
	static void PlayResolved(UObject* WorldContext, const FNCPHitsoundPitch& Resolved, float Volume);
};
