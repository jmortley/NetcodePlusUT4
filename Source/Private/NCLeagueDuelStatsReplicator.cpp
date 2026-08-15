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

		// Hitscan accuracy = Sniper + Lightning Gun. The duel weapon set is the Pro+
		// hitscan pair (UTNPSniper writes Sniper*; the LG BP is an AUTPlusSniper reskin
		// that OVERRIDES its stat names to LightningRifle* in Class Defaults) — a player
		// runs one or the other, so the unused weapon's stats are 0 and the sum is the
		// right per-shot ratio. This used to read NAME_LinkHits/NAME_LinkBeamShots (Link
		// Gun BEAM — a shaft-arena weapon nothing in duel fires), so the Acc column sat
		// at 0% in every duel: the exact defect ElimPlus fixed in e3823f5/575d579, never
		// ported here (community report 2026-07-01). Mirrors ElimPlusStatsReplicator.
		static const FName NAME_SniperHits(TEXT("SniperHits"));
		static const FName NAME_SniperShots(TEXT("SniperShots"));
		static const FName NAME_LightningRifleHits(TEXT("LightningRifleHits"));
		static const FName NAME_LightningRifleShots(TEXT("LightningRifleShots"));
		const float Hits  = UTPS->GetStatsValue(NAME_SniperHits)  + UTPS->GetStatsValue(NAME_LightningRifleHits);
		const float Shots = UTPS->GetStatsValue(NAME_SniperShots) + UTPS->GetStatsValue(NAME_LightningRifleShots);

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

		// Damage total — AUTPlayerState::DamageDone is server-only.
		// Read via reflection to match Wipeout/ElimPlus replicators
		// (WipeoutDamageReplicator.cpp:71, ElimPlusStatsReplicator.cpp:88).
		// Direct member access also works in this codebase (ShockDom does it),
		// but reflection is the established pattern for stats replicators here.
		if (UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone")))
		{
			Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
		}

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

bool ANCLeagueDuelStatsReplicator::GetArmorCountsForPlayer(const FString& Id, uint8 OutCounts[4]) const
{
	OutCounts[0] = OutCounts[1] = OutCounts[2] = OutCounts[3] = 0;
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	if (!E)
	{
		return false;
	}

	OutCounts[0] = E->BeltCount;
	OutCounts[1] = E->VestCount;
	OutCounts[2] = E->PadsCount;
	OutCounts[3] = E->HelmetCount;
	return true;
}

int32 ANCLeagueDuelStatsReplicator::GetDamageForPlayer(const FString& Id) const
{
	const FNCLeagueDuelStatsEntry* E = FindEntry(StatsEntries, Id);
	return E ? E->DamageDone : 0;
}
