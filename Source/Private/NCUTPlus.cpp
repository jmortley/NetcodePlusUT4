// NCUTPlus.cpp — C++ weapon replacement mutator for NetcodePlus
#include "NCUTPlus.h"
#include "UnrealTournament.h"
#include "UTPickupWeapon.h"
#include "UTPickupAmmo.h"
#include "UTGameMode.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "Kismet/GameplayStatics.h"

// NC+ weapon headers (for StaticClass refs in hardcoded map)
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTPlusFlakCannon.h"
#include "UTPlusWeap_RocketLauncher.h"
#include "UTWeap_LinkGun_Plus.h"

// Stock weapon headers (for StaticClass refs)
#include "UTWeap_Sniper.h"
#include "UTWeap_ShockRifle.h"
#include "UTWeap_FlakCannon.h"
#include "UTWeap_RocketLauncher.h"
#include "UTWeap_LinkGun.h"
#include "UTWeap_ImpactHammer.h"
#include "UTWeap_BioRifle.h"
#include "UTWeap_Enforcer.h"
#include "UTWeap_Minigun.h"

ANCUTPlus::ANCUTPlus(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("NCUTPlus", "DisplayName", "NC UTPlus");
	GroupNames.Add(FName(TEXT("Weapons")));

	bReplicates = true;
	bAlwaysRelevant = true;

	// Default configurable replacements (nullptr = no swap)
	LinkGunReplacement = AUTWeap_LinkGun_Plus::StaticClass();
	ImpactHammerReplacement = nullptr;
	BioRifleReplacement = nullptr;
	EnforcerReplacement = nullptr;
	MinigunReplacement = nullptr;
	LGWeaponClass = nullptr; // Must be set in BP subclass defaults
}

// ─── BuildFullReplacementMap ───────────────────────────────────────────

void ANCUTPlus::BuildFullReplacementMap()
{
	FullReplacementMap.Empty();

	// Hardcoded (always active)
	FullReplacementMap.Add(AUTWeap_Sniper::StaticClass(), AUTPlusSniper::StaticClass());
	FullReplacementMap.Add(AUTWeap_ShockRifle::StaticClass(), AUTPlusShockRifle::StaticClass());
	FullReplacementMap.Add(AUTWeap_FlakCannon::StaticClass(), AUTPlusFlakCannon::StaticClass());
	FullReplacementMap.Add(AUTWeap_RocketLauncher::StaticClass(), AUTPlusWeap_RocketLauncher::StaticClass());

	// Configurable (only if set)
	if (LinkGunReplacement)
	{
		FullReplacementMap.Add(AUTWeap_LinkGun::StaticClass(), LinkGunReplacement);
	}
	if (ImpactHammerReplacement)
	{
		FullReplacementMap.Add(AUTWeap_ImpactHammer::StaticClass(), ImpactHammerReplacement);
	}
	if (BioRifleReplacement)
	{
		FullReplacementMap.Add(AUTWeap_BioRifle::StaticClass(), BioRifleReplacement);
	}
	if (EnforcerReplacement)
	{
		FullReplacementMap.Add(AUTWeap_Enforcer::StaticClass(), EnforcerReplacement);
	}
	if (MinigunReplacement)
	{
		FullReplacementMap.Add(AUTWeap_Minigun::StaticClass(), MinigunReplacement);
	}
}

// ─── Helpers ───────────────────────────────────────────────────────────

TSubclassOf<AUTWeapon> ANCUTPlus::GetReplacementFor(UClass* StockWeaponClass) const
{
	if (!StockWeaponClass)
	{
		return nullptr;
	}

	// Exact match first (fastest path)
	const TSubclassOf<AUTWeapon>* Found = FullReplacementMap.Find(StockWeaponClass);
	if (Found)
	{
		return *Found;
	}

	// Blueprint subclass match — map weapons are BP children of the C++ classes
	// e.g., BP_Sniper_C is a child of AUTWeap_Sniper
	for (const auto& Pair : FullReplacementMap)
	{
		if (StockWeaponClass->IsChildOf(Pair.Key))
		{
			return Pair.Value;
		}
	}

	return nullptr;
}

bool ANCUTPlus::IsHitscanWeapon(UClass* WeaponClass) const
{
	if (!WeaponClass)
	{
		return false;
	}
	return WeaponClass->IsChildOf(AUTPlusSniper::StaticClass());
}

EHitscanChoice ANCUTPlus::GetHitscanChoice(AController* C) const
{
	if (!C)
	{
		return EHitscanChoice::Sniper;
	}

	APlayerController* PC = Cast<APlayerController>(C);
	if (!PC)
	{
		return EHitscanChoice::Sniper;
	}

	AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
	if (!PS || !PS->UniqueId.IsValid())
	{
		return EHitscanChoice::Sniper;
	}

	const FString UniqueIdStr = PS->UniqueId.ToString();
	const EHitscanChoice* Found = PlayerHitscanChoices.Find(UniqueIdStr);
	return Found ? *Found : EHitscanChoice::Sniper;
}

