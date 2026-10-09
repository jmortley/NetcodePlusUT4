#include "NCRocketVolley.h"
#include "UTPlusWeap_RocketLauncher.h"
#include "UTWeaponStateFiringChargedRocket_Transactional.h"
#include "UTPlusProj_Rocket.h"
#include "NCPlusVersionGate.h"
#include "NCClientFireTiming.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTGameState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"

static TAutoConsoleVariable<int32> CVarRocketVolleyDebug(TEXT("ncp.RocketVolleyDebug"), 0,
    TEXT("329 loaded rocket identities/results: 0=off, 1=volley and per-rocket outcomes."), ECVF_Default);

bool AUTPlusWeap_RocketLauncher::CanBeginLoadedVolleyInput()
{
    AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
    return UTOwner && !UTOwner->IsDead() && !UTOwner->IsFiringDisabled()
        && UTOwner->GetWeapon() == this && UTOwner->GetPendingWeapon() == nullptr
        && !bDisableAltLoading && HasAmmo(1) && CurrentState && CurrentState != InactiveState
        && CurrentState != UnequippingState && FiringState.IsValidIndex(1)
        && (!GS || !GS->PreventWeaponFire());
}

bool AUTPlusWeap_RocketLauncher::CanBeginLoadedVolley()
{
    return LoadedOwnershipEpoch != 0 && CanBeginLoadedVolleyInput();
}

void AUTPlusWeap_RocketLauncher::ClearLoadedVolleyInput()
{
    if (UWorld* PendingWorld = PendingLoadedVolleyWorld.Get())
        PendingWorld->GetTimerManager().ClearTimer(PendingLoadedVolleyInputHandle);
    bPendingLoadedVolleyInput = bPendingLoadedVolleyRelease = false;
    PendingLoadedVolleyInputAt = 0.0;
    PendingLoadedVolleyPawn.Reset();
    PendingLoadedVolleyWorld.Reset();
    PendingLoadedVolleyController.Reset();
}

void AUTPlusWeap_RocketLauncher::BufferLoadedVolleyInput()
{
    if (bPendingLoadedVolleyInput || bHandlingRetry || LoadedOwnershipEpoch != 0 || HasLoadedVolley()
        || Role == ROLE_Authority || !GetWorld() || !CanBeginLoadedVolleyInput()
        || !UTOwner->IsLocallyControlled() || !UTOwner->Controller) return;
    bPendingLoadedVolleyInput = true;
    bPendingLoadedVolleyRelease = false;
    PendingLoadedVolleyInputAt = FPlatformTime::Seconds();
    PendingLoadedVolleyPawn = UTOwner;
    PendingLoadedVolleyWorld = GetWorld();
    PendingLoadedVolleyController = UTOwner->Controller;
    // Stock Active/Equipping states must not treat this as permission to load.
    UTOwner->SetPendingFire(1, false);
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] ownership_input_buffered weapon=%s"), *GetName());
    TryDrainLoadedVolleyInput();
}

void AUTPlusWeap_RocketLauncher::TryDrainLoadedVolleyInput()
{
    if (!bPendingLoadedVolleyInput) return;
    const double Age = FPlatformTime::Seconds() - PendingLoadedVolleyInputAt;
    if (Role == ROLE_Authority || !GetWorld() || PendingLoadedVolleyWorld.Get() != GetWorld()
        || !UTOwner || PendingLoadedVolleyPawn.Get() != UTOwner
        || !UTOwner->Controller || PendingLoadedVolleyController.Get() != UTOwner->Controller
        || !UTOwner->IsLocallyControlled() || !CanBeginLoadedVolleyInput() || HasLoadedVolley()
        || !FMath::IsFinite(Age) || Age < 0.0 || Age >= NCRocketVolley::OwnershipInputWindowSeconds)
    {
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Log, TEXT("[RocketVolley] ownership_input_cancelled weapon=%s age=%.3f"), *GetName(), Age);
        ClearLoadedVolleyInput();
        return;
    }
    if (LoadedOwnershipEpoch == 0)
    {
        UTOwner->SetPendingFire(1, false);
        GetWorldTimerManager().SetTimer(PendingLoadedVolleyInputHandle, this,
            &AUTPlusWeap_RocketLauncher::TryDrainLoadedVolleyInput, 0.005f, false);
        return;
    }

    const bool bReleased = bPendingLoadedVolleyRelease;
    AUTCharacter* ExpectedPawn = PendingLoadedVolleyPawn.Get();
    AController* ExpectedController = PendingLoadedVolleyController.Get();
    UWorld* ExpectedWorld = PendingLoadedVolleyWorld.Get();
    const uint32 ExpectedEpoch = LoadedOwnershipEpoch;
    const uint32 ExpectedId = NCRocketVolley::Next(LastClientLoadedVolleyId);
    // Consume before dispatch: epoch notifications and retry timers cannot
    // manufacture a second request. All load/refire/ammo rules stay on StartFire.
    ClearLoadedVolleyInput();
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] ownership_input_ready weapon=%s epoch=%u age=%.3f released=%d"),
            *GetName(), LoadedOwnershipEpoch, Age, bReleased ? 1 : 0);
    StartFire(1);
    if (bReleased && HasLoadedVolley() && LoadedVolley.Id == ExpectedId
        && LoadedVolleyEpoch == ExpectedEpoch && LoadedOwnershipEpoch == ExpectedEpoch
        && GetWorld() == ExpectedWorld && UTOwner == ExpectedPawn && LoadedVolleyPawn.Get() == ExpectedPawn
        && UTOwner->Controller == ExpectedController && !UTOwner->IsDead() && !UTOwner->IsFiringDisabled()
        && UTOwner->GetWeapon() == this && UTOwner->GetPendingWeapon() == nullptr)
        StopFire(1);
}

