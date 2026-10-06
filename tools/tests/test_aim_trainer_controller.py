"""Exercise the real trainer input/state functions with a small native adapter.

This verifies phase gates, focus handoff and request admission. It does not stand
in for a UE client/server playtest of cursor capture or replicated presentation.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
using uint8 = uint8_t;
using int32 = int32_t;
constexpr int32 INDEX_NONE = -1;
constexpr int ROLE_Authority = 3;
#define UE_SERVER 0
enum class FKey { Other, F6, One, Two, Three, NumPadOne, NumPadTwo, NumPadThree, Enter };
using EKeys = FKey;
enum EInputEvent { IE_Pressed, IE_Released, IE_Repeat };
struct FPlatformTime { static double Now; static double Seconds() { return Now; } };
double FPlatformTime::Now = 100.;
struct FNCAimTrainerProgress { uint8 Scenario = 0, Phase = 0; int Score = 0; };
struct APawn { virtual ~APawn() = default; };
struct Movement { bool Enabled = true; void DisableMovement() { Enabled = false; } };
struct AUTCharacter : APawn { Movement Move; Movement* GetCharacterMovement() { return &Move; } };
template<class T> T* Cast(APawn* pawn) { return dynamic_cast<T*>(pawn); }
struct BaseController {
    int BaseKeys = 0, Fires = 0, AltFires = 0, Stops = 0, AltStops = 0;
    int MoveInputLocks = 0, Restarts = 0;
    bool IgnoreLook = false;
    APawn* Pawn = nullptr;
    APawn* GetPawn() const { return Pawn; }
    void BeginPlay() {}
    void ClientRestart_Implementation(APawn* pawn) {
        Pawn = pawn; MoveInputLocks = 0; ++Restarts;
        if (auto character = Cast<AUTCharacter>(pawn)) character->Move.Enabled = true;
    }
    bool IsMoveInputIgnored() const { return MoveInputLocks > 0; }
    void SetIgnoreMoveInput(bool value) { MoveInputLocks += value ? 1 : -1; }
    bool InputKey(FKey, EInputEvent, float, bool) { ++BaseKeys; return false; }
    void OnFire() { ++Fires; }
    void OnAltFire() { ++AltFires; }
    void OnStopFire() { ++Stops; }
    void OnStopAltFire() { ++AltStops; }
};
struct ANCAimTrainerPlayerController : BaseController {
    using Super = BaseController;
    FNCAimTrainerProgress TrainerProgress;
    double NextTrainerRequestTime[3] = { 0., 0., 0. };
    uint8 LastPresentedPhase = 255;
    bool InputFocus = true, Local = true;
    int Role = ROLE_Authority, Selects = 0, Starts = 0, Aborts = 0, NetUpdates = 0, InputUpdates = 0;
    uint8 LastSelection = 255;
    bool HasTrainerInputFocus() const { return InputFocus; }
    bool IsLocalController() const { return Local; }
    void ServerTrainerSelectScenario(uint8 value) { ++Selects; LastSelection = value; }
    void ServerTrainerStart() { ++Starts; }
    void ServerTrainerAbort() { ++Aborts; }
    void ForceNetUpdate() { ++NetUpdates; }
    void UpdateInputMode() { ++InputUpdates; }
    void BeginPlay();
    void ClientRestart_Implementation(APawn*);
    bool IsTrainerMenuVisible() const;
    bool InputKey(FKey, EInputEvent, float, bool);
    void OnFire();
    void OnAltFire();
    void SelectTrainerScenario(uint8);
    void StartTrainerRun();
    void ReturnToTrainerMenu();
    bool AdmitTrainerRequest(uint8);
    void SetTrainerProgress(const FNCAimTrainerProgress&);
    void OnRep_TrainerProgress();
};
'''

CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void MenuControls() {
    ANCAimTrainerPlayerController pc;
    for (uint8 phase : {uint8(0), uint8(3)}) {
        pc.TrainerProgress.Phase = phase;
        Require(pc.InputKey(EKeys::Three, IE_Pressed, 1.f, false), "menu mode key escaped");
        Require(pc.LastSelection == 2, "scenario mapping wrong");
        const int selects = pc.Selects;
        pc.InputKey(EKeys::Three, IE_Repeat, 1.f, false);
        pc.InputKey(EKeys::Three, IE_Released, 0.f, false);
        Require(pc.Selects == selects, "repeat/release generated extra requests");
        Require(pc.InputKey(EKeys::Enter, IE_Pressed, 1.f, false), "start key escaped");
        const int starts = pc.Starts;
        pc.InputKey(EKeys::Enter, IE_Released, 0.f, false);
        Require(pc.Starts == starts, "key release started another run");
    }
    const int selects = pc.Selects;
    pc.SelectTrainerScenario(255);
    Require(pc.Selects == selects, "invalid scenario sent");
}
void Focus() {
    ANCAimTrainerPlayerController pc;
    pc.InputFocus = false;
    for (FKey key : {EKeys::One, EKeys::Enter, EKeys::F6})
        Require(!pc.InputKey(key, IE_Pressed, 1.f, false), "stock menu/chat key intercepted");
    Require(pc.BaseKeys == 3 && pc.Selects == 0 && pc.Starts == 0 && pc.Aborts == 0,
            "focus handoff changed trainer state");
}
void ActiveControls() {
    ANCAimTrainerPlayerController pc;
    for (uint8 phase : {uint8(1), uint8(2)}) {
        pc.TrainerProgress.Phase = phase;
        pc.InputKey(EKeys::One, IE_Pressed, 1.f, false);
        pc.InputKey(EKeys::Enter, IE_Pressed, 1.f, false);
        pc.SelectTrainerScenario(1);
        pc.StartTrainerRun();
        Require(pc.Selects == 0 && pc.Starts == 0, "active/countdown run replaced locally");
        Require(pc.InputKey(EKeys::F6, IE_Pressed, 1.f, false), "abort key not consumed");
    }
    Require(pc.Aborts == 2, "abort unavailable during countdown/run");
}
void FireGates() {
    ANCAimTrainerPlayerController pc;
    for (uint8 scenario = 0; scenario < 3; ++scenario) {
        pc.TrainerProgress.Scenario = scenario;
        for (uint8 phase = 0; phase < 4; ++phase) {
            pc.TrainerProgress.Phase = phase;
            int before = pc.Fires;
            pc.OnFire(); pc.OnAltFire();
            const int expected = phase == 2 && scenario != 0 ? 1 : 0;
            Require(pc.Fires == before + expected && pc.AltFires == pc.Fires,
                    "firing allowed outside shooting scenario active phase");
        }
    }
}
void Admission() {
    ANCAimTrainerPlayerController pc;
    Require(pc.AdmitTrainerRequest(0), "first selection rejected");
    Require(!pc.AdmitTrainerRequest(0), "selection flood not bounded");
    Require(pc.AdmitTrainerRequest(1), "quick select/start dropped");
    Require(pc.AdmitTrainerRequest(2), "abort blocked by start");
    Require(!pc.AdmitTrainerRequest(255), "out-of-bounds action accepted");
    FPlatformTime::Now += .16;
    Require(pc.AdmitTrainerRequest(0), "throttle never recovered");
}
void StatePublish() {
    ANCAimTrainerPlayerController pc;
    FNCAimTrainerProgress next;
    next.Phase = 2; next.Score = 123;
    pc.Role = 1;
    pc.SetTrainerProgress(next);
    Require(pc.TrainerProgress.Score == 0 && pc.NetUpdates == 0, "client set authoritative score");
    pc.Role = ROLE_Authority;
    pc.SetTrainerProgress(next);
    Require(pc.TrainerProgress.Score == 123 && pc.NetUpdates == 1 && pc.Stops == 0,
            "run start presentation incorrect");
    next.Phase = 3;
    pc.SetTrainerProgress(next);
    Require(pc.Stops == 1 && pc.AltStops == 1, "held fire not released at run end");
    for (int i = 0; i < 20; ++i) pc.SetTrainerProgress(next);
    Require(pc.Stops == 1 && pc.AltStops == 1 && pc.NetUpdates == 2,
            "repeated presentation samples spammed fire releases or forced replication");
}
void PossessionLock() {
    ANCAimTrainerPlayerController pc;
    AUTCharacter pawn;
    pc.BeginPlay();
    pc.BeginPlay();
    Require(pc.MoveInputLocks == 1 && !pc.IgnoreLook, "initial movement-only lock stacks or blocks view");
    for (int i = 0; i < 3; ++i) {
        pc.ClientRestart_Implementation(&pawn);
        Require(pc.MoveInputLocks == 1 && !pc.IgnoreLook && !pawn.Move.Enabled,
                "possession erased translation lock, restored gravity, or blocked mouse-look");
    }
    ANCAimTrainerPlayerController remote;
    remote.Local = false;
    remote.BeginPlay(); remote.ClientRestart_Implementation(&pawn);
    Require(remote.MoveInputLocks == 0, "nonlocal controller acquired a client input lock");
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required"); const std::string name(argv[1]);
    if (name == "menu") MenuControls();
    else if (name == "focus") Focus();
    else if (name == "active") ActiveControls();
    else if (name == "fire") FireGates();
    else if (name == "admission") Admission();
    else if (name == "state") StatePublish();
    else if (name == "possession") PossessionLock();
    else Require(false, "unknown case");
}
'''


class AimTrainerControllerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-controller-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerPlayerController.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "void ANCAimTrainerPlayerController::BeginPlay",
            "void ANCAimTrainerPlayerController::ClientRestart_Implementation",
            "bool ANCAimTrainerPlayerController::IsTrainerMenuVisible",
            "bool ANCAimTrainerPlayerController::InputKey",
            "void ANCAimTrainerPlayerController::OnFire",
            "void ANCAimTrainerPlayerController::OnAltFire",
            "void ANCAimTrainerPlayerController::SelectTrainerScenario",
            "void ANCAimTrainerPlayerController::StartTrainerRun",
            "void ANCAimTrainerPlayerController::ReturnToTrainerMenu",
            "bool ANCAimTrainerPlayerController::AdmitTrainerRequest",
            "void ANCAimTrainerPlayerController::SetTrainerProgress",
            "void ANCAimTrainerPlayerController::OnRep_TrainerProgress",
        )
        source = directory / "trainer_controller.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_controller.exe" if os.name == "nt" else "trainer_controller")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_controller.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Controller adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_menu_controls_and_repeat_suppression(self): self.run_case("menu")
    def test_stock_menu_and_chat_keep_input_focus(self): self.run_case("focus")
    def test_cannot_replace_active_run_and_can_abort(self): self.run_case("active")
    def test_only_shooting_scenarios_can_fire_during_run(self): self.run_case("fire")
    def test_request_throttle_allows_quick_select_then_start(self): self.run_case("admission")
    def test_authority_and_one_time_held_fire_release(self): self.run_case("state")
    def test_possession_keeps_movement_locked_and_mouse_look_live(self): self.run_case("possession")


if __name__ == "__main__":
    unittest.main()
