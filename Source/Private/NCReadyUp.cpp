// NCReadyUp.cpp - server-authoritative UTComp-style ready-up.
#include "NCReadyUp.h"

#include "UnrealTournament.h"
#include "UTGameMode.h"
#include "UTGameSession.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Net/UnrealNetwork.h"

namespace
{
	TMap<TWeakObjectPtr<UWorld>, bool> GReadyUpEnabled;

	struct FReadyStateCacheEntry
	{
		TWeakObjectPtr<ANCReadyUpState> State;
		double NextSearchTime = 0.0;
	};

	TMap<TWeakObjectPtr<UWorld>, FReadyStateCacheEntry> GReadyStateCache;

	void CacheReadyState(UWorld* World, ANCReadyUpState* State)
	{
		if (World != nullptr)
		{
			const TWeakObjectPtr<UWorld> WorldKey(World);
			FReadyStateCacheEntry& Entry = GReadyStateCache.FindOrAdd(WorldKey);
			Entry.State = State;
			Entry.NextSearchTime = 0.0;
		}
	}

	void PruneWorldSettings()
	{
		for (auto It = GReadyUpEnabled.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent();
			}
		}
		for (auto It = GReadyStateCache.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}

	bool IsEligibleHuman(const AUTPlayerState* PS)
	{
		return PS != nullptr && !PS->bOnlySpectator && !PS->bIsInactive && !PS->bIsABot;
	}

	void ClearStartHostPresentation(AUTGameMode* GM)
	{
		if (GM == nullptr || GM->UTGameState == nullptr)
		{
			return;
		}

		// Keep both host-id strings intact so the stock host/admin panel and NCPlus
		// host-pause continue to recognize the host. Suppress only the engine's
		// Enter-to-start flags; NCPlus scoreboards hide HOST while ready-up exists.
		bool bGameStateChanged = false;
		if (GM->UTGameState->bHaveMatchHost)
		{
			GM->UTGameState->bHaveMatchHost = false;
			bGameStateChanged = true;
		}
		for (APlayerState* BasePS : GM->UTGameState->PlayerArray)
		{
			if (AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS))
			{
				if (PS->bIsMatchHost)
				{
					PS->bIsMatchHost = false;
					PS->ForceNetUpdate();
				}
			}
		}
		if (bGameStateChanged)
		{
			GM->UTGameState->ForceNetUpdate();
		}
	}

	void PrepareBotFillAtCountdownEnd(AUTGameMode* GM, float Remaining)
	{
		if (GM == nullptr || Remaining >= 2.f || GM->bRankedSession || GM->bOfflineChallenge
			|| GM->bForceNoBots || GM->bDevServer || GM->GetWorld()->WorldType == EWorldType::PIE)
		{
			return;
		}
		// CheckBotCount is protected; setting the target here is sufficient because
		// HandlePlayerIntro immediately performs the stock RemoveExtraBots /
		// CheckBotCount pair after ReadyToStartMatch returns true.
		GM->BotFillCount = FMath::Max(GM->BotFillCount, GM->AdjustedBotFillCount());
	}
}

ANCReadyUpState::ANCReadyUpState(const FObjectInitializer& OI)
	: Super(OI)
	, ReadyCount(0)
	, EligibleCount(0)
	, bCountdownLocked(false)
	, CountdownStartTime(0.f)
	, CountdownDuration(0.f)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = false;
	NetUpdateFrequency = 2.f;
}

void ANCReadyUpState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCReadyUpState, ReadyPlayers);
	DOREPLIFETIME(ANCReadyUpState, ReadyCount);
	DOREPLIFETIME(ANCReadyUpState, EligibleCount);
	DOREPLIFETIME(ANCReadyUpState, bCountdownLocked);
}

void ANCReadyUpState::BeginPlay()
{
	Super::BeginPlay();
	CacheReadyState(GetWorld(), this);
}

ANCReadyUpState* ANCReadyUpState::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	// Menu/HUD/scoreboard paths can ask every frame (and the scoreboard once per
	// row). Cache positive lookups, and throttle misses while still retrying for a
	// state actor that replicates after the local HUD was created.
	PruneWorldSettings();
	const TWeakObjectPtr<UWorld> WorldKey(const_cast<UWorld*>(World));
	FReadyStateCacheEntry& Entry = GReadyStateCache.FindOrAdd(WorldKey);
	if (Entry.State.IsValid())
	{
		return Entry.State.Get();
	}

	const double Now = FPlatformTime::Seconds();
	if (Now < Entry.NextSearchTime)
	{
		return nullptr;
	}
	for (TActorIterator<ANCReadyUpState> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ANCReadyUpState* State = *It;
		Entry.State = State;
		Entry.NextSearchTime = 0.0;
		return State;
	}
	Entry.NextSearchTime = Now + 1.0;
	return nullptr;
}