bool AUTPlusWeap_RocketLauncher::IsLoadedVolleyModeValid(uint8 Mode) const
{
    if (Mode == 0) return RocketFireModes.IsValidIndex(0) && RocketFireModes[0].ProjClass;
    if (Mode == 1) return (bAllowGrenades || bAllowAltModes)
        && RocketFireModes.IsValidIndex(1) && RocketFireModes[1].ProjClass;
    if (Mode == 2) return bAllowAltModes &&
        ((RocketFireModes.IsValidIndex(2) && RocketFireModes[2].ProjClass) || SpiralRocketClass);
    return false;
}

void AUTPlusWeap_RocketLauncher::ResetLoadedVolley(uint32 Id)
{
    LoadedVolley = NCRocketVolley::FProgress();
    LoadedVolleyEpoch = LoadedOwnershipEpoch;
    LoadedVolley.Begin(Id);
    LoadedVolleyPawn = UTOwner;
    LoadedVolleyRequestedCount = LoadedVolleySelectedMode = LoadedVolleyNextOrdinal = 0;
    LoadedVolleyAmmoSpent = 0;
    bLoadedVolleyEnteredState = false;
    LoadedVolleyBeginRequestedAt = GetWorld()->GetTimeSeconds();
    bLoadedVolleyReleaseSent = bLoadedVolleyReleaseReceived = false;
}

void AUTPlusWeap_RocketLauncher::StartFire(uint8 FireModeNum)
{
    if (FireModeNum != 1)
    {
        // A fresh primary press replaces the intent. An older primary retry
        // may wake during initialization and must not erase this newer input.
        if (!bHandlingRetry) ClearLoadedVolleyInput();
        Super::StartFire(FireModeNum);
        return;
    }
    AUTWeapon* const IncomingWeapon = UTOwner ? UTOwner->GetPendingWeapon() : nullptr;
    if (UTOwner && (CurrentState == UnequippingState || (IncomingWeapon && IncomingWeapon != this)))
    {
        // Alt pressed while this launcher is being put away belongs to the incoming
        // weapon, as in stock and 328. AUTWeaponFix's swap-away branch latches the
        // pawn's held bit and returns before any fire RPC; the loaded path below
        // would reject the press (CanBeginLoadedVolleyInput) and drop it.
        Super::StartFire(FireModeNum);
        return;
    }
    if (bPendingLoadedVolleyInput)
    {
        TryDrainLoadedVolleyInput(); // Repeated presses do not extend the deadline.
        if (bPendingLoadedVolleyInput || HasLoadedVolley()) return;
        // The old request was cancelled/expired. This fresh press may start a
        // new request if the weapon's current ownership and state allow it.
    }
    if (LoadedOwnershipEpoch == 0)
    {
        BufferLoadedVolleyInput();
        return;
    }
    if (!CanBeginLoadedVolley() || HasLoadedVolley()) return;
    // Remote authority may start loads only through the version-gated RPC.
    if (Role == ROLE_Authority && !UTOwner->IsLocallyControlled()
        && Cast<APlayerController>(UTOwner->Controller)) return;
    LastClientLoadedVolleyId = NCRocketVolley::Next(LastClientLoadedVolleyId);
    ResetLoadedVolley(LastClientLoadedVolleyId);
    if (Role < ROLE_Authority) ServerBeginLoadedVolley(LoadedVolleyEpoch, LoadedVolley.Id, UTOwner);
    if (CurrentFireMode == 0 && IsFiring()) Super::StopFire(0);
    TryBeginLoadedVolley();
}

void AUTPlusWeap_RocketLauncher::TryBeginLoadedVolley()
{
    GetWorldTimerManager().ClearTimer(LoadedVolleyBeginHandle);
    if (!HasLoadedVolley() || bLoadedVolleyEnteredState) return;
    if (Role == ROLE_Authority && !Is329FireProtocolReady())
    {
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Warning, TEXT("[RocketVolley] begin_cancel weapon=%s epoch=%u id=%u reason=protocol_revoked"),
                *GetName(), LoadedVolleyEpoch, LoadedVolley.Id);
        CompleteLoadedVolley(true);
        return;
    }
    const float Now = GetWorld()->GetTimeSeconds();
    if (!CanBeginLoadedVolley() || LoadedVolleyEpoch != LoadedOwnershipEpoch
        || LoadedVolleyPawn.Get() != UTOwner || Now - LoadedVolleyBeginRequestedAt > 5.f)
    {
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Warning, TEXT("[RocketVolley] begin_cancel weapon=%s epoch=%u id=%u role=%d age=%.3f state=%s ammo=%d ownerMatch=%d"),
                *GetName(), LoadedVolleyEpoch, LoadedVolley.Id, int32(Role), Now - LoadedVolleyBeginRequestedAt,
                *GetNameSafe(CurrentState), Ammo, LoadedVolleyPawn.Get() == UTOwner ? 1 : 0);
        CompleteLoadedVolley(true);
        return;
    }
    // Keep the queued request in its own identity rather than a generic held
    // bit which ActiveState could use to bypass the cooldown gate.
    UTOwner->SetPendingFire(1, false);
    const float Remaining = FMath::Max(EarliestFireTime - Now, NCClientFireTiming::MaxRemaining(this));
    if (Remaining > KINDA_SMALL_NUMBER || CurrentState != ActiveState)
    {
        GetWorldTimerManager().SetTimer(LoadedVolleyBeginHandle, this,
            &AUTPlusWeap_RocketLauncher::TryBeginLoadedVolley,
            FMath::Max(0.005f, Remaining), false);
        return;
    }
    ClearDeferredActiveState();
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] begin_ready weapon=%s epoch=%u id=%u role=%d state=%s"),
            *GetName(), LoadedVolleyEpoch, LoadedVolley.Id, int32(Role), *GetNameSafe(CurrentState));
    BeginFiringSequence(1, false);
    if (!bLoadedVolleyEnteredState) CompleteLoadedVolley(true);
}

