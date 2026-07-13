// NCPlusVersionGate.cpp — see header.
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"   // GuaranteedKick, ClientSay
#include "UTATypes.h"                 // ChatDestinations (advisor whisper)
#include "UTBaseGameMode.h"           // IsLobbyServer (hub advisor auto-spawn)
#include "UTPlayerState.h"
#include "GameFramework/GameModeBase.h" // FGameModeEvents::GameModePostLoginEvent
#include "UTCharacterMovement.h"      // CurrentServerMoveTime — server-side "client has moved" proof
#include "GameFramework/Pawn.h"
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

// After the no-report deadline, kick this many seconds later — a final beat for
// a very-late handshake to land and cancel the kick. (No public announcement:
// the plugin-less owner can't see chat anyway — their HUD class doesn't load,
// so ClientReceiveChat drops every line — and announcing to bystanders produced
// false "missing plugin" accusations for legit players whose handshake was lost
// and who simply hadn't moved yet, 2026-07-03 reports. The kick-reason screen
// carries the actionable message to the one player who needs it.)
static const float kKickGraceSec = 5.f;

// Poll cadence for the "has the owner ever moved?" latch (OnMoveWatch). Cheap (a couple
// casts + a float read). The first time the owner's pawn has moved we confirm + destroy,
// so a legit client is cleared within ~this long of its first move — long before any kick.
static const float kMoveWatchIntervalSec = 2.f;

// When the owner is pawnless BY DESIGN at a deadline (pure spectator, or a round-based
// player currently eliminated), don't kick — re-check this many seconds later. The latch
// confirms them if/when they spawn and move; a permanent spectator just keeps deferring.
static const float kPawnlessDeferSec = 30.f;

// ADVISOR MODE (hubs) cadence. First check is generous — the handshake normally
// lands within seconds, and a whisper (unlike a kick) tolerates the rare lost
// handshake, so no movement corroboration exists or is needed on a pawnless hub.
// Then nag slowly while unconfirmed; each ServerReportVersion match still
// confirms + destroys instantly, at any time.
static const float kAdvisorFirstCheckSec = 60.f;
static const float kAdvisorRepeatSec     = 180.f;

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

// Corroborating "this client is really here and functional" signal for the no-report
// kick. On the server, UUTCharacterMovement::CurrentServerMoveTime is set to the client's
// timestamp when a client movement update is processed (MoveAutonomous,
// UTCharMovementReplication.cpp) and is 0 until the first one arrives. (Two other
// server-authored writers exist in UTCharacterMovement::TickComponent — one writes a large
// positive value — but both are gated on IsLocallyControlled/bRunPhysicsWithNoController,
// never true for the remote, possessed, non-local pawn this reads; bots/local PCs, which
// CAN hit them, are gate-exempt in SpawnFor. So they can't false-positive here.) The
// intended player pawn is a NetcodePlus class (ATeamArenaCharacter) a plugin-LESS client
// can't load, so it never gets a controllable pawn, never sends a UTServerMove, and stays
// at 0 — while a moving client demonstrably loaded that class, i.e. has the plugin (its
// missing report is a lost handshake). ⚠️ the pawn type is BP-supplied (the gametype's
// DefaultPawnClass), NOT enforced in C++ — if a deployed mode used a stock, plugin-loadable
// pawn this inference would break. Guard the kick on this so we never boot a legit player
// whose reliable RPC didn't land in the window.
static bool OwnerClientHasMoved(AActor* Gate)
{
	APlayerController* PC = Gate ? Cast<APlayerController>(Gate->GetOwner()) : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UUTCharacterMovement* Move =
		Pawn ? Cast<UUTCharacterMovement>(Pawn->GetMovementComponent()) : nullptr;
	return Move != nullptr && Move->CurrentServerMoveTime > 0.f;
}

// True when the owner is pawnless BY DESIGN — a pure spectator, or a round-based player
// currently eliminated (bOutOfLives) and so pawnless for the rest of the round. The
// movement guard can't see these clients, so we DON'T kick them: we defer + re-check (the
// MoveWatch latch confirms them if/when they later spawn and move). A genuine version
// MISMATCH (a real report with the wrong number) still kicks immediately regardless.
static bool OwnerIsPawnlessByDesign(AActor* Gate)
{
	APlayerController* PC = Gate ? Cast<APlayerController>(Gate->GetOwner()) : nullptr;
	AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
	return PS != nullptr && (PS->bOnlySpectator || PS->bOutOfLives);
}

