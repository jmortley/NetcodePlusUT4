#include "NCPlusXTDMReplicator.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "NCPlusForceModels.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Net/UnrealNetwork.h"

namespace
{
	struct FXTDMStateCache
	{
		TWeakObjectPtr<ANCPlusXTDMReplicator> Actor;
		double NextSearchTime = 0.0;
	};
	TMap<TWeakObjectPtr<UWorld>, FXTDMStateCache> XTDMStates;
}

const uint8 ANCPlusXTDMReplicator::NoTeam;

ANCPlusXTDMReplicator::ANCPlusXTDMReplicator(const FObjectInitializer& OI)
	: Super(OI), TeamSize(2), bMatchEnded(false), WinningTeamIndex(NoTeam)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = false;
	NetUpdateFrequency = 5.f;
}

void ANCPlusXTDMReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCPlusXTDMReplicator, TeamSize);
	DOREPLIFETIME(ANCPlusXTDMReplicator, bMatchEnded);
	DOREPLIFETIME(ANCPlusXTDMReplicator, WinningTeamIndex);
	DOREPLIFETIME(ANCPlusXTDMReplicator, Players);
}

void ANCPlusXTDMReplicator::BeginPlay()
{
	Super::BeginPlay();
	XTDMStates.FindOrAdd(GetWorld()).Actor = this;
	if (GetNetMode() != NM_DedicatedServer)
	{
		GetWorldTimerManager().SetTimer(AppearanceReadyTimer, this,
			&ANCPlusXTDMReplicator::ApplyInitialTeamAppearance, 0.1f, true);
		ApplyInitialTeamAppearance();
	}
}

void ANCPlusXTDMReplicator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(AppearanceReadyTimer);
	FXTDMStateCache* Cache = XTDMStates.Find(GetWorld());
	if (Cache && Cache->Actor.Get() == this)
	{
		XTDMStates.Remove(GetWorld());
	}
	Super::EndPlay(EndPlayReason);
}

void ANCPlusXTDMReplicator::ApplyInitialTeamAppearance()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS || GS->Teams.Num() != 4)
	{
		return;
	}
	for (int32 Team = 0; Team < 4; ++Team)
	{
		if (!GS->Teams[Team] || GS->Teams[Team]->TeamIndex != Team)
		{
			return;
		}
	}
	GetWorldTimerManager().ClearTimer(AppearanceReadyTimer);
	NCPlusForceModels::ReapplyAll(GetWorld());
}

ANCPlusXTDMReplicator* ANCPlusXTDMReplicator::Find(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	for (auto It = XTDMStates.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}
	FXTDMStateCache& Cache = XTDMStates.FindOrAdd(World);
	if (Cache.Actor.IsValid())
	{
		return Cache.Actor.Get();
	}
	const double Now = FPlatformTime::Seconds();
	if (Now >= Cache.NextSearchTime)
	{
		Cache.NextSearchTime = Now + 0.25;
		for (TActorIterator<ANCPlusXTDMReplicator> It(World); It; ++It)
		{
			if (!It->IsPendingKillPending())
			{
				Cache.Actor = *It;
				return *It;
			}
		}
	}
	return nullptr;
}

const FNCPlusXTDMPlayerStatus* ANCPlusXTDMReplicator::FindPlayer(const AUTPlayerState* PS) const
{
	for (const FNCPlusXTDMPlayerStatus& Entry : Players)
	{
		if (Entry.PlayerState == PS)
		{
			return &Entry;
		}
	}
	return nullptr;
}

void ANCPlusXTDMReplicator::SetPlayerStatus(AUTPlayerState* PS, bool bAlive, float ReadyTime)
{
	if (Role != ROLE_Authority || !PS || PS->bOnlySpectator)
	{
		return;
	}
	for (FNCPlusXTDMPlayerStatus& Entry : Players)
	{
		if (Entry.PlayerState == PS)
		{
			if (Entry.bAlive != bAlive || Entry.RespawnReadyServerTime != ReadyTime)
			{
				Entry.bAlive = bAlive;
				Entry.RespawnReadyServerTime = ReadyTime;
				ForceNetUpdate();
			}
			return;
		}
	}
	FNCPlusXTDMPlayerStatus Entry;
	Entry.PlayerState = PS;
	Entry.bAlive = bAlive;
	Entry.RespawnReadyServerTime = ReadyTime;
	Players.Add(Entry);
	ForceNetUpdate();
}

void ANCPlusXTDMReplicator::RemovePlayer(AUTPlayerState* PS)
{
	if (Role == ROLE_Authority && Players.RemoveAll([PS](const FNCPlusXTDMPlayerStatus& Entry)
		{ return Entry.PlayerState == PS || !IsValid(Entry.PlayerState); }) > 0)
	{
		ForceNetUpdate();
	}
}

void ANCPlusXTDMReplicator::SetMatchResult(uint8 TeamIndex)
{
	if (Role == ROLE_Authority)
	{
		bMatchEnded = true;
		WinningTeamIndex = TeamIndex < 4 ? TeamIndex : NoTeam;
		ForceNetUpdate();
	}
}