bool AUTPlusWeap_RocketLauncher::ServerBeginLoadedVolley_Validate(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn)
{
    return Epoch != 0 && Id != 0;
}

void AUTPlusWeap_RocketLauncher::ServerBeginLoadedVolley_Implementation(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn)
{
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] server_begin weapon=%s epoch=%u id=%u active=%u expectedPawn=%s owner=%s state=%s ammo=%d"),
            *GetName(), Epoch, Id, LoadedVolley.Id, *GetNameSafe(ExpectedPawn), *GetNameSafe(UTOwner), *GetNameSafe(CurrentState), Ammo);
    if (Epoch != LoadedOwnershipEpoch || !ExpectedPawn || ExpectedPawn != UTOwner) return;
    APlayerController* PC = UTOwner ? Cast<APlayerController>(UTOwner->Controller) : nullptr;
    if (!PC) return; // Bots/listen host use the local path, never this remote entry.
    if (!NCPlusVersionGate::IsProtocolConfirmed(PC))
    {
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Warning, TEXT("[RocketVolley] server_reject weapon=%s epoch=%u id=%u reason=protocol_not_confirmed"), *GetName(), Epoch, Id);
        ClientLoadedVolleyResult(Epoch, Id, UTOwner, uint8(NCRocketVolley::EResult::Rejected), 0, 0);
        return;
    }
    if (!NCRocketVolley::IsNewer(Id, LastServerLoadedVolleyId))
    {
        for (const FNCLoadedVolleyReceipt& Receipt : LoadedVolleyReceipts)
        {
            if (Receipt.VolleyId == Id)
            {
                ClientLoadedVolleyResult(Epoch, Id, UTOwner, Receipt.Result, Receipt.Count, Receipt.SpawnedMask);
                return;
            }
        }
        return; // Duplicate active Begin must not restart its charge clock.
    }
    LastServerLoadedVolleyId = Id;
    if (ExpectedPawn != UTOwner || !CanBeginLoadedVolley() || HasLoadedVolley())
    {
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Warning, TEXT("[RocketVolley] server_reject weapon=%s epoch=%u id=%u reason=owner_state_ammo_or_busy active=%u state=%s ammo=%d"),
                *GetName(), Epoch, Id, LoadedVolley.Id, *GetNameSafe(CurrentState), Ammo);
        ClientLoadedVolleyResult(Epoch, Id, UTOwner, uint8(NCRocketVolley::EResult::Rejected), 0, 0);
        return;
    }
    ResetLoadedVolley(Id);
    if (CurrentFireMode == 0 && IsFiring()) EndFiringSequence(0);
    TryBeginLoadedVolley();
}

bool AUTPlusWeap_RocketLauncher::BeginLoadedVolleyState()
{
    // Bots/listen host may enter via the stock continued-fire state machinery.
    if (!HasLoadedVolley() && Role == ROLE_Authority && UTOwner
        && (UTOwner->IsLocallyControlled() || !Cast<APlayerController>(UTOwner->Controller)))
    {
        LastClientLoadedVolleyId = NCRocketVolley::Next(LastClientLoadedVolleyId);
        ResetLoadedVolley(LastClientLoadedVolleyId);
    }
    const bool bValid = HasLoadedVolley() && LoadedVolleyEpoch == LoadedOwnershipEpoch
        && LoadedVolleyPawn.Get() == UTOwner;
    if (bValid)
    {
        bLoadedVolleyEnteredState = true;
        CurrentlyFiringMode = 1;
        if (FireModeActiveState.IsValidIndex(1)) FireModeActiveState[1] = 1;
        GetWorldTimerManager().ClearTimer(LoadedVolleyBeginHandle);
    }
    return bValid;
}

void AUTPlusWeap_RocketLauncher::StopFire(uint8 FireModeNum)
{
    if (FireModeNum != 1)
    {
        Super::StopFire(FireModeNum);
        return;
    }
    if (!bHandlingRetry)
    {
        // Mirror AUTWeaponFix::StopFire without its legacy stop RPCs: a genuine
        // release ends held alt intent even mid-switch, so a press latched for
        // the incoming weapon (see StartFire) cannot ghost-fire it.
        bFireHeldByPlayer[1] = false;
        AUTWeapon* const IncomingWeapon = UTOwner ? UTOwner->GetPendingWeapon() : nullptr;
        if (UTOwner && (CurrentState == UnequippingState || (IncomingWeapon && IncomingWeapon != this)))
            UTOwner->SetPendingFire(1, false);
    }
    if (bPendingLoadedVolleyInput)
    {
        bPendingLoadedVolleyRelease = true;
        return; // No epoch/ID yet: retain the release without RPCs or state entry.
    }
    NotifyLoadedVolleyRelease();
    // A load requested during primary/equip has not entered its state yet.
    // Keep the pending transition, then release at its first-load boundary.
    if (Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState) || !HasLoadedVolley())
        EndFiringSequence(1); // No stock Stop, byte retry, or watermark ACK for 329 loads.
}