TSubclassOf<AUTWeapon> ANCUTPlus::GetHitscanWeaponFor(AController* C) const
{
	if (GetHitscanChoice(C) == EHitscanChoice::LG && LGWeaponClass)
	{
		return LGWeaponClass;
	}
	return AUTPlusSniper::StaticClass();
}

// ─── BeginPlay ─────────────────────────────────────────────────────────

void ANCUTPlus::BeginPlay()
{
	Super::BeginPlay();

	BuildFullReplacementMap();

	// Server: replace DefaultInventory in game mode
	if (HasAuthority())
	{
		AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
		if (GameMode)
		{
			for (int32 i = 0; i < GameMode->DefaultInventory.Num(); i++)
			{
				TSubclassOf<AUTWeapon> Replacement = GetReplacementFor(GameMode->DefaultInventory[i]);
				if (Replacement)
				{
					GameMode->DefaultInventory[i] = Replacement;
				}
			}
		}
	}

	// Hitscan choice is now sent from TeamArenaCharacter::BeginPlay,
	// which fires for every player regardless of whether this mutator is loaded.
}

// ─── CheckRelevance ────────────────────────────────────────────────────

bool ANCUTPlus::CheckRelevance_Implementation(AActor* Other)
{
	// Recursion guard (same pattern as UTMutator_WeaponReplacement)
	static bool bRecursionGuard = false;
	if (bRecursionGuard)
	{
		return Super::CheckRelevance_Implementation(Other);
	}
	TGuardValue<bool> Guard(bRecursionGuard, true);

	// Replace weapon pickups
	AUTPickupWeapon* WeaponPickup = Cast<AUTPickupWeapon>(Other);
	if (WeaponPickup)
	{
		TSubclassOf<AUTWeapon> Replacement = GetReplacementFor(WeaponPickup->WeaponType);
		if (Replacement)
		{
			UE_LOG(LogUTGame, Log, TEXT("NCUTPlus: Replacing weapon pickup %s → %s"),
				WeaponPickup->WeaponType ? *WeaponPickup->WeaponType->GetName() : TEXT("null"),
				*Replacement->GetName());
			WeaponPickup->WeaponType = Replacement;
		}
	}
	else
	{
		// Replace ammo pickups — rewrite Ammo.Type to NC weapon class
		AUTPickupAmmo* AmmoPickup = Cast<AUTPickupAmmo>(Other);
		if (AmmoPickup && AmmoPickup->Ammo.Type)
		{
			TSubclassOf<AUTWeapon> Replacement = GetReplacementFor(AmmoPickup->Ammo.Type);
			if (Replacement)
			{
				UE_LOG(LogUTGame, Log, TEXT("NCUTPlus: Rewriting ammo type %s → %s"),
					*AmmoPickup->Ammo.Type->GetName(), *Replacement->GetName());
				AmmoPickup->Ammo.Type = Replacement;
			}
		}
	}

	return Super::CheckRelevance_Implementation(Other);
}

// ─── ModifyPlayer ──────────────────────────────────────────────────────

void ANCUTPlus::ModifyPlayer_Implementation(APawn* Other, bool bIsNewSpawn)
{
	if (bIsNewSpawn)
	{
		AUTCharacter* UTChar = Cast<AUTCharacter>(Other);
		if (UTChar)
		{
			for (int32 i = 0; i < UTChar->DefaultCharacterInventory.Num(); i++)
			{
				UClass* Current = UTChar->DefaultCharacterInventory[i];
				TSubclassOf<AUTWeapon> Replacement = GetReplacementFor(Current);
				if (Replacement)
				{
					// For sniper slot: apply per-player hitscan choice
					if (IsHitscanWeapon(Replacement))
					{
						Replacement = GetHitscanWeaponFor(UTChar->GetController());
					}
					UTChar->DefaultCharacterInventory[i] = Replacement;
				}
				else if (IsHitscanWeapon(Current))
				{
					// Already an NC sniper/LG — still apply per-player choice
					UTChar->DefaultCharacterInventory[i] = GetHitscanWeaponFor(UTChar->GetController());
				}
			}
		}
	}

	Super::ModifyPlayer_Implementation(Other, bIsNewSpawn);
}

// ─── OverridePickupQuery (cross-hitscan ammo) ─────────────────────────

