// Minimal type/timer adapter for test_instagib_shared_hold.py. The test inserts
// actual production functions below; only engine plumbing and shot effects are
// adapted. RPCs are recorded, not delivered. This is not an Unreal runtime test.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>
using uint8 = uint8_t;
using int32 = int32_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
template<class T> struct TWeakObjectPtr {
    T* Value = nullptr;
    TWeakObjectPtr() = default;
    TWeakObjectPtr(T* value) : Value(value) {}
    T* Get() const { return Value; }
    bool IsValid() const { return Value != nullptr; }
    void Reset() { Value = nullptr; }
    bool operator<(const TWeakObjectPtr& other) const { return std::less<T*>()(Value, other.Value); }
};
template<class K, class V> struct TMap {
    std::map<K,V> Values;
    V* Find(const K& key) { auto it=Values.find(key); return it == Values.end() ? nullptr : &it->second; }
    V& FindChecked(const K& key) { return Values.at(key); }
    void Add(const K& key, const V& value) { Values[key]=value; }
};
constexpr int INDEX_NONE = -1;
namespace NCFireDiagnostics {
    struct FInputScope { template<class... T> FInputScope(T...) {} };
    template<class... T> void Record(T...) {}
}
#define TEXT(x) x
#define UE_LOG(...) ((void)0)
constexpr float SMALL_NUMBER = 1.e-8f;
constexpr int ROLE_Authority = 3, NM_DedicatedServer = 1, MOVE_Falling = 3;
constexpr int NM_Standalone = 0, NM_ListenServer = 2, NM_Client = 3;
uint64 GFrameCounter = 1;
const char* NAME_None = "None";
const char* NAME_Playing = "Playing";
const char* NAME_Inactive = "Inactive";
const char* NAME_Spectating = "Spectating";
struct FString : std::string {
    using std::string::string;
    bool Contains(const char* text) const { return find(text) != npos; }
};
struct UClass { FString Name; FString GetName() const { return Name; } };
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    bool IsValidIndex(int32 i) const { return i >= 0 && i < Num(); }
    int32 Num() const { return static_cast<int32>(this->size()); }
    void Empty() { this->clear(); }
    T& operator[](int32 i) { return this->at(i); }
    const T& operator[](int32 i) const { return this->at(i); }
};
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a, b); }
    template<class T> static T Max(T a, T b) { return std::max(a, b); }
    template<class T> static T Clamp(T a, T lo, T hi) { return Max(lo, Min(a, hi)); }
};
struct FRotator { float Yaw = 0.f; void Normalize() {} bool ContainsNaN() const { return false; } };
struct FTimerHandle { int Id = 0; };
struct FTimerDelegate {
    template<class F> static FTimerDelegate CreateLambda(F f) { FTimerDelegate d; d.Call = f; return d; }
    std::function<void()> Call;
    template<class T, class... Args> void BindUObject(T* obj, void(T::*method)(Args...), Args... args) {
        Call = [=]() { (obj->*method)(args...); };
    }
};
struct FTimerManager {
    struct Entry { float Due, Rate; bool Loop; std::function<void()> Call; };
    std::map<int, Entry> Entries;
    float Now = 10.f;
    int Next = 1;
    bool ReverseTies = false;
    void ClearTimer(FTimerHandle& h) { Entries.erase(h.Id); }
    bool IsTimerActive(const FTimerHandle& h) const { return Entries.count(h.Id) != 0; }
    float GetTimerRemaining(const FTimerHandle& h) const {
        auto it = Entries.find(h.Id); return it == Entries.end() ? -1.f : it->second.Due - Now;
    }
    void SetTimer(FTimerHandle& h, const FTimerDelegate& d, float rate, bool loop) {
        if (h.Id == 0) h.Id = Next++;
        Entries[h.Id] = Entry{Now + rate, rate, loop, d.Call};
    }
    template<class T> void SetTimer(FTimerHandle& h, T* obj, void(T::*method)(), float rate, bool loop) {
        FTimerDelegate d; d.BindUObject(obj, method); SetTimer(h, d, rate, loop);
    }
    void Advance(float end) {
        int iterations = 0;
        while (true) {
            int next = 0; float due = end + 1.f;
            for (const auto& pair : Entries) {
                if (pair.second.Due <= end && (pair.second.Due < due ||
                    (ReverseTies && pair.second.Due == due))) {
                    next = pair.first; due = pair.second.Due;
                }
            }
            if (next == 0) break;
            if (++iterations > 1000) { std::cerr << "timer livelock\n"; std::exit(1); }
            Now = due;
            Entry entry = Entries.at(next);
            if (entry.Loop) Entries.at(next).Due += entry.Rate;
            else Entries.erase(next);
            entry.Call();
        }
        Now = end;
    }
};
struct UDemoNetDriver { bool Playing = false; bool IsPlaying() const { return Playing; } };
struct AUTGameState { bool Prevent = false; bool PreventWeaponFire() const { return Prevent; } };
struct UWorld {
    FTimerManager Timers;
    UDemoNetDriver* DemoNetDriver = nullptr;
    AUTGameState GameState;
    float InputWorldTime = -1.f;
    float RealTimeOffset = 0.f;
    bool Paused = false;
    float GetTimeSeconds() const { return InputWorldTime >= 0 ? InputWorldTime : Timers.Now; }
    float GetRealTimeSeconds() const { return Timers.Now + RealTimeOffset; }
    bool IsPaused() const { return Paused; }
    template<class T> T* GetGameState() { return &GameState; }
};
struct AUTWeapon;
struct AUTCharacter;
struct UInputComponent {};
struct FViewport { bool Focused = true; bool HasFocus() const { return Focused; } };
struct UConsole { bool Active = false; bool ConsoleActive() const { return Active; } };
struct UGameViewportClient {
    bool Ignore = false;
    FViewport TestViewport;
    UConsole TestConsole;
    FViewport* Viewport = &TestViewport;
    UConsole* ViewportConsole = &TestConsole;
    bool IgnoreInput() const { return Ignore; }
};
struct UPlayer { virtual ~UPlayer() = default; };
struct UUTLocalPlayer : UPlayer {
    UGameViewportClient* ViewportClient = nullptr;
    bool MenusOpen = false, QuickChatOpen = false;
    bool AreMenusOpen() const { return MenusOpen; }
    bool IsQuickChatOpen() const { return QuickChatOpen; }
};
struct FDeferredFireInput { uint8 FireMode; bool bStartFire; };
struct AController { virtual ~AController() = default; };
struct AUTPlayerController : AController {
    AUTCharacter* Pawn = nullptr;
    AUTCharacter* UTCharacter = nullptr;
    AUTCharacter* AcknowledgedPawn = nullptr;
    UInputComponent* InputComponent = nullptr;
    UPlayer* Player = nullptr;
    bool Playing = true, IgnoreMove = false, Spectating = false, Cursor = false;
    const char* StateName = NAME_Playing;
    TArray<FDeferredFireInput> DeferredFireInputs;
    AUTCharacter* GetPawn() const { return Pawn; }
    bool IsInState(const char* state) const {
        return state == NAME_Playing ? Playing : state == NAME_Inactive && !Playing && !Spectating;
    }
    bool IsMoveInputIgnored() const { return IgnoreMove; }
    bool ShouldShowMouseCursor() const { return Cursor; }
    bool HasDeferredFireInputs();
    void ApplyDeferredFireInputs();
};
struct AUTPlayerState { virtual ~AUTPlayerState() = default; void NotIdle() {} };
struct AUTCharacter {
    bool Local = true, Player = true, Dead = false, PendingKill = false, Disabled = false;
    bool Feigning = false;
    int TauntCount = 0;
    bool Pending[2] = {false, false};
    float FireRateMultiplier = 1.f;
    AUTWeapon* Weapon = nullptr;
    AUTWeapon* PendingWeapon = nullptr;
    AUTWeapon* PendingAutoSwitchWeapon = nullptr;
    AUTPlayerState* PlayerState = nullptr;
    AController* Controller = nullptr;
    FRotator Aim;
    struct Movement { int MovementMode = 0; } Move;
    bool IsLocallyControlled() const { return Local; }
    bool IsPlayerControlled() const { return Player; }
    bool IsDead() const { return Dead; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsFiringDisabled() const { return Disabled; }
    bool IsFeigningDeath() const { return Feigning; }
    void StartFire(uint8 mode);
    void StopFire(uint8 mode);
    void PawnStartFire(uint8 mode) { StartFire(mode); }
    AUTWeapon* GetWeapon() const { return Weapon; }
    AUTWeapon* GetPendingWeapon() const { return PendingWeapon; }
    bool IsPendingFire(uint8 mode) const { return mode < 2 && Pending[mode]; }
    void SetPendingFire(uint8 mode, bool value) { if (mode < 2) Pending[mode] = value; }
    Movement* GetCharacterMovement() { return &Move; }
    FRotator GetViewRotation() { return Aim; }
    void NotifyPendingServerFire() {}
    template<class T=AUTPlayerState> T* GetPlayerState() { return static_cast<T*>(PlayerState); }
    float GetFireRateMultiplier() const { return FireRateMultiplier; }
    bool IsInInventory(AUTWeapon*) { return true; }
    void SwitchWeapon(AUTWeapon* weapon) { PendingWeapon = weapon; }
};
struct UUTWeaponState {
    AUTWeapon* Weapon = nullptr;
    virtual ~UUTWeaponState() = default;
    virtual UClass* GetClass() const { static UClass c{"State"}; return &c; }
    bool IsA(UClass* cls) const { return GetClass() == cls; }
    FString GetName() const { return GetClass()->GetName(); }
    const char* GetFName() const { return "State"; }
    AUTWeapon* GetOuterAUTWeapon() const { return Weapon; }
    AUTCharacter* GetUTOwner() const;
    virtual bool IsFiring() const { return false; }
    virtual void BeginState(const UUTWeaponState*) {}
    virtual void EndState() {}
    virtual bool BeginFiringSequence(uint8, bool) { return false; }
    virtual void EndFiringSequence(uint8) {}
    void PendingFireStopped() {}
    void PendingFireStarted() {}
};
struct UUTWeaponStateActive : UUTWeaponState {
    void BeginState(const UUTWeaponState*) override;
    bool BeginFiringSequence(uint8, bool) override;
};
struct UUTWeaponStateEquipping : UUTWeaponState {
    int PendingFireSequence = -1;
    void BringUpFinished();
    bool BeginFiringSequence(uint8, bool) override;
};
struct UUTWeaponStateFiring : UUTWeaponState {
    FTimerHandle RefireCheckHandle;
    int PendingFireSequence = -1;
    bool bDelayShot = false;
    bool IsFiring() const override { return true; }
    void ToggleLoopingEffects(bool) {}
    void FireShot();
    void EndState() override;
};
struct UUTWeaponStateFiring_Transactional : UUTWeaponStateFiring {
    static UClass* StaticClass() { static UClass c{"Transactional"}; return &c; }
    UClass* GetClass() const override { return StaticClass(); }
    void BeginState(const UUTWeaponState*) override;
    void RefireCheckTimer();
};
struct UUTWeaponStateFiringChargedRocket_Transactional : UUTWeaponStateFiring {
    bool bCharging = true;
    static UClass* StaticClass() { static UClass c{"Charged"}; return &c; }
    UClass* GetClass() const override { return StaticClass(); }
};
struct UUTWeaponStateZooming : UUTWeaponStateFiring {
    static UClass* StaticClass() { static UClass c{"Zoom"}; return &c; }
    UClass* GetClass() const override { return StaticClass(); }
};
struct FInstantHitDamageInfo {
    int32 Damage = 100;
    UClass* DamageType = nullptr;
    float Momentum = 250000.f, TraceRange = 25000.f, TraceHalfSize = 0.f, ConeDotAngle = 0.f;
};
struct AUTWeapon {
    virtual ~AUTWeapon() = default;
    AUTCharacter* UTOwner = nullptr;
    UWorld* TestWorld = nullptr;
    int Role = 2, NetMode = 0, Ammo = 100, MultiPressCount = 0;
    bool bRootWhileFiring = false, bNetDelayedShot = false, PendingKill = false;
    float LastContinuedFiring = 0.f;
    uint8 CurrentFireMode = 0;
    TArray<UUTWeaponStateFiring*> FiringState;
    TArray<float> FireInterval{1.f, 1.f};
    TArray<int32> AmmoCost{0, 0};
    TArray<FInstantHitDamageInfo> InstantHitInfo{FInstantHitDamageInfo{}, FInstantHitDamageInfo{}};
    TArray<UClass*> ProjClass;
    UUTWeaponState* CurrentState = nullptr;
    UUTWeaponState* ActiveState = nullptr;
    UUTWeaponState* EquippingState = nullptr;
    UUTWeaponState* UnequippingState = nullptr;
    UUTWeaponState* InactiveState = nullptr;
    AUTCharacter* GetUTOwner() const { return UTOwner; }
    UWorld* GetWorld() const { return TestWorld; }
    FTimerManager& GetWorldTimerManager() { return TestWorld->Timers; }
    uint8 GetCurrentFireMode() const { return CurrentFireMode; }
    UUTWeaponState* GetCurrentState() const { return CurrentState; }
    int GetNetMode() const { return NetMode; }
    int GetLocalRole() const { return Role; }
    uint8 GetNumFireModes() const {
        return static_cast<uint8>(std::min(255, std::min(FiringState.Num(), FireInterval.Num())));
    }
    UClass* GetClass() const { static UClass c{"Shock"}; return &c; }
    bool HasAmmo(uint8 mode) const { return AmmoCost.IsValidIndex(mode) && Ammo >= FMath::Min(1, AmmoCost[mode]); }
    bool HasAnyAmmo() const { return true; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool PutDown() { GotoState(UnequippingState); return true; }
    virtual void GotoState(UUTWeaponState* state) {
        UUTWeaponState* previous = CurrentState;
        if (previous) previous->EndState();
        CurrentState = state;
        state->BeginState(previous);
    }
    void GotoActiveState() { GotoState(ActiveState); }
    // Base bring-up cosmetics and RPC bookkeeping are outside this input test.
    // Preserve its state transition so the actual subclass lifetime hooks run.
    virtual void BringUp(float) { GotoState(EquippingState); }
    bool BeginFiringSequence(uint8, bool);
    void EndFiringSequence(uint8);
    float GetRefireTime(uint8);
    bool CanFireAgain();
    bool HandleContinuedFiring();
    void OnContinuedFiring() {}
    void OnStartedFiring() {}
    void OnMultiPress(uint8) { ++MultiPressCount; }
    virtual void StartFire(uint8 mode) { BeginFiringSequence(mode, false); }
    virtual void StopFire(uint8 mode) { EndFiringSequence(mode); }
    virtual void FireShot() = 0;
};
struct AUTWeaponFix : AUTWeapon {
    using Super = AUTWeapon;
    bool bHandlingRetry = false, bIsTransactionalFire = false;
    bool bBufferedClickPending[2] = {false, false}, bCrossModeRetryArmed[2] = {false, false};
    bool bFireHeldByPlayer[2] = {false, false};
    void* ShockInputTraceInputComponent = nullptr;
    uint8 CurrentlyFiringMode = 255;
    TArray<int32> FireModeActiveState{0, 0}, ClientFireEventIndex{0, 0};
    TArray<float> LastFireTime{0.f, 0.f}, LastReleaseTime{0.f, 0.f};
    float EarliestFireTime = 0.f, MouseDebounceWindow = 0.03f;
    FTimerHandle RetryFireHandle[2], DeferredActiveStateHandle;
    std::vector<std::pair<float, uint8>> Shots;
    std::vector<FRotator> ShotAims;
    std::function<void()> OnShot;
    std::vector<std::pair<uint8, int32>> Stops;
    bool TryPreserveInstagibHeldFire(uint8);
    void StartFire(uint8) override;
    void StopFire(uint8) override;
    void StopFireInternal(uint8);
    void OnRetryTimer(uint8);
    void DeferredGotoActiveState(uint8);
    void ClearDeferredActiveState();
    void ScheduleDeferredActiveState(uint8, float);
    bool IsFireModeOnCooldown(uint8, float);
    void OnBufferedClickRetryTimer(uint8, FRotator, float) { std::abort(); }
    void ServerStopFireFixed(uint8 mode, int32 event) { Stops.emplace_back(mode, event); }
    void QueueResendStopFireFixed(uint8, int32) {}
    void FireShot() override {
        Shots.emplace_back(GetWorld()->GetTimeSeconds(), CurrentFireMode);
        ShotAims.push_back(UTOwner->GetViewRotation());
        LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
        ++ClientFireEventIndex[CurrentFireMode];
        if (OnShot) OnShot();
    }
};
struct AUTPlusShockRifle : AUTWeaponFix {
    using Super = AUTWeaponFix;
    bool InstagibIdentity = true;
    uint8 PendingInstagibEquipTapMode = 255;
    bool bInstagibTapAwaitingPossession = false;
    bool bInstagibPossessionTapReleased = false;
    float InstagibPossessionTapDeadline = 0.f;
    TWeakObjectPtr<AUTCharacter> PendingInstagibEquipTapOwner, InstagibEquipPressOwner;
    TWeakObjectPtr<AUTPlayerController> PendingInstagibEquipTapController, InstagibEquipInputController;
    TWeakObjectPtr<UInputComponent> InstagibEquipInputComponent;
    bool bInstagibEquipPress[2] = {false, false};
    bool bProcessingInstagibEquipStart = false;
    uint64 InstagibEquipPressFrame[2] = {0, 0};
    uint32 InstagibEquipInputSerial = 0;
    bool IsInstagibBeamWeapon() const { return InstagibIdentity; }
    bool HasSharedInstagibFireModes() const;
    bool CanRetainInstagibEquipTap(uint8, bool = false);
    void ClearInstagibEquipTap();
    void PumpInstagibEquipTap();
    void NoteInstagibEquipPress(uint8);
    void NoteInstagibEquipRelease(uint8);
    bool ConsumeInstagibEquipPress(uint8);
    void StartFire(uint8) override;
    void StopFire(uint8) override;
    void GotoState(UUTWeaponState*) override;
    void BringUp(float) override;
    // Real binding installation/removal is covered by the provenance suite;
    // this harness models its lifetime reset but executes Note/Consume above.
    void StopInstagibEquipInput() {
        InstagibEquipInputController.Reset(); InstagibEquipInputComponent.Reset();
        InstagibEquipPressOwner.Reset();
        for (int mode = 0; mode < 2; ++mode) {
            bInstagibEquipPress[mode] = false; InstagibEquipPressFrame[mode] = 0;
        }
        ++InstagibEquipInputSerial; ClearInstagibEquipTap();
    }
    void RefreshInstagibEquipInput() {
        auto* pc = UTOwner ? dynamic_cast<AUTPlayerController*>(UTOwner->Controller) : nullptr;
        InstagibEquipInputController = pc;
        InstagibEquipInputComponent = pc ? pc->InputComponent : nullptr;
    }
};
AUTCharacter* UUTWeaponState::GetUTOwner() const { return Weapon->UTOwner; }
void AUTCharacter::StartFire(uint8 mode) { if (Weapon) Weapon->StartFire(mode); }
void AUTCharacter::StopFire(uint8 mode) { if (Weapon) Weapon->StopFire(mode); }
void UUTWeaponStateFiring::FireShot() { Weapon->FireShot(); }
void UUTWeaponStateFiring::EndState() { Weapon->GetWorldTimerManager().ClearTimer(RefireCheckHandle); }
template<class T, class U> T* Cast(U* p) { return dynamic_cast<T*>(p); }
template<class T> struct CVar { T Value; T GetValueOnGameThread() const { return Value; } };
CVar<int32> CVarInstagibSharedHold{1};
CVar<int32> CVarInstagibEquipTap{1};
CVar<float> CVarMouseDebounceCap{0.01f};
bool GhostEnabled = false;
bool GhostFix() { return GhostEnabled; }
bool CrossModeRetry() { return true; }
bool FireDbg() { return false; }
bool RocketPrimaryDiagFor(AUTWeapon*, uint8) { return false; }
bool RocketPrimaryDiagTransactional(AUTWeapon*) { return false; }
float GetClickBufferWindowSeconds() { return 0.f; }
bool IsShockPrimaryClickBuffer(AUTWeapon*, uint8) { return false; }
namespace NCShockInputTrace {
    void RecordWeaponStart(AUTWeapon*, bool, bool, float, const char*) {}
    void RecordWeaponStop(AUTWeapon*, bool, const char*) {}
}

// Existing production loops use int32 array indices with uint8 engine mode APIs.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#endif
// This input-state harness deliberately adapts the clock; the separate native
// NCClientFireTiming test executes the real clock helper with phased timers.
namespace NCClientFireTiming {
    bool IsLocal(AUTWeaponFix* w) {
        return w && w->GetWorld() && w->UTOwner && w->UTOwner->Local &&
            !(w->GetWorld()->DemoNetDriver && w->GetWorld()->DemoNetDriver->IsPlaying());
    }
    float Remaining(AUTWeaponFix* w, uint8 mode) {
        return w->LastFireTime.IsValidIndex(mode) && w->LastFireTime[mode] > 0
            ? w->LastFireTime[mode] + w->GetRefireTime(mode) - w->GetWorld()->GetTimeSeconds() : 0;
    }
    float MaxRemaining(AUTWeaponFix* w) {
        float remaining = FMath::Max(0.f, w->EarliestFireTime - w->GetWorld()->GetTimeSeconds());
        for (int32 mode = 0; mode < w->LastFireTime.Num(); ++mode)
            remaining=FMath::Max(remaining, Remaining(w, uint8(mode)));
        return remaining;
    }
}
static TMap<TWeakObjectPtr<AUTWeaponFix>, uint64> DeferredActiveGenerations;
static uint64 NextDeferredActiveGeneration = 1;
// NATIVE_METHODS
#ifdef _MSC_VER
#pragma warning(pop)
#endif

void Require(bool pass, const char* message) {
    if (!pass) { std::cerr << message << '\n'; std::exit(1); }
}
void Near(float actual, float expected, const char* message) { Require(std::abs(actual - expected) < 0.0001f, message); }
struct Fixture {
    AUTPlusShockRifle W;
    AUTCharacter Pawn;
    AUTPlayerController Controller;
    UInputComponent Input;
    UGameViewportClient Viewport;
    UUTLocalPlayer LocalPlayer;
    UWorld World;
    UUTWeaponStateActive Active;
    UUTWeaponStateEquipping Equip;
    UUTWeaponState Unequip, Inactive;
    UUTWeaponStateFiring_Transactional Mode[2];
    UClass DamageType{"Instagib"}, CoreType{"ShockBall"};
    Fixture() {
        Pawn.Weapon = &W; W.UTOwner = &Pawn; W.TestWorld = &World;
        Pawn.Controller = &Controller; Controller.Pawn = &Pawn; Controller.InputComponent = &Input;
        Controller.UTCharacter = &Pawn; Controller.AcknowledgedPawn = &Pawn;
        LocalPlayer.ViewportClient = &Viewport; Controller.Player = &LocalPlayer;
        W.RefreshInstagibEquipInput();
        W.ActiveState = &Active; W.EquippingState = &Equip;
        W.UnequippingState = &Unequip; W.InactiveState = &Inactive;
        W.CurrentState = &Active; W.FiringState = {&Mode[0], &Mode[1]};
        for (UUTWeaponState* s : std::initializer_list<UUTWeaponState*>{&Active, &Equip, &Unequip,
                                &Inactive, &Mode[0], &Mode[1]}) s->Weapon = &W;
        W.InstantHitInfo[0].DamageType = &DamageType; W.InstantHitInfo[1].DamageType = &DamageType;
    }
    void At(float offset) { ++GFrameCounter; World.Timers.Advance(10.f + offset); W.PumpInstagibEquipTap(); }
    void Down(uint8 mode) { W.StartFire(mode); }
    void PhysicalDown(uint8 mode) { W.NoteInstagibEquipPress(mode); Down(mode); }
    void BeginEquip() { W.BringUp(0.f); }
    void BeforePossession() {
        W.NetMode = NM_Client; Controller.Playing = false;
        Controller.StateName = NAME_Inactive; Controller.AcknowledgedPawn = nullptr;
    }
    void Acknowledge() {
        Controller.Playing = true; Controller.StateName = NAME_Playing;
        Controller.AcknowledgedPawn = &Pawn;
    }
    void QueuePress(uint8 mode) {
        Controller.DeferredFireInputs.push_back({mode, true}); W.NoteInstagibEquipPress(mode);
    }
    void QueueRelease(uint8 mode) {
        Controller.DeferredFireInputs.push_back({mode, false}); W.NoteInstagibEquipRelease(mode);
    }
    void FinishEquip() { Equip.BringUpFinished(); }
    void Up(uint8 mode) { W.StopFire(mode); }
    void Count(size_t count) const { Require(W.Shots.size() == count, "unexpected shot count"); }
    void Cadence(float interval = 1.f) const {
        for (size_t i = 1; i < W.Shots.size(); ++i)
            Require(W.Shots[i].first - W.Shots[i-1].first >= interval - .0001f, "shared hold exceeded refire rate");
    }
};

void Classifier() {
    Fixture base; Require(base.W.HasSharedInstagibFireModes(), "shipped identical beam shape rejected");
    for (int change = 0; change < 18; ++change) {
        Fixture f;
        switch (change) {
        case 0: f.W.InstagibIdentity = false; break;
        case 1: f.W.ProjClass = {nullptr, &f.CoreType}; break;
        case 2: f.W.ProjClass = {&f.CoreType, nullptr}; break;
        case 3: f.W.FireInterval[1] = .5f; break;
        case 4: f.W.FireInterval = {0.f, 0.f}; break;
        case 5: f.W.FireInterval.resize(1); break;
        case 6: f.W.FiringState.resize(1); break;
        case 7: f.W.FiringState[1] = nullptr; break;
        case 8: f.W.InstantHitInfo.resize(1); break;
        case 9: f.W.AmmoCost.resize(1); break;
        case 10: f.W.AmmoCost[1] = 1; break;
        case 11: f.W.InstantHitInfo[1].Damage = 50; break;
        case 12: f.W.InstantHitInfo[1].DamageType = nullptr; break;
        case 13: f.W.InstantHitInfo[1].Momentum = 20.f; break;
        case 14: f.W.InstantHitInfo[1].TraceRange = 15000.f; break;
        case 15: f.W.InstantHitInfo[1].TraceHalfSize = 4.f; break;
        case 16: f.W.InstantHitInfo[1].ConeDotAngle = .99f; break;
        case 17: f.W.FiringState[1] = f.W.FiringState[0]; break;
        }
        Require(!f.W.HasSharedInstagibFireModes(), "nonidentical/invalid beam configuration accepted");
    }
    UUTWeaponStateZooming zoom; UUTWeaponStateFiringChargedRocket_Transactional charged;
    UUTWeaponStateFiring custom;
    for (auto* state : std::initializer_list<UUTWeaponStateFiring*>{&zoom, &charged, &custom}) {
        Fixture f; f.W.FiringState[1] = state;
        Require(!f.W.HasSharedInstagibFireModes(), "custom mode accepted as plain beam");
    }
}
void Guards() {
    for (int change = 0; change < 14; ++change) {
        Fixture f; UDemoNetDriver demo; uint8 mode = 1;
        f.Down(0);
        switch (change) {
        case 0: CVarInstagibSharedHold.Value = 0; break;
        case 1: mode = 255; break;
        case 2: f.W.UTOwner = nullptr; break;
        case 3: f.Pawn.Local = false; break;
        case 4: f.Pawn.Player = false; break;
        case 5: f.Pawn.Dead = true; break;
        case 6: f.Pawn.Weapon = nullptr; break;
        case 7: f.Pawn.PendingWeapon = &f.W; break;
        case 8: f.W.bBufferedClickPending[1] = true; break;
        case 9: f.W.TestWorld = nullptr; break;
        case 10: demo.Playing = true; f.World.DemoNetDriver = &demo; break;
        case 11: f.W.CurrentFireMode = 1; break;
        case 12: f.W.CurrentState = &f.Active; break;
        case 13: f.Pawn.Pending[0] = false; break;
        }
        const bool pending = f.Pawn.Pending[0];
        auto* state = f.W.CurrentState;
        const auto timerCount = f.World.Timers.Entries.size();
        Require(!f.W.TryPreserveInstagibHeldFire(mode), "ineligible shared hold accepted");
        Require(f.Pawn.Pending[0] == pending && !f.Pawn.Pending[1] && f.W.CurrentState == state,
                "failed guard mutated pending input or state");
        Require(f.World.Timers.Entries.size() == timerCount, "failed guard mutated timers");
        f.Count(1); CVarInstagibSharedHold.Value = 1;
    }
}
void Overlap() {
    for (bool ghost : {false, true}) for (uint8 original : {uint8(0), uint8(1)}) {
        GhostEnabled = ghost;
        // Regression witness: the same inputs with the old branch lose the hold.
        CVarInstagibSharedHold.Value = 0;
        Fixture legacy; legacy.Down(original); legacy.At(.026f); legacy.Down(original ^ 1);
        legacy.At(.090f); legacy.Up(original ^ 1); legacy.At(2.1f); legacy.Count(1);
        CVarInstagibSharedHold.Value = 1;
        Fixture f; f.Down(original); f.At(.026f); f.Down(original ^ 1);
        Require(f.Pawn.Pending[0] && f.Pawn.Pending[1], "overlap discarded original held input");
        Require(f.W.CurrentFireMode == original && f.W.CurrentlyFiringMode == original, "overlap switched active beam");
        f.At(.090f); f.Up(original ^ 1); f.At(2.1f); f.Count(3); f.Cadence();
        if (ghost) Require(f.W.bFireHeldByPlayer[original], "other release cleared physical hold tracker");
        f.Up(original); f.At(4.f); f.Count(3);
        Require(!f.Pawn.Pending[0] && !f.Pawn.Pending[1], "both releases left held input");
    }
    GhostEnabled = false;
}
void Handoff() {
    for (uint8 original : {uint8(0), uint8(1)}) {
        Fixture f; f.Down(original); f.At(.1f); f.Down(original ^ 1); f.At(1.2f);
        f.Count(2); f.Cadence(); f.Up(original); f.At(1.999f); f.Count(2);
        f.At(2.f); f.Count(3);
        Require(f.W.Shots.back().second == (original ^ 1), "held alternate did not own handoff");
        Near(f.W.Shots.back().first, 12.f, "handoff missed original cooldown boundary");
        Require(f.W.Stops.back().first == original, "release RPC mode was remapped");
        f.At(3.f); f.Count(4); f.Cadence(); f.Up(original ^ 1); f.At(5.f); f.Count(4);
    }
}
void Recorded() {
    const float otherDown[] = {.026f, .028f, .076f};
    const float otherUp[] = {.090f, .081f, .134f};
    const float primaryUp[] = {.241f, .233f, .288f};
    for (int i = 0; i < 3; ++i) {
        Fixture f; f.Down(0); f.At(otherDown[i]); f.Down(1);
        f.At(otherUp[i]); f.Up(1); f.At(primaryUp[i]); f.Up(0); f.At(2.f); f.Count(1);
    }
}
void Debounce() {
    for (bool reverse : {false, true}) for (bool releaseOriginal : {false, true}) {
        Fixture f; f.World.Timers.ReverseTies = reverse;
        f.Down(0); f.At(.026f); f.Down(1); f.At(.08f); f.Up(1); f.At(.085f); f.Down(1);
        Require(!f.World.Timers.IsTimerActive(f.W.RetryFireHandle[1]), "ready shared-mode press needlessly deferred");
        if (releaseOriginal) { f.At(.3f); f.Up(0); }
        f.At(2.1f); f.Count(3); f.Cadence();
        Require(!f.World.Timers.IsTimerActive(f.W.RetryFireHandle[1]), "debounce retry survived handoff");
        f.Up(0); f.Up(1); f.At(4.f); f.Count(3);
    }
}
void Repress() {
    for (bool reverse : {false, true}) {
        Fixture f; f.World.Timers.ReverseTies = reverse;
        f.Down(0); f.At(.1f); f.Up(0); f.At(.2f); f.Down(0); f.At(.3f); f.Down(1);
        f.At(2.1f); f.Count(3); f.Cadence();
        Require(f.Pawn.Pending[0] && f.Pawn.Pending[1], "deferred/retry ordering lost one held mode");
        f.Up(0); f.Up(1); f.At(4.f); f.Count(3);
    }
}
void EarlyTaps() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture equip; equip.W.CurrentState = &equip.Equip; equip.Down(mode);
        equip.At(.1f); equip.Up(mode); equip.At(.4f); equip.W.GotoActiveState(); equip.At(2.f); equip.Count(0);
        Fixture cooldown; cooldown.Down(mode); cooldown.At(.1f); cooldown.Up(mode);
        cooldown.At(.25f); cooldown.Down(mode); cooldown.At(.4f); cooldown.Up(mode);
        cooldown.At(2.f); cooldown.Count(1);
        Fixture held; held.W.CurrentState = &held.Equip; held.Down(mode);
        held.At(.4f); held.W.GotoActiveState(); held.Count(1); held.At(.5f); held.Up(mode);
        held.At(2.f); held.Count(1);
    }
}
void Combo() {
    // Compare all input and timer outcomes with the old path, including core ->
    // M1 while held and after release. Projectile collision/combo effects require PIE.
    for (bool releaseCore : {false, true}) {
        std::vector<std::pair<float, uint8>> previous;
        int previousMulti = 0;
        for (int enabled : {0, 1}) {
            CVarInstagibSharedHold.Value = enabled;
            Fixture f; f.W.InstagibIdentity = false; f.W.ProjClass = {nullptr, &f.CoreType};
            f.Down(1); f.At(.05f); if (releaseCore) f.Up(1);
            f.At(.06f); f.Down(0); f.At(1.1f); f.Up(1); f.Up(0); f.At(3.f);
            f.Count(2); Require(f.W.Shots[0].second == 1 && f.W.Shots[1].second == 0,
                                "core then primary sequence was lost");
            if (enabled == 0) { previous = f.W.Shots; previousMulti = f.W.MultiPressCount; }
            else {
                Require(f.W.Shots == previous && f.W.MultiPressCount == previousMulti,
                        "shared hold changed normal Shock combo dispatch");
                Require(!f.Pawn.Pending[0] && !f.Pawn.Pending[1], "normal Shock release changed");
            }
        }
    }
    CVarInstagibSharedHold.Value = 1;
}
void Local() {
    for (int role : {2, ROLE_Authority}) {
        Fixture f; f.W.Role = role; f.Pawn.FireRateMultiplier = 2.f;
        f.Down(0); f.At(.05f); f.Down(1); f.At(.1f); f.Up(0);
        f.At(.499f); f.Count(1); f.At(.5f); f.Count(2); f.At(1.1f); f.Count(3); f.Cadence(.5f);
        f.Up(1); f.At(2.f); f.Count(3);
    }
}
void Boundary() {
    for (float release : {.999f, 1.001f}) for (uint8 firstRelease : {uint8(0), uint8(1)}) {
        Fixture f; f.Down(0); f.At(.1f); f.Down(1); f.At(release);
        const size_t count = release < 1.f ? 1 : 2;
        f.Up(firstRelease); f.Up(firstRelease ^ 1); f.At(3.f); f.Count(count); f.Cadence();
    }
}
void StaleRelease() {
    for (float newPress : {11.005f, 11.040f}) {
        Fixture f;
        f.Down(0); f.At(.1f); f.Up(0);
        const auto expiredCallback = f.World.Timers.Entries.at(f.W.DeferredActiveStateHandle.Id).Call;
        f.At(.2f); f.Down(0); f.At(.991f); f.Up(0);
        Require(f.W.CurrentState == &f.Active, "early release did not enter Active");
        Require(!f.World.Timers.IsTimerActive(f.W.DeferredActiveStateHandle), "old release timer survived");
        f.World.InputWorldTime = newPress;
        f.Down(0); f.Count(2);
        // Also exercise a callback already captured by a dispatcher: generation
        // validation must reject it even if the same state object was reused.
        expiredCallback();
        Require(f.Pawn.Pending[0], "old callback cleared new held intent");
        f.World.Timers.Advance(newPress);
        f.World.InputWorldTime = -1.f;
        f.At(2.2f); f.Count(3);
        Require(f.Pawn.Pending[0], "new firing cycle was cancelled");
        f.Up(0); f.At(4.f); f.Count(3);
    }
}
void ReadyDebounce() {
    for (uint8 mode : {uint8(0),uint8(1)}) {
        Fixture f; f.Down(mode); f.At(.995f); f.Up(mode); f.At(1.001f);
        f.Down(mode); f.Count(2);
        Require(!f.World.Timers.IsTimerActive(f.W.RetryFireHandle[mode]), "ready press was deferred");
        f.Up(mode); f.At(3.f); f.Count(2);
        Require(!f.Pawn.Pending[mode], "released ready click became a hold");
        // A real early press still obeys cooldown and still cancels on release.
        Fixture early; early.Down(mode); early.At(.90f); early.Up(mode);
        early.At(.906f); early.Down(mode);
        Require(early.World.Timers.IsTimerActive(early.W.RetryFireHandle[mode]), "early press lost retry");
        early.Up(mode); early.At(3.f); early.Count(1);
    }
}
void ReleaseOwnership() {
    Fixture f; f.Down(0); f.At(.1f); f.Up(0);
    const auto old = f.World.Timers.Entries.at(f.W.DeferredActiveStateHandle.Id).Call;
    AUTCharacter other; other.Weapon=&f.W; other.Pending[0]=true;
    f.W.UTOwner=&other;
    old();
    Require(other.Pending[0] && f.W.CurrentState == &f.Mode[0], "old owner callback altered replacement owner");
}
void EquipTap() {
    for (int netMode : {NM_Client, NM_Standalone, NM_ListenServer}) {
        for (uint8 mode : {uint8(0), uint8(1)}) {
            Fixture f; f.W.NetMode = netMode; f.W.Role = netMode == NM_Client ? 2 : ROLE_Authority;
            f.BeginEquip(); f.PhysicalDown(mode); f.At(.05f); f.Up(mode);
            Require(!f.Pawn.Pending[mode], "physical release retained a held bit");
            Require(f.W.PendingInstagibEquipTapMode == mode, "eligible equip action was not retained");
            f.At(.399f); f.Count(0); f.At(.4f); f.FinishEquip(); f.Count(1);
            Near(f.W.Shots[0].first, 10.4f, "equip tap fired before legal completion");
            Require(f.W.Shots[0].second == mode, "equip tap changed fire mode");
            Require(!f.Pawn.Pending[0] && !f.Pawn.Pending[1], "retained tap became a hold");
            f.At(4.f); f.Count(1);
        }
    }
}
void EquipHold() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (bool overlap : {false, true}) {
        Fixture f; f.BeginEquip(); f.PhysicalDown(mode);
        if (overlap) { f.At(.1f); f.PhysicalDown(mode ^ 1); }
        f.At(.4f); f.FinishEquip(); f.Count(1);
        Require(f.W.PendingInstagibEquipTapMode == 255, "normal held firing left equip intent");
        f.At(2.401f); f.Count(3); f.Cadence();
        f.Up(0); f.Up(1); f.At(4.f); f.Count(3);
    }
}
void EquipProvenance() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        // A prior frame already established a held mode. Cross-mode cleanup
        // for the first queued action must not invalidate a later real action.
        for (bool overlapping : {false, true}) {
            Fixture f; f.BeginEquip(); f.PhysicalDown(mode); f.At(.02f);
            f.W.NoteInstagibEquipPress(mode ^ 1); f.W.NoteInstagibEquipPress(mode);
            f.Down(mode ^ 1);
            if (overlapping) { f.Down(mode); f.Up(mode ^ 1); f.Up(mode); }
            else { f.Up(mode ^ 1); f.Down(mode); f.Up(mode); }
            Require(f.W.PendingInstagibEquipTapMode == mode,
                    "cross-mode equip cleanup erased a later queued physical action");
            f.At(.4f); f.FinishEquip(); f.Count(1);
            Require(f.W.Shots[0].second == mode, "queued return to held mode was not retained");
            f.At(3.f); f.Count(1);
        }
        // UE collects action observers before draining the controller's queued
        // fire inputs. Stamp all actions first, then replay their FIFO dispatch.
        for (bool overlapping : {false, true}) for (bool sameMode : {false, true}) {
            Fixture f; f.BeginEquip();
            const uint8 secondMode = sameMode ? mode : uint8(mode ^ 1);
            f.W.NoteInstagibEquipPress(mode);
            f.W.NoteInstagibEquipPress(secondMode);
            f.Down(mode);
            if (overlapping) { f.Down(secondMode); f.Up(mode); f.Up(secondMode); }
            else { f.Up(mode); f.Down(secondMode); f.Up(secondMode); }
            Require(f.W.PendingInstagibEquipTapMode == secondMode,
                    "queued same-frame actions lost the latest equip intent");
            f.At(.4f); f.FinishEquip(); f.Count(1);
            Require(f.W.Shots[0].second == secondMode, "queued action order changed retained mode");
            f.At(3.f); f.Count(1);
        }
        for (uint8 releasedFirst : {uint8(0), uint8(1)}) {
            Fixture f; f.BeginEquip(); f.PhysicalDown(mode);
            f.At(.03f); f.PhysicalDown(mode ^ 1);
            f.At(.06f); f.Up(releasedFirst); f.At(.08f); f.Up(releasedFirst ^ 1);
            f.At(.4f); f.FinishEquip(); f.Count(1);
            Require(f.W.Shots[0].second == uint8(mode ^ 1), "overlapping equip taps lost latest action");
            f.At(3.f); f.Count(1);
        }
        for (bool switchMode : {false, true}) {
            Fixture f; f.BeginEquip(); f.PhysicalDown(mode); f.At(.05f); f.Up(mode);
            const uint8 lastMode = switchMode ? uint8(mode ^ 1) : mode;
            f.At(.12f); f.PhysicalDown(lastMode); f.At(.16f); f.Up(lastMode);
            f.At(.4f); f.FinishEquip(); f.Count(1);
            Require(f.W.Shots[0].second == lastMode, "equip taps did not coalesce into latest mode");
            f.At(3.f); f.Count(1);
        }
        for (int synthetic = 0; synthetic < 4; ++synthetic) {
            Fixture f; f.BeginEquip();
            if (synthetic == 1) { f.Down(mode); f.Up(mode); }
            if (synthetic == 2) { f.W.NoteInstagibEquipPress(mode); ++GFrameCounter; f.Down(mode); f.Up(mode); }
            if (synthetic == 3) {
                f.W.NoteInstagibEquipPress(mode);
                Require(f.W.ConsumeInstagibEquipPress(mode), "valid token was rejected");
                Require(!f.W.ConsumeInstagibEquipPress(mode), "token was reusable");
                f.Down(mode); f.Up(mode);
            }
            f.At(.4f); f.FinishEquip(); f.At(2.f); f.Count(0);
        }
        // A respawn/repossess cannot inherit an old pawn's same-frame action.
        Fixture f; f.BeginEquip(); f.W.NoteInstagibEquipPress(mode);
        AUTCharacter replacement; replacement.Controller = &f.Controller; replacement.Weapon = &f.W;
        f.Controller.Pawn = &replacement; f.W.UTOwner = &replacement;
        f.Down(mode); f.Up(mode); f.At(.4f); f.FinishEquip(); f.At(2.f); f.Count(0);
    }
}
void EquipGuards() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (int change = 0; change < 20; ++change) {
        Fixture f; UDemoNetDriver demo; AUTPlayerController other; UInputComponent otherInput;
        f.BeginEquip();
        switch (change) {
        case 0: CVarInstagibEquipTap.Value = 0; break;
        case 1: f.Pawn.Dead = true; break;
        case 2: f.Pawn.PendingKill = true; break;
        case 3: f.W.PendingKill = true; break;
        case 4: f.Pawn.Local = false; break;
        case 5: f.Pawn.Player = false; break;
        case 6: f.Pawn.Weapon = nullptr; break;
        case 7: f.Pawn.PendingWeapon = &f.W; break;
        case 8: f.Pawn.Disabled = true; break;
        case 9: f.World.GameState.Prevent = true; break;
        case 10: f.W.Ammo = 0; f.W.AmmoCost = {1, 1}; break;
        case 11: f.W.InstagibIdentity = false; break;
        case 12: demo.Playing = true; f.World.DemoNetDriver = &demo; break;
        case 13: f.Controller.Playing = false; break;
        case 14: f.Controller.IgnoreMove = true; break;
        case 15: f.W.bRootWhileFiring = true; f.Pawn.Move.MovementMode = MOVE_Falling; break;
        case 16: f.Controller.Pawn = nullptr; break;
        case 17: f.Pawn.Controller = &other; other.Pawn = &f.Pawn; other.InputComponent = &otherInput; break;
        case 18: f.Controller.InputComponent = &otherInput; break;
        case 19: f.W.NetMode = NM_DedicatedServer; break;
        }
        f.W.NoteInstagibEquipPress(mode);
        Require(!f.W.ConsumeInstagibEquipPress(mode), "ineligible state accepted equip action provenance");
        Require(f.W.PendingInstagibEquipTapMode == 255, "ineligible action queued a shot");
        CVarInstagibEquipTap.Value = 1;
    }
}
void EquipLifecycle() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (int change = 0; change < 11; ++change) {
        Fixture f; AUTPlayerController other; AUTCharacter replacement;
        f.BeginEquip(); f.PhysicalDown(mode); f.Up(mode);
        switch (change) {
        case 0: f.W.StopFireInternal(mode); break;
        case 1: f.BeginEquip(); break; // Re-bring-up while the same state object is current.
        case 2: f.W.GotoState(&f.Unequip); f.W.GotoState(&f.Equip); break;
        case 3: f.W.GotoState(&f.Inactive); f.W.GotoState(&f.Equip); break;
        case 4: f.Pawn.Dead = true; f.W.PumpInstagibEquipTap(); f.Pawn.Dead = false; break;
        case 5: f.Pawn.PendingWeapon = &f.W; f.W.PumpInstagibEquipTap(); f.Pawn.PendingWeapon = nullptr; break;
        case 6: f.World.GameState.Prevent = true; f.W.PumpInstagibEquipTap(); f.World.GameState.Prevent = false; break;
        case 7: f.W.Ammo = 0; f.W.AmmoCost = {1,1}; f.W.PumpInstagibEquipTap(); f.W.Ammo = 100; break;
        case 8:
            other.Pawn = &f.Pawn; other.InputComponent = &f.Input; f.Pawn.Controller = &other;
            f.W.PumpInstagibEquipTap(); f.Pawn.Controller = &f.Controller; break;
        case 9:
            replacement.Controller = &f.Controller; replacement.Weapon = &f.W;
            f.Controller.Pawn = &replacement; f.W.UTOwner = &replacement;
            f.W.PumpInstagibEquipTap(); f.W.UTOwner = &f.Pawn; f.Controller.Pawn = &f.Pawn; break;
        case 10: f.Pawn.Disabled = true; f.W.PumpInstagibEquipTap(); f.Pawn.Disabled = false; break;
        }
        Require(f.W.PendingInstagibEquipTapMode == 255, "invalidated lifecycle retained equip intent");
        f.At(.4f); f.FinishEquip(); f.At(3.f); f.Count(0);
    }
    // A new bring-up invalidates even an action stamped in this same frame.
    Fixture f; f.BeginEquip(); f.W.NoteInstagibEquipPress(0); f.BeginEquip();
    f.Down(0); f.Up(0); f.At(.4f); f.FinishEquip(); f.At(2.f); f.Count(0);
}
void EquipDispatch() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (bool earliest : {false, true}) {
        Fixture f; f.BeginEquip(); f.Pawn.Aim.Yaw = 20.f;
        f.PhysicalDown(mode); f.At(.05f); f.Up(mode);
        if (earliest) f.W.EarliestFireTime = 10.40002f;
        else f.W.LastFireTime[mode] = 9.40002f;
        f.At(.4f); f.FinishEquip(); f.Count(0);
        Require(f.W.PendingInstagibEquipTapMode == mode, "tiny positive readiness residue discarded intent");
        f.Pawn.Aim.Yaw = 75.f; f.At(.401f); f.Count(1);
        Near(f.W.Shots[0].first, 10.401f, "retained shot reused input time");
        Near(f.W.ShotAims[0].Yaw, 75.f, "retained shot reused input aim");
        Require(!f.Pawn.Pending[0] && !f.Pawn.Pending[1], "dispatch failed one-shot cleanup");
        f.At(3.f); f.Count(1);
    }
    // A later ordinary cooldown tap still cancels when released.
    Fixture f; f.BeginEquip(); f.PhysicalDown(0); f.Up(0); f.At(.4f); f.FinishEquip();
    f.At(.6f); f.PhysicalDown(0); f.At(.7f); f.Up(0); f.At(3.f); f.Count(1);
}
void EquipFreshInput() {
    Fixture f; f.BeginEquip(); f.PhysicalDown(0); f.Up(0);
    f.W.EarliestFireTime = 10.8f; f.At(.4f); f.FinishEquip(); f.Count(0);
    f.At(.5f); f.PhysicalDown(1); f.At(.6f); f.Up(1); f.At(2.f); f.Count(0);
    Require(f.W.PendingInstagibEquipTapMode == 255, "fresh active action left stale equip intent");
}
void EquipReentrantCleanup() {
    for (int change = 0; change < 4; ++change) {
        Fixture f; AUTCharacter replacement; AUTPlayerController other;
        f.BeginEquip(); f.PhysicalDown(0); f.Up(0);
        const auto priorStops = f.W.Stops.size();
        bool changed = false;
        f.W.OnShot = [&]() {
            if (changed) return;
            changed = true;
            if (change == 0) {
                // The actual action observer changes the serial before deferred
                // controller processing. A new held action owns its own release.
                f.PhysicalDown(0);
            } else if (change == 1) {
                replacement.Controller = &f.Controller; replacement.Weapon = &f.W;
                replacement.Pending[0] = true;
                f.Controller.Pawn = &replacement; f.W.UTOwner = &replacement;
            } else if (change == 2) {
                other.Pawn = &f.Pawn; other.InputComponent = &f.Input;
                f.Pawn.Controller = &other;
            } else {
                f.W.BringUp(0.f); f.Pawn.Pending[0] = true;
            }
        };
        f.At(.4f); f.FinishEquip(); f.Count(1);
        Require(changed, "reentrant shot hook did not run");
        Require(f.W.Stops.size() == priorStops, "old dispatch sent a stop for a replacement input or lifecycle");
        Require(f.W.UTOwner->Pending[0], "old dispatch cleared a replacement hold");
        Require(!f.W.bHandlingRetry, "dispatch failed to restore retry guard");
    }
}
void EquipTokenStop() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture f; f.BeginEquip(); f.W.NoteInstagibEquipPress(mode);
        f.W.StopFireInternal(mode); f.Down(mode); f.Up(mode);
        Require(f.W.PendingInstagibEquipTapMode == 255,
                "internal stop did not invalidate an observed action awaiting dispatch");
        f.At(.4f); f.FinishEquip(); f.At(2.f); f.Count(0);
    }
}
void PossessionTap() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (bool enabled : {false, true})
    for (bool equipFirst : {false, true}) {
        Fixture f; f.BeginEquip(); f.BeforePossession();
        CVarInstagibEquipTap.Value = enabled ? 1 : 0;
        f.Pawn.Aim.Yaw = 10.f;
        f.QueuePress(mode); f.Controller.ApplyDeferredFireInputs();
        Require(!f.Pawn.Pending[mode], "stock did not discard the pre-Playing start");
        f.At(.12f); f.QueueRelease(mode); f.Controller.ApplyDeferredFireInputs();
        Require(f.W.Stops.size() == 1, "stock dropped the release with the start");
        f.Count(0);
        if (equipFirst) {
            f.At(.2f); f.FinishEquip(); f.Count(0);
            f.At(.3f); f.Acknowledge();
        } else {
            f.At(.2f); f.Acknowledge(); f.W.PumpInstagibEquipTap(); f.Count(0);
            f.At(.3f); f.FinishEquip();
        }
        // Use execution-time aim, not the earlier click. No backdated fire.
        if (equipFirst) { f.Pawn.Aim.Yaw = 80.f; f.W.PumpInstagibEquipTap(); }
        f.Count(enabled ? 1 : 0);
        if (enabled) {
            Near(f.W.Shots[0].first, 10.3f, "handoff fired before both gates opened");
            if (equipFirst) Near(f.W.ShotAims[0].Yaw, 80.f, "handoff retained old aim");
            Require(!f.Pawn.Pending[0] && !f.Pawn.Pending[1], "released handoff became a hold");
        }
        f.At(3.f); f.Count(enabled ? 1 : 0);
        CVarInstagibEquipTap.Value = 1;
    }
}
void PossessionGuards() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (int change = 0; change < 19; ++change) {
        Fixture f; UDemoNetDriver demo; f.BeginEquip(); f.BeforePossession();
        switch (change) {
        case 0: f.Controller.Spectating = true; break;
        case 1: f.W.NetMode = NM_Standalone; break;
        case 2: f.Controller.AcknowledgedPawn = &f.Pawn; break;
        case 3: f.Controller.Cursor = true; break;
        case 4: f.World.Paused = true; break;
        case 5: f.Viewport.TestViewport.Focused = false; break;
        case 6: f.Viewport.Ignore = true; break;
        case 7: f.Viewport.TestConsole.Active = true; break;
        case 8: f.Pawn.Feigning = true; break;
        case 9: f.Pawn.TauntCount = 1; break;
        case 10: f.Controller.IgnoreMove = true; break;
        case 11: f.Pawn.Dead = true; break;
        case 12: f.Pawn.Weapon = nullptr; break;
        case 13: f.W.InstagibIdentity = false; break;
        case 14: demo.Playing = true; f.World.DemoNetDriver = &demo; break;
        case 15: f.World.GameState.Prevent = true; break;
        case 16: f.LocalPlayer.MenusOpen = true; break;
        case 17: f.LocalPlayer.QuickChatOpen = true; break;
        case 18: f.Controller.Player = nullptr; break;
        }
        f.W.NoteInstagibEquipPress(mode);
        Require(f.W.PendingInstagibEquipTapMode == 255, "blocked action retained a possession tap");
        Require(!f.W.bInstagibTapAwaitingPossession, "blocked action armed recovery");
    }
    // Neither an old synthetic start nor a press after weapon raise can invent a tap.
    for (bool active : {false, true}) {
        Fixture f; f.BeginEquip(); f.BeforePossession();
        if (active) { f.FinishEquip(); f.W.NoteInstagibEquipPress(0); }
        else { f.Down(0); f.Up(0); }
        f.Acknowledge(); f.FinishEquip(); f.At(2.f); f.Count(0);
    }
}
void PossessionInvalidation() {
    for (int change = 0; change < 14; ++change) {
        Fixture f; UInputComponent otherInput; AUTCharacter replacement; AUTPlayerController other;
        f.BeginEquip(); f.BeforePossession(); f.QueuePress(0);
        f.Controller.ApplyDeferredFireInputs(); f.QueueRelease(0); f.Controller.ApplyDeferredFireInputs();
        switch (change) {
        case 0: f.Controller.Cursor = true; break;
        case 1: f.World.Paused = true; break;
        case 2: f.Viewport.TestViewport.Focused = false; break;
        case 3: f.Viewport.TestConsole.Active = true; break;
        case 4: f.Controller.InputComponent = &otherInput; break;
        case 5: f.Controller.Spectating = true; break;
        case 6: f.Pawn.Dead = true; break;
        case 7: f.Pawn.PendingWeapon = &f.W; break;
        case 8: f.BeginEquip(); break;
        case 9: f.W.StopFireInternal(0); break;
        case 10: f.W.GotoState(&f.Unequip); break;
        case 11: f.Pawn.Controller = &other; break;
        case 12: f.W.UTOwner = &replacement; break;
        case 13: f.W.StopInstagibEquipInput(); break;
        }
        f.W.PumpInstagibEquipTap();
        Require(f.W.PendingInstagibEquipTapMode == 255, "invalid lifecycle retained pre-Playing tap");
        f.Controller.Cursor = false; f.World.Paused = false; f.Viewport.TestViewport.Focused = true;
        f.Viewport.TestConsole.Active = false; f.Controller.InputComponent = &f.Input;
        f.Controller.Spectating = false; f.Pawn.Dead = false; f.Pawn.PendingWeapon = nullptr;
        f.Pawn.Controller = &f.Controller; f.W.UTOwner = &f.Pawn;
        f.W.RefreshInstagibEquipInput(); f.Acknowledge(); f.BeginEquip(); f.FinishEquip(); f.At(2.f); f.Count(0);
    }
}
void PossessionDeadline() {
    for (bool playing : {false, true}) {
        Fixture f; f.BeginEquip(); f.BeforePossession(); f.QueuePress(0);
        f.Controller.ApplyDeferredFireInputs(); f.QueueRelease(0); f.Controller.ApplyDeferredFireInputs();
        if (playing) f.Acknowledge();
        // A frozen/dilated game clock must not preserve input through a stall.
        f.World.InputWorldTime = 10.f; f.World.RealTimeOffset = .501f;
        f.W.PumpInstagibEquipTap();
        Require(f.W.PendingInstagibEquipTapMode == 255, "handoff ignored real-time expiry");
        f.Acknowledge(); f.FinishEquip(); f.At(2.f); f.Count(0);
    }
    Fixture f; f.BeginEquip(); f.BeforePossession(); f.QueuePress(0);
    f.Controller.ApplyDeferredFireInputs(); f.QueueRelease(0); f.Controller.ApplyDeferredFireInputs();
    f.At(.15f); f.Controller.Playing = true; f.Controller.StateName = NAME_Playing;
    f.FinishEquip(); f.Count(0); // A Playing state without acknowledgment is not ready.
    f.At(.2f); f.Controller.AcknowledgedPawn = &f.Pawn; f.W.PumpInstagibEquipTap(); f.Count(1);
}
void PossessionCoalescing() {
    for (uint8 last : {uint8(0), uint8(1)}) {
        Fixture f; f.BeginEquip(); f.BeforePossession();
        f.QueuePress(0); f.QueueRelease(0); f.QueuePress(last); f.QueueRelease(last);
        f.Controller.ApplyDeferredFireInputs();
        Require(f.W.PendingInstagibEquipTapMode == last, "handoff did not retain only latest action");
        f.At(.2f); f.Acknowledge(); f.FinishEquip(); f.Count(1);
        Require(f.W.Shots[0].second == last, "handoff fired wrong mode");
        f.At(3.f); f.Count(1);
    }
    Fixture f; f.BeginEquip(); f.BeforePossession(); f.QueuePress(0); f.QueueRelease(0);
    f.Controller.ApplyDeferredFireInputs(); f.At(.1f); f.Acknowledge();
    f.QueuePress(1); f.QueueRelease(1); f.Controller.ApplyDeferredFireInputs();
    f.At(.3f); f.FinishEquip(); f.Count(1);
    Require(f.W.Shots[0].second == 1, "fresh Playing action failed to supersede possession tap");
    f.At(3.f); f.Count(1);
}
void PossessionHeldRecovery() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (bool equipFirst : {false, true})
    for (bool releaseBeforeEquip : {false, true}) {
        Fixture f; f.BeginEquip(); f.BeforePossession(); f.QueuePress(mode);
        f.Controller.ApplyDeferredFireInputs(); f.At(.1f);
        if (equipFirst) f.FinishEquip();
        f.Acknowledge();
        f.W.PumpInstagibEquipTap(); f.Count(0); // Weapon tick can precede held recovery.
        // Model the legitimate verifier's synthetic held start, without an action observer.
        f.Controller.DeferredFireInputs.push_back({mode, true});
        f.W.PumpInstagibEquipTap(); f.Count(0); // Must not overtake queued normal input.
        f.Controller.ApplyDeferredFireInputs();
        if (releaseBeforeEquip) {
            f.QueueRelease(mode); f.Controller.ApplyDeferredFireInputs();
        }
        if (!equipFirst) { f.At(.3f); f.FinishEquip(); }
        f.Count(1);
        if (!releaseBeforeEquip) { f.At(2.31f); f.Count(3); f.Cadence(); }
        f.QueueRelease(mode); f.Controller.ApplyDeferredFireInputs();
        const size_t shots = f.W.Shots.size(); f.At(4.f); f.Count(shots);
        Require(f.W.PendingInstagibEquipTapMode == 255, "held recovery left duplicate tap intent");
    }
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture f; f.BeginEquip(); f.BeforePossession();
        // All real action observers run before the deferred queue is drained.
        // The old release must not release the latest same-mode held press.
        f.QueuePress(mode); f.QueueRelease(mode); f.QueuePress(mode);
        f.Controller.ApplyDeferredFireInputs();
        Require(!f.W.bInstagibPossessionTapReleased, "old deferred stop released a newer held action");
        f.At(.2f); f.Acknowledge(); f.FinishEquip(); f.W.PumpInstagibEquipTap(); f.Count(0);
        f.Controller.DeferredFireInputs.push_back({mode, true});
        f.Controller.ApplyDeferredFireInputs(); f.Count(1);
        f.At(2.21f); f.Count(3); f.Cadence();
        f.QueueRelease(mode); f.Controller.ApplyDeferredFireInputs(); f.At(4.f); f.Count(3);
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "possession_tap") PossessionTap();
    else if (name == "possession_guards") PossessionGuards();
    else if (name == "possession_invalidation") PossessionInvalidation();
    else if (name == "possession_deadline") PossessionDeadline();
    else if (name == "possession_coalescing") PossessionCoalescing();
    else if (name == "possession_held") PossessionHeldRecovery();
    else if (name == "equip_tap") EquipTap();
    else if (name == "equip_hold") EquipHold();
    else if (name == "equip_provenance") EquipProvenance();
    else if (name == "equip_guards") EquipGuards();
    else if (name == "equip_lifecycle") EquipLifecycle();
    else if (name == "equip_dispatch") EquipDispatch();
    else if (name == "equip_fresh_input") EquipFreshInput();
    else if (name == "equip_reentrant") EquipReentrantCleanup();
    else if (name == "equip_token_stop") EquipTokenStop();
    else if (name == "stale_release") StaleRelease();
    else if (name == "ready_debounce") ReadyDebounce();
    else if (name == "release_ownership") ReleaseOwnership();
    else if (name == "classifier") Classifier();
    else if (name == "guards") Guards();
    else if (name == "overlap") Overlap();
    else if (name == "handoff") Handoff();
    else if (name == "recorded") Recorded();
    else if (name == "debounce") Debounce();
    else if (name == "repress") Repress();
    else if (name == "early_taps") EarlyTaps();
    else if (name == "combo") Combo();
    else if (name == "local") Local();
    else if (name == "boundary") Boundary();
    else Require(false, "unknown case");
}