void AUTPlusWeap_RocketLauncher::NotifyLoadedVolleyRelease()
{
    if (!HasLoadedVolley() || bLoadedVolleyReleaseSent || bLoadedVolleyApplyingResult) return;
    if (Role == ROLE_Authority && UTOwner && !UTOwner->IsLocallyControlled()
        && Cast<APlayerController>(UTOwner->Controller)) return;
    bLoadedVolleyReleaseSent = true;
    LoadedVolleySelectedMode = uint8(FMath::Clamp(CurrentRocketFireMode, 0, 2));
    LoadedVolleyRequestedCount = uint8(FMath::Clamp(FMath::Max(1, NumLoadedRockets), 1, 3));
    if (Role < ROLE_Authority)
        ServerReleaseLoadedVolley(LoadedVolleyEpoch, LoadedVolley.Id, UTOwner,
            LoadedVolleySelectedMode, LoadedVolleyRequestedCount);
}

bool AUTPlusWeap_RocketLauncher::ServerReleaseLoadedVolley_Validate(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn,
    uint8 Mode, uint8 Count)
{
    return Epoch != 0 && Id != 0 && Mode <= 2 && Count >= 1 && Count <= 3;
}

void AUTPlusWeap_RocketLauncher::ServerReleaseLoadedVolley_Implementation(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn,
    uint8 Mode, uint8 Count)
{
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] server_release weapon=%s epoch=%u id=%u active=%u mode=%u requested=%u loaded=%d state=%s expectedPawn=%s"),
            *GetName(), Epoch, Id, LoadedVolley.Id, Mode, Count, NumLoadedRockets, *GetNameSafe(CurrentState), *GetNameSafe(ExpectedPawn));
    APlayerController* PC = UTOwner ? Cast<APlayerController>(UTOwner->Controller) : nullptr;
    if (!PC || !NCPlusVersionGate::IsProtocolConfirmed(PC)) return;
    if (Epoch != LoadedOwnershipEpoch || Epoch != LoadedVolleyEpoch
        || !ExpectedPawn || ExpectedPawn != UTOwner || LoadedVolleyPawn.Get() != ExpectedPawn) return;
    if (Id != LoadedVolley.Id || LoadedVolley.Terminal)
    {
        for (const FNCLoadedVolleyReceipt& Receipt : LoadedVolleyReceipts)
            if (Receipt.VolleyId == Id)
                ClientLoadedVolleyResult(Epoch, Id, UTOwner, Receipt.Result, Receipt.Count, Receipt.SpawnedMask);
        return;
    }
    if (bLoadedVolleyReleaseReceived || LoadedVolley.Released) return;
    if (!UTOwner || LoadedVolleyPawn.Get() != UTOwner || UTOwner->IsDead()
        || UTOwner->GetWeapon() != this || !IsLoadedVolleyModeValid(Mode))
    {
        CompleteLoadedVolley(true);
        return;
    }
    bLoadedVolleyReleaseReceived = true;
    LoadedVolleySelectedMode = Mode;
    LoadedVolleyRequestedCount = Count;
    CurrentRocketFireMode = Mode; // Absolute snapshot, immutable through this burst.
    if (Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState)) EndFiringSequence(1);
}

bool AUTPlusWeap_RocketLauncher::ServerSetLoadedRocketMode_Validate(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn, uint8 Mode)
{
    return Epoch != 0 && Id != 0 && Mode <= 2;
}

void AUTPlusWeap_RocketLauncher::ServerSetLoadedRocketMode_Implementation(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn, uint8 Mode)
{
    APlayerController* PC = UTOwner ? Cast<APlayerController>(UTOwner->Controller) : nullptr;
    if (!PC || !NCPlusVersionGate::IsProtocolConfirmed(PC)) return;
    if (Epoch != LoadedOwnershipEpoch || Epoch != LoadedVolleyEpoch
        || !ExpectedPawn || ExpectedPawn != UTOwner || LoadedVolleyPawn.Get() != ExpectedPawn) return;
    if (Id != LoadedVolley.Id || !HasLoadedVolley() || LoadedVolley.Released
        || bLoadedVolleyReleaseReceived || !IsLoadedVolleyModeValid(Mode)) return;
    CurrentRocketFireMode = Mode;
    bDrawRocketModeString = true;
    SetRocketFlashExtra(1, NumLoadedRockets + 1, Mode, true);
}

bool AUTPlusWeap_RocketLauncher::CommitLoadedVolley()
{
    if (!HasLoadedVolley() || LoadedVolleyEpoch != LoadedOwnershipEpoch
        || LoadedVolleyPawn.Get() != UTOwner || LoadedVolley.Released) return false;
    const uint8 ActualCount = uint8(FMath::Clamp(NumLoadedRockets, 0, 3));
    // Client count may reduce a delayed release, never manufacture another load
    // or spend ammunition the authoritative load callbacks haven't consumed.
    const uint8 Count = bLoadedVolleyReleaseReceived
        ? FMath::Min(ActualCount, LoadedVolleyRequestedCount) : ActualCount;
    if (Count == 0 || !IsLoadedVolleyModeValid(uint8(CurrentRocketFireMode)))
    {
        CompleteLoadedVolley(true);
        return false;
    }
    if (!LoadedVolley.Release(LoadedVolley.Id, Count)) return false;
    // Release latency can let an additional server-only load complete. It is
    // neither fired nor silently consumed when the requested volley is smaller.
    if (Role == ROLE_Authority && ActualCount > Count)
        AddAmmo(FMath::Min<int32>(ActualCount - Count, LoadedVolleyAmmoSpent));
    LoadedVolleySelectedMode = uint8(CurrentRocketFireMode);
    LoadedVolleyNextOrdinal = 0;
    NumLoadedRockets = Count;
    if (Role == ROLE_Authority) SendLoadedVolleyReceipt(NCRocketVolley::EResult::Accepted);
    return true;
}

