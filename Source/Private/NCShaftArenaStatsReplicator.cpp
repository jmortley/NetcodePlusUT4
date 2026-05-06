// NCShaftArenaStatsReplicator.cpp

#include "NCShaftArenaStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"

ANCShaftArenaStatsReplicator::ANCShaftArenaStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void ANCShaftArenaStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCShaftArenaStatsReplicator, StatsEntries);
}

void ANCShaftArenaStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.f;
}

void ANCShaftArenaStatsReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Role != ROLE_Authority) return;
	TimeSinceLastUpdate += DeltaTime;
	if (TimeSinceLastUpdate >= UpdateInterval)
	{
		TimeSinceLastUpdate = 0.f;
		UpdateFromPlayerStates();
	}
}

void ANCShaftArenaStatsReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	StatsEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		FNCShaftArenaStatsEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);

		// Link accuracy (only weapon in shaft arena). Clamp at 100% defensively
		// in case any future weapon-bookkeeping bug inflates Hits.
		const float Hits  = UTPS->GetStatsValue(NAME_LinkHits);
		const float Shots = UTPS->GetStatsValue(NAME_LinkShots);
		if (Shots > 0.f)
		{
			const float Acc = (Hits / Shots) * 100.f;
			Entry.LinkAccuracyTimes100 = FMath::RoundToInt(FMath::Clamp(Acc, 0.f, 100.f) * 100.f);
		}

		Entry.DamageDone = int32(UTPS->DamageDone);

		StatsEntries.Add(Entry);
	}
}

namespace
{
	const FNCShaftArenaStatsEntry* FindEntry(const TArray<FNCShaftArenaStatsEntry>& Entries, const FString& Id)
	{
		for (const FNCShaftArenaStatsEntry& E : Entries)
		{
			if (E.PlayerId == Id) return &E;
		}
		return nullptr;
	}
}

float ANCShaftArenaStatsReplicator::GetAccuracyForPlayer(const FString& Id) const
{
	const FNCShaftArenaStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? float(E->LinkAccuracyTimes100) / 100.f : 0.f;
}

int32 ANCShaftArenaStatsReplicator::GetDamageForPlayer(const FString& Id) const
{
	const FNCShaftArenaStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->DamageDone : 0;
}