ANCVersionGate::ANCVersionGate(const FObjectInitializer& OI)
	: Super(OI)
	, bAdvisorMode(false)
	, bConfirmed(false)
	, AdvisorNagCount(0)
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
		// ADVISOR MODE (hubs): whisper, never kick. No MoveWatch — hub players are
		// pawnless, and with no kick at stake the movement corroboration is moot.
		if (bAdvisorMode)
		{
			if (UWorld* W = GetWorld())
			{
				W->GetTimerManager().SetTimer(
					TimeoutHandle, this, &ANCVersionGate::OnAdvisorCheck,
					kAdvisorFirstCheckSec, /*bLoop*/ false);
			}
			return;
		}
		// ARMED 2026-06-29 — the no-reply timeout kick is ON. It's the only channel that
		// reaches a plugin-less client already inside an NCPlus instance: their HUD is an
		// NCPlus class they can't load, so MyHUD is null/non-AUTHUD and ClientReceiveChat
		// drops every chat/HUD line (the same reason they see broken player models). The
		// kick-reason screen (GuaranteedKick → ClientWasKicked → viewport KickReason) is
		// NOT HUD-gated, so it renders + names netcodeplus.com.
		//
		// Safe to run (the BARE timeout kick is why 9159128 disabled it — a lossy legit
		// joiner got instance-banned): (1) KickOwner routes through non-banning
		// GuaranteedKick, never GameSession->KickPlayer, so a mistaken kick is rejoinable,
		// not a ban; (2) OwnerClientHasMoved stands down any client whose pawn has sent
		// moves (loaded the NCPlus pawn ⇒ has the plugin — a lost handshake, not a missing
		// plugin); (3) a MoveWatch latch (armed below) confirms + destroys the gate the
		// FIRST time the owner ever moves, so a legit client that later goes pawnless
		// (respawn, eliminated for a round) can't be re-exposed to the kick; and (4)
		// OnTimeout/OnKickDeadline DEFER rather than kick a pawnless-by-design owner
		// (spectator / bOutOfLives). ⚠️ (2)/(3) assume the gametype's DefaultPawnClass is
		// the NCPlus pawn (ATeamArenaCharacter) — BP-supplied, not enforced here. Default
		// 100s grace, Mod.ini VersionReportTimeoutSec.
		TimeoutSec = ResolveVersionReportTimeoutSec();
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().SetTimer(
				TimeoutHandle, this, &ANCVersionGate::OnTimeout,
				TimeoutSec, /*bLoop*/ false);
			// Latch: confirm + destroy the moment the owner ever moves (see class comment).
			W->GetTimerManager().SetTimer(
				MoveWatchHandle, this, &ANCVersionGate::OnMoveWatch,
				kMoveWatchIntervalSec, /*bLoop*/ true);
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
			W->GetTimerManager().ClearTimer(KickHandle);   // cancel a pending grace-kick if they reported late
			W->GetTimerManager().ClearTimer(MoveWatchHandle);
		}
		// Job done — drop the actor so it doesn't linger in the per-player
		// relevancy set for the rest of the match.
		Destroy();
		return;
	}

	// Mismatch, advisor mode (hub) — they HAVE a plugin, just the wrong build.
	// One courtesy whisper (definitive knowledge, no need to nag), then done;
	// the instance-side gate still enforces on an actual match join.
	if (bAdvisorMode)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] hub advisor: %s reported v%d, server is v%d — whispering update pointer."),
			*ResolveOwnerName(this), ClientVersion, ServerVersion);
		WhisperOwner(FString::Printf(
			TEXT("Your NetcodePlus plugin is v%d — matches on this hub run v%d. Update via the launcher at netcodeplus.com."),
			ClientVersion, ServerVersion));
		bConfirmed = true;
		Destroy();
		return;
	}

	// Mismatch — immediate kick. (The no-reply timeout path in OnTimeout also kicks —
	// armed + movement-guarded; see BeginPlay.)
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking owner: client v%d != server v%d (player: %s)"),
		ClientVersion, ServerVersion, *ResolveOwnerName(this));
	KickOwner(FString::Printf(
		TEXT("NetcodePlus version mismatch: server is v%d, you are v%d. Update via the launcher at netcodeplus.com."),
		ServerVersion, ClientVersion));
}

