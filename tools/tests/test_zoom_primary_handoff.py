"""Compile production zoom handoff predicates and state-entry guards.

Uses real stock Zoom::BeginFiringSequence/GotoFireMode and NCP GotoState /
HasAcceptedTransactionalRequest. The RPC wrapper, UObject containers and shot
counter are adapters: this does not simulate networking, hit traces or retail UE.
The legacy variant removes only the zoom-tail exclusion in memory and must
reproduce the blocked entry followed by a busy reservation.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler, native_function
except ImportError:
    from test_wipeout_healing import find_compiler, native_function

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))
STOCK = PLUGIN.parents[1] / "Source/UnrealTournament/Private"


def statement(source, start):
    offset = source.index(start)
    return source[offset:source.index(";", offset) + 1]


ADAPTER = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <vector>
using uint8 = uint8_t;
using uint32 = uint32_t;
using int32 = int32_t;
#define TEXT(x) x
#define UE_LOG(...) ((void)0)
template<class T> struct TWeakObjectPtr {
    T* Value = nullptr;
    TWeakObjectPtr() = default;
    TWeakObjectPtr(T* v) : Value(v) {}
    T* Get() const { return Value; }
    bool operator<(const TWeakObjectPtr& other) const { return Value < other.Value; }
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    bool IsValidIndex(int i) const { return i >= 0 && i < Num(); }
    int Num() const { return static_cast<int>(this->size()); }
};
template<class K, class V> struct TMap {
    std::map<K, V> Values;
    V* Find(const K& key) { auto i = Values.find(key); return i == Values.end() ? nullptr : &i->second; }
    const V* Find(const K& key) const { auto i = Values.find(key); return i == Values.end() ? nullptr : &i->second; }
    void Add(const K& key, const V& value) { Values[key] = value; }
    void Remove(const K& key) { Values.erase(key); }
    bool Contains(const K& key) const { return Find(key) != nullptr; }
};
struct FMath { template<class T> static T Min(T a, T b) { return a < b ? a : b; } };
class AUTWeapon;
class AUTWeaponFix;
struct UUTWeaponState {
    AUTWeapon* Weapon = nullptr;
    virtual ~UUTWeaponState() = default;
};
struct UUTWeaponStateFiring : UUTWeaponState {};
struct UUTWeaponStateFiring_Transactional : UUTWeaponStateFiring {};
struct UUTWeaponStateZooming : UUTWeaponStateFiring {
    AUTWeapon* GetOuterAUTWeapon() { return Weapon; }
    bool BeginFiringSequence(uint8, bool);
};
struct UUTWeaponStateFiringChargedRocket_Transactional : UUTWeaponStateFiring {};
struct UUTWeaponStateFiringSpinUp : UUTWeaponStateFiring {};
struct UUTWeaponStateEquipping : UUTWeaponState { uint8 PendingFireSequence = 255; };
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct AUTCharacter {
    AUTWeapon* Weapon = nullptr;
    AUTWeapon* PendingWeapon = nullptr;
    bool Pending[2] = {false, true};
    bool Dead = false;
    bool IsDead() const { return Dead; }
    bool IsPendingKillPending() const { return false; }
    AUTWeapon* GetWeapon() const { return Weapon; }
    AUTWeapon* GetPendingWeapon() const { return PendingWeapon; }
    bool IsPendingFire(int mode) const { return Pending[mode]; }
};
class AUTWeapon {
public:
    virtual ~AUTWeapon() = default;
    AUTCharacter* UTOwner = nullptr;
    TArray<UUTWeaponStateFiring*> FiringState;
    UUTWeaponState* CurrentState = nullptr;
    UUTWeaponState* ActiveState = nullptr;
    UUTWeaponState* InactiveState = nullptr;
    UUTWeaponState* UnequippingState = nullptr;
    UUTWeaponStateEquipping* EquippingState = nullptr;
    uint8 CurrentFireMode = 1;
    bool Ammo = true;
    bool HasAmmo(uint8) const { return Ammo; }
    uint8 GetCurrentFireMode() const { return CurrentFireMode; }
    int GetNumFireModes() const { return FiringState.Num(); }
    bool IsPendingKillPending() const { return false; }
    void GotoFireMode(uint8);
    virtual void GotoState(UUTWeaponState*);
};
struct FDeferredEquipFireContext {
    uint32 Generation = 0;
    bool bShotDispatched = false;
    uint8 FireMode = 0;
    TWeakObjectPtr<AUTCharacter> Owner;
    TWeakObjectPtr<UUTWeaponState> ExpectedFiringState;
    TWeakObjectPtr<UUTWeaponState> WaitingState;
    bool bDeferredState = false;
    bool bDeferredEquip = false;
    bool bDeferredRate = false;
    bool bDispatchReady = false;
};
TMap<TWeakObjectPtr<AUTWeaponFix>, FDeferredEquipFireContext> DeferredEquipFireContexts;
TMap<TWeakObjectPtr<AUTWeaponFix>, uint32> DeferredEquipFireGenerations;
TMap<TWeakObjectPtr<AUTWeaponFix>, bool> StockSyncWeapons;
bool FireProvenance() { return false; }
namespace NCFireDiagnostics { struct FStateScope { FStateScope(AUTWeaponFix*, UUTWeaponState*) {} }; }
class AUTWeaponFix : public AUTWeapon {
public:
    using Super = AUTWeapon;
    int Shots = 0;
    int Busy = 0;
    uint32 NextGeneration = 0;
    bool RequireRequest = true;
    bool RequiresTransactionalRequest() const { return RequireRequest; }
    bool HasAcceptedTransactionalRequest(uint8) const;
    void GotoState(UUTWeaponState*) override;
    // Completion callbacks are deliberately outside this adapter. Tail tests
    // explicitly mark the captured request ready at the completion boundary.
    bool CompleteAcceptedDeferredFire(UUTWeaponState*) { return false; }
    void ClearDeferredActiveState() {}
    void ClearDeferredEquipFireContext(bool = false, const char* = "") {
        DeferredEquipFireContexts.Remove(this);
    }
    bool Submit(uint8);
    void DispatchShot() {
        auto* context = DeferredEquipFireContexts.Find(this);
        assert(context && HasAcceptedTransactionalRequest(CurrentFireMode));
        assert(CurrentState == FiringState[CurrentFireMode]);
        context->bShotDispatched = true;
        ++Shots;
    }
};
void AUTWeapon::GotoState(UUTWeaponState* state) {
    CurrentState = state;
    if (Cast<UUTWeaponStateFiring_Transactional>(state))
        static_cast<AUTWeaponFix*>(this)->DispatchShot();
}

// PRODUCTION_METHODS

bool AUTWeaponFix::Submit(uint8 FireModeNum) {
    // PRODUCTION_BUSY_PREDICATE
    {
        ++Busy;
        return false;
    }
    // PRODUCTION_CLASSIFICATION
    const bool bQueuedDeferredEquip = false;
    const bool bQueuedDeferredRate = false;
    FDeferredEquipFireContext NewContext;
    NewContext.Generation = ++NextGeneration;
    NewContext.FireMode = FireModeNum;
    NewContext.Owner = UTOwner;
    NewContext.ExpectedFiringState = RequestedFiringState;
    NewContext.WaitingState = CurrentState;
    NewContext.bDeferredState = bQueuedDeferredState;
    // PRODUCTION_READINESS
    DeferredEquipFireContexts.Add(this, NewContext);
    DeferredEquipFireGenerations.Add(this, NewContext.Generation);
    UTOwner->Pending[FireModeNum] = true;
    if (auto* zoom = Cast<UUTWeaponStateZooming>(CurrentState))
        zoom->BeginFiringSequence(FireModeNum, true);
    else if (Cast<UUTWeaponStateFiring_Transactional>(CurrentState))
        DispatchShot();
    // Charged and spin-up state completion is asynchronous and is not simulated.
    return true;
}
struct Fixture {
    AUTCharacter Pawn;
    AUTWeaponFix Weapon;
    UUTWeaponStateZooming Zoom;
    UUTWeaponStateFiring_Transactional Primary;
    UUTWeaponStateFiringChargedRocket_Transactional Charged;
    UUTWeaponStateFiringSpinUp SpinUp;
    UUTWeaponState Active, Inactive, Unequipping;
    UUTWeaponStateEquipping Equipping;
    Fixture() {
        DeferredEquipFireContexts.Values.clear();
        DeferredEquipFireGenerations.Values.clear();
        Pawn.Weapon = &Weapon;
        Weapon.UTOwner = &Pawn;
        Weapon.CurrentState = &Zoom;
        Weapon.ActiveState = &Active;
        Weapon.InactiveState = &Inactive;
        Weapon.UnequippingState = &Unequipping;
        Weapon.EquippingState = &Equipping;
        Weapon.FiringState = {&Primary, &Zoom};
        Zoom.Weapon = &Weapon;
    }
};

int main() {
    {
        Fixture f;
        assert(f.Weapon.Submit(0));
#ifdef EXPECT_LEGACY_BUG
        assert(f.Weapon.Shots == 0);
        assert(f.Weapon.CurrentState == &f.Zoom);
        assert(f.Weapon.CurrentFireMode == 0); // stock changes mode before blocked state entry
        assert(!f.Weapon.HasAcceptedTransactionalRequest(0));
        assert(!f.Weapon.Submit(0));
        assert(!f.Weapon.Submit(0));
        assert(f.Weapon.Busy == 2 && f.Weapon.Shots == 0);
        std::cout << "legacy: blocked zoom primary and busy follow-up requests\n";
        return 0;
#else
        assert(f.Weapon.CurrentState == &f.Primary && f.Weapon.Shots == 1);
        assert(!f.Weapon.HasAcceptedTransactionalRequest(0)); // consumed exactly once
        assert(f.Pawn.IsPendingFire(1)); // scope hold was not erased
        assert(f.Weapon.Submit(0));
        assert(f.Weapon.Submit(0));
        assert(f.Weapon.Busy == 0 && f.Weapon.Shots == 3);
#endif
    }
#ifndef EXPECT_LEGACY_BUG
    // No authorization means no primary shot, including through stock zoom.
    {
        Fixture f;
        f.Pawn.Pending[0] = true;
        f.Zoom.BeginFiringSequence(0, true);
        assert(f.Weapon.Shots == 0 && f.Weapon.CurrentState == &f.Zoom);
    }
    // Other non-transactional states must still retain their accepted request.
    for (int kind = 0; kind != 2; ++kind) {
        Fixture f;
        f.Weapon.CurrentState = kind == 0
            ? static_cast<UUTWeaponState*>(&f.Charged) : &f.SpinUp;
        assert(f.Weapon.Submit(0));
        assert(f.Weapon.Shots == 0);
        auto* context = DeferredEquipFireContexts.Find(&f.Weapon);
        assert(context && context->bDeferredState && !context->bDispatchReady);
        assert(!f.Weapon.Submit(0));
        context->bDispatchReady = true;
        f.Weapon.GotoFireMode(0);
        assert(f.Weapon.Shots == 1);
    }
    // Ready requests remain bounded to owner, generation and requested mode.
    for (int invalid = 0; invalid != 3; ++invalid) {
        Fixture f;
        f.Weapon.CurrentState = &f.Charged;
        f.Weapon.Submit(0);
        auto* context = DeferredEquipFireContexts.Find(&f.Weapon);
        context->bDispatchReady = true;
        if (invalid == 0) context->Generation++;
        if (invalid == 1) context->Owner = nullptr;
        if (invalid == 2) context->FireMode = 1;
        f.Weapon.CurrentState = &f.Zoom;
        f.Zoom.BeginFiringSequence(0, true);
        assert(f.Weapon.Shots == 0 && f.Weapon.CurrentState == &f.Zoom);
    }
    std::cout << "zoom primary, continued shots, state tails and authorization passed\n";
#endif
}
'''


class ZoomPrimaryHandoffTests(unittest.TestCase):
    def test_native_zoom_transition_and_legacy_failure(self):
        compiler, environment, msvc = find_compiler()
        source = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        zoom = (STOCK / "UTWeaponStateZooming.cpp").read_text(encoding="utf-8-sig")
        weapon = (STOCK / "UTWeapon.cpp").read_text(encoding="utf-8-sig")
        methods = "\n".join([
            native_function(source, "bool AUTWeaponFix::HasAcceptedTransactionalRequest"),
            native_function(source, "void AUTWeaponFix::GotoState"),
            native_function(zoom, "bool UUTWeaponStateZooming::BeginFiringSequence"),
            native_function(weapon, "void AUTWeapon::GotoFireMode"),
        ])
        rpc = native_function(source, "void AUTWeaponFix::ServerStartFireFixed_Implementation")
        classification = "\n".join(statement(rpc, name) for name in (
            "UUTWeaponState* const RequestedFiringState",
            "const bool bRequestedTransactional",
            "const bool bQueuedDeferredState",
        ))
        busy = rpc[rpc.index("if (Reserved && !Reserved->bShotDispatched && !bQueueBehindRate") :]
        busy = busy[:busy.index("{")]
        busy = ("const auto* Reserved = DeferredEquipFireContexts.Find(this);\n"
                "const bool bQueueBehindRate = false;\n" + busy)
        code = (ADAPTER.replace("// PRODUCTION_METHODS", methods)
                .replace("// PRODUCTION_BUSY_PREDICATE", busy)
                .replace("// PRODUCTION_CLASSIFICATION", classification)
                .replace("// PRODUCTION_READINESS", statement(rpc, "NewContext.bDispatchReady =")))
        exclusion = "&& Cast<UUTWeaponStateZooming>(CurrentState) == nullptr"
        self.assertIn(exclusion, classification, "Production zoom handoff fix is missing")
        with tempfile.TemporaryDirectory(prefix="ncp-zoom-primary-") as temporary:
            directory = Path(temporary)
            for variant in ("fixed", "legacy"):
                with self.subTest(variant=variant):
                    cpp = directory / f"{variant}.cpp"
                    cpp.write_text(code if variant == "fixed" else (
                        "#define EXPECT_LEGACY_BUG\n" + code.replace(exclusion, "")), encoding="utf-8")
                    executable = directory / (variant + (".exe" if os.name == "nt" else ""))
                    if msvc:
                        # Production GotoState scans a bounded int32 mode index
                        # before passing it to stock's uint8 APIs (C4244).
                        command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/wd4100", "/wd4244", "/std:c++14",
                                   str(cpp), f"/Fe{executable}", f"/Fo{directory / (variant + '.obj')}"]
                    else:
                        command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror",
                                   "-Wno-unused-parameter", str(cpp), "-o", str(executable)]
                    result = subprocess.run(command, cwd=directory, env=environment,
                                            capture_output=True, text=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(executable)], env=environment,
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("legacy:" if variant == "legacy" else "authorization passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
