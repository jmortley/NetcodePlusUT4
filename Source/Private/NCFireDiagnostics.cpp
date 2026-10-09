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
#include "Engine/NetConnection.h"
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
static TAutoConsoleVariable<FString> CVarFireTraceSession(TEXT("ncp.FireTraceSession"), TEXT("unset"),
    TEXT("Fresh shared 32-hex nonce for one paired capture. Not a wire field."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarFireTraceLimit(TEXT("ncp.FireTraceLimit"), 200000,
    TEXT("Maximum detailed fire records per process/run (100..1000000). LIMIT marks incomplete evidence."), ECVF_Default);

namespace NCFireDiagnostics
{
    struct FModeObservation
    {
        uint64 Hold = 0, LastHold = 0, Volley = 0, VolleyHold = 0;
        int32 StartByte = INDEX_NONE;
    };
    struct FWeaponObservation
    {
        float EquipFinished = -1.f;
        double LastBeam = -1.0;
        uint32 Player = 0, Owner = 0, Connection = 0;
        bool LayoutRecorded = false;
        TMap<uint8, FModeObservation> Modes;
    };
    static TMap<TWeakObjectPtr<AUTWeapon>, FWeaponObservation> Observations;
    static TMap<TWeakObjectPtr<AUTWeapon>, FShotScope*> Scopes;
    static TMap<TWeakObjectPtr<AUTWeapon>, FRequestScope*> Requests;
    static TMap<TWeakObjectPtr<AUTWeapon>, FInputScope*> Inputs;
    static TMap<TWeakObjectPtr<AUTWeapon>, FStockScope*> Stock;
    static FString Capture, LastRun, LastSession;
    static uint64 Sequence = 0, NextScope = 0, CaptureEpoch = 1;
    static bool bLimitReported = false, bWasEnabled = false;
    static TMap<TWeakObjectPtr<UNetDriver>, double> DriverSamples;

    static FString Token(FString Value);
    static void ResetCapture(bool Explicit)
    {
        ++CaptureEpoch;
        Capture = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        LastRun = Token(CVarFireTraceRun.GetValueOnGameThread());
        LastSession = Token(CVarFireTraceSession.GetValueOnGameThread());
        Sequence = 0;
        bLimitReported = false;
        bWasEnabled = true;
        Observations.Empty(); DriverSamples.Empty();
        Scopes.Empty(); Requests.Empty(); Inputs.Empty(); Stock.Empty();
        UE_LOG(LogNCFireTrace, Warning,
            TEXT("[NCFireTrace] BEGIN schema=2 run=%s session=%s capture=%s seq=%llu explicit=%d"),
            *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence, Explicit);
    }
    static void ToggleChanged(IConsoleVariable* Toggle)
    {
        // This plugin owns the toggle. Its callback only invalidates diagnostic state;
        // it never sets a cvar or invokes gameplay code.
        if (bWasEnabled && Toggle->GetInt() <= 0)
        {
            UE_LOG(LogNCFireTrace, Warning,
                TEXT("[NCFireTrace] ABORT schema=2 run=%s session=%s capture=%s seq=%llu reason=toggle_off"),
                *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence);
            bWasEnabled = false; ++CaptureEpoch;
            Scopes.Empty(); Requests.Empty(); Inputs.Empty(); Stock.Empty();
        }
    }
    static IConsoleVariable* TraceToggle()
    {
        static IConsoleVariable* Toggle = nullptr;
        if (!Toggle)
        {
            Toggle = IConsoleManager::Get().FindConsoleVariable(TEXT("ncp.FireProvenance"));
            if (Toggle) Toggle->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&ToggleChanged));
        }
        return Toggle;
    }
    bool Enabled()
    {
        // Gameplay-thread observers only. No logging or allocations while disabled.
        IConsoleVariable* Toggle = TraceToggle();
        const bool bOn = Toggle && Toggle->GetInt() > 0;
        if (!bOn)
        {
            if (bWasEnabled)
            {
                bWasEnabled = false; ++CaptureEpoch;
                Scopes.Empty(); Requests.Empty(); Inputs.Empty(); Stock.Empty();
            }
            return false;
        }
        if (!bWasEnabled || LastRun != Token(CVarFireTraceRun.GetValueOnGameThread())
            || LastSession != Token(CVarFireTraceSession.GetValueOnGameThread())) ResetCapture(false);
        return true;
    }
    static void BeginCapture(const TArray<FString>& Args)
    {
        bool Valid = Args.Num() == 2 && !Args[0].IsEmpty() && Args[0] != TEXT("unset")
            && Token(Args[0]) == Args[0] && Args[1].Len() == 32;
        if (Valid) for (TCHAR C : Args[1])
            if (!FChar::IsHexDigit(C)) { Valid = false; break; }
        if (!Valid)
        {
            UE_LOG(LogNCFireTrace, Warning, TEXT("Usage: ncp.FireTraceBegin LABEL FRESH_32_HEX_NONCE (same nonce on both processes)"));
            return;
        }
        CVarFireTraceRun->Set(*Args[0], ECVF_SetByConsole);
        CVarFireTraceSession->Set(*Args[1].ToLower(), ECVF_SetByConsole);
        if (IConsoleVariable* Toggle = TraceToggle())
            Toggle->Set(1, ECVF_SetByConsole);
        ResetCapture(true); // Includes an empty capture; no weapon event is required.
    }
    static void EndCapture()
    {
        if (Enabled() && bWasEnabled && !Capture.IsEmpty())
            UE_LOG(LogNCFireTrace, Warning, TEXT("[NCFireTrace] END schema=2 run=%s session=%s capture=%s seq=%llu limited=%d"),
                *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence, bLimitReported);
        bWasEnabled = false; // The explicit END above completes this capture, not ABORT.
        if (IConsoleVariable* Toggle = TraceToggle())
            Toggle->Set(0, ECVF_SetByConsole);
        ++CaptureEpoch;
        Scopes.Empty(); Requests.Empty(); Inputs.Empty(); Stock.Empty();
    }
    static FAutoConsoleCommand BeginCommand(TEXT("ncp.FireTraceBegin"),
        TEXT("Begin a paired capture: LABEL FRESH_32_HEX_NONCE. Use the same nonce on client and server."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&BeginCapture));
    static FAutoConsoleCommand EndCommand(TEXT("ncp.FireTraceEnd"),
        TEXT("Write the capture completion marker and disable fire provenance."),
        FConsoleCommandDelegate::CreateStatic(&EndCapture));

    FString WireTimestamp(float Value)
    {
        uint32 Bits = 0;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return FString::Printf(TEXT("clientT=%.9g clientBits=%08x"), double(Value), Bits);
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
        const uint64 Limit = uint64(FMath::Clamp(CVarFireTraceLimit.GetValueOnGameThread(), 100, 1000000));
        if (Sequence >= Limit)
        {
            if (!bLimitReported)
            {
                UE_LOG(LogNCFireTrace, Warning, TEXT("[NCFireTrace] LIMIT schema=2 run=%s session=%s capture=%s seq=%llu"),
                    *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence);
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
        FWeaponObservation& Obs = Observations.FindOrAdd(TWeakObjectPtr<AUTWeapon>(W));
        UNetDriver* NetDriver = World->GetNetDriver();
        UNetConnection* Connection = NetDriver && NetDriver->ServerConnection ? NetDriver->ServerConnection
            : (Owner ? Owner->GetNetConnection() : nullptr);
        if (Connection) Obs.Connection = Connection->GetUniqueID();
        const uint32 CurrentOwner = Guid(Owner), CurrentPlayer = Guid(Player);
        const bool Retained = !Owner && Obs.Owner > 0;
        if (Owner)
        {
            Obs.Owner = CurrentOwner; Obs.Player = CurrentPlayer;
        }
        const uint32 OwnerId = Retained ? Obs.Owner : CurrentOwner;
        const uint32 PlayerId = Retained ? Obs.Player : CurrentPlayer;
        if (!Obs.LayoutRecorded && Fix)
        {
            Obs.LayoutRecorded = true; // Set before recursive LAYOUT records.
            Fix->DescribeFireTraceLayout();
        }
        const float SinceEquipMs = Obs.EquipFinished >= 0.f ? (Now - Obs.EquipFinished) * 1000.f : -1.f;
        const FModeObservation& M = Obs.Modes.FindOrAdd(Mode);
        FInputScope* Input = Inputs.FindRef(W);
        FStockScope* Rpc = Stock.FindRef(W);
        const uint64 Action = Input ? Input->Id : (Rpc ? Rpc->Id : 0);
        const float LastFire = Fix && Fix->LastFireTime.IsValidIndex(Mode) ? Fix->LastFireTime[Mode] : -1.f;
        const float Refire = Mode < W->GetNumFireModes() ? W->GetRefireTime(Mode) : -1.f;
        UUTWeaponStateEquipping* Equip = Cast<UUTWeaponStateEquipping>(W->GetCurrentState());
        const float EquipRemaining = Equip ? World->GetTimerManager().GetTimerRemaining(Equip->BringUpFinishedHandle) : -1.f;
        const TCHAR* Side = World->GetNetMode() == NM_Client ? TEXT("client")
            : World->GetNetMode() == NM_Standalone ? TEXT("standalone") : TEXT("server");
        // Split formatting for 4.15's fixed-arity logging templates.
        const FString Identity = FString::Printf(
            TEXT("[NCFireTrace] %s schema=2 run=%s session=%s capture=%s seq=%llu engine=%d.%d build=fire-trace-v2 side=%s world=%u driver=%u weapon=%u player=%u owner=%u actor=%s class=%s mode=%u event=%d generation=%u protocol=%s "),
            Kind, *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence, ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION,
            Side, World->GetUniqueID(), World->GetNetDriver() ? World->GetNetDriver()->GetUniqueID() : 0,
            Guid(W), PlayerId, OwnerId, *Token(W->GetName()),
            *Token(W->GetClass()->GetName()), Mode, Event, Generation, Protocol);
        const FString Timing = FString::Printf(
            TEXT("frame=%llu t=%.6f mono=%.6f dt=%.6f state=%s current=%u pending=%u held0=%d held1=%d lft=%.6f refire=%.6f earliest=%.6f sinceEquipMs=%.3f equipRemaining=%.6f %s"),
            (unsigned long long)GFrameCounter, Now, FPlatformTime::Seconds(), World->GetDeltaSeconds(),
            W->GetCurrentState() ? *Token(W->GetCurrentState()->GetClass()->GetName()) : TEXT("null"),
            Guid(Owner ? Owner->GetWeapon() : nullptr), Guid(Owner ? Owner->GetPendingWeapon() : nullptr),
            Owner && Owner->IsPendingFire(0), Owner && Owner->IsPendingFire(1), LastFire, Refire,
            W->EarliestFireTime, SinceEquipMs, EquipRemaining, *Details);
        const FString Attribution = FString::Printf(
            TEXT(" identity=%s local=%d action=%llu hold=%llu lastHold=%llu startByte=%d volley=%llu volleyHold=%llu connection=%u weaponLocal=%u"),
            Retained ? TEXT("retained") : TEXT("live"), Owner && Owner->IsLocallyControlled(),
            (unsigned long long)Action, (unsigned long long)M.Hold, (unsigned long long)M.LastHold,
            M.StartByte, (unsigned long long)M.Volley, (unsigned long long)M.VolleyHold, Obs.Connection, W->GetUniqueID());
        UE_LOG(LogNCFireTrace, Warning, TEXT("%s%s%s"), *Identity, *Timing, *Attribution);
        UNetDriver* Driver = World->GetNetDriver();
        if (Driver)
        {
            const double RealNow = FPlatformTime::Seconds();
            double* Last = DriverSamples.Find(TWeakObjectPtr<UNetDriver>(Driver));
            if (!Last || RealNow - *Last >= 10.0)
            {
                DriverSamples.Add(Driver, RealNow);
                // Raw counters: InPackets/Lost reset with network stats (except Shipping clients).
                // InOutOfOrderPackets is cumulative. Do not subtract resetting counters.
                UE_LOG(LogNCFireTrace, Warning,
                    TEXT("[NCFireTrace] NETWORK schema=2 run=%s session=%s capture=%s seq=%llu side=%s driver=%u inPacketsRaw=%u inLostRaw=%u outOfOrderTotal=%u counterWindow=stats_reset_or_start"),
                    *LastRun, *LastSession, *Capture, (unsigned long long)++Sequence, Side, Driver->GetUniqueID(),
                    Driver->InPackets, Driver->InPacketsLost, Driver->InOutOfOrderPackets);
            }
        }
    }

    FRequestScope::FRequestScope(AUTWeapon* W, uint8 InMode, int32 InEvent, uint32 InGeneration)
        : Weapon(W), Mode(InMode), Event(InEvent), Generation(InGeneration)
    {
        if (!Enabled() || !W) return;
        Epoch = CaptureEpoch; Previous = Requests.FindRef(W); Requests.Add(W, this);
    }
    FRequestScope::~FRequestScope()
    {
        if (!Enabled() || Epoch != CaptureEpoch) return;
        if (Previous) Requests.Add(Weapon, Previous); else Requests.Remove(Weapon);
    }
    FInputScope::FInputScope(AUTWeapon* W, uint8 InMode, bool InStart)
        : Weapon(W), Mode(InMode), Start(InStart)
    {
        if (!Enabled() || !W) return;
        Epoch = CaptureEpoch; Previous = Inputs.FindRef(W);
        // Subclass and base StartFire/StopFire describe the same synchronous input.
        Id = Previous && Previous->Mode == Mode && Previous->Start == Start ? Previous->Id : ++NextScope;
        Inputs.Add(W, this);
    }
    FInputScope::~FInputScope()
    {
        if (!Enabled() || Epoch != CaptureEpoch) return;
        if (Previous) Inputs.Add(Weapon, Previous); else Inputs.Remove(Weapon);
    }
    FStockScope::FStockScope(AUTWeapon* W, uint8 InMode, uint8 InEvent, bool InStart, bool InSync, const TCHAR* Route)
        : Weapon(W), Mode(InMode), Event(InEvent), Start(InStart), Sync(InSync)
    {
        if (!Enabled() || !W) return;
        Epoch = CaptureEpoch; Id = ++NextScope; Previous = Stock.FindRef(W); Stock.Add(W, this);
        Record(W, TEXT("STOCK_RECEIVE"), Mode, Event, 0,
            FString::Printf(TEXT("rpc=%llu parentRpc=%llu start=%d origin=%s route=%s"),
                (unsigned long long)Id, (unsigned long long)(Previous ? Previous->Id : 0), Start,
                Sync ? TEXT("sync") : TEXT("wire"), Route), TEXT("stock"));
    }
    FStockScope::~FStockScope()
    {
        if (!Enabled() || Epoch != CaptureEpoch) return;
        Record(Weapon.Get(), TEXT("STOCK_RESULT"), Mode, Event, 0,
            FString::Printf(TEXT("rpc=%llu start=%d origin=%s accepted=%d applied=%d applyResult=%d"),
                (unsigned long long)Id, Start, Sync ? TEXT("sync") : TEXT("wire"), Accepted, Applied, ApplyResult), TEXT("stock"));
        if (Previous) Stock.Add(Weapon, Previous); else Stock.Remove(Weapon);
    }
    void Layout(AUTWeapon* W, uint8 Mode, UUTWeaponState* State)
    {
        if (!Enabled()) return;
        Record(W, TEXT("LAYOUT"), Mode, INDEX_NONE, 0,
            FString::Printf(TEXT("firingState=%s"), State ? *Token(State->GetClass()->GetName()) : TEXT("null")), TEXT("state"));
    }
    void StockValidated(AUTWeapon* W, uint8 Mode, uint8 Event, uint8 Before, uint8 After, bool Accepted)
    {
        if (!Enabled() || !W) return;
        FStockScope* S = Stock.FindRef(W);
        if (S && S->Mode == Mode) S->Accepted = Accepted;
        Record(W, TEXT("STOCK_VALIDATE"), Mode, Event, 0,
            FString::Printf(TEXT("rpc=%llu accepted=%d byteBefore=%u byteAfter=%u"),
                (unsigned long long)(S ? S->Id : 0), Accepted, Before, After), TEXT("stock"));
    }
    void StockSent(AUTWeapon* W, uint8 Mode, uint8 Event, bool Start, bool ClientFired)
    {
        if (!Enabled() || !W) return;
        FModeObservation& M = Observations.FindOrAdd(W).Modes.FindOrAdd(Mode);
        if (Start) M.StartByte = Event;
        Record(W, TEXT("STOCK_SEND"), Mode, Event, 0,
            FString::Printf(TEXT("start=%d clientFired=%d"), Start, ClientFired), TEXT("stock"));
    }
    void SequenceBegin(AUTWeapon* W, uint8 Mode, bool Start)
    {
        if (!Enabled() || !W) return;
        FInputScope* I = Inputs.FindRef(W);
        FStockScope* S = Stock.FindRef(W);
        const bool FromInput = I && I->Mode == Mode && I->Start == Start && W->GetNetMode() == NM_Client;
        const bool FromRpc = S && S->Mode == Mode && S->Start == Start && S->Accepted == 1;
        if (FromRpc) ++S->Applied;
        if (Start && (FromInput || FromRpc))
        {
            FModeObservation& M = Observations.FindOrAdd(W).Modes.FindOrAdd(Mode);
            if (M.Hold) Record(W, TEXT("HOLD_END"), Mode, INDEX_NONE, 0, TEXT("reason=superseded"), TEXT("stock"));
            M.Hold = M.LastHold = ++NextScope;
            M.StartByte = FromRpc ? int32(S->Event) : INDEX_NONE;
            Record(W, TEXT("HOLD_BEGIN"), Mode, INDEX_NONE, 0,
                FromRpc && S->Sync ? TEXT("origin=sync") : TEXT("origin=wire"), TEXT("stock"));
        }
        Record(W, TEXT("SEQUENCE_BEGIN"), Mode, INDEX_NONE, 0,
            FString::Printf(TEXT("start=%d rpc=%llu"), Start, (unsigned long long)(S ? S->Id : 0)), TEXT("state"));
    }
    void SequenceEnd(AUTWeapon* W, uint8 Mode, bool Start, bool Result)
    {
        if (!Enabled() || !W) return;
        FStockScope* S = Stock.FindRef(W);
        if (S && S->Mode == Mode && S->Start == Start) S->ApplyResult = Result;
        Record(W, TEXT("SEQUENCE_RESULT"), Mode, INDEX_NONE, 0,
            FString::Printf(TEXT("start=%d result=%d rpc=%llu"), Start, Result, (unsigned long long)(S ? S->Id : 0)), TEXT("state"));
        if (!Start)
        {
            FModeObservation& M = Observations.FindOrAdd(W).Modes.FindOrAdd(Mode);
            if (M.Hold) Record(W, TEXT("HOLD_END"), Mode, INDEX_NONE, 0, TEXT("reason=end_sequence"), TEXT("stock"));
            M.Hold = 0;
        }
    }
    void ChargeCommitted(AUTWeapon* W, uint8 Mode, int32 Loaded)
    {
        if (!Enabled() || !W) return;
        FModeObservation& M = Observations.FindOrAdd(W).Modes.FindOrAdd(Mode);
        M.Volley = ++NextScope; M.VolleyHold = M.LastHold;
        Record(W, TEXT("CHARGE_COMMIT"), Mode, INDEX_NONE, 0,
            FString::Printf(TEXT("loaded=%d"), Loaded), TEXT("stream"));
    }
    void ChargeEnded(AUTWeapon* W, uint8 Mode)
    {
        if (!Enabled() || !W) return;
        Record(W, TEXT("CHARGE_END"), Mode, INDEX_NONE, 0, TEXT("reason=state_end"), TEXT("stream"));
        FModeObservation& M = Observations.FindOrAdd(W).Modes.FindOrAdd(Mode);
        M.Volley = M.VolleyHold = 0;
    }

    FShotScope::FShotScope(AUTWeapon* W, uint8 InMode, int32 InEvent, uint32 InGeneration, const TCHAR* InProtocol)
        : Weapon(W), Mode(InMode), Event(InEvent), Generation(InGeneration), Protocol(InProtocol), bEnabled(Enabled())
    {
        if (!bEnabled || !W) return;
        Epoch = CaptureEpoch;
        FRequestScope* Request = Requests.FindRef(W);
        if (Request && Request->Mode == Mode && !Request->Claimed)
        {
            if (Event == INDEX_NONE) { Event = Request->Event; Generation = Request->Generation; Protocol = TEXT("fixed"); }
            Request->Claimed = true;
        }
        Scope = ++NextScope;
        FShotScope** Existing = Scopes.Find(Weapon);
        Previous = Existing ? *Existing : nullptr;
        Scopes.Add(Weapon, this);
        Record(W, W->GetNetMode() == NM_Client ? TEXT("PREDICT") : TEXT("DISPATCH"), Mode, Event, Generation,
            FString::Printf(TEXT("scope=%llu"), (unsigned long long)Scope), Protocol);
    }
    FShotScope::~FShotScope()
    {
        if (!bEnabled || !Enabled() || Epoch != CaptureEpoch) return;
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
        Epoch = CaptureEpoch;
        if (bEnabled && W) Record(W, TEXT("STATE_REQUEST"), W->GetCurrentFireMode(), INDEX_NONE, 0,
            FString::Printf(TEXT("requested=%s"), Requested ? *Token(Requested->GetClass()->GetName()) : TEXT("null")), TEXT("state"));
    }
    FStateScope::~FStateScope()
    {
        AUTWeapon* W = Weapon.Get();
        if (!bEnabled || !Enabled() || Epoch != CaptureEpoch || !W) return;
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