void ANCVersionGate::OnTimeout()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	// Corroboration guard: if the owner is demonstrably playing (their pawn has sent
	// moves), the missing report is a failed handshake on a functional client, not a
	// missing plugin — a plugin-less client can't load the NCPlus pawn class so it never
	// moves (see OwnerClientHasMoved). Stand down silently: don't broadcast a false
	// "missing plugin" accusation or schedule the kick.
	if (OwnerClientHasMoved(this))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] no version report within %.0fs from %s, but their pawn is moving — treating as a lost handshake on a functional client, NOT kicking."),
			TimeoutSec, *ResolveOwnerName(this));
		bConfirmed = true;
		Destroy();
		return;
	}

	// Pawnless BY DESIGN (pure spectator, or eliminated this round) → the movement guard
	// can't see them; don't accuse/kick — defer and re-check (the MoveWatch latch confirms
	// them if/when they spawn and move; a permanent spectator just keeps deferring).
	if (OwnerIsPawnlessByDesign(this))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] %s is pawnless by design (spectator/eliminated) — deferring the plugin check %.0fs."),
			*ResolveOwnerName(this), kPawnlessDeferSec);
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().SetTimer(
				TimeoutHandle, this, &ANCVersionGate::OnTimeout,
				kPawnlessDeferSec, /*bLoop*/ false);
		}
		return;
	}

	// No version report within the deadline → the client never handshook, i.e. no
	// NetcodePlus plugin (or a pre-gate build). Kick kKickGraceSec from now — the
	// delay is a final beat for a very-late handshake to land (a matching report
	// in the window clears KickHandle, see ServerReportVersion_Implementation)
	// and OnKickDeadline re-runs the movement/pawnless guards before acting.
	// Server log only — no in-game announcement (see kKickGraceSec comment).
	const FString PlayerName = ResolveOwnerName(this);
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] no version report within %.0fs: %s missing plugin (server v%d) — kicking in %.0fs."),
		TimeoutSec, *PlayerName, NETCODE_PLUGIN_VERSION, kKickGraceSec);

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
	// Final corroboration re-check — covers a client whose pawn only just spawned and
	// started moving during the kKickGraceSec grace (so OnTimeout still saw 0). Moving
	// == functional == has the plugin → stand down instead of kicking.
	if (OwnerClientHasMoved(this))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] %s started moving during the kick grace — functional client, standing down."),
			*ResolveOwnerName(this));
		bConfirmed = true;
		Destroy();
		return;
	}

	// Pawnless by design at the final beat (they may have died / gone to spectate during
	// the grace) → defer, don't kick a legit spectator or eliminated round player.
	if (OwnerIsPawnlessByDesign(this))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] %s pawnless by design at the kick deadline — deferring %.0fs instead of kicking."),
			*ResolveOwnerName(this), kPawnlessDeferSec);
		if (UWorld* W = GetWorld())
		{
			W->GetTimerManager().SetTimer(
				KickHandle, this, &ANCVersionGate::OnKickDeadline,
				kPawnlessDeferSec, /*bLoop*/ false);
		}
		return;
	}

	bConfirmed = true;
	UE_LOG(LogGameMode, Warning,
		TEXT("[NCPlusVersionGate] kicking %s — no NetcodePlus plugin (server v%d)."),
		*ResolveOwnerName(this), NETCODE_PLUGIN_VERSION);
	KickOwner(FString::Printf(
		TEXT("NetcodePlus plugin required (server v%d) — not detected. Get the launcher at netcodeplus.com, install/update, then rejoin."),
		NETCODE_PLUGIN_VERSION));
}

// Latch: once the owner's pawn has ever moved, they demonstrably loaded the NCPlus pawn
// (⇒ have the plugin), so confirm + destroy immediately. Closes the point-sampling hole
// in OnTimeout/OnKickDeadline (which only look at the two deadline instants, and
// CurrentServerMoveTime doesn't persist across pawns): a legit client is cleared within
// ~kMoveWatchIntervalSec of its first move and can never be re-judged while pawnless.
void ANCVersionGate::OnMoveWatch()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}
	if (OwnerClientHasMoved(this))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] %s has moved — plugin confirmed via movement, standing down (latched)."),
			*ResolveOwnerName(this));
		bConfirmed = true;
		Destroy();   // Destroy() invalidates our Timeout/Kick/MoveWatch timers.
	}
}

