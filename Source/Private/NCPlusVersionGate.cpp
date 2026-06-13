// NCPlusVersionGate.cpp — see header.
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameMode.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

// Default kick deadline (seconds). 10s gives a hitchy client time to fully
// initialize replication before we judge them. Overridable per-server via
// Mod.ini [NetcodePlus] VersionReportTimeoutSec, clamped 1.0-60.0.
static const float kVersionReportTimeoutDefault = 10.f;
static const float kVersionReportTimeoutMin     =  1.f;
static const float kVersionReportTimeoutMax     = 60.f;

// Resolve the timeout from Mod.ini [NetcodePlus] VersionReportTimeoutSec, falling
// back to the default if the file/section/key is missing or out of range. Cheap
// enough to call per spawn — gives admins same-process live-ish control (each new
// joiner reads fresh; no actor change needed mid-match) and survives restarts.
static float ResolveVersionReportTimeoutSec()
{
	const FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");
	if (!FPaths::FileExists(ModIniPath))
	{
		return kVersionReportTimeoutDefault;
	}
	FConfigFile ModIni;
	ModIni.Read(ModIniPath);
	const FConfigSection* Section = ModIni.Find(TEXT("NetcodePlus"));
	if (!Section)
	{
		return kVersionReportTimeoutDefault;
	}
	const FConfigValue* V = Section->Find(FName(TEXT("VersionReportTimeoutSec")));
	if (!V)
	{
		return kVersionReportTimeoutDefault;
	}
	const float Parsed = FCString::Atof(*V->GetValue());
	if (Parsed <= 0.f)
	{
		return kVersionReportTimeoutDefault;     // Atof returns 0 on garbage input
	}
	return FMath::Clamp(Parsed, kVersionReportTimeoutMin, kVersionReportTimeoutMax);
}

// Best-effort owning player's name for audit logs. Owner / PlayerState can race
// to null on disconnect, so guard every hop and fall back to "<unknown>".
static FString ResolveOwnerName(AActor* Gate)
{
	APlayerController* PC = Gate ? Cast<APlayerController>(Gate->GetOwner()) : nullptr;
	return (PC && PC->PlayerState) ? PC->PlayerState->PlayerName : FString(TEXT("<unknown>"));
}

ANCVersionGate::ANCVersionGate(const FObjectInitializer& OI)
	: Super(OI)
	, bConfirmed(false)
	, TimeoutSec(kVersionReportTimeoutDefault)
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
			TimeoutSec = ResolveVersionReportTimeoutSec();
			W->GetTimerManager().SetTimer(
				TimeoutHandle, this, &ANCVersionGate::OnTimeout,
				TimeoutSec, /*bLoop*/ false);
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

	// Mismatch — KICK DISABLED FOR NOW. The version-mismatch path is logged but
	// no longer kicks the player; mismatched clients are allowed in. The 10s
	// no-reply timeout (OnTimeout) is still active and will kick clients that
	// never report at all (no-plugin / wrong-path installs). To restore the
	// mismatch kick: replace this block with the KickOwner call (see git history
	// — commit before this disable).
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] version mismatch ALLOWED IN (kick disabled): client v%d != server v%d (player: %s)"),
		ClientVersion, ServerVersion, *ResolveOwnerName(this));
	// Mark confirmed + clear timer + destroy actor so the 10s timeout doesn't
	// fire and kick a player who DID reply (just with a different version).
	bConfirmed = true;
	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().ClearTimer(TimeoutHandle);
	}
	Destroy();
}

void ANCVersionGate::OnTimeout()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking owner: no version report within %.0fs (server v%d). ")
		TEXT("Likely missing/outdated NetcodePlus plugin. (player: %s)"),
		TimeoutSec, NETCODE_PLUGIN_VERSION, *ResolveOwnerName(this));
	KickOwner(FString::Printf(
		TEXT("NetcodePlus plugin missing or outdated (server is v%d). Update via launcher."),
		NETCODE_PLUGIN_VERSION));
}

void ANCVersionGate::KickOwner(const FString& Reason)
{
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	UWorld* W = GetWorld();
	// UE4.15: UWorld::GetAuthGameMode() returns AGameModeBase*, not AGameMode*.
	// Cast to AGameMode so we can reach ->GameSession (lives on the AGameMode
	// subclass in this engine). Both UT4 modes derive from AGameMode, so the
	// cast always succeeds for our PostLogin spawn path.
	AGameMode* GM = W ? Cast<AGameMode>(W->GetAuthGameMode()) : nullptr;
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
