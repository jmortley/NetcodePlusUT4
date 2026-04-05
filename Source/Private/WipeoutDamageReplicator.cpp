// WipeoutDamageReplicator.cpp
#include "WipeoutDamageReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
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

		DamageEntries.Add(Entry);
	}
}

int32 AWipeoutDamageReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
{
	for (const FReplicatedDamageEntry& Entry : DamageEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return Entry.DamageDone;
		}
	}
	return 0;
}

int32 AWipeoutDamageReplicator::GetBeltsForPlayer(const FString& UniqueIdStr) const
{
	for (const FReplicatedDamageEntry& Entry : DamageEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return Entry.BeltPickups;
		}
	}
	return 0;
}

int32 AWipeoutDamageReplicator::GetAmpsForPlayer(const FString& UniqueIdStr) const
{
	for (const FReplicatedDamageEntry& Entry : DamageEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return Entry.AmpPickups;
		}
	}
	return 0;
}
