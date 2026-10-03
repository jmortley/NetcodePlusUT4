// NCPlusVersionGate.cpp -- see header.
#include "NCPlusVersionGate.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"
#include "UTATypes.h"
#include "UTBaseGameMode.h"
#include "UTPlayerState.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "TimerManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

static const float kVersionReportTimeoutDefault = 100.f;
static const float kVersionReportTimeoutMin = 1.f;
static const float kVersionReportTimeoutMax = 120.f;
static const float kKickGraceSec = 5.f;
static const float kAdvisorFirstCheckSec = 60.f;
static const float kAdvisorRepeatSec = 180.f;

namespace
{
    // Server-only, game-thread state. No replicated properties or new gate RPCs.
    // A successful hub report does not authorize a match protocol. Travel and
    // reconnect must establish a fresh identity even if a controller survives.
    enum class EProtocolState { Pending, Confirmed, Rejected };
    struct FProtocolSession
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<UNetConnection> Connection;
        TWeakObjectPtr<ANCVersionGate> Gate;
        EProtocolState State = EProtocolState::Pending;
        bool bAdvisor = false;
    };
    TMap<TWeakObjectPtr<APlayerController>, FProtocolSession> ProtocolSessions;
    FDelegateHandle ProtocolCleanupHandle;
    FDelegateHandle ProtocolLogoutHandle;

    // BEGIN PROTOCOL IDENTITY CORE (compiled by the native adapter tests).
    bool SessionIdentityMatches(const FProtocolSession& Entry, APlayerController* PC)
    {
        return PC && !PC->IsPendingKillPending() && PC->HasAuthority()
            && Entry.World.IsValid() && Entry.World.Get() == PC->GetWorld()
            && Entry.Connection.IsValid() && Entry.Connection.Get() == PC->GetNetConnection()
            && Entry.Connection->State == USOCK_Open;
    }

    bool IsConfirmedSession(const FProtocolSession& Entry, APlayerController* PC)
    {
        return SessionIdentityMatches(Entry, PC) && !Entry.bAdvisor
            && Entry.State == EProtocolState::Confirmed;
    }

    bool RecordVersion(FProtocolSession& Entry, APlayerController* PC,
        ANCVersionGate* Gate, int32 ClientVersion)
    {
        if (!SessionIdentityMatches(Entry, PC) || Entry.Gate.Get() != Gate
            || Entry.State != EProtocolState::Pending)
        {
            return false;
        }
        Entry.State = ClientVersion == NETCODE_PLUGIN_VERSION
            ? EProtocolState::Confirmed : EProtocolState::Rejected;
        return true;
    }
    // END PROTOCOL IDENTITY CORE

    void PruneProtocolSessions()
    {
        for (auto It = ProtocolSessions.CreateIterator(); It; ++It)
        {
            if (!SessionIdentityMatches(It.Value(), It.Key().Get())
                || (It.Value().State == EProtocolState::Pending && !It.Value().Gate.IsValid()))
                It.RemoveCurrent();
        }
    }

    bool GateOwnsSession(ANCVersionGate* Gate)
    {
        APlayerController* PC = Gate ? Cast<APlayerController>(Gate->GetOwner()) : nullptr;
        FProtocolSession* Entry = ProtocolSessions.Find(TWeakObjectPtr<APlayerController>(PC));
        return Entry && SessionIdentityMatches(*Entry, PC) && Entry->Gate.Get() == Gate;
    }

    void CleanupProtocolWorld(UWorld* World, bool, bool)
    {
        for (auto It = ProtocolSessions.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid() || !It.Value().World.IsValid()
                || It.Value().World.Get() == World) It.RemoveCurrent();
        }
    }

    void OnProtocolLogout(AGameModeBase*, AController* Exiting)
    {
        APlayerController* PC = Cast<APlayerController>(Exiting);
        const TWeakObjectPtr<APlayerController> Key(PC);
        FProtocolSession* Entry = ProtocolSessions.Find(Key);
        ANCVersionGate* Gate = Entry ? Entry->Gate.Get() : nullptr;
        ProtocolSessions.Remove(Key);
        if (Gate) Gate->Destroy();
    }

    void EnsureProtocolLifecycle()
    {
        // SpawnFor is also used on listen servers where hub registration is not run.
        if (!ProtocolCleanupHandle.IsValid())
            ProtocolCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&CleanupProtocolWorld);
        if (!ProtocolLogoutHandle.IsValid())
            ProtocolLogoutHandle = FGameModeEvents::GameModeLogoutEvent.AddStatic(&OnProtocolLogout);
    }
}