bool ANCUTPlus::OverridePickupQuery_Implementation(APawn* Other, TSubclassOf<AUTInventory> ItemClass, AActor* Pickup, bool& bAllowPickup)
{
	// Handle cross-hitscan ammo: LG player picks up sniper ammo → gets LG ammo, and vice versa
	AUTPickupAmmo* AmmoPickup = Cast<AUTPickupAmmo>(Pickup);
	if (AmmoPickup && IsHitscanWeapon(AmmoPickup->Ammo.Type))
	{
		AUTCharacter* UTChar = Cast<AUTCharacter>(Other);
		if (UTChar)
		{
			TSubclassOf<AUTWeapon> PlayerHitscan = GetHitscanWeaponFor(UTChar->GetController());

			// If the ammo type doesn't match the player's hitscan weapon, manually add ammo
			if (AmmoPickup->Ammo.Type != PlayerHitscan)
			{
				// Find the player's actual hitscan weapon in inventory
				AUTWeapon* HitscanWeapon = nullptr;
				for (TInventoryIterator<AUTWeapon> It(UTChar); It; ++It)
				{
					if (It->GetClass()->IsChildOf(PlayerHitscan))
					{
						HitscanWeapon = *It;
						break;
					}
				}

				if (HitscanWeapon)
				{
					HitscanWeapon->AddAmmo(AmmoPickup->Ammo.Amount);
					// Let the normal pickup logic handle respawn timing etc.
					// but tell it to allow pickup so the respawn timer starts
					bAllowPickup = true;
					return true;
				}
			}
		}
	}

	// Handle cross-hitscan weapon pickup: player picks up dropped hitscan weapon,
	// give them their preferred hitscan instead
	AUTPickupWeapon* WeaponPickup = Cast<AUTPickupWeapon>(Pickup);
	if (WeaponPickup && IsHitscanWeapon(WeaponPickup->WeaponType))
	{
		AUTCharacter* UTChar = Cast<AUTCharacter>(Other);
		if (UTChar)
		{
			TSubclassOf<AUTWeapon> PlayerHitscan = GetHitscanWeaponFor(UTChar->GetController());

			// If pickup type doesn't match player's preference, swap it
			// (e.g., LG player picks up dropped sniper → gets LG)
			if (WeaponPickup->WeaponType != PlayerHitscan)
			{
				WeaponPickup->WeaponType = PlayerHitscan;
			}
		}
	}

	return Super::OverridePickupQuery_Implementation(Other, ItemClass, Pickup, bAllowPickup);
}

// ─── Mutate (hitscan choice command) ──────────────────────────────────

void ANCUTPlus::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	Super::Mutate_Implementation(MutateString, Sender);

	if (!HasAuthority() || !Sender)
	{
		return;
	}

	// hsc_SR = Sniper Rifle, hsc_LG = Lightning Gun
	if (MutateString.Equals(TEXT("hsc_SR"), ESearchCase::IgnoreCase) ||
		MutateString.Equals(TEXT("hsc_LG"), ESearchCase::IgnoreCase))
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Sender->PlayerState);
		if (!PS || !PS->UniqueId.IsValid())
		{
			return;
		}

		FString UniqueIdStr = PS->UniqueId.ToString();
		EHitscanChoice NewChoice = MutateString.Equals(TEXT("hsc_LG"), ESearchCase::IgnoreCase)
			? EHitscanChoice::LG
			: EHitscanChoice::Sniper;

		PlayerHitscanChoices.Add(UniqueIdStr, NewChoice);

		UE_LOG(LogUTGame, Log, TEXT("NCUTPlus: %s set hitscan choice to %s"),
			*PS->PlayerName, NewChoice == EHitscanChoice::LG ? TEXT("LG") : TEXT("Sniper"));
	}

	// `mutate teamskins` / `mutate forcemodels` → open the NetcodePlus (F5) menu
	// on the sending client (Force Models lives there). Onboarding alias for users
	// coming from dc's MutTeamSkins. Multicast → only LocalPC==Sender acts.
	if (MutateString.Equals(TEXT("teamskins"), ESearchCase::IgnoreCase) ||
		MutateString.Equals(TEXT("forcemodels"), ESearchCase::IgnoreCase))
	{
		MulticastOpenNCPlusMenu(Sender);
	}
}

void ANCUTPlus::MulticastOpenNCPlusMenu_Implementation(APlayerController* Sender)
{
	// NetMulticast fires on every client; only the player who typed the mutate
	// command opens the menu. `ncpmenu forcemodels` = the F5 menu's console command
	// (SUTNCPlusMenu) with its optional tab arg → opens straight to the Force Models
	// tab. Same PC->ConsoleCommand channel as the menu's weaponskins/nchud buttons.
	APlayerController* LocalPC = UGameplayStatics::GetPlayerController(this, 0);
	if (LocalPC && LocalPC == Sender)
	{
		LocalPC->ConsoleCommand(TEXT("ncpmenu forcemodels"));
	}
}

// ─── PostPlayerInit ────────────────────────────────────────────────────

void ANCUTPlus::PostPlayerInit_Implementation(AController* C)
{
	Super::PostPlayerInit_Implementation(C);
	// Hitscan choice is auto-sent from client BeginPlay via ServerMutate.
	// This override exists as a hook point for future enhancements.
}

// ─── NotifyLogout ──────────────────────────────────────────────────────

void ANCUTPlus::NotifyLogout_Implementation(AController* C)
{
	// Don't remove — keep the preference so reconnecting players retain their choice.
	// The TMap is keyed by UniqueNetId so it won't grow unbounded per match.
	Super::NotifyLogout_Implementation(C);
}