// Advisor mode: the no-report check. Whisper the install pointer privately and
// re-arm at the nag cadence — never kick, never announce to anyone else. A lost
// handshake on a legit client costs one ignorable whisper (the wording says so);
// a real plugin-less player gets told while their UI still works (stock lobby
// HUD renders chat — unlike NCPlus instances, where they're deaf).
void ANCVersionGate::OnAdvisorCheck()
{
	if (Role != ROLE_Authority || bConfirmed)
	{
		return;
	}

	if (AdvisorNagCount == 0)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[NCPlusVersionGate] hub advisor: no version report from %s within %.0fs — whispering install pointer (nagging every %.0fs while unconfirmed)."),
			*ResolveOwnerName(this), kAdvisorFirstCheckSec, kAdvisorRepeatSec);
	}
	AdvisorNagCount++;

	WhisperOwner(TEXT("Matches on this hub require the NetcodePlus plugin — grab the launcher at netcodeplus.com (already installed? ignore this)."));

	if (UWorld* W = GetWorld())
	{
		W->GetTimerManager().SetTimer(
			TimeoutHandle, this, &ANCVersionGate::OnAdvisorCheck,
			kAdvisorRepeatSec, /*bLoop*/ false);
	}
}

// Private system-chat line to the owning player only. ClientSay is base-UT
// (AUTBasePlayerController) so it isn't plugin-gated, and on a hub the stock
// lobby HUD renders it even for plugin-less clients. Speaker = the recipient's
// own PlayerState (same pattern the rest of the codebase uses for system lines).
void ANCVersionGate::WhisperOwner(const FString& Msg)
{
	AUTBasePlayerController* PC = Cast<AUTBasePlayerController>(GetOwner());
	if (PC && PC->UTPlayerState)
	{
		PC->ClientSay(PC->UTPlayerState, Msg, ChatDestinations::System);
	}
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
	AUTBasePlayerController* PC = Cast<AUTBasePlayerController>(GetOwner());
	if (PC && !PC->IsPendingKillPending()
		&& PC->APlayerController::GetNetConnection() != nullptr)
	{
		// Use the stock non-banning implementation directly. The current shipped Linux
		// virtual slot matches, but qualification avoids depending on that ABI slot and
		// bypasses any native controller-subclass override.
		PC->AUTBasePlayerController::GuaranteedKick(
			FText::FromString(Reason), /*bKickToHubIfPossible*/ false);
	}
	Destroy();
}

namespace NCPlusVersionGate
{
	// Shared eligibility check for both modes. Bots have no client — nothing to
	// handshake with. The listen-host's local PC also has no remote: replicating
	// an owner-only actor to a local PC works, but it's churn for no win since
	// the host obviously has a matching version (same DLL load).
	static bool ShouldGate(APlayerController* PC)
	{
		if (PC == nullptr || !PC->HasAuthority() || PC->IsLocalController())
		{
			return false;
		}
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(PC);
		if (UTPC && UTPC->UTPlayerState && UTPC->UTPlayerState->bIsABot)
		{
			return false;
		}
		return PC->GetWorld() != nullptr;
	}

	void SpawnFor(APlayerController* PC)
	{
		if (!ShouldGate(PC))
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.Owner = PC;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PC->GetWorld()->SpawnActor<ANCVersionGate>(ANCVersionGate::StaticClass(), Params);
	}

	void SpawnAdvisorFor(APlayerController* PC)
	{
		if (!ShouldGate(PC))
		{
			return;
		}
		// Deferred spawn so bAdvisorMode is set BEFORE BeginPlay picks the timer set.
		ANCVersionGate* Gate = PC->GetWorld()->SpawnActorDeferred<ANCVersionGate>(
			ANCVersionGate::StaticClass(), FTransform::Identity,
			/*Owner*/ PC, /*Instigator*/ nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Gate)
		{
			Gate->bAdvisorMode = true;
			Gate->FinishSpawning(FTransform::Identity);
		}
	}

	// -----------------------------------------------------------------------
	// Hub auto-spawn: the stock lobby gamemode obviously never calls SpawnFor,
	// so hook the engine-level PostLogin event (fires for EVERY gamemode,
	// GameModeBase.cpp) and filter to lobby servers. This reaches hubs with
	// zero hub-side config: loading the plugin DLL is enough.
	// -----------------------------------------------------------------------
	static FDelegateHandle GHubAdvisorHandle;

	static void OnAnyGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
	{
		AUTBaseGameMode* Base = Cast<AUTBaseGameMode>(GameMode);
		if (Base && Base->IsLobbyServer())
		{
			SpawnAdvisorFor(NewPlayer);
		}
	}

	void RegisterHubAdvisor()
	{
		if (!GHubAdvisorHandle.IsValid())
		{
			GHubAdvisorHandle = FGameModeEvents::GameModePostLoginEvent.AddStatic(&OnAnyGameModePostLogin);
		}
	}

	void UnregisterHubAdvisor()
	{
		if (GHubAdvisorHandle.IsValid())
		{
			FGameModeEvents::GameModePostLoginEvent.Remove(GHubAdvisorHandle);
			GHubAdvisorHandle.Reset();
		}
	}
}