static float ResolveVersionReportTimeoutSec()
{
    const FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");
    if (!FPaths::FileExists(ModIniPath)) return kVersionReportTimeoutDefault;
    FConfigFile ModIni;
    ModIni.Read(ModIniPath);
    const FConfigSection* Section = ModIni.Find(TEXT("NetcodePlus"));
    if (!Section) return kVersionReportTimeoutDefault;
    const FConfigValue* Value = Section->Find(FName(TEXT("VersionReportTimeoutSec")));
    if (!Value) return kVersionReportTimeoutDefault;
    const float Parsed = FCString::Atof(*Value->GetValue());
    if (!FMath::IsFinite(Parsed) || Parsed <= 0.f) return kVersionReportTimeoutDefault;
    return FMath::Clamp(Parsed, kVersionReportTimeoutMin, kVersionReportTimeoutMax);
}

static FString ResolveOwnerName(AActor* Gate)
{
    APlayerController* PC = Gate ? Cast<APlayerController>(Gate->GetOwner()) : nullptr;
    return (PC && PC->PlayerState) ? PC->PlayerState->PlayerName : FString(TEXT("<unknown>"));
}

ANCVersionGate::ANCVersionGate(const FObjectInitializer& OI)
    : Super(OI), bAdvisorMode(false), bConfirmed(false), AdvisorNagCount(0)
    , TimeoutSec(kVersionReportTimeoutDefault)
{
    bReplicates = true;
    bOnlyRelevantToOwner = true;
    PrimaryActorTick.bCanEverTick = false;
    NetUpdateFrequency = 1.f;
}

void ANCVersionGate::BeginPlay()
{
    Super::BeginPlay();
    if (Role != ROLE_Authority) return;
    if (!GateOwnsSession(this))
    {
        Destroy();
        return;
    }
    if (UWorld* World = GetWorld())
    {
        if (bAdvisorMode)
        {
            World->GetTimerManager().SetTimer(TimeoutHandle, this,
                &ANCVersionGate::OnAdvisorCheck, kAdvisorFirstCheckSec, false);
        }
        else
        {
            // A loaded pawn proves neither the exact protocol nor compatible
            // RPC parameters. Pending players cannot fire during this grace.
            TimeoutSec = ResolveVersionReportTimeoutSec();
            World->GetTimerManager().SetTimer(TimeoutHandle, this,
                &ANCVersionGate::OnTimeout, TimeoutSec, false);
        }
    }
}

void ANCVersionGate::PostNetInit()
{
    Super::PostNetInit();
    if (Role != ROLE_Authority) ServerReportVersion(NETCODE_PLUGIN_VERSION);
}

void ANCVersionGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(TimeoutHandle);
        World->GetTimerManager().ClearTimer(KickHandle);
    }
    // Unexpected gate destruction must not leave an unanswerable pending gate.
    // Completed confirmation/rejection stays bound to its session until logout
    // or world cleanup; destroying the owner-only actor is normal on success.
    if (Role == ROLE_Authority)
    {
        APlayerController* PC = Cast<APlayerController>(GetOwner());
        const TWeakObjectPtr<APlayerController> Key(PC);
        FProtocolSession* Entry = ProtocolSessions.Find(Key);
        if (Entry && Entry->Gate.Get() == this && Entry->State == EProtocolState::Pending)
            ProtocolSessions.Remove(Key);
    }
    Super::EndPlay(EndPlayReason);
}

bool ANCVersionGate::ServerReportVersion_Validate(int32 /*ClientVersion*/)
{
    // Accept decoding any version so the implementation can provide a helpful
    // mismatch reason instead of the engine's generic RPC validation failure.
    return true;
}

