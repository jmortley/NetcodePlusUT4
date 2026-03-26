// NCUTPlus.h — C++ weapon replacement mutator for NetcodePlus
// Replaces stock UT4 weapons with NC+ equivalents, supports per-player hitscan choice (Sniper vs LG)

#pragma once

#include "CoreMinimal.h"
#include "UTMutator.h"
#include "NCUTPlus.generated.h"

// Forward declarations — avoid pulling in full weapon headers
class AUTWeapon;
class AUTCharacter;
class AUTPickupWeapon;
class AUTPickupAmmo;
class AUTInventory;

UENUM(BlueprintType)
enum class EHitscanChoice : uint8
{
	Sniper		UMETA(DisplayName = "Sniper Rifle"),
	LG			UMETA(DisplayName = "Lightning Gun")
};

/**
 * NCUTPlus — NetcodePlus weapon replacement mutator.
 *
 * Hardcoded replacements: Sniper, Shock, Flak, Rocket → NC+ equivalents.
 * Configurable replacements: Link Gun, Impact Hammer, Bio, Enforcer, Minigun via BP dropdown.
 * Per-player hitscan choice: clients select Sniper or LG from weapon settings menu.
 * Ammo compatibility: rewrites stock ammo pickups and handles cross-hitscan ammo via OverridePickupQuery.
 */
UCLASS(Blueprintable, Meta = (ChildCanTick))
class NETCODEPLUS_API ANCUTPlus : public AUTMutator
{
	GENERATED_UCLASS_BODY()

public:

	// ─── Configurable weapon replacements (set in BP subclass defaults) ───

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Replacements")
	TSubclassOf<AUTWeapon> LinkGunReplacement;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Replacements")
	TSubclassOf<AUTWeapon> ImpactHammerReplacement;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Replacements")
	TSubclassOf<AUTWeapon> BioRifleReplacement;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Replacements")
	TSubclassOf<AUTWeapon> EnforcerReplacement;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Replacements")
	TSubclassOf<AUTWeapon> MinigunReplacement;

	// ─── Hitscan choice ───

	/** Lightning Gun weapon class — set this in BP defaults to the LG Blueprint class.
	 *  Must be a subclass of AUTPlusSniper. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "NCUTPlus|Hitscan")
	TSubclassOf<AUTWeapon> LGWeaponClass;

	/** Server-side map of player hitscan preferences, keyed by UniqueNetId */
	UPROPERTY(Transient)
	TMap<FString, EHitscanChoice> PlayerHitscanChoices;

	// ─── Mutator overrides ───

	virtual void BeginPlay() override;

	bool CheckRelevance_Implementation(AActor* Other) override;

	void ModifyPlayer_Implementation(APawn* Other, bool bIsNewSpawn) override;

	void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;

	void NotifyLogout_Implementation(AController* C) override;

	bool OverridePickupQuery_Implementation(APawn* Other, TSubclassOf<AUTInventory> ItemClass, AActor* Pickup, bool& bAllowPickup) override;

	void PostPlayerInit_Implementation(AController* C) override;

protected:

	/** Combined replacement map (hardcoded + configurable), built in BeginPlay */
	UPROPERTY(Transient)
	TMap<UClass*, TSubclassOf<AUTWeapon>> FullReplacementMap;

	/** Look up the NC replacement for a stock weapon class. Returns nullptr if no replacement. */
	TSubclassOf<AUTWeapon> GetReplacementFor(UClass* StockWeaponClass) const;

	/** Get the correct hitscan weapon class for a player (Sniper or LG). */
	TSubclassOf<AUTWeapon> GetHitscanWeaponFor(AController* C) const;

	/** Get the hitscan choice for a controller. */
	EHitscanChoice GetHitscanChoice(AController* C) const;

	/** Build the full replacement map from hardcoded + configurable entries. */
	void BuildFullReplacementMap();

	/** Check if a weapon class is a hitscan type (sniper or LG). */
	bool IsHitscanWeapon(UClass* WeaponClass) const;
};
