// NCPlusVersionGate.cpp — see header.
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameMode.h"
#include "Engine/World.h"
#include "TimerManager.h"

// Server kick deadline. User asked for 10s so a hitchy client has time to fully
// initialize replication before we judge them. Long enough to never false-kick;
// short enough that warmup play time for an outdated client is bounded.
static const float kVersionReportTimeoutSec = 10.f;

ANCVersionGate::ANCVersionGate(const FObjectInitializer& OI)
	: Super(OI)
	, bConfirmed(false)
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	// Don't tick — we run off a single timer + an RPC. Pointless heartbeat would
	// just allocate work for every joining player.
	PrimaryActorTick.bCanEverTick = false;
	NetUpdateFrequency = 1.f;
}

void ANCVersionGate::BeginPlay()
{
	Super::BeginPlay();

	// Server side: start the timeout. Client side's PostNetInit hits the RPC
	// as soon as the actor is fully replicated + owner-resolved.
	if (Role == ROLE_Authority)
	{
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().SetTimer(
				TimeoutHandle, this, &ANCVersionGate::OnTimeout,
				kVersionReportTimeoutSec, /*bLoop*/ false);
		}
	}
}

void ANCVersionGate::PostNetInit()
{
	Super::PostNetInit();

	// Client side, owner-only replication means we only fire on the actual
	// owning player. NETCODE_PLUGIN_VERSION is compiled into the client's
	// plugin DLL — if they're outdated, the value won't match the server's.
	if (Role != ROLE_Authority)
	{
		ServerReportVersion(NETCODE_PLUGIN_VERSION);
	}
}

bool ANCVersionGate::ServerReportVersion_Validate(int32 /*ClientVersion*/)
{
	return true;
}

void ANCVersionGate::ServerReportVersion_Implementation(int32 ClientVersion)
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	const int32 ServerVersion = NETCODE_PLUGIN_VERSION;
	if (ClientVersion == ServerVersion)
	{
		bConfirmed = true;
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().ClearTimer(TimeoutHandle);
		}
		// Job done — drop the actor so it doesn't linger in the per-player
		// relevancy set for the rest of the match.
		Destroy();
		return;
	}

	// Mismatch — immediate kick.
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking owner: client v%d != server v%d"),
		ClientVersion, ServerVersion);
	KickOwner(FString::Printf(
		TEXT("NetcodePlus version mismatch: server is v%d, you are v%d. Update via launcher."),
		ServerVersion, ClientVersion));
}

void ANCVersionGate::OnTimeout()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking owner: no version report within %.0fs (server v%d). ")
		TEXT("Likely missing/outdated NetcodePlus plugin."),
		kVersionReportTimeoutSec, NETCODE_PLUGIN_VERSION);
	KickOwner(FString::Printf(
		TEXT("NetcodePlus plugin missing or outdated (server is v%d). Update via launcher."),
		NETCODE_PLUGIN_VERSION));
}

void ANCVersionGate::KickOwner(const FString& Reason)
{
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	UWorld* W = GetWorld();
	AGameMode* GM = W ? W->GetAuthGameMode() : nullptr;
	if (PC && GM && GM->GameSession)
	{
		GM->GameSession->KickPlayer(PC, FText::FromString(Reason));
	}
	Destroy();
}

namespace NCPlusVersionGate
{
	void SpawnFor(APlayerController* PC)
	{
		if (PC == nullptr || !PC->HasAuthority())
		{
			return;
		}
		// Bots have no client — nothing to handshake with. The listen-host's
		// local PC also has no remote: replicating an owner-only actor to a
		// local PC works, but it's churn for no win since the host obviously
		// has a matching version (same DLL load).
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(PC);
		if (UTPC && UTPC->UTPlayerState && UTPC->UTPlayerState->bIsABot)
		{
			return;
		}
		if (PC->IsLocalController())
		{
			return;
		}
		UWorld* W = PC->GetWorld();
		if (W == nullptr)
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.Owner = PC;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		W->SpawnActor<ANCVersionGate>(ANCVersionGate::StaticClass(), Params);
	}
}
