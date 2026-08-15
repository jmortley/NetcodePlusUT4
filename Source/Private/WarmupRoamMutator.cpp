// WarmupRoamMutator.cpp - see header.
#include "WarmupRoamMutator.h"
#include "NCReadyUp.h"
#include "UnrealTournament.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTGameState.h"
#include "UTGameMode.h"
#include "UTBaseGameMode.h"
#include "UTPlayerState.h"

AWarmupRoamMutator::AWarmupRoamMutator(const FObjectInitializer& OI)
	: Super(OI)
{
	DisplayName = NSLOCTEXT("NCPlus", "WarmupRoam", "Warmup Roam");
}

bool AWarmupRoamMutator::IsWarmup() const
{
	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	return GS != nullptr && GS->GetMatchState() == MatchState::WaitingToStart;
}

bool AWarmupRoamMutator::IsRoaming(AController* C) const
{
	for (const TWeakObjectPtr<AController>& W : RoamingControllers)
	{
		if (W.Get() == C)
		{
			return true;
		}
	}
	return false;
}

void AWarmupRoamMutator::SetRoaming(AController* C, bool bRoam)
{
	// Drop stale + any existing entry for C first, then re-add if enabling.
	RoamingControllers.RemoveAll([C](const TWeakObjectPtr<AController>& W)
	{
		return !W.IsValid() || W.Get() == C;
	});
	if (bRoam && C != nullptr)
	{
		RoamingControllers.Add(C);
	}
}

void AWarmupRoamMutator::ApplyRoam(AUTCharacter* Char, bool bEnable)
{
	if (Char == nullptr)
	{
		return;
	}
	// bDamageHurtsHealth=false -> TakeDamage applies no health damage (server gate).
	// DisallowWeaponFiring(true) -> weapon fire paths bail on IsFiringDisabled().
	Char->bDamageHurtsHealth = !bEnable;
	Char->DisallowWeaponFiring(bEnable);
}

void AWarmupRoamMutator::ClearAll()
{
	for (const TWeakObjectPtr<AController>& W : RoamingControllers)
	{
		if (AController* C = W.Get())
		{
			ApplyRoam(Cast<AUTCharacter>(C->GetPawn()), false);
		}
	}
	RoamingControllers.Empty();
}

void AWarmupRoamMutator::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	if (Sender != nullptr && MutateString.Equals(TEXT("warmup"), ESearchCase::IgnoreCase))
	{
		if (!IsWarmup())
		{
			Sender->ClientMessage(TEXT("Warmup roam is only available during warmup."));
		}
		else
		{
			const bool bNowRoaming = !IsRoaming(Sender);
			SetRoaming(Sender, bNowRoaming);
			ApplyRoam(Cast<AUTCharacter>(Sender->GetPawn()), bNowRoaming);
			Sender->ClientMessage(bNowRoaming
				? TEXT("Warmup roam ON - invulnerable, firing disabled. Ends when the match starts.")
				: TEXT("Warmup roam OFF."));
		}
		// fall through to Super so the rest of the mutator chain still sees the command.
	}
	else if (Sender != nullptr && MutateString.Equals(TEXT("host"), ESearchCase::IgnoreCase))
	{
		AUTGameMode* GameMode = GetWorld() ? Cast<AUTGameMode>(GetWorld()->GetAuthGameMode()) : nullptr;
		if (NCReadyUp::ShouldHandle(GameMode))
		{
			ANCReadyUpState* ReadyState = ANCReadyUpState::Find(GetWorld());
			Sender->ClientMessage(ReadyState != nullptr && ReadyState->bCountdownLocked
				? TEXT("All players are ready; the match is starting.")
				: TEXT("Player ready-up is enabled. Press F5; the match starts after every active player is ready."));
			Super::Mutate_Implementation(MutateString, Sender);
			return;
		}

		// Stopgap host visibility: the scoreboard HOST badge is unreliable on some
		// servers until the ANCHostInfo client roll (feature/host-badge-info-actor),
		// so let anyone ask the server directly. The answer travels over the stock
		// ClientMessage RPC, so this works for current clients with no recook.
		bool bHostConfigured = false;
		AUTPlayerState* HostPS = ResolveHostPS(bHostConfigured);
		if (!bHostConfigured)
		{
			Sender->ClientMessage(TEXT("No match host is configured on this server."));
		}
		else
		{
			Sender->ClientMessage(HostPS != nullptr
				? FString::Printf(TEXT("Match host: %s"), *HostPS->PlayerName)
				: TEXT("Match host is not connected yet."));
			// Server-log echo so admins can grep who the host resolved to per match.
			UE_LOG(LogGameMode, Log, TEXT("[WarmupRoam] mutate host: %s (asked by %s)"),
				HostPS ? *HostPS->PlayerName : TEXT("<not connected>"),
				(Sender->PlayerState != nullptr) ? *Sender->PlayerState->PlayerName : TEXT("<unknown>"));
		}
		// fall through to Super, same as `warmup`.
	}

	Super::Mutate_Implementation(MutateString, Sender);
}

