// CTFStatsReplicator.cpp
#include "CTFStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "UTGameMode.h"
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
	DOREPLIFETIME(ACTFStatsReplicator, bIsInstagibMatch);
}

void ACTFStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.0f;

	// Seed bIsInstagibMatch immediately so the scoreboard layout selection is
	// correct from the first frame the replicator exists. Without this, clients
	// see the non-instagib layout (Armors/Amp columns) for up to 0.5s after
	// joining an instagib match, and during warmup the layout was wrong for
	// the entire warmup period because Tick / UpdateFromPlayerStates didn't
	// fire on dedicated servers until the match transitioned to InProgress
	// in the old spawn-at-HandleMatchHasStarted code path.
	if (Role == ROLE_Authority)
	{
		if (AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>())
		{
			bIsInstagibMatch = GM->bIsInstagib;
		}
	}
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

	// Read instagib state directly from the gamemode (server-only). Authoritative
	// from match start — no waiting for the first instagib shot before the
	// scoreboard layout settles.
	if (AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>())
	{
		bIsInstagibMatch = GM->bIsInstagib;
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

		// Auto-detect instagib vs normal hitscan. Instagib mode shows the
		// instagib rifle's accuracy; normal CTF shows the Sniper / Lightning
		// Gun (NAME_SniperHits/Shots — the LG is a Blueprint reskin of
		// AUTPlusSniper, so both skins land here). Shock/rifles are excluded —
		// their accuracy isn't representative of the fights that decide picks.
		// (Was reading the Link Gun beam, whose NAME_LinkBeamShots denominator
		// is only fed by the custom UTWeap_LinkGun_Plus; matches ElimPlus fix.)
		if (bIsInstagibMatch)
		{
			Entry.HitscanHits  = UTPS->GetStatsValue(NAME_InstagibHits);
			Entry.HitscanShots = UTPS->GetStatsValue(NAME_InstagibShots);
		}
		else
		{
			Entry.HitscanHits  = UTPS->GetStatsValue(NAME_SniperHits);
			Entry.HitscanShots = UTPS->GetStatsValue(NAME_SniperShots);
		}

		// Armor pickup counts — clamp to uint8 (255 max). CTF matches don't
		// realistically exceed that for any single armor type.
		const auto Clamp255 = [](float V) -> uint8 {
			return uint8(FMath::Clamp(FMath::RoundToInt(V), 0, 255));
		};
		Entry.BeltCount   = Clamp255(UTPS->GetStatsValue(NAME_ShieldBeltCount));
		Entry.VestCount   = Clamp255(UTPS->GetStatsValue(NAME_ArmorVestCount));
		Entry.PadsCount   = Clamp255(UTPS->GetStatsValue(NAME_ArmorPadsCount));
		Entry.HelmetCount = Clamp255(UTPS->GetStatsValue(NAME_HelmetCount));

		// UDamage / Amp pickup count + total seconds held. NAME_UDamageTime is
		// incremented per-tick by AUTTimedPowerup while held (StatsNameTime
		// path; UDamage CDO sets this to NAME_UDamageTime).
		Entry.AmpCount = Clamp255(UTPS->GetStatsValue(NAME_UDamageCount));
		Entry.AmpTimeS = FMath::RoundToInt(UTPS->GetStatsValue(NAME_UDamageTime));

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

void ACTFStatsReplicator::GetArmorCountsForPlayer(const FString& UniqueIdStr, uint8 OutCounts[4]) const
{
	OutCounts[0] = OutCounts[1] = OutCounts[2] = OutCounts[3] = 0;
	for (const FCTFReplicatedStatsEntry& Entry : StatsEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			OutCounts[0] = Entry.BeltCount;
			OutCounts[1] = Entry.VestCount;
			OutCounts[2] = Entry.PadsCount;
			OutCounts[3] = Entry.HelmetCount;
			return;
		}
	}
}

void ACTFStatsReplicator::GetAmpForPlayer(const FString& UniqueIdStr, uint8& OutCount, int32& OutTimeSeconds) const
{
	OutCount = 0;
	OutTimeSeconds = 0;
	for (const FCTFReplicatedStatsEntry& Entry : StatsEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			OutCount = Entry.AmpCount;
			OutTimeSeconds = Entry.AmpTimeS;
			return;
		}
	}
}
