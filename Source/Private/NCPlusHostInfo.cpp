// NCPlusHostInfo.cpp — see header.
#include "NCPlusHostInfo.h"
#include "UnrealTournament.h"
#include "UTBaseGameMode.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

ANCHostInfo::ANCHostInfo(const FObjectInitializer& OI)
	: Super(OI)
	, HostPS(nullptr)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	// No tick — a 1Hz resolve timer is plenty for a value that changes on
	// join/leave only, and the property only replicates on actual change.
	PrimaryActorTick.bCanEverTick = false;
	NetUpdateFrequency = 1.f;
}

void ANCHostInfo::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ANCHostInfo, HostPS);
}

void ANCHostInfo::BeginPlay()
{
	Super::BeginPlay();

	if (Role == ROLE_Authority)
	{
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().SetTimer(
				ResolveHandle, this, &ANCHostInfo::ResolveHost,
				1.f, /*bLoop*/ true);
			ResolveHost();   // don't wait a second for the first resolve
		}
	}
}

void ANCHostInfo::ResolveHost()
{
	UWorld* W = GetWorld();
	AUTGameState* GS = W ? W->GetGameState<AUTGameState>() : nullptr;
	if (GS == nullptr)
	{
		return;
	}
	// Badge is pre-match only — once live, stop polling and drop out of every
	// client's relevancy set (same cleanup ethos as the version gate).
	if (GS->HasMatchStarted())
	{
		W->GetTimerManager().ClearTimer(ResolveHandle);
		Destroy();
		return;
	}

	// UE4.15: GetAuthGameMode() returns AGameModeBase* — cast to reach GetHostId().
	AUTBaseGameMode* GM = Cast<AUTBaseGameMode>(W->GetAuthGameMode());
	const FString HostId = GM ? GM->GetHostId() : FString();

	AUTPlayerState* Found = nullptr;
	if (!HostId.IsEmpty())
	{
		for (int32 i = 0; i < GS->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(GS->PlayerArray[i]);
			// Mirror the engine host loop's match exactly (UTGameMode.cpp
			// ReadyToStartMatch): id equality, case-insensitive, skip inactive.
			if (PS != nullptr && !PS->bIsInactive && PS->UniqueId.IsValid()
				&& HostId.Equals(PS->UniqueId.ToString(), ESearchCase::IgnoreCase))
			{
				Found = PS;
				break;
			}
		}
	}
	HostPS = Found;   // property comparison makes same-value reassignment free
}

namespace NCPlusHostInfo
{
	void EnsureSpawned(UWorld* World)
	{
		if (World == nullptr || World->GetAuthGameMode() == nullptr)
		{
			return;   // server-only
		}
		for (TActorIterator<ANCHostInfo> It(World); It; ++It)
		{
			return;   // singleton already up for this match
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<ANCHostInfo>(ANCHostInfo::StaticClass(), Params);
	}
}
