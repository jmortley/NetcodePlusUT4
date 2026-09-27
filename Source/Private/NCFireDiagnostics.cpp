#include "NCFireDiagnostics.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "UTProjectile.h"
#include "UTWeaponState.h"
#include "UTWeaponStateEquipping.h"
#include "UTWeaponStateUnequipping.h"
#include "UTWeaponStateInactive.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Runtime/Launch/Resources/Version.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCFireTrace, Log, All);

static TAutoConsoleVariable<FString> CVarFireTraceRun(TEXT("ncp.FireTraceRun"), TEXT("unset"),
    TEXT("Shared label for one paired client/server capture. Set before ncp.FireProvenance 1. No network traffic."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarFireTraceLimit(TEXT("ncp.FireTraceLimit"), 200000,
    TEXT("Maximum detailed fire records per process/run (100..1000000). LIMIT marks incomplete evidence."), ECVF_Default);

namespace NCFireDiagnostics
{
    struct FWeaponObservation
    {
        float EquipFinished = -1.f;
        double LastBeam = -1.0;
    };
    static TMap<TWeakObjectPtr<AUTWeapon>, FWeaponObservation> Observations;
    static TMap<TWeakObjectPtr<AUTWeapon>, FShotScope*> Scopes;
    static FString Capture;
    static FString LastRun;
    static uint64 Sequence = 0;
    static uint64 NextScope = 0;
    static bool bLimitReported = false;
    static bool bWasEnabled = false;
    static TMap<TWeakObjectPtr<UNetDriver>, double> DriverSamples;

    static void EndCapture()
    {
        if (bWasEnabled && !Capture.IsEmpty())
            UE_LOG(LogNCFireTrace, Warning, TEXT("[NCFireTrace] END schema=1 run=%s capture=%s seq=%llu limited=%d"),
                *LastRun, *Capture, (unsigned long long)++Sequence, bLimitReported);
        if (IConsoleVariable* Toggle = IConsoleManager::Get().FindConsoleVariable(TEXT("ncp.FireProvenance")))
            Toggle->Set(0, ECVF_SetByConsole);
        bWasEnabled = false;
    }
    static FAutoConsoleCommand EndCommand(TEXT("ncp.FireTraceEnd"),
        TEXT("End the paired fire capture, write its completion marker, and disable fire provenance."),
        FConsoleCommandDelegate::CreateStatic(&EndCapture));

    bool Enabled()
    {
        // Registration completes before gameplay. Retry lookup if called earlier.
        static IConsoleVariable* Toggle = nullptr;
        if (!Toggle) Toggle = IConsoleManager::Get().FindConsoleVariable(TEXT("ncp.FireProvenance"));
        const bool bOn = Toggle && Toggle->GetInt() > 0;
        if (!bOn) bWasEnabled = false;
        return bOn;
    }

    static FString Token(FString Value)
    {
        Value.ReplaceInline(TEXT(" "), TEXT("_"));
        Value.ReplaceInline(TEXT("\t"), TEXT("_"));
        Value.ReplaceInline(TEXT("\r"), TEXT("_"));
        Value.ReplaceInline(TEXT("\n"), TEXT("_"));
        return Value;
    }

    static uint32 Guid(const AActor* Actor)
    {
        UNetDriver* Driver = Actor && Actor->GetWorld() ? Actor->GetWorld()->GetNetDriver() : nullptr;
        // Never allocate a GUID or open an actor channel for diagnostics.
        const FNetworkGUID NetGuid = Driver && Driver->GuidCache.IsValid() && Actor
            ? Driver->GuidCache->GetNetGUID(Actor) : FNetworkGUID();
        return NetGuid.IsValid() && !NetGuid.IsDefault() ? NetGuid.Value : 0;
    }

    void Record(AUTWeapon* W, const TCHAR* Kind, uint8 Mode, int32 Event,
        uint32 Generation, const TCHAR* Details, const TCHAR* Protocol)
    {
        // Avoid temporary FString allocations for literal details when tracing is off.
        if (Enabled()) Record(W, Kind, Mode, Event, Generation, FString(Details), Protocol);
    }

    void Record(AUTWeapon* W, const TCHAR* Kind, uint8 Mode, int32 Event,
        uint32 Generation, const FString& Details, const TCHAR* Protocol)
    {
        if (!Enabled() || !W || !W->GetWorld()) return;
        const FString Run = Token(CVarFireTraceRun.GetValueOnGameThread());
        if (!bWasEnabled || Capture.IsEmpty() || LastRun != Run)
        {
            Capture = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            LastRun = Run;
            Sequence = 0;
            bLimitReported = false;
            Observations.Empty();
            DriverSamples.Empty();
            bWasEnabled = true;
        }
        const uint64 Limit = uint64(FMath::Clamp(CVarFireTraceLimit.GetValueOnGameThread(), 100, 1000000));
        if (Sequence >= Limit)
        {
            if (!bLimitReported)
            {
                UE_LOG(LogNCFireTrace, Warning, TEXT("[NCFireTrace] LIMIT schema=1 run=%s capture=%s seq=%llu"),
                    *Run, *Capture, (unsigned long long)++Sequence);
                bLimitReported = true;
            }
            return;
        }
        if ((Sequence & 255) == 0)
        {
            for (auto It = Observations.CreateIterator(); It; ++It)
                if (!It.Key().IsValid()) It.RemoveCurrent();
            for (auto It = DriverSamples.CreateIterator(); It; ++It)
                if (!It.Key().IsValid()) It.RemoveCurrent();
        }
        UWorld* World = W->GetWorld();
        AUTCharacter* Owner = W->GetUTOwner();
#if ENGINE_MINOR_VERSION >= 25
        APlayerState* Player = Owner ? Owner->GetPlayerState() : nullptr;
#else
        APlayerState* Player = Owner ? Owner->PlayerState : nullptr;
#endif
        AUTWeaponFix* Fix = Cast<AUTWeaponFix>(W);
        const float Now = World->GetTimeSeconds();
        const FWeaponObservation* Obs = Observations.Find(TWeakObjectPtr<AUTWeapon>(W));
        const float SinceEquipMs = Obs && Obs->EquipFinished >= 0.f ? (Now - Obs->EquipFinished) * 1000.f : -1.f;
        const float LastFire = Fix && Fix->LastFireTime.IsValidIndex(Mode) ? Fix->LastFireTime[Mode] : -1.f;
        const float Refire = Mode < W->GetNumFireModes() ? W->GetRefireTime(Mode) : -1.f;
        UUTWeaponStateEquipping* Equip = Cast<UUTWeaponStateEquipping>(W->GetCurrentState());
        const float EquipRemaining = Equip ? World->GetTimerManager().GetTimerRemaining(Equip->BringUpFinishedHandle) : -1.f;
        const TCHAR* Side = World->GetNetMode() == NM_Client ? TEXT("client")
            : World->GetNetMode() == NM_Standalone ? TEXT("standalone") : TEXT("server");
        // Split formatting for 4.15's fixed-arity logging templates.
        const FString Identity = FString::Printf(
            TEXT("[NCFireTrace] %s schema=1 run=%s capture=%s seq=%llu engine=%d.%d build=fire-trace-v1 side=%s world=%u driver=%u weapon=%u player=%u owner=%u actor=%s class=%s mode=%u event=%d generation=%u protocol=%s "),
            Kind, *Run, *Capture, (unsigned long long)++Sequence, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION,
            Side, World->GetUniqueID(), World->GetNetDriver() ? World->GetNetDriver()->GetUniqueID() : 0,
            Guid(W), Guid(Player), Guid(Owner), *Token(W->GetName()),
            *Token(W->GetClass()->GetName()), Mode, Event, Generation, Protocol);
        const FString Timing = FString::Printf(
            TEXT("frame=%llu t=%.6f mono=%.6f dt=%.6f state=%s current=%u pending=%u held0=%d held1=%d lft=%.6f refire=%.6f earliest=%.6f sinceEquipMs=%.3f equipRemaining=%.6f %s"),
            (unsigned long long)GFrameCounter, Now, FPlatformTime::Seconds(), World->GetDeltaSeconds(),
            W->GetCurrentState() ? *Token(W->GetCurrentState()->GetClass()->GetName()) : TEXT("null"),
            Guid(Owner ? Owner->GetWeapon() : nullptr), Guid(Owner ? Owner->GetPendingWeapon() : nullptr),
            Owner && Owner->IsPendingFire(0), Owner && Owner->IsPendingFire(1), LastFire, Refire,
            W->EarliestFireTime, SinceEquipMs, EquipRemaining, *Details);
        UE_LOG(LogNCFireTrace, Warning, TEXT("%s%s"), *Identity, *Timing);
        UNetDriver* Driver = World->GetNetDriver();
        if (Driver)
        {
            const double RealNow = FPlatformTime::Seconds();
            double* Last = DriverSamples.Find(TWeakObjectPtr<UNetDriver>(Driver));
            if (!Last || RealNow - *Last >= 10.0)
            {
                DriverSamples.Add(Driver, RealNow);
                // Driver-wide sampled counters, not per-shot causes or cumulative totals.
                UE_LOG(LogNCFireTrace, Warning,
                    TEXT("[NCFireTrace] NETWORK schema=1 run=%s capture=%s seq=%llu side=%s driver=%u inPackets=%u inLost=%u outOfOrder=%u scope=driver_sample"),
                    *Run, *Capture, (unsigned long long)++Sequence, Side, Driver->GetUniqueID(),
                    Driver->InPackets, Driver->InPacketsLost, Driver->InOutOfOrderPackets);
            }
        }
    }

    FShotScope::FShotScope(AUTWeapon* W, uint8 InMode, int32 InEvent, uint32 InGeneration, const TCHAR* InProtocol)
        : Weapon(W), Mode(InMode), Event(InEvent), Generation(InGeneration), Protocol(InProtocol), bEnabled(Enabled())
    {
        if (!bEnabled || !W) return;
        Scope = ++NextScope;
        FShotScope** Existing = Scopes.Find(Weapon);
        Previous = Existing ? *Existing : nullptr;
        Scopes.Add(Weapon, this);
        Record(W, W->GetNetMode() == NM_Client ? TEXT("PREDICT") : TEXT("DISPATCH"), Mode, Event, Generation,
            FString::Printf(TEXT("scope=%llu"), (unsigned long long)Scope), Protocol);
    }
    FShotScope::~FShotScope()
    {
        if (!bEnabled) return;
        Record(Weapon.Get(), TEXT("DISPATCH_END"), Mode, Event, Generation,
            FString::Printf(TEXT("scope=%llu projectiles=%d traces=%d damagingTraces=%d"),
                (unsigned long long)Scope, Projectiles, Traces, DamagingTraces), Protocol);
        if (Previous) Scopes.Add(Weapon, Previous); else Scopes.Remove(Weapon);
    }

    void Projectile(AUTWeapon* W, uint8 Mode, AUTProjectile* Result, const TCHAR* Outcome)
    {
        if (!Enabled() || !W) return;
        FShotScope** Found = Scopes.Find(TWeakObjectPtr<AUTWeapon>(W));
        FShotScope* S = Found ? *Found : nullptr;
        if (S && S->Mode != Mode) S = nullptr;
        if (S && Result) ++S->Projectiles;
        Record(W, TEXT("PROJECTILE"), Mode, S ? S->Event : INDEX_NONE, S ? S->Generation : 0,
            FString::Printf(TEXT("scope=%llu result=%s projectile=%s"),
                (unsigned long long)(S ? S->Scope : 0), Outcome, Result ? *Token(Result->GetName()) : TEXT("null")),
            S ? S->Protocol : TEXT("unscoped"));
    }
    void Hitscan(AUTWeapon* W, uint8 Mode, const FHitResult& Hit, bool bDealDamage)
    {
        if (!Enabled() || !W) return;
        FShotScope** Found = Scopes.Find(TWeakObjectPtr<AUTWeapon>(W));
        FShotScope* S = Found ? *Found : nullptr;
        if (S && S->Mode != Mode) S = nullptr;
        if (!S && !bDealDamage) return; // Beam tick has a separate, sampled observation.
        if (S) { ++S->Traces; if (bDealDamage) ++S->DamagingTraces; }
        Record(W, TEXT("HITSCAN"), Mode, S ? S->Event : INDEX_NONE, S ? S->Generation : 0,
            FString::Printf(TEXT("scope=%llu damagePath=%d target=%u blocking=%d"),
                (unsigned long long)(S ? S->Scope : 0), bDealDamage, Guid(Hit.GetActor()), Hit.bBlockingHit),
            S ? S->Protocol : TEXT("unscoped"));
    }
    void BeamSample(AUTWeapon* W, uint8 Mode, const FHitResult& Hit)
    {
        if (!Enabled() || !W) return;
        FWeaponObservation& Obs = Observations.FindOrAdd(TWeakObjectPtr<AUTWeapon>(W));
        const double Now = FPlatformTime::Seconds();
        if (Obs.LastBeam >= 0.0 && Now - Obs.LastBeam < 0.1) return;
        Obs.LastBeam = Now;
        Record(W, TEXT("BEAM_SAMPLE"), Mode, INDEX_NONE, 0,
            FString::Printf(TEXT("target=%u blocking=%d sampleMs=100"), Guid(Hit.GetActor()), Hit.bBlockingHit), TEXT("stream"));
    }

    FStateScope::FStateScope(AUTWeapon* W, UUTWeaponState* Requested)
        : Weapon(W), Before(W ? W->GetCurrentState() : nullptr), bEnabled(Enabled())
    {
        if (bEnabled && W) Record(W, TEXT("STATE_REQUEST"), W->GetCurrentFireMode(), INDEX_NONE, 0,
            FString::Printf(TEXT("requested=%s"), Requested ? *Token(Requested->GetClass()->GetName()) : TEXT("null")), TEXT("state"));
    }
    FStateScope::~FStateScope()
    {
        AUTWeapon* W = Weapon.Get();
        if (!bEnabled || !W) return;
        if (Before.Get() != W->GetCurrentState())
        {
            if (Cast<UUTWeaponStateEquipping>(Before.Get()) && W->GetCurrentState()
                && !Cast<UUTWeaponStateInactive>(W->GetCurrentState()) && !Cast<UUTWeaponStateUnequipping>(W->GetCurrentState()))
                Observations.FindOrAdd(Weapon).EquipFinished = W->GetWorld()->GetTimeSeconds();
            Record(W, TEXT("STATE_CHANGED"), W->GetCurrentFireMode(), INDEX_NONE, 0,
                FString::Printf(TEXT("before=%s"), Before.IsValid() ? *Token(Before->GetClass()->GetName()) : TEXT("null")), TEXT("state"));
        }
    }
}
