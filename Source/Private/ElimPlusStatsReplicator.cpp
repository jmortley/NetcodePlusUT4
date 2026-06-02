// ElimPlusStatsReplicator.cpp

#include "ElimPlusStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"

AElimPlusStatsReplicator::AElimPlusStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void AElimPlusStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AElimPlusStatsReplicator, StatsEntries);
	DOREPLIFETIME(AElimPlusStatsReplicator, bBalanceTeamsActive);
}

void AElimPlusStatsReplicator::SetBalanceTeamsActive(bool bActive)
{
	if (Role != ROLE_Authority) return;
	bBalanceTeamsActive = bActive;
}

void AElimPlusStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.f;
}

void AElimPlusStatsReplicator::Tick(float DeltaTime)
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

void AElimPlusStatsReplicator::UpdateFromPlayerStates()
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
		if (!UTPS || UTPS->bOnlySpectator)
		{
			continue;
		}

		// Bots have invalid UniqueIds — key them with the synthetic "BOT:<name>"
		// shape used everywhere else (rating system, balancer). Lets bot ELOs
		// flow through the same replicator path when bRandomizeBotElo is on.
		FElimPlusStatsEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);

		// PPR(Current) — pulled from server-only side cache populated by gamemode
		// at end-of-round via SetPlayerPPRCurrent.
		if (const float* PPRPtr = PPRCurrentCache.Find(Entry.PlayerId))
		{
			Entry.PPRCurrent = *PPRPtr;
		}

		// DamageDone is server-side on AUTPlayerState but not replicated — pull via reflection
		UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone"));
		if (DmgProp)
		{
			Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
		}

		// ELO: source of truth is FElimPlusRatingSystem on the gamemode. It pushes
		// values via SetPlayerEloAndDelta — but ONLY at HandleMatchHasEnded, not
		// per round. So during a match the cache stays at the snapshotted match-
		// start value (or 1400 baseline for fresh players who haven't loaded yet).
		// We never read AUTPlayerState::TDMRank — defunct in this fork (see
		// feedback_no_epic_mcp_or_tdmrank memory).
		if (const int32* EloPtr = EloCache.Find(Entry.PlayerId))
		{
			Entry.Elo = *EloPtr;
		}
		if (const int32* DeltaPtr = EloDeltaCache.Find(Entry.PlayerId))
		{
			Entry.EloDeltaThisMatch = *DeltaPtr;
		}

		// PPR + EloDeltaThisMatch are populated by the gamemode (next phase).
		// For now they stay at defaults — the LG accuracy is computed below.

		// "LG_Acc" is hitscan accuracy. In instagib it's the instagib rifle;
		// otherwise it's the Sniper / Lightning Gun — the LG is a Blueprint
		// reskin of AUTPlusSniper, so both skins record to the SAME
		// NAME_SniperHits/NAME_SniperShots stats and one read covers either.
		// Auto-detect instagib via NAME_InstagibShots.
		float HitscanShots = 0.f;
		float HitscanHits  = 0.f;
		const float InstagibShots = UTPS->GetStatsValue(NAME_InstagibShots);
		if (InstagibShots > 0.f)
		{
			HitscanShots = InstagibShots;
			HitscanHits  = UTPS->GetStatsValue(NAME_InstagibHits);
		}
		else
		{
			// Non-instagib "LG" = the Lightning Gun, a Blueprint reskin of our
			// AUTPlusSniper (players run the Sniper OR the LG skin — both are
			// AUTPlusSniper). It records single-shot hitscan accuracy to
			// NAME_SniperHits / NAME_SniperShots via ModifyStatsValue
			// (UTPlusSniper.cpp), so this is a clean per-shot hit ratio.
			// BUG FIX: was reading the Link Gun beam (NAME_LinkHits/LinkBeamShots),
			// which stays ~0 in ElimPlus — that's why the column was stuck at 0%.
			HitscanHits  = UTPS->GetStatsValue(NAME_SniperHits);
			HitscanShots = UTPS->GetStatsValue(NAME_SniperShots);
		}
		if (HitscanShots > 0.f)
		{
			const float Acc = (HitscanHits / HitscanShots) * 100.f;
			Entry.LinkGunAccuracyTimes100 = FMath::RoundToInt(FMath::Clamp(Acc, 0.f, 100.f) * 100.f);
		}
		else
		{
			// No shots with the tracked weapon -> sentinel so the scoreboard
			// prints "-" instead of a misleading 0% (matches NCPlusCTFScoreboard).
			Entry.LinkGunAccuracyTimes100 = -1;
		}

		StatsEntries.Add(Entry);
	}
}

const FElimPlusStatsEntry* AElimPlusStatsReplicator::FindEntry(const FString& UniqueIdStr) const
{
	for (const FElimPlusStatsEntry& E : StatsEntries)
	{
		if (E.PlayerId == UniqueIdStr)
		{
			return &E;
		}
	}
	return nullptr;
}

int32 AElimPlusStatsReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->DamageDone : 0;
}

float AElimPlusStatsReplicator::GetPPRCurrentForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->PPRCurrent : 0.f;
}

int32 AElimPlusStatsReplicator::GetEloForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->Elo : 1400;
}

int32 AElimPlusStatsReplicator::GetEloDeltaForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->EloDeltaThisMatch : 0;
}

float AElimPlusStatsReplicator::GetLinkGunAccuracyForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return (E && E->LinkGunAccuracyTimes100 >= 0) ? (static_cast<float>(E->LinkGunAccuracyTimes100) / 100.f) : -1.f;
}

void AElimPlusStatsReplicator::SetPlayerPPRCurrent(const FString& UniqueIdStr, float Value)
{
	if (Role != ROLE_Authority) return;
	PPRCurrentCache.FindOrAdd(UniqueIdStr) = Value;
}

void AElimPlusStatsReplicator::SetPlayerEloAndDelta(const FString& UniqueIdStr, int32 NewElo, int32 DeltaThisMatch)
{
	if (Role != ROLE_Authority) return;
	EloCache.FindOrAdd(UniqueIdStr) = NewElo;
	EloDeltaCache.FindOrAdd(UniqueIdStr) = DeltaThisMatch;
}
