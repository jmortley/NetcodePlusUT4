// CTFStatsReplicator.cpp
#include "CTFStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"

ACTFStatsReplicator::ACTFStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void ACTFStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACTFStatsReplicator, StatsEntries);
}

void ACTFStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.0f;
}

void ACTFStatsReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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

void ACTFStatsReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS)
	{
		return;
	}

	StatsEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || !UTPS->UniqueId.IsValid())
		{
			continue;
		}

		FCTFReplicatedStatsEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.ToString();
		Entry.FlagGrabs = UTPS->GetStatsValue(NAME_FlagGrabs);

		// Auto-detect instagib vs normal hitscan
		int32 InstagibShots = UTPS->GetStatsValue(NAME_InstagibShots);
		if (InstagibShots > 0)
		{
			Entry.HitscanHits = UTPS->GetStatsValue(NAME_InstagibHits);
			Entry.HitscanShots = InstagibShots;
		}
		else
		{
			Entry.HitscanHits = UTPS->GetStatsValue(NAME_LightningRifleHits)
				+ UTPS->GetStatsValue(NAME_SniperHits);
			Entry.HitscanShots = UTPS->GetStatsValue(NAME_LightningRifleShots)
				+ UTPS->GetStatsValue(NAME_SniperShots);
		}

		StatsEntries.Add(Entry);
	}
}

int32 ACTFStatsReplicator::GetGrabsForPlayer(const FString& UniqueIdStr) const
{
	for (const FCTFReplicatedStatsEntry& Entry : StatsEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return Entry.FlagGrabs;
		}
	}
	return 0;
}

void ACTFStatsReplicator::GetAccuracyForPlayer(const FString& UniqueIdStr, int32& OutHits, int32& OutShots) const
{
	for (const FCTFReplicatedStatsEntry& Entry : StatsEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			OutHits = Entry.HitscanHits;
			OutShots = Entry.HitscanShots;
			return;
		}
	}
	OutHits = 0;
	OutShots = 0;
}