void AUTPlusWeap_RocketLauncher::NoteLoadedRocketAmmoSpent(int32 Amount)
{
    LoadedVolleyAmmoSpent += FMath::Max(0, Amount);
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] load_complete weapon=%s epoch=%u id=%u role=%d ordinal=%d loaded=%d ammoSpent=%d state=%s"),
            *GetName(), LoadedVolleyEpoch, LoadedVolley.Id, int32(Role), NumLoadedRockets - 1, NumLoadedRockets,
            LoadedVolleyAmmoSpent, *GetNameSafe(CurrentState));
}

void AUTPlusWeap_RocketLauncher::SendLoadedVolleyReceipt(NCRocketVolley::EResult Result)
{
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] server_result weapon=%s epoch=%u id=%u outcome=%u mode=%u count=%u spawned=%u resolved=%u state=%s"),
            *GetName(), LoadedVolleyEpoch, LoadedVolley.Id, uint8(Result), LoadedVolleySelectedMode, LoadedVolley.Count,
            LoadedVolley.SpawnedMask, LoadedVolley.ResolvedMask, *GetNameSafe(CurrentState));
    ClientLoadedVolleyResult(LoadedVolleyEpoch, LoadedVolley.Id, LoadedVolleyPawn.Get(), uint8(Result), LoadedVolley.Count, LoadedVolley.SpawnedMask);
    if (Result != NCRocketVolley::EResult::Accepted)
    {
        FNCLoadedVolleyReceipt Receipt;
        Receipt.VolleyId = LoadedVolley.Id;
        Receipt.Result = uint8(Result);
        Receipt.Count = LoadedVolley.Count;
        Receipt.SpawnedMask = LoadedVolley.SpawnedMask;
        LoadedVolleyReceipts.Add(Receipt);
        if (LoadedVolleyReceipts.Num() > 16) LoadedVolleyReceipts.RemoveAt(0);
    }
}

void AUTPlusWeap_RocketLauncher::CompleteLoadedVolley(bool bCancelled)
{
    if (!HasLoadedVolley()) return;
    const uint32 Id = LoadedVolley.Id;
    if (Role == ROLE_Authority)
    {
        for (uint8 Ordinal = 0; Ordinal < LoadedVolley.Count; ++Ordinal)
            if (!(LoadedVolley.ResolvedMask & (1u << Ordinal)))
                ClientLoadedRocketResult(LoadedVolleyEpoch, Id, LoadedVolleyPawn.Get(), Ordinal, uint8(NCRocketVolley::ERocketResult::Cancelled), nullptr, 0);
        SendLoadedVolleyReceipt(bCancelled ? NCRocketVolley::EResult::Cancelled : NCRocketVolley::EResult::Completed);
    }
    LoadedVolley.Finish(Id);
    if (CurrentlyFiringMode == 1) CurrentlyFiringMode = 255;
    if (FireModeActiveState.IsValidIndex(1)) FireModeActiveState[1] = 0;
    GetWorldTimerManager().ClearTimer(LoadedVolleyBeginHandle);
}

void AUTPlusWeap_RocketLauncher::CaptureLoadedRocketSpawn(AUTProjectile* Projectile)
{
    if (!bLoadedVolleySpawnInProgress || !Projectile) return;
    bLoadedVolleySpawnSucceeded = true;
    LoadedVolleySpawnedProjectile = Projectile;
    if (AUTPlusProj_Rocket* Rocket = Cast<AUTPlusProj_Rocket>(Projectile))
    {
        Rocket->LoadedOwnershipEpoch = LoadedVolleyEpoch;
        Rocket->LoadedVolleyId = LoadedVolley.Id;
        Rocket->LoadedRocketOrdinal = LoadedVolleyNextOrdinal;
        Rocket->LoadedVolleyWeapon = this;
    }
}

