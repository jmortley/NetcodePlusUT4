// MutHitsounds.h
// C++ Implementation of MutHitsounds Blueprint Mutator
// Compatible with TeamArenaCharacter and NetcodePlus

#pragma once

#include "CoreMinimal.h"
#include "UTMutator.h"
#include "Sound/SoundBase.h"
#include "MutHitsounds.generated.h"

// Forward declarations
class AUTPlayerController;
class AUTPlayerState;
class AUTCharacter;
class UUTLocalMessage;

/**
 * Individual hitsound preset with volume/pitch and damage-scaled sounds
 */
USTRUCT(BlueprintType)
struct FHitsound
{
	GENERATED_BODY()

	/** Volume multiplier (default 1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	float Volume;

	/** Pitch multiplier (default 1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	float Pitch;

	/** Sound for low damage hits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* Low;

	/** Sound for medium damage hits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* Med;

	/** Sound for high damage hits */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	USoundBase* High;

	/** Display name for UI */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsound")
	FString DisplayName;

	FHitsound()
		: Volume(1.0f)
		, Pitch(1.0f)
		, Low(nullptr)
		, Med(nullptr)
		, High(nullptr)
		, DisplayName(TEXT("Default"))
	{
	}
};

/**
 * Hitsound configuration structure
 * Stores settings for enemy/friendly hitsound playback
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

	/** Whether to play hitsound on zero damage (e.g., friendly fire with 0 damage) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	bool bPlayZeroFriendly;

	/** User-defined volume multiplier (applied on top of preset volume) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hitsounds")
	float UserMultiplier;

	FHitsoundsConfig()
		: bPlayZeroFriendly(false)
		, UserMultiplier(1.0f)
	{
	}
};

/**
 * Flak hit event for batching multiple shard hits
 */
USTRUCT()
struct FFlakHitEvent
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Damage;

	UPROPERTY()
	float WorldTime;

	UPROPERTY()
	AController* CausedBy;

	UPROPERTY()
	APawn* Victim;

	FFlakHitEvent()
		: Damage(0)
		, WorldTime(0.0f)
		, CausedBy(nullptr)
		, Victim(nullptr)
	{
	}

	FFlakHitEvent(int32 InDamage, float InTime, AController* InCausedBy, APawn* InVictim)
		: Damage(InDamage)
		, WorldTime(InTime)
		, CausedBy(InCausedBy)
		, Victim(InVictim)
	{
	}
};

/**
 * MutHitsounds - Hitsound Mutator
 *
 * Provides configurable hitsound feedback for damage dealt.
 * Features:
 * - Per-player configuration via INI
 * - Separate sounds for enemy/friendly hits
 * - Flak shard batching to prevent sound spam
 * - Replay spectator support
 * - Menu system for configuration
 */
UCLASS(Blueprintable, Meta = (ChildCanTick))
class NETCODEPLUS_API AMutHitsounds : public AUTMutator
{
	GENERATED_BODY()

public:
	AMutHitsounds(const FObjectInitializer& ObjectInitializer);

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
	// Configuration
	// ============================================================

	/** Read configuration from mod config (uses UT4's mod config system) */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	void ReadConfig();

	/** Write configuration to mod config */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	void WriteConfig();

	/**
	 * Read a single config section into an FHitsound struct
	 * @param Section - Config section name (e.g., "Enemy", "Friendly")
	 * @param Default - Default FHitsound to use if config missing
	 * @return Configured FHitsound
	 */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	FHitsound ReadConfigSection(const FString& Section, const FHitsound& Default);

	/**
	 * Write a single config section from an FHitsound struct
	 * @param Section - Config section name
	 * @param Hitsound - FHitsound to write
	 */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	void WriteConfigSection(const FString& Section, const FHitsound& Hitsound);

	/** Get hitsound preset by display name */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Config")
	FHitsound GetHitsoundByName(const FString& Name);

	/** Build hitsounds array from available presets (override in BP to add custom sounds) */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Hitsounds|Config")
	void BuildHitsounds();

	/** Make a hitsound struct (helper for Blueprint) */
	UFUNCTION(BlueprintPure, Category = "Hitsounds|Config")
	static FHitsound MakeHitsound(float Volume, float Pitch, USoundBase* Low, USoundBase* Med, USoundBase* High, const FString& InDisplayName);

	// ============================================================
	// Menu System
	// ============================================================