bool ANCReadyUpState::IsPlayerReady(const AUTPlayerState* PlayerState) const
{
	return PlayerState != nullptr && ReadyPlayers.Contains(const_cast<AUTPlayerState*>(PlayerState));
}

bool ANCReadyUpState::SetPlayerReady(APlayerController* Sender, bool bReady)
{
	if (Role != ROLE_Authority || Sender == nullptr)
	{
		return false;
	}

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	AUTPlayerState* PS = Cast<AUTPlayerState>(Sender->PlayerState);
	if (GS == nullptr || GS->GetMatchState() != MatchState::WaitingToStart || !IsEligibleHuman(PS))
	{
		return false;
	}

	if (bCountdownLocked)
	{
		Sender->ClientMessage(TEXT("Ready-up is locked; the match is starting."));
		return false;
	}
	if (IsPlayerReady(PS) == bReady)
	{
		return false;
	}

	if (bReady)
	{
		ReadyPlayers.AddUnique(PS);
		ReadyTeamByPlayer.Add(PS, PS->GetTeamNum());
	}
	else
	{
		ReadyPlayers.Remove(PS);
		ReadyTeamByPlayer.Remove(PS);
	}

	RefreshEligibility();
	ForceNetUpdate();
	Sender->ClientMessage(bReady ? TEXT("You are READY.") : TEXT("You are NOT READY."));
	return true;
}

void ANCReadyUpState::RefreshEligibility()
{
	if (Role != ROLE_Authority || bCountdownLocked)
	{
		return;
	}

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (GS == nullptr)
	{
		ReadyPlayers.Empty();
		ReadyTeamByPlayer.Empty();
		ReadyCount = 0;
		EligibleCount = 0;
		return;
	}

	const int32 OldReadyCount = ReadyCount;
	const int32 OldEligibleCount = EligibleCount;
	const int32 OldReadyPlayersNum = ReadyPlayers.Num();

	TSet<AUTPlayerState*> Eligible;
	for (APlayerState* BasePS : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (IsEligibleHuman(PS))
		{
			Eligible.Add(PS);
		}
	}

	ReadyPlayers.RemoveAll([this, &Eligible](AUTPlayerState* PS)
	{
		const uint8* ReadyTeam = ReadyTeamByPlayer.Find(PS);
		const bool bRemove = PS == nullptr || !Eligible.Contains(PS)
			|| ReadyTeam == nullptr || *ReadyTeam != PS->GetTeamNum();
		if (bRemove && PS != nullptr)
		{
			ReadyTeamByPlayer.Remove(PS);
		}
		return bRemove;
	});

	for (auto It = ReadyTeamByPlayer.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid() || !ReadyPlayers.Contains(It->Key.Get()))
		{
			It.RemoveCurrent();
		}
	}

	ReadyCount = ReadyPlayers.Num();
	EligibleCount = Eligible.Num();
	if (OldReadyCount != ReadyCount || OldEligibleCount != EligibleCount
		|| OldReadyPlayersNum != ReadyPlayers.Num())
	{
		ForceNetUpdate();
	}
}

void ANCReadyUpState::LockCountdown(float StartDelay)
{
	if (Role != ROLE_Authority || bCountdownLocked)
	{
		return;
	}
	RefreshEligibility();
	bCountdownLocked = true;
	CountdownStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	CountdownDuration = FMath::Max(0.f, StartDelay);
	ForceNetUpdate();
}

void ANCReadyUpState::CancelCountdown()
{
	if (Role != ROLE_Authority)
	{
		return;
	}
	bCountdownLocked = false;
	CountdownStartTime = 0.f;
	CountdownDuration = 0.f;
	ForceNetUpdate();
}

float ANCReadyUpState::GetRemainingCountdown(float Now) const
{
	return bCountdownLocked
		? FMath::Max(0.f, CountdownDuration - (Now - CountdownStartTime))
		: CountdownDuration;
}

ANCReadyUpMutator::ANCReadyUpMutator(const FObjectInitializer& OI)
	: Super(OI)
{
	DisplayName = NSLOCTEXT("NCPlus", "PlayerReadyUp", "Player Ready-Up");
}

void ANCReadyUpMutator::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	if (Sender != nullptr)
	{
		if (MutateString.Equals(TEXT("nc_ready"), ESearchCase::IgnoreCase))
		{
			if (ANCReadyUpState* State = ANCReadyUpState::Find(GetWorld()))
			{
				State->SetPlayerReady(Sender, true);
			}
		}
		else if (MutateString.Equals(TEXT("nc_notready"), ESearchCase::IgnoreCase))
		{
			if (ANCReadyUpState* State = ANCReadyUpState::Find(GetWorld()))
			{
				State->SetPlayerReady(Sender, false);
			}
		}
	}
	Super::Mutate_Implementation(MutateString, Sender);
}