void ANCVersionGate::ServerReportVersion_Implementation(int32 ClientVersion)
{
    if (Role != ROLE_Authority || bConfirmed) return;
    APlayerController* PC = Cast<APlayerController>(GetOwner());
    FProtocolSession* Entry = ProtocolSessions.Find(TWeakObjectPtr<APlayerController>(PC));
    if (!Entry || !RecordVersion(*Entry, PC, this, ClientVersion))
    {
        Destroy(); // An old world's/connection's gate cannot approve the new one.
        return;
    }
    bConfirmed = true;
    if (ClientVersion == NETCODE_PLUGIN_VERSION)
    {
        UE_LOG(LogGameMode, Log,
            TEXT("[NCPlusVersionGate] %s confirmed exact v%d (%s)."),
            *ResolveOwnerName(this), ClientVersion, bAdvisorMode ? TEXT("hub advice") : TEXT("match protocol"));
        Destroy();
        return;
    }
    if (bAdvisorMode)
    {
        UE_LOG(LogGameMode, Warning,
            TEXT("[NCPlusVersionGate] hub advisor: %s reported v%d, server v%d; update required for matches."),
            *ResolveOwnerName(this), ClientVersion, NETCODE_PLUGIN_VERSION);
        WhisperOwner(FString::Printf(
            TEXT("Your NetcodePlus plugin is v%d; matches on this hub run v%d. Update via the launcher at netcodeplus.com."),
            ClientVersion, NETCODE_PLUGIN_VERSION));
        Destroy();
        return;
    }
    UE_LOG(LogGameMode, Warning,
        TEXT("[NCPlusVersionGate] kicking owner: client v%d != server v%d (player: %s)."),
        ClientVersion, NETCODE_PLUGIN_VERSION, *ResolveOwnerName(this));
    KickOwner(FString::Printf(
        TEXT("NetcodePlus version mismatch: server is v%d, you are v%d. Update via the launcher at netcodeplus.com."),
        NETCODE_PLUGIN_VERSION, ClientVersion));
}

void ANCVersionGate::OnTimeout()
{
    if (Role != ROLE_Authority || bConfirmed) return;
    if (!GateOwnsSession(this))
    {
        Destroy();
        return;
    }
    // Silence is unknown, not proof the plugin is absent. Spectators and
    // eliminated players require the same exact report as active players.
    UE_LOG(LogGameMode, Warning,
        TEXT("[NCPlusVersionGate] no exact version report within %.0fs from %s (server v%d); protocol remains blocked, disconnect in %.0fs."),
        TimeoutSec, *ResolveOwnerName(this), NETCODE_PLUGIN_VERSION, kKickGraceSec);
    if (UWorld* World = GetWorld())
        World->GetTimerManager().SetTimer(KickHandle, this,
            &ANCVersionGate::OnKickDeadline, kKickGraceSec, false);
}

void ANCVersionGate::OnKickDeadline()
{
    if (Role != ROLE_Authority || bConfirmed) return;
    if (!GateOwnsSession(this))
    {
        Destroy();
        return;
    }
    APlayerController* PC = Cast<APlayerController>(GetOwner());
    FProtocolSession* Entry = ProtocolSessions.Find(TWeakObjectPtr<APlayerController>(PC));
    if (Entry) Entry->State = EProtocolState::Rejected;
    bConfirmed = true;
    UE_LOG(LogGameMode, Warning,
        TEXT("[NCPlusVersionGate] kicking %s; v%d protocol handshake was not confirmed."),
        *ResolveOwnerName(this), NETCODE_PLUGIN_VERSION);
    KickOwner(FString::Printf(
        TEXT("NetcodePlus v%d handshake was not confirmed. Install/update via netcodeplus.com, then rejoin."),
        NETCODE_PLUGIN_VERSION));
}

void ANCVersionGate::OnAdvisorCheck()
{
    if (Role != ROLE_Authority || bConfirmed) return;
    if (!GateOwnsSession(this))
    {
        Destroy();
        return;
    }
    if (AdvisorNagCount++ == 0)
        UE_LOG(LogGameMode, Warning,
            TEXT("[NCPlusVersionGate] hub advisor: no version report from %s; whispering install pointer."),
            *ResolveOwnerName(this));
    WhisperOwner(TEXT("Matches on this hub require the NetcodePlus plugin; grab the launcher at netcodeplus.com (already installed? ignore this)."));
    if (UWorld* World = GetWorld())
        World->GetTimerManager().SetTimer(TimeoutHandle, this,
            &ANCVersionGate::OnAdvisorCheck, kAdvisorRepeatSec, false);
}

void ANCVersionGate::WhisperOwner(const FString& Msg)
{
    AUTBasePlayerController* PC = Cast<AUTBasePlayerController>(GetOwner());
    if (PC && PC->UTPlayerState) PC->ClientSay(PC->UTPlayerState, Msg, ChatDestinations::System);
}

void ANCVersionGate::KickOwner(const FString& Reason)
{
    // Non-banning disconnect. GameSession->KickPlayer would instance-ban and
    // can hide the reason by returning to the hub; keep the existing safe path.
    AUTBasePlayerController* PC = Cast<AUTBasePlayerController>(GetOwner());
    if (GateOwnsSession(this) && PC && !PC->IsPendingKillPending()
        && PC->APlayerController::GetNetConnection() != nullptr)
        PC->AUTBasePlayerController::GuaranteedKick(FText::FromString(Reason), false);
    Destroy();
}