AUTPlayerState* AWarmupRoamMutator::ResolveHostPS(bool& bOutHostConfigured) const
{
	bOutHostConfigured = false;
	// UE4.15: GetAuthGameMode() returns AGameModeBase* — cast to reach GetHostId().
	AUTBaseGameMode* GM = GetWorld() ? Cast<AUTBaseGameMode>(GetWorld()->GetAuthGameMode()) : nullptr;
	const FString HostId = GM ? GM->GetHostId() : FString();
	if (HostId.IsEmpty())
	{
		return nullptr;
	}
	bOutHostConfigured = true;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS != nullptr)
	{
		for (int32 i = 0; i < GS->PlayerArray.Num(); i++)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(GS->PlayerArray[i]);
			// Mirror the engine host loop's match (UTGameMode::ReadyToStartMatch):
			// id equality, case-insensitive, skip inactive.
			if (PS != nullptr && !PS->bIsInactive && PS->UniqueId.IsValid()
				&& HostId.Equals(PS->UniqueId.ToString(), ESearchCase::IgnoreCase))
			{
				return PS;
			}
		}
	}
	return nullptr;
}

void AWarmupRoamMutator::PostPlayerInit_Implementation(AController* C)
{
	Super::PostPlayerInit_Implementation(C);

	// Auto-announce the active warmup start control. PostPlayerInit also fires
	// for bots; only humans get console lines.
	AUTPlayerState* PS = (C != nullptr) ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
	if (C == nullptr || (PS != nullptr && PS->bIsABot))
	{
		return;
	}
	AUTGameMode* GameMode = GetWorld() ? Cast<AUTGameMode>(GetWorld()->GetAuthGameMode()) : nullptr;
	if (IsWarmup() && NCReadyUp::ShouldHandle(GameMode))
	{
		if (APlayerController* PC = Cast<APlayerController>(C))
		{
			FTimerHandle Unused;
			GetWorldTimerManager().SetTimer(Unused,
				FTimerDelegate::CreateUObject(this, &AWarmupRoamMutator::SendWarmupStartInfoTo,
					TWeakObjectPtr<APlayerController>(PC)),
				10.f, false);
		}
		return;
	}
	bool bHostConfigured = false;
	AUTPlayerState* HostPS = ResolveHostPS(bHostConfigured);
	if (!bHostConfigured || !IsWarmup())
	{
		return;   // hostless server or mid-match join — nothing worth announcing
	}

	// The joiner IS the host: tell everyone already connected, immediately — players
	// who joined before the host otherwise never learn the name.
	if (HostPS != nullptr && PS == HostPS)
	{
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* Other = *It;
			if (Other != nullptr && Other != C)
			{
				Other->ClientMessage(FString::Printf(TEXT("Match host: %s has joined."), *HostPS->PlayerName));
			}
		}
		return;   // the host knows who they are
	}

	// Regular joiner: a message sent right now lands while they're still on the
	// loading screen and scrolls away unseen, so delay it past the travel (and past
	// the version-gate kick window — no point messaging a client about to be kicked).
	if (APlayerController* PC = Cast<APlayerController>(C))
	{
		FTimerHandle Unused;
		GetWorldTimerManager().SetTimer(Unused,
			FTimerDelegate::CreateUObject(this, &AWarmupRoamMutator::SendWarmupStartInfoTo, TWeakObjectPtr<APlayerController>(PC)),
			10.f, false);
	}
}

void AWarmupRoamMutator::SendWarmupStartInfoTo(TWeakObjectPtr<APlayerController> WeakPC)
{
	APlayerController* PC = WeakPC.Get();
	if (PC == nullptr || !IsWarmup())
	{
		return;   // left during the delay, or the match already started
	}
	if (ANCReadyUpState* ReadyState = ANCReadyUpState::Find(GetWorld()))
	{
		PC->ClientMessage(ReadyState->bCountdownLocked
			? TEXT("All players are ready; the match is starting.")
			: TEXT("Player ready-up enabled: press F5 and choose Ready. Readiness locks when the final countdown begins."));
		return;
	}
	bool bHostConfigured = false;
	AUTPlayerState* HostPS = ResolveHostPS(bHostConfigured);
	if (!bHostConfigured)
	{
		return;
	}
	PC->ClientMessage(HostPS != nullptr
		? FString::Printf(TEXT("Match host: %s (starts the match when ready)"), *HostPS->PlayerName)
		: TEXT("The match host hasn't joined yet."));
}

void AWarmupRoamMutator::ModifyPlayer_Implementation(APawn* Other, bool bIsNewSpawn)
{
	Super::ModifyPlayer_Implementation(Other, bIsNewSpawn);

	// Firing-disable + invuln reset to defaults on every spawn, so re-assert for
	// roamers while we're still in warmup.
	if (IsWarmup())
	{
		AUTCharacter* Char = Cast<AUTCharacter>(Other);
		if (Char != nullptr && IsRoaming(Char->GetController()))
		{
			ApplyRoam(Char, true);
		}
	}
}

void AWarmupRoamMutator::NotifyMatchStateChange_Implementation(FName NewState)
{
	Super::NotifyMatchStateChange_Implementation(NewState);

	// Roam is warmup-only. The instant we leave WaitingToStart (countdown OR match
	// start), strip it from everyone so godmode can never carry into live play.
	if (NewState != MatchState::WaitingToStart)
	{
		ClearAll();
	}
}
