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

		// Link accuracy (Quake-style): per-beam-tick hit ratio.
		// NAME_LinkHits ticks per damage chunk landed (engine-side, in the
		// FiringBeam state when on target). NAME_LinkBeamShots ticks per
		// ConsumeAmmo call in beam mode (UTWeap_LinkGun_Plus override) — one
		// per refire interval regardless of target. Hits/BeamShots gives the
		// fraction of beam ticks that connected. Stock NAME_LinkShots only
		// ticks on trigger pull, which made sustained beams report ratios
		// >> 100%. Defensive clamp kept as a safety net.
		static const FName NAME_LinkBeamShots(TEXT("LinkBeamShots"));
		const float Hits  = UTPS->GetStatsValue(NAME_LinkHits);
		const float Shots = UTPS->GetStatsValue(NAME_LinkBeamShots);
		if (Shots > 0.f)
		{
			const float Acc = (Hits / Shots) * 100.f;
			Entry.LinkAccuracyTimes100 = FMath::RoundToInt(FMath::Clamp(Acc, 0.f, 100.f) * 100.f);
		}

		// AUTPlayerState::DamageDone is server-only. Read via reflection to
		// match Wipeout/ElimPlus replicators (WipeoutDamageReplicator.cpp:71,
		// ElimPlusStatsReplicator.cpp:88). Direct member access also works
		// (ShockDomReplicator does it), but reflection is the established
		// pattern for stats replicators here.
		if (UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone")))
		{
			Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
		}

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
