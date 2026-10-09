"""Compile the actual Instagib action observer installation/removal methods.

The adapter models delegate ownership, handles and the mutable UE4.15 delegate
accessor. It checks the four physical action bindings without substituting the
production Refresh/Stop methods. It is not an Unreal input-dispatch playtest.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler, native_function
except ImportError:
    from test_wipeout_healing import find_compiler, native_function


PLUGIN = Path(os.environ.get("NCP_TEST_PLUGIN", Path(__file__).resolve().parents[2]))
ADAPTER = r'''
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
using int32 = int32_t;
using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#define TEXT(x) x
using FName = std::string;
enum EInputEvent { IE_Pressed, IE_Released, IE_Repeat };
constexpr int NM_Client = 3, NM_DedicatedServer = 1;
template<class T> struct TWeakObjectPtr {
    T* Ptr = nullptr;
    T* Get() const { return Ptr; }
    bool IsValid() const { return Ptr != nullptr; }
    void Reset() { Ptr = nullptr; }
    TWeakObjectPtr& operator=(T* value) { Ptr = value; return *this; }
};
template<class T, class U> T* Cast(U* value) { return static_cast<T*>(value); }
struct FDelegateHandle {
    int Value = 0;
    bool IsValid() const { return Value != 0; }
    void Reset() { Value = 0; }
    bool operator==(FDelegateHandle other) const { return Value == other.Value; }
};
struct Delegate {
    void* Owner = nullptr;
    FDelegateHandle Handle;
    std::function<void()> Callback;
    FDelegateHandle GetHandle() const { return Handle; }
};
struct FInputActionUnifiedDelegate {
    Delegate Simple, WithKey;
    bool IsBoundToObject(void* object) const {
        return Simple.Owner == object || WithKey.Owner == object;
    }
    Delegate& GetDelegateForManualSet() {
        // Matches the relevant UE accessor side effect: requesting the simple
        // delegate unbinds the key-taking delegate. Production must use a copy.
        WithKey = Delegate{};
        return Simple;
    }
    void Execute() const {
        if (Simple.Callback) Simple.Callback();
        else if (WithKey.Callback) WithKey.Callback();
    }
};
struct FInputActionBinding {
    FName ActionName;
    EInputEvent KeyEvent = IE_Pressed;
    bool bConsumeInput = true, bExecuteWhenPaused = true;
    FInputActionUnifiedDelegate ActionDelegate;
};
struct UInputComponent {
    std::vector<FInputActionBinding> Bindings;
    int NextHandle = 1;
    int32 GetNumActionBindings() const { return int32(Bindings.size()); }
    const FInputActionBinding& GetActionBinding(int32 index) const { return Bindings.at(index); }
    void RemoveActionBinding(int32 index) { Bindings.erase(Bindings.begin() + index); }
    template<class T> FInputActionBinding& BindAction(const char* name, EInputEvent event,
                                                    T* object, void (T::*method)()) {
        FInputActionBinding binding;
        binding.ActionName = name; binding.KeyEvent = event;
        binding.ActionDelegate.Simple.Owner = object;
        binding.ActionDelegate.Simple.Handle.Value = NextHandle++;
        binding.ActionDelegate.Simple.Callback = [object, method]() { (object->*method)(); };
        Bindings.push_back(binding);
        return Bindings.back();
    }
    void AddKeyBinding(const char* name, EInputEvent event, void* object, int* count) {
        FInputActionBinding binding;
        binding.ActionName = name; binding.KeyEvent = event;
        binding.ActionDelegate.WithKey.Owner = object;
        binding.ActionDelegate.WithKey.Handle.Value = NextHandle++;
        binding.ActionDelegate.WithKey.Callback = [count]() { ++*count; };
        Bindings.push_back(binding);
    }
    void Dispatch(const char* name, EInputEvent event) {
        for (const auto& binding : Bindings)
            if (binding.ActionName == name && binding.KeyEvent == event)
                binding.ActionDelegate.Execute();
    }
};
struct AUTCharacter;
struct AUTPlayerController {
    AUTCharacter* Pawn = nullptr;
    UInputComponent* InputComponent = nullptr;
    int Calls[4] = {0, 0, 0, 0};
    AUTCharacter* GetPawn() const { return Pawn; }
    void Start() { ++Calls[0]; }
    void AltStart() { ++Calls[1]; }
    void Stop() { ++Calls[2]; }
    void AltStop() { ++Calls[3]; }
};
struct AUTPlusShockRifle;
struct AUTCharacter {
    AUTPlayerController* Controller = nullptr;
    AUTPlusShockRifle* Weapon = nullptr;
    bool Dead = false, PendingKill = false, Local = true, Player = true;
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsDead() const { return Dead; }
    bool IsLocallyControlled() const { return Local; }
    bool IsPlayerControlled() const { return Player; }
    AUTPlusShockRifle* GetWeapon() const { return Weapon; }
};
struct UDemoNetDriver { bool IsPlaying() const { return true; } };
struct UWorld { UDemoNetDriver* DemoNetDriver = nullptr; };
struct AUTPlusShockRifle {
    AUTCharacter* UTOwner = nullptr;
    UWorld* TestWorld = nullptr;
    bool PendingKill = false, Shared = true;
    int NetMode = NM_Client, Clears = 0, OtherCalls = 0;
    int Presses[2] = {0, 0}, Releases[2] = {0, 0};
    TWeakObjectPtr<AUTPlayerController> InstagibEquipInputController;
    TWeakObjectPtr<UInputComponent> InstagibEquipInputComponent;
    TWeakObjectPtr<AUTCharacter> InstagibEquipPressOwner;
    // HANDLES
    bool bInstagibEquipPress[2] = {false, false};
    uint64 InstagibEquipPressFrame[2] = {0, 0};
    uint32 InstagibEquipInputSerial = 0;
    UWorld* GetWorld() const { return TestWorld; }
    int GetNetMode() const { return NetMode; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool HasSharedInstagibFireModes() const { return Shared; }
    void ClearInstagibEquipTap() { ++Clears; }
    void NoteInstagibEquipPress(uint8 mode) { ++Presses[mode]; }
    void NoteInstagibEquipRelease(uint8 mode) { ++Releases[mode]; }
    void OtherObserver() { ++OtherCalls; }
    void RefreshInstagibEquipInput();
    void StopInstagibEquipInput();
    void InstagibEquipPrimaryPressed();
    void InstagibEquipAlternatePressed();
    void InstagibEquipPrimaryReleased();
    void InstagibEquipAlternateReleased();
};
// NATIVE_METHODS
void Require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void AddStock(UInputComponent& input, AUTPlayerController& pc, int omit = -1) {
    if (omit != 0) input.BindAction("StartFire", IE_Pressed, &pc, &AUTPlayerController::Start);
    if (omit != 1) input.BindAction("StartAltFire", IE_Pressed, &pc, &AUTPlayerController::AltStart);
    if (omit != 2) input.BindAction("StopFire", IE_Released, &pc, &AUTPlayerController::Stop);
    if (omit != 3) input.BindAction("StopAltFire", IE_Released, &pc, &AUTPlayerController::AltStop);
}
struct Fixture {
    UWorld World;
    UInputComponent Input;
    AUTPlayerController PC;
    AUTCharacter Pawn;
    AUTPlusShockRifle Rifle;
    explicit Fixture(int omit = -1) {
        Rifle.TestWorld = &World; Rifle.UTOwner = &Pawn;
        Pawn.Weapon = &Rifle; Pawn.Controller = &PC;
        PC.Pawn = &Pawn; PC.InputComponent = &Input;
        AddStock(Input, PC, omit);
    }
};
void InstallAndDispatch() {
    Fixture f; f.Rifle.RefreshInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == 8, "did not install exactly four observers");
    for (int i = 4; i < 8; ++i) {
        const auto& binding = f.Input.GetActionBinding(i);
        Require(!binding.bConsumeInput && !binding.bExecuteWhenPaused,
                "observer consumes input or runs while paused");
    }
    f.Rifle.RefreshInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == 8, "refresh duplicated observers");
    f.Input.Dispatch("StartFire", IE_Pressed); f.Input.Dispatch("StartAltFire", IE_Pressed);
    f.Input.Dispatch("StopFire", IE_Released); f.Input.Dispatch("StopAltFire", IE_Released);
    for (int i = 0; i < 4; ++i) Require(f.PC.Calls[i] == 1, "stock callback changed");
    for (int i = 0; i < 2; ++i)
        Require(f.Rifle.Presses[i] == 1 && f.Rifle.Releases[i] == 1,
                "observer callback missing or routed to wrong mode");
}
void ExactCleanup() {
    Fixture f; int keyCalls = 0; AUTPlusShockRifle diagnostic;
    f.Input.BindAction("StartFire", IE_Pressed, &f.Rifle, &AUTPlusShockRifle::OtherObserver);
    f.Input.BindAction("StopFire", IE_Released, &diagnostic, &AUTPlusShockRifle::OtherObserver);
    f.Input.AddKeyBinding("StopAltFire", IE_Released, &f.Rifle, &keyCalls);
    const int before = f.Input.GetNumActionBindings();
    f.Rifle.RefreshInstagibEquipInput(); f.Rifle.StopInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == before, "cleanup removed non-owned delegate");
    f.Input.Dispatch("StartFire", IE_Pressed); f.Input.Dispatch("StopFire", IE_Released);
    f.Input.Dispatch("StopAltFire", IE_Released);
    Require(f.Rifle.OtherCalls == 1 && diagnostic.OtherCalls == 1 && keyCalls == 1,
            "cleanup mutated another observer or its key delegate");
    Require(f.Rifle.Presses[0] == 0 && f.Rifle.Releases[0] == 0 && f.Rifle.Releases[1] == 0,
            "an owned observer survived cleanup");
    f.Rifle.StopInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == before, "repeat cleanup removed another binding");
}
void ComponentSwap() {
    Fixture f; f.Rifle.RefreshInstagibEquipInput();
    f.Rifle.bInstagibEquipPress[0] = true; f.Rifle.InstagibEquipPressFrame[0] = 123;
    f.Rifle.InstagibEquipPressOwner = &f.Pawn;
    const int clears = f.Rifle.Clears;
    UInputComponent replacement; AddStock(replacement, f.PC);
    f.PC.InputComponent = &replacement; f.Rifle.RefreshInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == 4 && replacement.GetNumActionBindings() == 8,
            "component swap did not transfer observers");
    Require(!f.Rifle.bInstagibEquipPress[0] && f.Rifle.InstagibEquipPressFrame[0] == 0
            && !f.Rifle.InstagibEquipPressOwner.IsValid() && f.Rifle.Clears > clears,
            "component swap retained old input intent");
    f.Input.Dispatch("StartFire", IE_Pressed);
    Require(f.Rifle.Presses[0] == 0, "old input component still notifies weapon");
    replacement.Dispatch("StartFire", IE_Pressed);
    Require(f.Rifle.Presses[0] == 1, "new input component missing observer");
    f.Rifle.StopInstagibEquipInput();
}
void IncompleteHandlers() {
    for (int omit = 0; omit < 4; ++omit) {
        Fixture f(omit); f.Rifle.RefreshInstagibEquipInput();
        Require(f.Input.GetNumActionBindings() == 3, "partial stock handlers installed observers");
    }
    Fixture f; AUTPlayerController other;
    f.Input.Bindings.clear(); AddStock(f.Input, other);
    f.Rifle.RefreshInstagibEquipInput();
    Require(f.Input.GetNumActionBindings() == 4, "another controller's handlers were accepted");
}
void IneligibleOwner() {
    for (int change = 0; change < 8; ++change) {
        Fixture f; UDemoNetDriver demo; f.Rifle.RefreshInstagibEquipInput();
        switch (change) {
        case 0: f.Pawn.Dead = true; break;
        case 1: f.Pawn.Local = false; break;
        case 2: f.Pawn.Weapon = nullptr; break;
        case 3: f.PC.Pawn = nullptr; break;
        case 4: f.PC.InputComponent = nullptr; break;
        case 5: f.Rifle.NetMode = NM_DedicatedServer; break;
        case 6: f.World.DemoNetDriver = &demo; break;
        case 7: f.Rifle.Shared = false; break;
        }
        f.Rifle.RefreshInstagibEquipInput();
        Require(f.Input.GetNumActionBindings() == 4, "invalid owner retained observers");
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "one test case required"); const std::string name(argv[1]);
    if (name == "install") InstallAndDispatch();
    else if (name == "cleanup") ExactCleanup();
    else if (name == "swap") ComponentSwap();
    else if (name == "incomplete") IncompleteHandlers();
    else if (name == "owner") IneligibleOwner();
    else Require(false, "unknown test case");
}
'''


class InstagibInputBindingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-instagib-bindings-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = (PLUGIN / "Source/Private/NCInstagibEquipInput.cpp").read_text(encoding="utf-8-sig")
        header = (PLUGIN / "Source/Public/UTPlusShockRifle.h").read_text(encoding="utf-8-sig")
        handles = re.findall(r"FDelegateHandle InstagibEquip\w+Handle;", header)
        if len(handles) != 4:
            raise AssertionError("Expected distinct handles for both presses and both releases")
        methods = [native_function(source, signature) for signature in (
            "FDelegateHandle GetInstagibEquipActionHandle",
            "void AUTPlusShockRifle::RefreshInstagibEquipInput",
            "void AUTPlusShockRifle::StopInstagibEquipInput",
            "void AUTPlusShockRifle::InstagibEquipPrimaryPressed",
            "void AUTPlusShockRifle::InstagibEquipAlternatePressed",
            "void AUTPlusShockRifle::InstagibEquipPrimaryReleased",
            "void AUTPlusShockRifle::InstagibEquipAlternateReleased",
        )]
        path = directory / "bindings.cpp"
        path.write_text(ADAPTER.replace("// HANDLES", "\n".join(handles))
                        .replace("// NATIVE_METHODS", "\n".join(methods)), encoding="utf-8")
        cls.executable = directory / ("bindings.exe" if os.name == "nt" else "bindings")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(path),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'bindings.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(path), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Binding adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_four_observers_install_once_and_dispatch_both_press_release_modes(self):
        self.run_case("install")

    def test_cleanup_preserves_diagnostic_other_same_owner_and_key_delegates(self):
        self.run_case("cleanup")

    def test_component_replacement_removes_old_bindings_and_cancels_old_intent(self):
        self.run_case("swap")

    def test_missing_or_foreign_stock_actions_cannot_arm_observers(self):
        self.run_case("incomplete")

    def test_ineligible_owner_removes_all_observers(self):
        self.run_case("owner")


if __name__ == "__main__":
    unittest.main()
