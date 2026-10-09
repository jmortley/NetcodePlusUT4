// Actual production methods are inserted below. The base renderer records its
// documented beam interval/asset contract; particle pixels and transport require
// the normal Unreal playtest. Settings and Instagib identity are fixture inputs.
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
using uint8 = uint8_t;
using int32 = int32_t;
constexpr float KINDA_SMALL_NUMBER = 1.e-4f;
constexpr int NM_Standalone = 0, NM_DedicatedServer = 1, NM_Client = 3;
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    bool IsValidIndex(int32 index) const { return index >= 0 && index < static_cast<int32>(this->size()); }
    T& operator[](int32 index) { return this->at(index); }
    const T& operator[](int32 index) const { return this->at(index); }
};
struct FVector { float X = 0.f; explicit FVector(float x = 0.f) : X(x) {} };
struct FRotator {};
struct UParticleSystem {};
struct UClass {};
struct UUTWeaponStateFiring_Transactional {
    UClass* Class = StaticClass();
    static UClass* StaticClass() { static UClass value; return &value; }
    UClass* GetClass() const { return Class; }
};
struct FInstantHitDamageInfo {
    float Damage = 100.f, Momentum = 250000.f, TraceRange = 25000.f;
    float TraceHalfSize = 0.f, ConeDotAngle = 0.f;
    UClass* DamageType = nullptr;
};
struct AUTPlayerState {};
struct AController { virtual ~AController() = default; };
struct AUTPlayerController : AController {
    AUTPlayerState* PlayerState = nullptr;
    float SleepTime = .25f;
    float GetProjectileSleepTime() const { return SleepTime; }
};
template<class T> T* Cast(AController* value) { return dynamic_cast<T*>(value); }
struct UDemoNetDriver {
    bool FastForwarding = false;
    bool IsFastForwarding() const { return FastForwarding; }
};
struct UWorld { UDemoNetDriver* DemoNetDriver = nullptr; };
struct AUTCharacter {
    AController* Controller = nullptr;
    UWorld World;
    bool Local = true, bLocalFlashLoc = false;
    struct { FVector Position; int Count = 0; } FlashLocation;
    uint8 FireMode = 0;
    int Updates = 0;
    std::function<void()> Update;
    bool IsLocallyControlled() const { return Local; }
    UWorld* GetWorld() { return &World; }
    void FiringInfoUpdated() { ++Updates; if (Update) Update(); }
    void SetFlashLocation(const FVector&, uint8);
    void FiringInfoReplicated();
};
struct FTimerHandle { bool Active = false; };
struct FTimerManager {
    int SetCalls = 0;
    std::function<void()> Callback;
    bool IsTimerActive(const FTimerHandle& handle) const { return handle.Active; }
    template<class T> void SetTimer(FTimerHandle& handle, T* owner, void(T::*method)(), float, bool) {
        ++SetCalls; handle.Active = true;
        Callback = [owner, method]() { (owner->*method)(); };
    }
};
struct AUTWeapon {
    virtual ~AUTWeapon() = default;
    AUTCharacter* UTOwner = nullptr;
    uint8 CurrentFireMode = 0;
    FTimerManager Timers;
    FTimerHandle PlayDelayedImpactEffectsHandle;
    struct { FVector ImpactLocation, SpawnLocation; FRotator SpawnRotation; uint8 FireMode = 0; } DelayedHitScan;
    TArray<UParticleSystem*> FireEffect;
    int FireEffectCount = 0, FireEffectInterval = 1, NetMode = NM_Client;
    bool FirstPerson = true;
    int BaseImpacts = 0, EndpointEffects = 0, BeamEffects = 0;
    uint8 LastEffectMode = 255;
    UParticleSystem* SeenBeam = nullptr;
    UParticleSystem* SeenOtherBeam = nullptr;
    FTimerManager& GetWorldTimerManager() { return Timers; }
    int GetNetMode() const { return NetMode; }
    bool ShouldPlay1PVisuals() const { return FirstPerson; }
    void GetImpactSpawnPosition(const FVector&, FVector&, FRotator&) {}
    virtual void PlayPredictedImpactEffects(FVector);
    void PlayDelayedImpactEffects();
    void PlayImpactEffects_Implementation(const FVector&, uint8 mode, const FVector&, const FRotator&) {
        ++BaseImpacts;
        LastEffectMode = mode;
        SeenBeam = FireEffect.IsValidIndex(mode) ? FireEffect[mode] : nullptr;
        SeenOtherBeam = FireEffect.IsValidIndex(mode ^ 1) ? FireEffect[mode ^ 1] : nullptr;
        if (NetMode == NM_DedicatedServer) return;
        ++EndpointEffects;
        ++FireEffectCount;
        if (SeenBeam != nullptr && FireEffectCount >= FireEffectInterval) {
            ++BeamEffects;
            FireEffectCount = 0;
        }
    }
};
struct AUTWeaponFix : AUTWeapon {};
struct AUTPlusShockRifle : AUTWeaponFix {
    using Super = AUTWeaponFix;
    bool Identity = true, ShowOwnBeam = true;
    int NumModes = 2, Layers = 0;
    uint8 LayerMode = 255;
    UParticleSystem* LayerAsset = nullptr;
    TArray<UUTWeaponStateFiring_Transactional*> FiringState;
    TArray<float> FireInterval;
    TArray<int> AmmoCost;
    TArray<UClass*> ProjClass;
    TArray<FInstantHitDamageInfo> InstantHitInfo;
    int GetNumFireModes() const { return NumModes; }
    bool IsInstagibBeamWeapon() const { return Identity; }
    bool ShouldShowOwnInstagibBeam() const { return ShowOwnBeam; }
    bool HasSharedInstagibFireModes() const;
    bool IsInstagibBeamFireMode(uint8) const;
    bool NeedsLegacyInstagibBeamLayer(uint8) const;
    void PlayPredictedImpactEffects(FVector) override;
    void PlayImpactEffects_Implementation(const FVector&, uint8, const FVector&, const FRotator&);
    void SpawnLegacyInstagibBeamLayer(const FVector&, uint8 mode, const FVector&, const FRotator&) {
        ++Layers; LayerMode = mode;
        LayerAsset = FireEffect.IsValidIndex(mode) ? FireEffect[mode] : nullptr;
    }
};