AUTProjectile* AUTPlusWeap_RocketLauncher::SpawnNetPredictedProjectile(
    TSubclassOf<AUTProjectile> Class, FVector Location, FRotator Rotation)
{
    if (CurrentFireMode != 1) return Super::SpawnNetPredictedProjectile(Class, Location, Rotation);
    // No unnumbered fallback for charged fire: all normal, death and grace
    // dispatches must own a committed volley in this ownership lifetime.
    if (!HasLoadedVolley() || !LoadedVolley.Released || LoadedVolleyEpoch != LoadedOwnershipEpoch)
        return nullptr;
    const uint32 Id = LoadedVolley.Id;
    const uint8 Ordinal = LoadedVolleyNextOrdinal;
    if (Ordinal >= LoadedVolley.Count) return nullptr;
    TGuardValue<bool> SpawnGuard(bLoadedVolleySpawnInProgress, true);
    bLoadedVolleySpawnSucceeded = false;
    LoadedVolleySpawnedProjectile.Reset();
    // Every ordinal is independent. The legacy one-slot high-ping delay cannot
    // represent a three-projectile burst and must not receive these requests.
    AUTProjectile* Result = SpawnNetPredictedProjectileInternal(Class, Location, Rotation, 1, INDEX_NONE, false);
    AUTProjectile* Spawned = LoadedVolleySpawnedProjectile.Get();
    LoadedVolley.Resolve(Id, Ordinal, bLoadedVolleySpawnSucceeded);
    ++LoadedVolleyNextOrdinal;
    if (Role == ROLE_Authority)
    {
        const bool bLive = Spawned && !Spawned->IsPendingKillPending() && !Spawned->bExploded;
        const NCRocketVolley::ERocketResult Outcome = !bLoadedVolleySpawnSucceeded
            ? NCRocketVolley::ERocketResult::Rejected
            : bLive ? NCRocketVolley::ERocketResult::Spawned : NCRocketVolley::ERocketResult::Resolved;
        uint32 ProjectileNetGUID = 0;
        if (bLive)
            if (UNetDriver* Driver = GetWorld()->GetNetDriver())
                if (Driver->GuidCache.IsValid())
                    ProjectileNetGUID = Driver->GuidCache->GetOrAssignNetGUID(Spawned).Value;
        if (CVarRocketVolleyDebug.GetValueOnGameThread())
            UE_LOG(LogTemp, Log, TEXT("[RocketVolley] server_rocket weapon=%s epoch=%u id=%u ordinal=%u mode=%u outcome=%u guid=%u actor=%s"),
                *GetName(), LoadedVolleyEpoch, Id, Ordinal, LoadedVolleySelectedMode, uint8(Outcome), ProjectileNetGUID, *GetNameSafe(Spawned));
        ClientLoadedRocketResult(LoadedVolleyEpoch, Id, LoadedVolleyPawn.Get(), Ordinal,
            uint8(Outcome), bLive ? Spawned : nullptr, ProjectileNetGUID);
    }
    else
    {
        FNCLoadedRocketPrediction& Prediction = FindOrAddLoadedRocket(LoadedVolleyEpoch, Id, Ordinal);
        Prediction.Fake = Spawned;
        // Generic class/direction matching cannot distinguish siblings, old
        // volleys or another rocket mode. Pair only from the exact owner RPC.
        if (AUTPlayerController* PC = UTOwner ? Cast<AUTPlayerController>(UTOwner->Controller) : nullptr)
            PC->FakeProjectiles.Remove(Spawned);
        for (int32 i = PendingFakeProjectiles.Num() - 1; i >= 0; --i)
            if (PendingFakeProjectiles[i].Projectile.Get() == Spawned)
                PendingFakeProjectiles.RemoveAt(i);
        ReconcileLoadedRockets();
    }
    return Result;
}

FNCLoadedRocketPrediction& AUTPlusWeap_RocketLauncher::FindOrAddLoadedRocket(uint32 Epoch, uint32 Id, uint8 Ordinal)
{
    for (FNCLoadedRocketPrediction& Prediction : LoadedRocketPredictions)
        if (Prediction.OwnershipEpoch == Epoch && Prediction.VolleyId == Id && Prediction.Ordinal == Ordinal) return Prediction;
    FNCLoadedRocketPrediction Prediction;
    Prediction.OwnershipEpoch = Epoch;
    Prediction.VolleyId = Id;
    Prediction.Ordinal = Ordinal;
    Prediction.CreatedAt = GetWorld()->GetTimeSeconds();
    LoadedRocketPredictions.Add(Prediction);
    return LoadedRocketPredictions.Last();
}

void AUTPlusWeap_RocketLauncher::ClientLoadedRocketResult_Implementation(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn,
    uint8 Ordinal, uint8 Result, AUTProjectile* Projectile, uint32 ProjectileNetGUID)
{
    if (Role == ROLE_Authority || Epoch != LoadedOwnershipEpoch || !ExpectedPawn || ExpectedPawn != UTOwner || Id == 0 || Ordinal >= 3
        || Result > uint8(NCRocketVolley::ERocketResult::Cancelled)) return;
    FNCLoadedRocketPrediction& Prediction = FindOrAddLoadedRocket(Epoch, Id, Ordinal);
    // Reliable duplicate outcomes are idempotent; terminal results cannot be
    // overwritten by an older acceptance or applied to a different volley.
    if (Prediction.Outcome != 255)
    {
        // A replicated identity can resolve an Actor* that was unavailable when
        // the receipt arrived. It cannot reverse a rejection/cancellation.
        if (Prediction.Outcome == uint8(NCRocketVolley::ERocketResult::Spawned)
            && Result == Prediction.Outcome && !Prediction.Real.Get() && Projectile)
        {
            Prediction.Real = Projectile;
            ReconcileLoadedRockets();
        }
        return;
    }
    Prediction.Outcome = Result;
    Prediction.Real = Projectile;
    Prediction.ProjectileNetGUID = ProjectileNetGUID;
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] client_rocket weapon=%s epoch=%u id=%u ordinal=%u outcome=%u guid=%u actor=%s"),
            *GetName(), Epoch, Id, Ordinal, Result, ProjectileNetGUID, *GetNameSafe(Projectile));
    ReconcileLoadedRockets();
}

bool AUTPlusWeap_RocketLauncher::ObserveLoadedRocketActor(uint32 Epoch, uint32 Id, uint8 Ordinal, AUTProjectile* Projectile)
{
    if (Role == ROLE_Authority || Epoch != LoadedOwnershipEpoch || !UTOwner
        || !Projectile || Projectile->Instigator != UTOwner) return false;
    ClientLoadedRocketResult_Implementation(Epoch, Id, UTOwner, Ordinal,
        uint8(NCRocketVolley::ERocketResult::Spawned), Projectile, 0);
    return true;
}