namespace NCPlusVersionGate
{
    static bool IsExempt(APlayerController* PC)
    {
        return PC && (PC->IsLocalController()
            || (PC->PlayerState && PC->PlayerState->bIsABot));
    }

    static void SpawnGate(APlayerController* PC, bool bAdvisor)
    {
        if (!PC || !PC->HasAuthority() || PC->IsPendingKillPending() || IsExempt(PC)
            || !PC->GetWorld() || !PC->GetNetConnection()
            || PC->GetNetConnection()->State != USOCK_Open) return;
        EnsureProtocolLifecycle();
        PruneProtocolSessions();
        const TWeakObjectPtr<APlayerController> Key(PC);
        FProtocolSession* Existing = ProtocolSessions.Find(Key);
        if (Existing)
        {
            // Duplicate PostLogin/hooks must not reset a mismatch or restart a
            // pending timeout. A hub-advisor entry may be upgraded to a match
            // entry, never the reverse; it still needs a new exact report.
            if (!Existing->bAdvisor || bAdvisor) return;
            ANCVersionGate* OldGate = Existing->Gate.Get();
            ProtocolSessions.Remove(Key);
            if (OldGate) OldGate->Destroy();
        }
        ANCVersionGate* Gate = PC->GetWorld()->SpawnActorDeferred<ANCVersionGate>(
            ANCVersionGate::StaticClass(), FTransform::Identity, PC, nullptr,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Gate) return; // Failure remains unknown/blocked; next query retries.
        Gate->bAdvisorMode = bAdvisor;
        FProtocolSession Entry;
        Entry.World = PC->GetWorld();
        Entry.Connection = PC->GetNetConnection();
        Entry.Gate = Gate;
        Entry.bAdvisor = bAdvisor;
        ProtocolSessions.Add(Key, Entry);
        Gate->FinishSpawning(FTransform::Identity);
    }

    void SpawnFor(APlayerController* PC) { SpawnGate(PC, false); }
    void SpawnAdvisorFor(APlayerController* PC) { SpawnGate(PC, true); }

    bool IsProtocolConfirmed(APlayerController* PC)
    {
        if (!PC || !PC->HasAuthority() || PC->IsPendingKillPending() || !PC->GetWorld()) return false;
        if (IsExempt(PC)) return true;
        PruneProtocolSessions();
        FProtocolSession* Entry = ProtocolSessions.Find(TWeakObjectPtr<APlayerController>(PC));
        if (Entry && IsConfirmedSession(*Entry, PC)) return true;
        AUTBaseGameMode* GameMode = Cast<AUTBaseGameMode>(PC->GetWorld()->GetAuthGameMode());
        if (GameMode && !GameMode->IsLobbyServer()) SpawnFor(PC);
        return false;
    }

    static FDelegateHandle GHubAdvisorHandle;
    static void OnAnyGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
    {
        AUTBaseGameMode* Base = Cast<AUTBaseGameMode>(GameMode);
        if (Base && Base->IsLobbyServer()) SpawnAdvisorFor(NewPlayer);
    }

    void RegisterHubAdvisor()
    {
        EnsureProtocolLifecycle();
        if (!GHubAdvisorHandle.IsValid())
            GHubAdvisorHandle = FGameModeEvents::GameModePostLoginEvent.AddStatic(&OnAnyGameModePostLogin);
    }

    void UnregisterHubAdvisor()
    {
        if (GHubAdvisorHandle.IsValid())
        {
            FGameModeEvents::GameModePostLoginEvent.Remove(GHubAdvisorHandle);
            GHubAdvisorHandle.Reset();
        }
        if (ProtocolCleanupHandle.IsValid())
        {
            FWorldDelegates::OnWorldCleanup.Remove(ProtocolCleanupHandle);
            ProtocolCleanupHandle.Reset();
        }
        if (ProtocolLogoutHandle.IsValid())
        {
            FGameModeEvents::GameModeLogoutEvent.Remove(ProtocolLogoutHandle);
            ProtocolLogoutHandle.Reset();
        }
        // Timers are bound to the gate actor and cleared by EndPlay. Clear the
        // registry before destruction so no callbacks can see a live permission.
        TArray<TWeakObjectPtr<ANCVersionGate>> Gates;
        for (auto It = ProtocolSessions.CreateIterator(); It; ++It) Gates.Add(It.Value().Gate);
        ProtocolSessions.Empty();
        for (const TWeakObjectPtr<ANCVersionGate>& Gate : Gates)
            if (Gate.IsValid()) Gate->Destroy();
    }
}