// NATIVE_METHODS

void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    AUTPlayerState PS;
    AUTPlayerController PC;
    AUTCharacter Pawn;
    UClass DamageType, CustomState, Projectile;
    UUTWeaponStateFiring_Transactional Primary, Alternate;
    UParticleSystem PrimaryBeam, AlternateBeam;
    AUTPlusShockRifle Weapon;
    Fixture() {
        PC.PlayerState = &PS;
        Pawn.Controller = &PC;
        Weapon.UTOwner = &Pawn;
        Weapon.FiringState = {&Primary, &Alternate};
        Weapon.FireInterval = {1.f, 1.f};
        Weapon.AmmoCost = {0, 0};
        Weapon.ProjClass = {nullptr, nullptr};
        FInstantHitDamageInfo info;
        info.DamageType = &DamageType;
        Weapon.InstantHitInfo = {info, info};
        Weapon.FireEffect = {&PrimaryBeam, &AlternateBeam};
    }
    void Impact(uint8 mode) { Weapon.PlayImpactEffects_Implementation(FVector(500.f), mode, FVector(), FRotator()); }
};
void Immediate() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (float sleep : {0.f, .001f, .25f}) {
        Fixture f; f.PC.SleepTime = sleep; f.Weapon.CurrentFireMode = mode;
        f.Pawn.Update = [&f, mode]() { f.Impact(mode); };
        f.Weapon.PlayPredictedImpactEffects(FVector(777.f));
        Require(f.Pawn.FlashLocation.Count == 1 && f.Pawn.Updates == 1, "predicted beam was not immediate and singular");
        Require(f.Pawn.FlashLocation.Position.X == 777.f && f.Pawn.FireMode == mode, "prediction changed endpoint or mode");
        Require(f.Pawn.bLocalFlashLoc, "prediction did not claim local flash ownership");
        Require(!f.Weapon.PlayDelayedImpactEffectsHandle.Active && f.Weapon.Timers.SetCalls == 0, "Instagib queued a delayed beam");
        Require(f.Weapon.BaseImpacts == 1, "prediction did not reach impact path once");
        f.Pawn.FiringInfoReplicated();
        Require(f.Pawn.Updates == 1 && f.Weapon.BaseImpacts == 1, "replicated echo repeated the local beam");
    }
}
void Hidden() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture f; f.Weapon.ShowOwnBeam = false;
        auto* own = f.Weapon.FireEffect[mode]; auto* other = f.Weapon.FireEffect[mode ^ 1];
        f.Impact(mode);
        Require(f.Weapon.BaseImpacts == 1 && f.Weapon.EndpointEffects == 1, "hiding beam removed or duplicated endpoint processing");
        Require(f.Weapon.BeamEffects == 0 && f.Weapon.Layers == 0 && f.Weapon.SeenBeam == nullptr, "hidden beam emitted a layer");
        Require(f.Weapon.SeenOtherBeam == other, "hiding selected mode removed the other asset");
        Require(f.Weapon.FireEffect[mode] == own && f.Weapon.FireEffect[mode ^ 1] == other, "hidden beam assets were not restored");
        f.Weapon.ShowOwnBeam = true;
        f.Impact(mode);
        Require(f.Weapon.BeamEffects == 1 && f.Weapon.Layers == 1, "restored selected beam could not emit");
    }
}
void Layers() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture f; f.Weapon.CurrentFireMode = mode; f.Impact(mode);
        Require(f.Weapon.BeamEffects == 1 && f.Weapon.Layers == 1, "shown Instagib beam did not get exactly one extra layer");
        Require(f.Weapon.BaseImpacts == 1 && f.Weapon.EndpointEffects == 1, "extra layer repeated endpoint processing");
        Require(f.Weapon.LayerMode == mode && f.Weapon.LayerAsset == f.Weapon.FireEffect[mode], "extra layer used the wrong asset");
    }
}
void EffectMode() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture f; f.Weapon.CurrentFireMode = 2; f.Weapon.FireEffectInterval = 2;
        Require(f.Weapon.NeedsLegacyInstagibBeamLayer(mode), "layer eligibility consulted mutable current mode");
        f.Impact(mode);
        Require(f.Weapon.BeamEffects == 0 && f.Weapon.Layers == 0, "extra layer bypassed effect interval");
        f.Impact(mode);
        Require(f.Weapon.BeamEffects == 1 && f.Weapon.Layers == 1, "scheduled effect did not receive layer parity");
        Require(f.Weapon.LayerMode == mode, "layer did not use explicit effect mode");
        Require(!f.Weapon.NeedsLegacyInstagibBeamLayer(2), "invalid effect mode received Instagib layer");
        Fixture missing; missing.Weapon.FireEffect[mode] = nullptr; missing.Impact(mode);
        Require(missing.Weapon.BeamEffects == 0 && missing.Weapon.Layers == 0 && missing.Weapon.EndpointEffects == 1,
                "missing beam asset emitted layer or suppressed endpoint");
    }
}
void Excluded() {
    for (int change = 0; change < 10; ++change) {
        Fixture f; uint8 mode = 1;
        switch (change) {
        case 0: f.Weapon.Identity = false; mode = 0; break;
        case 1: f.Weapon.Identity = false; break;
        case 2: f.Weapon.ProjClass[1] = &f.Projectile; break;
        case 3: f.Alternate.Class = &f.CustomState; break; // zoom/charged/custom class
        case 4: f.Weapon.FiringState[1] = nullptr; break;
        case 5: f.Weapon.InstantHitInfo.resize(1); break;
        case 6: f.Weapon.FireInterval[1] = 2.f; break;
        case 7: f.Weapon.InstantHitInfo[1].Damage = 200.f; break;
        case 8: f.Weapon.NumModes = 1; f.Weapon.FiringState.resize(1); break;
        case 9: mode = 2; break;
        }
        f.Weapon.CurrentFireMode = mode; f.Weapon.ShowOwnBeam = false;
        Require(!f.Weapon.IsInstagibBeamFireMode(mode), "normal/custom/incomplete mode accepted as Instagib beam");
        f.Weapon.PlayPredictedImpactEffects(FVector(77.f));
        Require(f.Pawn.Updates == 0 && f.Weapon.Timers.SetCalls == 1, "excluded mode bypassed stock delayed prediction");
        Require(f.Weapon.DelayedHitScan.FireMode == mode, "excluded mode was remapped");
        f.Impact(mode);
        Require(f.Weapon.BaseImpacts == 1 && f.Weapon.EndpointEffects == 1 && f.Weapon.Layers == 0, "excluded mode did not retain base effects");
        if (mode < 2) Require(f.Weapon.BeamEffects == 1 && f.Weapon.SeenBeam != nullptr, "Instagib preference hid excluded mode");
    }
}
void PredictionGuards() {
    for (uint8 mode : {uint8(0), uint8(1)}) {
        Fixture remote; remote.Pawn.Local = false; remote.Weapon.CurrentFireMode = mode;
        remote.Weapon.PlayPredictedImpactEffects(FVector(33.f));
        Require(remote.Pawn.Updates == 0 && remote.Weapon.Timers.SetCalls == 1, "remote owner received new local prediction path");
        remote.Weapon.Timers.Callback();
        Require(remote.Pawn.Updates == 1 && !remote.Pawn.bLocalFlashLoc && remote.Pawn.FireMode == mode, "remote stock delay changed");
        Fixture absent; absent.Weapon.UTOwner = nullptr; absent.Weapon.CurrentFireMode = mode;
        absent.Weapon.PlayPredictedImpactEffects(FVector(33.f));
        Require(absent.Weapon.Timers.SetCalls == 0 && absent.Pawn.Updates == 0, "null owner produced prediction");
        Fixture standalone; standalone.Weapon.NetMode = NM_Standalone; standalone.Weapon.CurrentFireMode = mode;
        standalone.Weapon.PlayPredictedImpactEffects(FVector(33.f)); standalone.Impact(mode);
        Require(standalone.Pawn.Updates == 1 && standalone.Weapon.Timers.SetCalls == 0, "standalone beam was delayed");
        Require(standalone.Weapon.BeamEffects == 1 && standalone.Weapon.Layers == 0, "standalone beam received network overlap layer");
    }
}
void LayerGuards() {
    for (uint8 mode : {uint8(0), uint8(1)}) for (int change = 0; change < 8; ++change) {
        Fixture f;
        switch (change) {
        case 0: f.Weapon.UTOwner = nullptr; break;
        case 1: f.Pawn.Local = false; break;
        case 2: f.Weapon.FirstPerson = false; break;
        case 3: f.Pawn.Controller = nullptr; break;
        case 4: f.PC.PlayerState = nullptr; break;
        case 5: f.Weapon.NetMode = NM_Standalone; break;
        case 6: f.PC.SleepTime = 0.f; break;
        case 7: f.PC.SleepTime = KINDA_SMALL_NUMBER; break;
        }
        Require(!f.Weapon.NeedsLegacyInstagibBeamLayer(mode), "ineligible view received overlap layer");
        f.Impact(mode);
        Require(f.Weapon.Layers == 0 && f.Weapon.BaseImpacts == 1, "layer guard did not preserve base call");
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "expected case");
    const std::string name = argv[1];
    if (name == "immediate") Immediate();
    else if (name == "hidden") Hidden();
    else if (name == "layers") Layers();
    else if (name == "effect_mode") EffectMode();
    else if (name == "excluded") Excluded();
    else if (name == "prediction_guards") PredictionGuards();
    else if (name == "layer_guards") LayerGuards();
    else Require(false, "unknown case");
    return 0;
}
