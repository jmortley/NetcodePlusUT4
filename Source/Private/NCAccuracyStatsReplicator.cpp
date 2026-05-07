// NCAccuracyStatsReplicator.cpp - shared per-weapon hits/shots replicator.

#include "NCAccuracyStatsReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"

ANCAccuracyStatsReplicator::ANCAccuracyStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void ANCAccuracyStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCAccuracyStatsReplicator, StatsEntries);
}

void ANCAccuracyStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.f;
}

void ANCAccuracyStatsReplicator::Tick(float DeltaTime)
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

void ANCAccuracyStatsReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	// LinkBeamShots is the per-refire-tick counter UTWeap_LinkGun_Plus increments
	// in beam mode; stock NAME_LinkShots only ticks per trigger pull which the
	// widget intentionally avoids. Mirror NCShaftArenaStatsReplicator's choice.
	static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));

	StatsEntries.Reset();
	StatsEntries.Reserve(GS->PlayerArray.Num());

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		FNCAccuracyStatsEntry E;
		E.PlayerId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);

		E.LinkHits          = UTPS->GetStatsValue(NAME_LinkHits);
		E.LinkShots         = UTPS->GetStatsValue(NAME_LinkBeamShots);
		E.ShockHits         = UTPS->GetStatsValue(NAME_ShockRifleHits);
		E.ShockShots        = UTPS->GetStatsValue(NAME_ShockRifleShots);
		E.RocketHits        = UTPS->GetStatsValue(NAME_RocketHits);
		E.RocketShots       = UTPS->GetStatsValue(NAME_RocketShots);
		E.FlakHits          = UTPS->GetStatsValue(NAME_FlakHits);
		E.FlakShots         = UTPS->GetStatsValue(NAME_FlakShots);
		E.MinigunHits       = UTPS->GetStatsValue(NAME_MinigunHits);
		E.MinigunShots      = UTPS->GetStatsValue(NAME_MinigunShots);
		E.SniperHits        = UTPS->GetStatsValue(NAME_SniperHits);
		E.SniperShots       = UTPS->GetStatsValue(NAME_SniperShots);
		E.LightningHits     = UTPS->GetStatsValue(NAME_LightningRifleHits);
		E.LightningShots    = UTPS->GetStatsValue(NAME_LightningRifleShots);
		E.EnforcerHits      = UTPS->GetStatsValue(NAME_EnforcerHits);
		E.EnforcerShots     = UTPS->GetStatsValue(NAME_EnforcerShots);
		E.BioHits           = UTPS->GetStatsValue(NAME_BioRifleHits);
		E.BioShots          = UTPS->GetStatsValue(NAME_BioRifleShots);
		E.RedeemerHits      = UTPS->GetStatsValue(NAME_RedeemerHits);
		E.RedeemerShots     = UTPS->GetStatsValue(NAME_RedeemerShots);
		E.InstagibHits      = UTPS->GetStatsValue(NAME_InstagibHits);
		E.InstagibShots     = UTPS->GetStatsValue(NAME_InstagibShots);

		StatsEntries.Add(E);
	}
}

namespace
{
	const FNCAccuracyStatsEntry* FindEntry(const TArray<FNCAccuracyStatsEntry>& Entries, const FString& Id)
	{
		for (const FNCAccuracyStatsEntry& E : Entries)
		{
			if (E.PlayerId == Id) return &E;
		}
		return nullptr;
	}

	// Maps a weapon HitsStatsName / ShotsStatsName to the corresponding entry
	// field. Returns INDEX_NONE for stats we don't track. Caller indexes a
	// const int32* array to avoid a switch-per-stat at every widget query.
	int32 HitsFieldOffset(FName StatName, const FNCAccuracyStatsEntry& E)
	{
		// LinkBeamShots is intentionally absent here - it's a Shots stat, not Hits.
		if (StatName == NAME_LinkHits)            return E.LinkHits;
		if (StatName == NAME_ShockRifleHits)      return E.ShockHits;
		if (StatName == NAME_RocketHits)          return E.RocketHits;
		if (StatName == NAME_FlakHits)            return E.FlakHits;
		if (StatName == NAME_MinigunHits)         return E.MinigunHits;
		if (StatName == NAME_SniperHits)          return E.SniperHits;
		if (StatName == NAME_LightningRifleHits)  return E.LightningHits;
		if (StatName == NAME_EnforcerHits)        return E.EnforcerHits;
		if (StatName == NAME_BioRifleHits)        return E.BioHits;
		if (StatName == NAME_RedeemerHits)        return E.RedeemerHits;
		if (StatName == NAME_InstagibHits)        return E.InstagibHits;
		return 0;
	}

	int32 ShotsFieldOffset(FName StatName, const FNCAccuracyStatsEntry& E)
	{
		// Both NAME_LinkShots (stock per-trigger-pull) and NAME_LinkBeamShots
		// (Quake-style per-refire-tick) resolve to the same wire field. The
		// widget's link-special-case at NCPlusHUDWidget_Accuracy.cpp:170 swaps
		// to NAME_LinkBeamShots before querying, but server-stored stat names
		// vary by weapon (UTWeapon::ShotsStatsName), so accept either.
		static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));
		if (StatName == NAME_LinkShots ||
		    StatName == NAME_LinkBeamShots)        return E.LinkShots;
		if (StatName == NAME_ShockRifleShots)      return E.ShockShots;
		if (StatName == NAME_RocketShots)          return E.RocketShots;
		if (StatName == NAME_FlakShots)            return E.FlakShots;
		if (StatName == NAME_MinigunShots)         return E.MinigunShots;
		if (StatName == NAME_SniperShots)          return E.SniperShots;
		if (StatName == NAME_LightningRifleShots)  return E.LightningShots;
		if (StatName == NAME_EnforcerShots)        return E.EnforcerShots;
		if (StatName == NAME_BioRifleShots)        return E.BioShots;
		if (StatName == NAME_RedeemerShots)        return E.RedeemerShots;
		if (StatName == NAME_InstagibShots)        return E.InstagibShots;
		return 0;
	}
}

int32 ANCAccuracyStatsReplicator::GetHitsForPlayer(const FString& PlayerId, FName StatName) const
{
	const FNCAccuracyStatsEntry* E = FindEntry(StatsEntries, PlayerId);
	return E ? HitsFieldOffset(StatName, *E) : 0;
}

int32 ANCAccuracyStatsReplicator::GetShotsForPlayer(const FString& PlayerId, FName StatName) const
{
	const FNCAccuracyStatsEntry* E = FindEntry(StatsEntries, PlayerId);
	return E ? ShotsFieldOffset(StatName, *E) : 0;
}

ANCAccuracyStatsReplicator* ANCAccuracyStatsReplicator::EnsureSpawned(AActor* Owner)
{
	if (!Owner || !Owner->HasAuthority()) return nullptr;
	UWorld* World = Owner->GetWorld();
	if (!World) return nullptr;

	for (TActorIterator<ANCAccuracyStatsReplicator> It(World); It; ++It)
	{
		return *It;
	}

	FActorSpawnParameters SP;
	SP.Owner = Owner;
	return World->SpawnActor<ANCAccuracyStatsReplicator>(SP);
}
