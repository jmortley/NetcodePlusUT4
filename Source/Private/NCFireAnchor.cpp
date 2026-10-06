#include "NCFireAnchor.h"
#include "NCFireAnchorPolicy.h"
#include "UnrealTournament.h"
#include "UTCharacter.h"
#include "UTCharacterMovement.h"
#include "UTWeapon.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCFireAnchor, Log, All);
static TAutoConsoleVariable<int32> CVarAnchorMode(TEXT("ncp.FireAnchorMode"), 1,
    TEXT("329 precision hitscan: 0=off, 1=shadow (default), 2=bounded movement/fire skew compensation. Requires matching 329 peers."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarAnchorDebug(TEXT("ncp.FireAnchorDebug"), 0,
    TEXT("329 per-request anchor admission diagnostics; 0=off. Movement markers are not trusted click timestamps."), ECVF_Default);

namespace
{
    struct FMove
    {
        float LastTimestamp = -1.f;
        float LastProcessedAt = -1.f;
        float Timestamp = -1.f;
        float ProcessedAt = -1.f;
        uint64 Epoch = 0;
        bool bConsumed = true;
        FVector Eye = FVector::ZeroVector;
        FRotator Aim = FRotator::ZeroRotator;
        TWeakObjectPtr<AUTWeapon> Weapon;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<APlayerController> Controller;
        TWeakObjectPtr<UNetConnection> Connection;
    };
    TMap<TWeakObjectPtr<AUTCharacter>, FMove> Moves;
    uint64 NextEpoch = 1;
    FDelegateHandle CleanupHandle;

    uint64 AllocateEpoch()
    {
        const uint64 Result = NextEpoch++;
        if (NextEpoch == 0) NextEpoch = 1;
        return Result;
    }

    void ResetMarker(FMove& Move)
    {
        Move.Epoch = AllocateEpoch();
        Move.Timestamp = -1.f;
        Move.ProcessedAt = -1.f;
        Move.Weapon.Reset();
        Move.bConsumed = true;
    }

    void CleanupWorld(UWorld* World, bool, bool)
    {
        for (auto It = Moves.CreateIterator(); It; ++It)
            if (!It.Key().IsValid() || !It.Value().World.IsValid()
                || It.Value().World.Get() == World) It.RemoveCurrent();
    }

    void Prune()
    {
        for (auto It = Moves.CreateIterator(); It; ++It)
            if (!It.Key().IsValid() || !It.Value().World.IsValid()
                || !It.Value().Controller.IsValid() || !It.Value().Connection.IsValid())
                It.RemoveCurrent();
    }

    APlayerController* RemoteController(AUTCharacter* Pawn)
    {
        APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
        return PC && !PC->IsPendingKillPending() && !PC->IsLocalController()
            && PC->GetWorld() == Pawn->GetWorld() && PC->GetNetConnection() ? PC : nullptr;
    }

    bool HasTeleportSince(AUTCharacter* Pawn, float Since)
    {
        if (!Pawn->UTCharacterMovement || Pawn->UTCharacterMovement->bJustTeleported) return true;
        for (const FSavedPosition& Position : Pawn->SavedPositions)
            if (Position.bTeleported && Position.Time >= Since) return true;
        return false;
    }

    bool HasRecentAck(UNetConnection* Connection)
    {
        // This proves recent ACK traffic, NOT the age of the last valid AvgLag
        // measurement (the engine exposes no last-successful-sample timestamp).
        // Refuse extra compensation when liveness is unknown; legacy validation
        // remains available. Compare both times in the net driver's time domain.
        return Connection && Connection->State == USOCK_Open && Connection->Driver
            && NCFireAnchorPolicy::AckIsRecent(Connection->Driver->Time, Connection->LastRecvAckTime);
    }
}

void NCFireAnchor::Startup()
{
    if (!CleanupHandle.IsValid()) CleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&CleanupWorld);
}

void NCFireAnchor::Shutdown()
{
    if (CleanupHandle.IsValid())
    {
        FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
        CleanupHandle.Reset();
    }
    Moves.Empty();
}

int32 NCFireAnchor::Mode() { return FMath::Clamp(CVarAnchorMode.GetValueOnGameThread(), 0, 2); }
bool NCFireAnchor::LogEnabled() { return CVarAnchorDebug.GetValueOnGameThread() > 0; }

void NCFireAnchor::RecordMove(AUTCharacter* Pawn, bool bShotSpawned)
{
    if (!Pawn || Pawn->Role != ROLE_Authority || !Pawn->GetWorld() || !Pawn->UTCharacterMovement) return;
    Prune();
    APlayerController* PC = RemoteController(Pawn);
    if (!PC || Pawn->IsDead() || Pawn->IsPendingKillPending())
    {
        Moves.Remove(TWeakObjectPtr<AUTCharacter>(Pawn));
        return;
    }
    FMove& M = Moves.FindOrAdd(TWeakObjectPtr<AUTCharacter>(Pawn));
    const float Stamp = Pawn->GetCurrentSynchTime(false);
    const float Now = Pawn->GetWorld()->GetTimeSeconds();
    if (!FMath::IsFinite(Stamp) || Stamp < 0.f || !FMath::IsFinite(Now))
    {
        ResetMarker(M);
        return;
    }
    const bool bNewIdentity = M.World.Get() != Pawn->GetWorld() || M.Controller.Get() != PC
        || M.Connection.Get() != PC->GetNetConnection();
    if (bNewIdentity || Stamp < M.LastTimestamp || Now < M.LastProcessedAt
        || Pawn->UTCharacterMovement->bJustTeleported
        || (M.Weapon.IsValid() && M.Weapon.Get() != Pawn->GetWeapon()))
    {
        ResetMarker(M);
        if (bNewIdentity) M.LastTimestamp = -1.f;
    }
    M.World = Pawn->GetWorld();
    M.Controller = PC;
    M.Connection = PC->GetNetConnection();
    const bool bNewStamp = Stamp != M.LastTimestamp;
    M.LastTimestamp = Stamp;
    M.LastProcessedAt = Now;
    if (!bShotSpawned || !bNewStamp || Pawn->UTCharacterMovement->bJustTeleported) return;
    AUTWeapon* Weapon = Pawn->GetWeapon();
    const FVector Eye = Pawn->GetPawnViewLocation();
    const FRotator Aim = Pawn->GetViewRotation();
    if (!Weapon || Weapon->IsPendingKillPending() || Weapon->GetUTOwner() != Pawn
        || Eye.ContainsNaN() || Aim.ContainsNaN())
    {
        ResetMarker(M);
        return;
    }
    // Capture before later NotifyPendingServerFire changes SavedPositions.
    // Only the newest observed shot marker is eligible, not a searchable history.
    M.Timestamp = Stamp;
    M.ProcessedAt = Now;
    M.Eye = Eye;
    M.Aim = Aim;
    M.Weapon = Weapon;
    M.bConsumed = false;
}

void NCFireAnchor::InvalidateWeapon(AUTWeapon* Weapon)
{
    Prune();
    for (auto& Pair : Moves)
        if (Pair.Value.Weapon.Get() == Weapon) ResetMarker(Pair.Value);
}

FNCFireAnchor NCFireAnchor::Resolve(AUTWeapon* Weapon, AUTCharacter* Pawn,
    uint8 FireMode, int32 Event, float MoveTime, const FVector& Origin,
    const FRotator& Aim, float BaseRewind, float RTT, float Cap)
{
    FNCFireAnchor A;
    const int32 PolicyMode = Mode();
    if (!PolicyMode || !Weapon || !Pawn || !Pawn->GetWorld() || Pawn->Role != ROLE_Authority) return A;
    Prune();
    APlayerController* PC = RemoteController(Pawn);
    FMove* M = Moves.Find(TWeakObjectPtr<AUTCharacter>(Pawn));
    const float Now = Pawn->GetWorld()->GetTimeSeconds();
    NCFireAnchorPolicy::Input I;
    I.Now = Now;
    I.BaseRewind = BaseRewind;
    I.Cap = FMath::IsFinite(Cap) ? FMath::Min(Cap, 0.125f) : Cap;
    // Match only the current remote identity and exact original float stamp.
    // An approximate lookup could choose another high-FPS sample or reset epoch.
    I.Match = PC && M && Pawn->UTCharacterMovement
        && M->World.Get() == Pawn->GetWorld() && M->Controller.Get() == PC
        && M->Connection.Get() == PC->GetNetConnection() && M->Weapon.Get() == Weapon
        && !Pawn->IsDead() && !Pawn->IsPendingKillPending() && !Weapon->IsPendingKillPending()
        && Pawn->GetWeapon() == Weapon && Weapon->GetUTOwner() == Pawn
        && Weapon->GetWorld() == Pawn->GetWorld() && !HasTeleportSince(Pawn, M->ProcessedAt)
        && FMath::IsFinite(MoveTime) && MoveTime >= 0.f && MoveTime == M->Timestamp;
    const bool bFinitePayload = !Origin.ContainsNaN() && !Aim.ContainsNaN()
        && FMath::IsFinite(RTT) && RTT >= 0.f && RTT < 5000.f;
    if (I.Match)
    {
        I.Consumed = M->bConsumed;
        I.MoveProcessed = M->ProcessedAt;
        // A matched marker is one-shot even for malformed/rejected claims.
        M->bConsumed = true;
        if (bFinitePayload)
        {
            I.OriginXY = (Origin - M->Eye).Size2D();
            I.OriginZ = Origin.Z - M->Eye.Z;
            I.AimDelta = FMath::Max(FMath::Abs(FMath::FindDeltaAngleDegrees(Aim.Pitch, M->Aim.Pitch)),
                FMath::Abs(FMath::FindDeltaAngleDegrees(Aim.Yaw, M->Aim.Yaw)));
            // Never send NaN/inf or already-ineligible claims into a physics query.
            if (!NCFireAnchorPolicy::Admit(I))
            {
                FCollisionQueryParams Params(FName(TEXT("NC329FireOrigin")), true, Pawn);
                FHitResult Hit;
                I.Obstructed = Pawn->GetWorld()->LineTraceSingleByChannel(Hit, M->Eye, Origin, ECC_Visibility, Params);
            }
        }
    }
    const char* Reason = !bFinitePayload ? "nonfinite" : NCFireAnchorPolicy::Admit(I);
    if (!Reason && !HasRecentAck(PC->GetNetConnection())) Reason = "ack_not_recent";
    if (!Reason)
    {
        A.bValid = true;
        A.bEnforce = PolicyMode == 2;
        A.AcceptedAt = Now;
        A.MoveProcessedAt = M->ProcessedAt;
        A.ExtraAtAccept = Now - M->ProcessedAt;
        A.BaseRewind = BaseRewind;
        A.ObservedRTTMs = RTT;
        A.Cap = I.Cap;
        A.Epoch = M->Epoch;
        A.Origin = Origin;
        A.Pawn = Pawn;
        A.Weapon = Weapon;
        A.World = Pawn->GetWorld();
        A.Controller = PC;
        A.Connection = PC->GetNetConnection();
    }
    if (LogEnabled())
    {
        const UNetConnection* Connection = PC ? PC->GetNetConnection() : nullptr;
        const double AckAgeMs = Connection && Connection->Driver
            ? (Connection->Driver->Time - Connection->LastRecvAckTime) * 1000.0 : -1.0;
        const float MarkerAgeMs = M && M->ProcessedAt >= 0.f
            ? (Now - M->ProcessedAt) * 1000.f : -1.f;
        // Preserve the raw marker age on rejection: a zero-filled invalid anchor
        // must not hide an 81+ ms transport gap during the shadow investigation.
        UE_LOG(LogNCFireAnchor, Log,
            TEXT("[FireAnchor329] event=%d mode=%d weapon=%s policy=%d result=%s move=%.6f observed=%.6f matched=%d ageMs=%.3f baseMs=%.2f capMs=%.2f originXY=%.2f originZ=%.2f aimDeg=%.2f ackAgeMs=%.1f"),
            Event, FireMode, *Weapon->GetName(), PolicyMode, Reason ? ANSI_TO_TCHAR(Reason) : TEXT("candidate"),
            MoveTime, M ? M->Timestamp : -1.f, I.Match ? 1 : 0, MarkerAgeMs,
            BaseRewind * 1000.f, I.Cap * 1000.f, I.OriginXY, I.OriginZ, I.AimDelta, AckAgeMs);
    }
    return A;
}

bool NCFireAnchor::Dispatch(const FNCFireAnchor& A, float Now, float& Extra)
{
    Extra = 0.f;
    const FMove* M = A.Pawn.IsValid() ? Moves.Find(A.Pawn) : nullptr;
    return A.bValid && A.Pawn.IsValid() && A.Weapon.IsValid() && A.World.IsValid()
        && A.Controller.IsValid() && A.Connection.IsValid() && M && M->Epoch == A.Epoch
        && M->World == A.World && M->Controller == A.Controller && M->Connection == A.Connection
        && A.Pawn->GetWorld() == A.World.Get() && A.Weapon->GetWorld() == A.World.Get()
        && A.Pawn->GetController() == A.Controller.Get()
        && A.Controller->GetWorld() == A.World.Get()
        && A.Controller->GetNetConnection() == A.Connection.Get()
        && A.Pawn->Role == ROLE_Authority && !A.Pawn->IsPendingKillPending()
        && !A.Weapon->IsPendingKillPending() && A.Pawn->GetWeapon() == A.Weapon.Get()
        && A.Weapon->GetUTOwner() == A.Pawn.Get() && !A.Pawn->IsDead()
        && !HasTeleportSince(A.Pawn.Get(), A.MoveProcessedAt)
        && HasRecentAck(A.Connection.Get())
        && NCFireAnchorPolicy::DispatchAge(Now, A.AcceptedAt, A.ExtraAtAccept, A.BaseRewind, A.Cap, Extra);
}
