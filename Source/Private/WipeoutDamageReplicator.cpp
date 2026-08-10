// WipeoutDamageReplicator.cpp
#include "WipeoutDamageReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "WipeoutGame.h"
#include "Net/UnrealNetwork.h"

AWipeoutDamageReplicator::AWipeoutDamageReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f; // Don't need per-frame
	NetUpdateFrequency = 2.0f; // 2 updates/sec is plenty for scoreboard display
}

void AWipeoutDamageReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWipeoutDamageReplicator, DamageEntries);
}

void AWipeoutDamageReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.0f;
}

void AWipeoutDamageReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server only: periodically snapshot damage from PlayerStates
	if (Role != ROLE_Authority)
	{
		return;
	}

	TimeSinceLastUpdate += DeltaTime;
	if (TimeSinceLastUpdate >= UpdateInterval)
	{
		TimeSinceLastUpdate = 0.0f;
		UpdateFromPlayerStates();
	}
}

void AWipeoutDamageReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS)
	{
		return;
	}

	// Server-only path, so the authoritative GameMode is reachable here. Null in
	// any non-Wipeout mode that somehow spawns this replicator — healing simply
	// stays 0 rather than failing the whole refresh.
	AUWipeoutGame* WipeoutGame = GetWorld()->GetAuthGameMode<AUWipeoutGame>();

	DamageEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || !UTPS->UniqueId.IsValid())
		{
			continue;
		}

		FReplicatedDamageEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.ToString();

		// DamageDone is tracked server-side on AUTPlayerState but not replicated
		UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone"));
		if (DmgProp)
		{
			Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
		}

		// Belt and Amp pickup counts — server-side stats
		Entry.BeltPickups = UTPS->GetStatsValue(NAME_ShieldBeltCount);
		Entry.AmpPickups = UTPS->GetStatsValue(NAME_UDamageCount);

		// Vest, and Siphon (which reuses the Berserk stat names — see
		// SiphonPowerup.cpp — so no engine-side stat had to be added).
		Entry.VestPickups = UTPS->GetStatsValue(NAME_ArmorVestCount);
		Entry.SiphonPickups = UTPS->GetStatsValue(NAME_BerserkCount);

		// Healing is accumulated on the GameMode, not the PlayerState.
		if (WipeoutGame != nullptr)
		{
			Entry.HealingDone = WipeoutGame->GetHealingDoneForPlayer(UTPS);
		}

		DamageEntries.Add(Entry);
	}
}

const FReplicatedDamageEntry* AWipeoutDamageReplicator::FindEntry(const FString& UniqueIdStr) const
{
	for (const FReplicatedDamageEntry& Entry : DamageEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return &Entry;
		}
	}
	return nullptr;
}

int32 AWipeoutDamageReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->DamageDone : 0;
}

int32 AWipeoutDamageReplicator::GetBeltsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->BeltPickups : 0;
}

int32 AWipeoutDamageReplicator::GetAmpsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->AmpPickups : 0;
}

int32 AWipeoutDamageReplicator::GetVestsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->VestPickups : 0;
}

int32 AWipeoutDamageReplicator::GetSiphonsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->SiphonPickups : 0;
}

int32 AWipeoutDamageReplicator::GetHealingForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->HealingDone : 0;
}