void NCReadyUp::Initialize(AUTGameMode* GameMode)
{
	if (GameMode == nullptr || GameMode->GetWorld() == nullptr || !GameMode->HasAuthority())
	{
		return;
	}

	PruneWorldSettings();
	bool bEnabled = false;
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig != nullptr)
	{
		GConfig->GetBool(TEXT("NetcodePlus"), TEXT("bUsePlayerReadyUp"), bEnabled, ModIni);
	}
	GReadyUpEnabled.Add(GameMode->GetWorld(), bEnabled);
	if (bEnabled)
	{
		GameMode->AddMutatorClass(ANCReadyUpMutator::StaticClass());
		UE_LOG(LogGameMode, Log, TEXT("[NCReadyUp] Player ready-up enabled (host start control disabled)."));
	}
}

void NCReadyUp::PostLogin(AUTGameMode* GameMode, APlayerController* /*NewPlayer*/)
{
	if (!ShouldHandle(GameMode) || GameMode == nullptr || GameMode->GetWorld() == nullptr
		|| ANCReadyUpState::Find(GameMode->GetWorld()) != nullptr)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ANCReadyUpState* State = GameMode->GetWorld()->SpawnActor<ANCReadyUpState>(
		ANCReadyUpState::StaticClass(), Params);
	if (State != nullptr)
	{
		CacheReadyState(GameMode->GetWorld(), State);
		State->RefreshEligibility();
		ClearStartHostPresentation(GameMode);
	}
}

bool NCReadyUp::ShouldHandle(const AUTGameMode* GameMode)
{
	if (GameMode == nullptr || GameMode->GetWorld() == nullptr || !GameMode->bDelayedStart
		|| GameMode->GetWorld()->IsPlayInEditor()
		|| GameMode->GetNetMode() == NM_Standalone || GameMode->bRankedSession)
	{
		return false;
	}

	PruneWorldSettings();
	const bool* bEnabled = GReadyUpEnabled.Find(GameMode->GetWorld());
	return bEnabled != nullptr && *bEnabled;
}

bool NCReadyUp::ReadyToStartMatch(AUTGameMode* GameMode)
{
	if (!ShouldHandle(GameMode) || GameMode == nullptr || GameMode->UTGameState == nullptr
		|| GameMode->GetMatchState() != MatchState::WaitingToStart)
	{
		return false;
	}

	ClearStartHostPresentation(GameMode);
	ANCReadyUpState* State = ANCReadyUpState::Find(GameMode->GetWorld());
	if (State == nullptr)
	{
		// Enabled but no player has completed PostLogin yet. Never fall through to
		// host control during this short startup window.
		return false;
	}

	AUTGameState* GS = GameMode->UTGameState;
	GameMode->StartPlayTime = GameMode->NumPlayers > 0
		? FMath::Min(GameMode->StartPlayTime, GameMode->GetWorld()->GetTimeSeconds())
		: 10000000.f;
	if (State->bCountdownLocked)
	{
		// Once the final countdown begins, readiness and roster churn cannot
		// unwind it. Only an entirely empty server cancels.
		GS->PlayersNeeded = 0;
		int32 CurrentHumans = 0;
		for (APlayerState* BasePS : GS->PlayerArray)
		{
			if (IsEligibleHuman(Cast<AUTPlayerState>(BasePS)))
			{
				++CurrentHumans;
			}
		}
		if (CurrentHumans == 0)
		{
			State->CancelCountdown();
			GameMode->LastMatchNotReady = GameMode->GetWorld()->GetTimeSeconds();
			GS->SetRemainingTime(0.f);
			return false;
		}

		const float Remaining = State->GetRemainingCountdown(GameMode->GetWorld()->GetTimeSeconds());
		GS->SetRemainingTime(Remaining);
		PrepareBotFillAtCountdownEnd(GameMode, Remaining);
		return Remaining <= 0.f;
	}

	const int32 Capacity = GameMode->GameSession != nullptr
		? GameMode->GameSession->MaxPlayers : GameMode->DefaultMaxPlayers;
	GS->PlayersNeeded = GameMode->bRequireFull
		? FMath::Max(0, Capacity - GameMode->NumPlayers) : 0;

	State->RefreshEligibility();
	if (GameMode->bRequireFull && GS->PlayersNeeded > 0)
	{
		GameMode->LastMatchNotReady = GameMode->GetWorld()->GetTimeSeconds();
		GS->SetRemainingTime(0.f);
		return false;
	}

	if (State->EligibleCount > 0 && State->ReadyCount == State->EligibleCount)
	{
		State->LockCountdown(GameMode->StartDelay);
		GS->SetRemainingTime(GameMode->StartDelay);
		return GameMode->StartDelay <= 0.f;
	}

	GameMode->LastMatchNotReady = GameMode->GetWorld()->GetTimeSeconds();
	GS->SetRemainingTime(0.f);
	return false;
}