void AUTPlusWeap_RocketLauncher::ClientLoadedVolleyResult_Implementation(uint32 Epoch, uint32 Id, AUTCharacter* ExpectedPawn,
    uint8 Result, uint8 Count, uint8 SpawnedMask)
{
    if (Role == ROLE_Authority || Epoch != LoadedOwnershipEpoch || !ExpectedPawn || ExpectedPawn != UTOwner || Id == 0 || Count > 3
        || Result > uint8(NCRocketVolley::EResult::Cancelled)) return;
    const NCRocketVolley::EResult Outcome = NCRocketVolley::EResult(Result);
    if (CVarRocketVolleyDebug.GetValueOnGameThread())
        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] client_result weapon=%s epoch=%u id=%u outcome=%u count=%u spawned=%u current=%u state=%s"),
            *GetName(), Epoch, Id, Result, Count, SpawnedMask, LoadedVolley.Id, *GetNameSafe(CurrentState));
    for (uint8 Ordinal = 0; Ordinal < 3; ++Ordinal)
    {
        const bool bRejected = NCRocketVolley::CancelOrdinal(Outcome, Count, SpawnedMask, Ordinal);
        if (bRejected)
        {
            FNCLoadedRocketPrediction& Prediction = FindOrAddLoadedRocket(Epoch, Id, Ordinal);
            if (Prediction.Outcome == 255)
                Prediction.Outcome = uint8(NCRocketVolley::ERocketResult::Cancelled);
        }
    }
    if (Epoch == LoadedVolleyEpoch && Id == LoadedVolley.Id && (Outcome == NCRocketVolley::EResult::Rejected || Outcome == NCRocketVolley::EResult::Cancelled))
    {
        TGuardValue<bool> ResultGuard(bLoadedVolleyApplyingResult, true);
        LoadedVolley.Finish(Id);
        if (CurrentlyFiringMode == 1) CurrentlyFiringMode = 255;
        if (FireModeActiveState.IsValidIndex(1)) FireModeActiveState[1] = 0;
        if (UTOwner) UTOwner->SetPendingFire(1, false);
        UUTWeaponStateFiringChargedRocket_Transactional* State =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState);
        if (State && State->StateVolleyEpoch == Epoch && State->StateVolleyId == Id) GotoActiveState();
    }
    ReconcileLoadedRockets();
}

void AUTPlusWeap_RocketLauncher::ReconcileLoadedRockets()
{
    const float Now = GetWorld()->GetTimeSeconds();
    for (int32 i = LoadedRocketPredictions.Num() - 1; i >= 0; --i)
    {
        FNCLoadedRocketPrediction& Prediction = LoadedRocketPredictions[i];
        if (Prediction.OwnershipEpoch != LoadedOwnershipEpoch)
        {
            LoadedRocketPredictions.RemoveAt(i);
            continue;
        }
        AUTProjectile* Fake = Prediction.Fake.Get();
        AUTProjectile* Real = Prediction.Real.Get();
        if (!Real && Prediction.ProjectileNetGUID != 0
            && Prediction.OwnershipEpoch == LoadedOwnershipEpoch)
        {
            // 4.15 does not replay an RPC whose Actor* was initially unmapped.
            // The server's immutable NetGUID resolves on later actor replication
            // for every projectile class, including stock grenade and spiral BPs.
            if (UNetDriver* Driver = GetWorld()->GetNetDriver())
                if (Driver->GuidCache.IsValid())
                    Real = Cast<AUTProjectile>(Driver->GuidCache->GetObjectFromNetGUID(
                        FNetworkGUID(Prediction.ProjectileNetGUID), false));
            if (Real && Real->Instigator == UTOwner) Prediction.Real = Real;
            else Real = nullptr;
        }
        if (Fake && !Fake->IsPendingKillPending())
        {
            if (Prediction.Outcome != 255 && Prediction.Outcome != uint8(NCRocketVolley::ERocketResult::Spawned))
            {
                // Only this exact ordinal was rejected/resolved/cancelled.
                if (!Fake->MasterProjectile) Fake->Destroy();
                LoadedRocketPredictions.RemoveAt(i);
                continue;
            }
            if (Real && !Real->IsPendingKillPending() && !Real->bExploded && !Fake->bExploded)
            {
                if (Real->Instigator == Fake->Instigator && Real->GetClass() != Fake->GetClass()
                    && !Fake->MasterProjectile && !Real->MyFakeProjectile)
                {
                    // The server resolved a different legal mode (e.g. grace
                    // won a late mode change). Its visible actor replaces only
                    // this exact predicted ordinal; no direction search occurs.
                    if (CVarRocketVolleyDebug.GetValueOnGameThread())
                        UE_LOG(LogTemp, Log, TEXT("[RocketVolley] mode_correction weapon=%s epoch=%u id=%u ordinal=%u"),
                            *GetName(), Prediction.OwnershipEpoch, Prediction.VolleyId, Prediction.Ordinal);
                    Fake->Destroy();
                    LoadedRocketPredictions.RemoveAt(i);
                    continue;
                }
                if (!Fake->MasterProjectile && !Real->MyFakeProjectile && Real->GetClass() == Fake->GetClass()
                    && Real->Instigator == Fake->Instigator)
                    Real->BeginFakeProjectileSynch(Fake);
                // A conflicting pairing is never repaired by deleting another
                // volley's visual. Leave the authoritative actor visible.
                if (Fake->MasterProjectile == Real)
                {
                    LoadedRocketPredictions.RemoveAt(i);
                    continue;
                }
            }
        }
        if (Now - Prediction.CreatedAt > 12.f)
        {
            if (CVarRocketVolleyDebug.GetValueOnGameThread())
                UE_LOG(LogTemp, Warning, TEXT("[RocketVolley] reconciliation expired epoch=%u id=%u ordinal=%u outcome=%u guid=%u"),
                    Prediction.OwnershipEpoch, Prediction.VolleyId, Prediction.Ordinal, Prediction.Outcome, Prediction.ProjectileNetGUID);
            LoadedRocketPredictions.RemoveAt(i); // Normal projectile lifespan owns cleanup.
        }
    }
    if (LoadedRocketPredictions.Num() && !GetWorldTimerManager().IsTimerActive(LoadedRocketReconcileHandle))
        GetWorldTimerManager().SetTimer(LoadedRocketReconcileHandle, this,
            &AUTPlusWeap_RocketLauncher::ReconcileLoadedRockets, 0.05f, true);
    else if (!LoadedRocketPredictions.Num()) GetWorldTimerManager().ClearTimer(LoadedRocketReconcileHandle);
}

