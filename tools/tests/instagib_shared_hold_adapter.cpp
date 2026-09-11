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
#define TEXT(x) x
#define UE_LOG(...) ((void)0)
constexpr float SMALL_NUMBER = 1.e-8f;
constexpr int ROLE_Authority = 3, NM_DedicatedServer = 1, MOVE_Falling = 3;
const char* NAME_None = "None";
struct FString : std::string {
    using std::string::string;
    bool Contains(const char* text) const { return find(text) != npos; }
};
struct UClass { FString Name; FString GetName() const { return Name; } };
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    bool IsValidIndex(int32 i) const { return i >= 0 && i < Num(); }
    int32 Num() const { return static_cast<int32>(this->size()); }
    T& operator[](int32 i) { return this->at(i); }
    const T& operator[](int32 i) const { return this->at(i); }
};
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a, b); }
    template<class T> static T Max(T a, T b) { return std::max(a, b); }
    template<class T> static T Clamp(T a, T lo, T hi) { return Max(lo, Min(a, hi)); }
};
struct FRotator { void Normalize() {} bool ContainsNaN() const { return false; } };
struct FTimerHandle { int Id = 0; };
struct FTimerDelegate {
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
    float GetTimeSeconds() const { return Timers.Now; }
    template<class T> T* GetGameState() { return &GameState; }
};
struct AUTWeapon;
struct AUTPlayerState { virtual ~AUTPlayerState() = default; void NotIdle() {} };
struct AUTCharacter {
    bool Local = true, Player = true, Dead = false, PendingKill = false, Disabled = false;
    bool Pending[2] = {false, false};
    float FireRateMultiplier = 1.f;
    AUTWeapon* Weapon = nullptr;
    AUTWeapon* PendingWeapon = nullptr;
    AUTWeapon* PendingAutoSwitchWeapon = nullptr;
    AUTPlayerState* PlayerState = nullptr;
    struct Movement { int MovementMode = 0; } Move;
    bool IsLocallyControlled() const { return Local; }
    bool IsPlayerControlled() const { return Player; }
    bool IsDead() const { return Dead; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsFiringDisabled() const { return Disabled; }
    AUTWeapon* GetWeapon() const { return Weapon; }
    AUTWeapon* GetPendingWeapon() const { return PendingWeapon; }
    bool IsPendingFire(uint8 mode) const { return mode < 2 && Pending[mode]; }
    void SetPendingFire(uint8 mode, bool value) { if (mode < 2) Pending[mode] = value; }
    Movement* GetCharacterMovement() { return &Move; }
    FRotator GetViewRotation() { return {}; }
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
    bool bRootWhileFiring = false, bNetDelayedShot = false;
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
    uint8 GetNumFireModes() const {
        return static_cast<uint8>(std::min(255, std::min(FiringState.Num(), FireInterval.Num())));
    }
    UClass* GetClass() const { static UClass c{"Shock"}; return &c; }
    bool HasAmmo(uint8 mode) const { return AmmoCost.IsValidIndex(mode) && Ammo >= FMath::Min(1, AmmoCost[mode]); }
    bool HasAnyAmmo() const { return true; }
    bool IsPendingKillPending() const { return false; }
    bool PutDown() { GotoState(UnequippingState); return true; }
    void GotoState(UUTWeaponState* state) {
        UUTWeaponState* previous = CurrentState;
        if (previous) previous->EndState();
        CurrentState = state;
        state->BeginState(previous);
    }
    void GotoActiveState() { GotoState(ActiveState); }
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
    std::vector<std::pair<uint8, int32>> Stops;
    bool TryPreserveInstagibHeldFire(uint8);
    void StartFire(uint8) override;
    void StopFire(uint8) override;
    void StopFireInternal(uint8);
    void OnRetryTimer(uint8);
    void DeferredGotoActiveState(uint8);
    bool IsFireModeOnCooldown(uint8, float);
    void OnBufferedClickRetryTimer(uint8, FRotator, float) { std::abort(); }
    void ServerStopFireFixed(uint8 mode, int32 event) { Stops.emplace_back(mode, event); }
    void QueueResendStopFireFixed(uint8, int32) {}
    void FireShot() override {
        Shots.emplace_back(GetWorld()->GetTimeSeconds(), CurrentFireMode);
        LastFireTime[CurrentFireMode] = GetWorld()->GetTimeSeconds();
        ++ClientFireEventIndex[CurrentFireMode];
    }
};
struct AUTPlusShockRifle : AUTWeaponFix {
    bool InstagibIdentity = true;
    bool IsInstagibBeamWeapon() const { return InstagibIdentity; }
    bool HasSharedInstagibFireModes() const;
};
void UUTWeaponStateFiring::FireShot() { Weapon->FireShot(); }
void UUTWeaponStateFiring::EndState() { Weapon->GetWorldTimerManager().ClearTimer(RefireCheckHandle); }
template<class T, class U> T* Cast(U* p) { return dynamic_cast<T*>(p); }
template<class T> struct CVar { T Value; T GetValueOnGameThread() const { return Value; } };
CVar<int32> CVarInstagibSharedHold{1};
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
    UWorld World;
    UUTWeaponStateActive Active;
    UUTWeaponState Equip, Unequip, Inactive;
    UUTWeaponStateFiring_Transactional Mode[2];
    UClass DamageType{"Instagib"}, CoreType{"ShockBall"};
    Fixture() {
        Pawn.Weapon = &W; W.UTOwner = &Pawn; W.TestWorld = &World;
        W.ActiveState = &Active; W.EquippingState = &Equip;
        W.UnequippingState = &Unequip; W.InactiveState = &Inactive;
        W.CurrentState = &Active; W.FiringState = {&Mode[0], &Mode[1]};
        for (UUTWeaponState* s : std::initializer_list<UUTWeaponState*>{&Active, &Equip, &Unequip,
                                &Inactive, &Mode[0], &Mode[1]}) s->Weapon = &W;
        W.InstantHitInfo[0].DamageType = &DamageType; W.InstantHitInfo[1].DamageType = &DamageType;
    }
    void At(float offset) { World.Timers.Advance(10.f + offset); }
    void Down(uint8 mode) { W.StartFire(mode); }
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
        Require(f.World.Timers.IsTimerActive(f.W.RetryFireHandle[1]), "test did not exercise debounce retry");
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
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "classifier") Classifier();
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
