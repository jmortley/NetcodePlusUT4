#include "ShockDomReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "WipeoutDamageReplicator.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"


AShockDomReplicator::AShockDomReplicator(const FObjectInitializer& OI)
	: Super(OI)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = true;
	NetUpdateFrequency = 5.f;
	PrimaryActorTick.bCanEverTick = true;

	DomScoreGoal = 200;
}


void AShockDomReplicator::BeginPlay()
{
	Super::BeginPlay();
}


void AShockDomReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (HasAuthority())
	{
		TimeSinceLastUpdate += DeltaTime;
		if (TimeSinceLastUpdate >= UpdateInterval)
		{
			TimeSinceLastUpdate = 0.f;
			UpdateFromPlayerStates();
		}
	}
}


int32 AShockDomReplicator::GetCapturesForPlayer(const FString& UniqueIdStr) const
{
	for (const FShockDomCaptureEntry& Entry : PlayerCaptureStats)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return Entry.Captures;
		}
	}
	return 0;
}


void AShockDomReplicator::IncrementCaptures(const FString& UniqueIdStr)
{
	for (FShockDomCaptureEntry& Entry : PlayerCaptureStats)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			Entry.Captures++;
			return;
		}
	}
	FShockDomCaptureEntry NewEntry;
	NewEntry.PlayerId = UniqueIdStr;
	NewEntry.Captures = 1;
	PlayerCaptureStats.Add(NewEntry);
}


int32 AShockDomReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
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


void AShockDomReplicator::UpdateFromPlayerStates()
{
	UWorld* World = GetWorld();
	if (!World) return;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>((*It)->PlayerState);
		if (!PS || !PS->UniqueId.IsValid()) continue;

		FString PId = PS->UniqueId.ToString();
		int32 Damage = int32(PS->DamageDone);

		// Find or create entry
		bool bFound = false;
		for (FReplicatedDamageEntry& Entry : DamageEntries)
		{
			if (Entry.PlayerId == PId)
			{
				Entry.DamageDone = Damage;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FReplicatedDamageEntry NewEntry;
			NewEntry.PlayerId = PId;
			NewEntry.DamageDone = Damage;
			DamageEntries.Add(NewEntry);
		}
	}
}


void AShockDomReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AShockDomReplicator, DomScoreGoal);
	DOREPLIFETIME(AShockDomReplicator, PlayerCaptureStats);
	DOREPLIFETIME(AShockDomReplicator, DamageEntries);
}
