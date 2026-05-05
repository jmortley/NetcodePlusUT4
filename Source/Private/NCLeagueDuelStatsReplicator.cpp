// NCLeagueDuelStatsReplicator.cpp

#include "NCLeagueDuelStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"

ANCLeagueDuelStatsReplicator::ANCLeagueDuelStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void ANCLeagueDuelStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCLeagueDuelStatsReplicator, StatsEntries);
}

void ANCLeagueDuelStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.f;
}

void ANCLeagueDuelStatsReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Role != ROLE_Authority)
	{
		return;
	}
	TimeSinceLastUpdate += DeltaTime;
	if (TimeSinceLastUpdate >= UpdateInterval)
	{
		TimeSinceLastUpdate = 0.f;
		UpdateFromPlayerStates();
	}
}

void ANCLeagueDuelStatsReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	StatsEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		FNCLeagueDuelStatsEntry Entry;
		// Mirror the bot ID convention used elsewhere (rating system, balancer)
		// so future bot-accuracy displays line up.
		Entry.PlayerId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);

		// Same combined-hitscan formula the scoreboard was already using locally —
		// LG + Shock + Sniper. NCLeagueDuel doesn't really care about flak/rocket
		// accuracy for "skill" display; this matches duel convention.
		const float Hits  = UTPS->GetStatsValue(NAME_LinkHits)
		                  + UTPS->GetStatsValue(NAME_ShockRifleHits)
		                  + UTPS->GetStatsValue(NAME_SniperHits);
		const float Shots = UTPS->GetStatsValue(NAME_LinkShots)
		                  + UTPS->GetStatsValue(NAME_ShockRifleShots)
		                  + UTPS->GetStatsValue(NAME_SniperShots);

		if (Shots > 0.f)
		{
			const float Acc = (Hits / Shots) * 100.f;
			Entry.HitscanAccuracyTimes100 = FMath::RoundToInt(FMath::Clamp(Acc, 0.f, 100.f) * 100.f);
		}

		// Armor pickup counts (server-only StatsData -> replicate per-frame).
		// Stock UT4 increments these on every armor pickup via
		// AUTPickupInventory::GiveTo. uint8 clamp matches the wire format -
		// duel matches won't see more than 255 of any single armor type.
		const auto Clamp255 = [](float V) -> uint8 {
			const int32 R = FMath::RoundToInt(V);
			return uint8(FMath::Clamp(R, 0, 255));
		};
		Entry.BeltCount   = Clamp255(UTPS->GetStatsValue(NAME_ShieldBeltCount));
		Entry.VestCount   = Clamp255(UTPS->GetStatsValue(NAME_ArmorVestCount));
		Entry.PadsCount   = Clamp255(UTPS->GetStatsValue(NAME_ArmorPadsCount));
		Entry.HelmetCount = Clamp255(UTPS->GetStatsValue(NAME_HelmetCount));

		StatsEntries.Add(Entry);
	}
}

float ANCLeagueDuelStatsReplicator::GetAccuracyForPlayer(const FString& UniqueIdStr) const
{
	for (const FNCLeagueDuelStatsEntry& E : StatsEntries)
	{
		if (E.PlayerId == UniqueIdStr)
		{
			return float(E.HitscanAccuracyTimes100) / 100.f;
		}
	}
	return 0.f;
}

namespace
{
	const FNCLeagueDuelStatsEntry* FindEntry(const TArray<FNCLeagueDuelStatsEntry>& Entries, const FString& UniqueIdStr)
	{
		for (const FNCLeagueDuelStatsEntry& E : Entries)
		{
			if (E.PlayerId == UniqueIdStr) return &E;
		}
		return nullptr;
	}
}

uint8 ANCLeagueDuelStatsReplicator::GetBeltCountForPlayer(const FString& Id) const
{
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->BeltCount : 0;
}
uint8 ANCLeagueDuelStatsReplicator::GetVestCountForPlayer(const FString& Id) const
{
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->VestCount : 0;
}
uint8 ANCLeagueDuelStatsReplicator::GetPadsCountForPlayer(const FString& Id) const
{
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->PadsCount : 0;
}
uint8 ANCLeagueDuelStatsReplicator::GetHelmetCountForPlayer(const FString& Id) const
{
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->HelmetCount : 0;
}