	/** Show the hitsounds configuration menu */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Menu")
	void ShowMenu(const FString& Command, APlayerController* InPlayerOwner);

	// ============================================================
	// Damage Handling (Server)
	// ============================================================

	/** Get scaled damage value (can be overridden for damage scaling) */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	int32 GetScaledDamage(AController* InstigatorController, int32 BaseDamage);

	/** Handle damage event and trigger hitsound */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void HandleDamage(AController* CausedBy, APawn* Victim, int32 Damage, TSubclassOf<UDamageType> Type);

	/** Notify damage to attacker and replay spectators */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void NotifyDamage(int32 Damage, AController* CausedBy, APawn* Victim);

	// ============================================================
	// Flak Queue System
	// ============================================================

	/** Add a flak hit to the batching queue */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void AppendFlakQueue(int32 Damage, AController* CausedBy, APawn* Victim);

	/** Process queued flak hits */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Server")
	void ProcessFlakQueue();

	// ============================================================
	// Hitsound Playback
	// ============================================================

	/**
	 * Play hitsound for a specific player
	 * @param Damage - Damage amount (used to select Low/Med/High sound)
	 * @param bIsFriendly - Whether this was friendly fire
	 * @param Type - Damage type
	 * @param HitsoundPlayer - Actor to play sound through (implements interface or uses fallback)
	 */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds")
	void PlayHitsound(int32 Damage, bool bIsFriendly, TSubclassOf<UDamageType> Type, AActor* HitsoundPlayer);

	/** Play a sample hitsound (for menu preview) */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds")
	void PlaySampleHitsound();

	/**
	 * Select appropriate sound from hitsound based on damage
	 * @param Hitsound - The hitsound preset
	 * @param Damage - Damage dealt
	 * @return Sound to play (Low, Med, or High based on thresholds)
	 */
	UFUNCTION(BlueprintPure, Category = "Hitsounds")
	USoundBase* SelectSoundForDamage(const FHitsound& Hitsound, int32 Damage) const;

	/** Get current config (for LocalMessage access) */
	UFUNCTION(BlueprintPure, Category = "Hitsounds")
	const FHitsoundsConfig& GetConfig() const { return Config; }

	// ============================================================
	// Client RPCs
	// ============================================================

	/** Replicate mutate command to all clients */
	UFUNCTION(NetMulticast, Reliable)
	void EventMutateClientSide(const FString& MutateString, APlayerController* Sender);

protected:
	/** LocalMessage class used to send hitsound notifications to clients */
	UPROPERTY(EditDefaultsOnly, Category = "Hitsounds")
	TSubclassOf<class UUTLocalMessage> HitsoundMessageClass;

protected:
	// ============================================================
	// Configuration Properties
	// ============================================================

	/** Current hitsounds configuration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	FHitsoundsConfig Config;

	/** Default hitsounds configuration (set in BuildHitsounds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	FHitsoundsConfig HitsoundsDefaults;

	/** Array of all available hitsound presets */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	TArray<FHitsound> AllHitsounds;

	// ============================================================
	// Damage Thresholds for Low/Med/High sound selection
	// ============================================================

	/** Damage threshold for medium sound (damage >= this uses Med) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	int32 MedDamageThreshold;

	/** Damage threshold for high sound (damage >= this uses High) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Config")
	int32 HighDamageThreshold;

	// ============================================================
	// Flak Batching Properties
	// ============================================================

	/** Queue for batching flak shard hits */
	UPROPERTY()
	TArray<FFlakHitEvent> FlakHitQueue;

	/** Timer handle for flak queue processing */
	UPROPERTY()
	FTimerHandle FlakTimer;

	/** Minimum age (seconds) before flak hits are processed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flak")
	float FlakHitMinAge;

	// ============================================================
	// Damage Type Identification
	// ============================================================

	/** Check if damage type is from flak shards */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Util")
	bool IsFlakDamage(TSubclassOf<UDamageType> DamageType) const;

	/** Check if damage type should play a hitsound */
	UFUNCTION(BlueprintCallable, Category = "Hitsounds|Util")
	bool ShouldPlayHitsound(TSubclassOf<UDamageType> DamageType) const;

private:
	/** Config section name for mod config */
	static const FString ConfigSection;

	/** Helper to check if player is on same team */
	bool IsFriendlyFire(AController* Attacker, APawn* Victim) const;
};
