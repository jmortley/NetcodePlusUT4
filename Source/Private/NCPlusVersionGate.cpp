// NCPlusVersionGate.cpp — see header.
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"   // ClientSay
#include "UTATypes.h"                 // ChatDestinations
#include "UTPlayerState.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameMode.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

// Default kick deadline (seconds). 100s is a generous grace so only genuinely
// plugin-less clients are still unreported by the deadline — a legit client with
// lossy owner-only replication has ample time to receive the gate actor + run the
// handshake before we judge it (this lossiness is why the timeout kick was
// disabled in 9159128; the long grace is the mitigation). Overridable per-server
// via Mod.ini [NetcodePlus] VersionReportTimeoutSec, clamped 1.0-120.0.
static const float kVersionReportTimeoutDefault = 100.f;
static const float kVersionReportTimeoutMin     =   1.f;
static const float kVersionReportTimeoutMax     = 120.f;

// After the no-report deadline we announce to the whole server, then kick this
// many seconds later — context for everyone + a final beat for a very-late
// handshake to land and cancel the kick.
static const float kKickGraceSec = 5.f;

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

// Push a system chat line to every player on the server. ClientSay is base-UT
// (AUTBasePlayerController), not plugin-gated, so plugin-less clients see it too.
static void BroadcastSystemMessage(UWorld* W, const FString& Msg)
{
	if (!W)
	{
		return;
	}
	for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		if (PC && PC->UTPlayerState)
		{
			PC->ClientSay(PC->UTPlayerState, Msg, ChatDestinations::System);
		}
	}
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
		// DISABLED 2026-06-24 — the no-reply timeout kick was false-positiving on
		// hub-instance joins: a legit, correctly-versioned client whose version
		// report didn't land inside the window got kicked, and KickOwner's
		// GameSession->KickPlayer adds them to the instance/hub ban list, so the
		// client can't rejoin ("PendingNetDriver[PendingConnectionFailure]: BANNED").
		// Mismatch reports (ServerReportVersion_Implementation) still kick genuinely
		// outdated clients; we just stop kicking clients that never handshake.
		// Re-arm by uncommenting the SetTimer below.
		TimeoutSec = ResolveVersionReportTimeoutSec();   // resolved for the OnTimeout log line; the kick timer is NOT armed
		// if (UWorld* W = GetWorld())
		// {
		// 	W->GetTimerManager().SetTimer(
		// 		TimeoutHandle, this, &ANCVersionGate::OnTimeout,
		// 		TimeoutSec, /*bLoop*/ false);
		// }
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
			W->GetTimerManager().ClearTimer(KickHandle);   // cancel a pending grace-kick if they reported late
		}
		// Job done — drop the actor so it doesn't linger in the per-player
		// relevancy set for the rest of the match.
		Destroy();
		return;
	}

	// Mismatch — immediate kick. (The no-reply timeout path in OnTimeout also
	// kicks now — re-enabled 2026-06-19 at a 100s grace.)
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking owner: client v%d != server v%d (player: %s)"),
		ClientVersion, ServerVersion, *ResolveOwnerName(this));
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
	// No version report within the deadline → the client never handshook, i.e. no
	// NetcodePlus plugin (or a pre-gate build). Rather than an abrupt kick, announce
	// it to the whole server for context, then kick kKickGraceSec later. The delay
	// is also a final beat for a very-late handshake to land — a matching report in
	// the window clears KickHandle (see ServerReportVersion_Implementation).
	const FString PlayerName = ResolveOwnerName(this);
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] no version report within %.0fs: %s missing plugin (server v%d) — announcing + kicking in %.0fs."),
		TimeoutSec, *PlayerName, NETCODE_PLUGIN_VERSION, kKickGraceSec);

	BroadcastSystemMessage(GetWorld(), FString::Printf(
		TEXT("%s does not have the NetcodePlus plugin (server is v%d) — removing in %.0fs. Get it from the launcher."),
		*PlayerName, NETCODE_PLUGIN_VERSION, kKickGraceSec));

	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().SetTimer(
			KickHandle, this, &ANCVersionGate::OnKickDeadline,
			kKickGraceSec, /*bLoop*/ false);
	}
}

void ANCVersionGate::OnKickDeadline()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	bConfirmed = true;
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking %s — no NetcodePlus plugin (server v%d)."),
		*ResolveOwnerName(this), NETCODE_PLUGIN_VERSION);
	KickOwner(FString::Printf(
		TEXT("NetcodePlus plugin required (server v%d) — not detected. Install/update via the launcher."),
		NETCODE_PLUGIN_VERSION));
}

void ANCVersionGate::KickOwner(const FString& Reason)
{
	// Non-banning disconnect WITH a visible reason. GameSession->KickPlayer adds the
	// player to UTGameEngine->InstanceBannedUsers on a hub instance
	// (AUTGameSessionNonRanked::KickPlayer) → "PendingConnectionFailure: BANNED" on
	// rejoin, and its bKickToHubIfPossible=true path doesn't even surface the reason.
	// Call GuaranteedKick(reason, /*bKickToHubIfPossible*/ false) directly instead:
	// it routes through ClientWasKicked (which shows the reason on the client) plus a
	// 1s-delayed Destroy, and skips the instance-ban add. An outdated client sees the
	// version message and can rejoin once they've updated via the launcher.
	if (AUTBasePlayerController* PC = Cast<AUTBasePlayerController>(GetOwner()))
	{
		PC->GuaranteedKick(FText::FromString(Reason), /*bKickToHubIfPossible*/ false);
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