void AUTPlusWeap_RocketLauncher::ContinueLoadedVolley()
{
    if (!UTOwner || (!UTOwner->IsLocallyControlled() && Cast<APlayerController>(UTOwner->Controller)))
    {
        if (UTOwner) UTOwner->SetPendingFire(1, false);
        GotoActiveState();
        return;
    }
    const bool bHeld = UTOwner->IsPendingFire(1);
    UTOwner->SetPendingFire(1, false);
    GotoActiveState();
    if (UTOwner && bHeld && UTOwner->GetWeapon() == this && !UTOwner->GetPendingWeapon()) StartFire(1);
}

void AUTPlusWeap_RocketLauncher::Removed()
{
    ClearLoadedVolleyInput();
    // Preserve the existing death discharge for completed loads, but route it
    // through the same identities. Never let base Removed manufacture an
    // unnumbered volley from the stale visual barrel count.
    if (Role == ROLE_Authority && HasLoadedVolley() && UTOwner && NumLoadedRockets > 0)
    {
        if (UUTWeaponStateFiringChargedRocket_Transactional* State =
            Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState))
        {
            if (LoadedVolley.Released) State->FireLoadedRocket();
            else State->EndFiringSequence(1);
        }
    }
    CompleteLoadedVolley(true);
    NumLoadedRockets = NumLoadedBarrels = 0;
    GetWorldTimerManager().ClearTimer(LoadedRocketReconcileHandle);
    GetWorldTimerManager().ClearTimer(LoadedVolleyBeginHandle);
    // Accepted fakes may already own the visible authoritative projectile. Do
    // not destroy them merely because the weapon was dropped or switched away.
    LoadedRocketPredictions.Empty();
    if (Role == ROLE_Authority)
    {
        LoadedOwnershipEpoch = NCRocketVolley::Next(LoadedOwnershipEpoch);
        ResetLoadedOwnershipState();
        ForceNetUpdate();
    }
    Super::Removed();
}

void AUTPlusWeap_RocketLauncher::ResetLoadedOwnershipState(bool bPreservePendingInput)
{
    if (!bPreservePendingInput) ClearLoadedVolleyInput();
    // No actor destruction: accepted projectiles outlive the inventory lifetime.
    LoadedVolley = NCRocketVolley::FProgress();
    LoadedVolleyEpoch = LoadedOwnershipEpoch;
    LastClientLoadedVolleyId = LastServerLoadedVolleyId = 0;
    LoadedVolleyPawn.Reset();
    LoadedVolleyReceipts.Empty();
    LoadedRocketPredictions.Empty();
    GetWorldTimerManager().ClearTimer(LoadedVolleyBeginHandle);
    GetWorldTimerManager().ClearTimer(LoadedRocketReconcileHandle);
}

void AUTPlusWeap_RocketLauncher::GivenTo(AUTCharacter* NewOwner, bool bAutoActivate)
{
    ClearLoadedVolleyInput();
    if (Role == ROLE_Authority)
    {
        CompleteLoadedVolley(true);
        LoadedOwnershipEpoch = NCRocketVolley::Next(LoadedOwnershipEpoch);
        ResetLoadedOwnershipState();
    }
    Super::GivenTo(NewOwner, bAutoActivate);
    if (Role == ROLE_Authority) ForceNetUpdate();
}

void AUTPlusWeap_RocketLauncher::ClientGivenTo_Internal(bool bAutoActivate)
{
    ClearLoadedVolleyInput();
    Super::ClientGivenTo_Internal(bAutoActivate);
}

bool AUTPlusWeap_RocketLauncher::PutDown()
{
    ClearLoadedVolleyInput();
    return Super::PutDown();
}

void AUTPlusWeap_RocketLauncher::DetachFromOwner_Implementation()
{
    ClearLoadedVolleyInput();
    Super::DetachFromOwner_Implementation();
}

void AUTPlusWeap_RocketLauncher::OnRep_LoadedOwnershipEpoch()
{
    if (LoadedVolleyEpoch == LoadedOwnershipEpoch) return;
    const bool bInitialOwnership = LoadedVolleyEpoch == 0 && LoadedOwnershipEpoch != 0;
    CompleteLoadedVolley(true);
    if (UTOwner) UTOwner->SetPendingFire(1, false);
    if (Cast<UUTWeaponStateFiringChargedRocket_Transactional>(CurrentState)) GotoActiveState();
    ResetLoadedOwnershipState(bInitialOwnership);
    TryDrainLoadedVolleyInput();
}
