"""Exercise real client leaderboard cache/callback functions with fake public HTTP."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(x) x
using uint8 = uint8_t;
using uint32 = uint32_t;
using int32 = int32_t;
using FString = std::string;
constexpr int NM_Standalone = 0, NM_Client = 1, NM_DedicatedServer = 2;
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int32 Num() const { return int32(this->size()); }
    void SetNum(int32 n) { this->resize(n); }
};
struct FMath { template<class T> static T Clamp(T v, T a, T b) { return std::max(a, std::min(v, b)); } };
struct FPlatformTime { static double Now; static double Seconds() { return Now; } };
double FPlatformTime::Now = 100.;
struct UWorld { bool Valid = true; };
template<class T> struct TWeakObjectPtr {
    T* Ptr;
    TWeakObjectPtr(T* p) : Ptr(p) {}
    T* Get() const { return Ptr && Ptr->Valid ? Ptr : nullptr; }
    bool IsValid() const { return Get() != nullptr; }
};
struct FNCAimTrainerLeaderboardRow { int32 Score = 0; };
struct FNCAimTrainerProgress { uint8 Scenario = 0, Phase = 0; bool bMovementPractice = false; };
using Completion = std::function<void(bool, const TArray<FNCAimTrainerLeaderboardRow>&)>;
struct Request { UWorld* World; int Scenario; bool Local, Movement; Completion Callback; };
struct FNCAimTrainerOnline {
    enum { PresetRevision = 14, AirbornePresetRevision = 16, PopupPresetRevision = 19, HeadshotPresetRevision = 18 };
    static int32 PresetRevisionForScenario(int32 scenario);
    static const char* ScenarioSlug(int32 scenario);
    static std::vector<Request> Requests;
    static void Fetch(UWorld* world, int32 scenario, Completion callback, bool local, bool movement) {
        Requests.push_back({world, scenario, local, movement, callback});
    }
};
std::vector<Request> FNCAimTrainerOnline::Requests;
struct ANCAimTrainerPlayerController {
    UWorld World, OtherWorld;
    UWorld* CurrentWorld = &World;
    bool Valid = true, Local = true;
    int NetMode = NM_Standalone;
    FNCAimTrainerProgress TrainerProgress;
    FString OnlineStatus = "Score submission status";
    TArray<FNCAimTrainerLeaderboardRow> LeaderboardCache[NCAimTrainerScenarioId::LeaderboardCount];
    double NextLeaderboardFetch[NCAimTrainerScenarioId::LeaderboardCount] = {};
    uint32 LeaderboardGeneration[NCAimTrainerScenarioId::LeaderboardCount] = {};
    bool LeaderboardInFlight[NCAimTrainerScenarioId::LeaderboardCount] = {};
    bool LeaderboardLoaded[NCAimTrainerScenarioId::LeaderboardCount] = {};
    bool LeaderboardFailed[NCAimTrainerScenarioId::LeaderboardCount] = {};
    bool bLeaderboardSourceSelected = false, bLeaderboardLocal = false, bLeaderboardEnded = false;
    bool IsLocalController() const { return Local; }
    bool IsTrainerMenuVisible() const { return TrainerProgress.Phase == 0 || TrainerProgress.Phase == 3; }
    UWorld* GetWorld() const { return CurrentWorld; }
    int GetNetMode() const { return NetMode; }
    bool IsTrainerLeaderboardLocal() const;
    int32 SelectedLeaderboardKey() const;
    const TArray<FNCAimTrainerLeaderboardRow>& GetTrainerLeaderboard() const;
    FString GetTrainerLeaderboardStatus() const;
    void SelectTrainerLeaderboardSource(bool);
    void RefreshTrainerLeaderboard();
    void ClientTrainerLeaderboard_Implementation(const TArray<FNCAimTrainerLeaderboardRow>&);
    void ClientTrainerLeaderboardSubmitted_Implementation(uint8, bool, bool);
};
'''

CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int Requests() { return int(FNCAimTrainerOnline::Requests.size()); }
void Reply(int index, bool success, int score = 100) {
    TArray<FNCAimTrainerLeaderboardRow> rows;
    if (score >= 0) { FNCAimTrainerLeaderboardRow row; row.Score = score; rows.push_back(row); }
    auto callback = FNCAimTrainerOnline::Requests.at(index).Callback;
    callback(success, rows);
}
void Sources() {
    ANCAimTrainerPlayerController pc;
    Require(pc.IsTrainerLeaderboardLocal(), "standalone default is not local");
    pc.RefreshTrainerLeaderboard();
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 1 && FNCAimTrainerOnline::Requests[0].Local, "in-flight local read duplicated");
    pc.SelectTrainerLeaderboardSource(false);
    Require(Requests() == 2 && !FNCAimTrainerOnline::Requests[1].Local, "source selection failed");
    Reply(0, true, 300);
    Require(pc.GetTrainerLeaderboard().Num() == 0, "late local response flashed on approved board");
    Reply(1, true, 400);
    Require(pc.GetTrainerLeaderboard()[0].Score == 400, "approved response lost");
    pc.SelectTrainerLeaderboardSource(true);
    Require(Requests() == 2 && pc.GetTrainerLeaderboard()[0].Score == 300, "source cache was mixed");
    pc.TrainerProgress.Scenario = 1;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 3 && FNCAimTrainerOnline::Requests[2].Scenario == 1,
            "scenario does not have its own cache");
    Require(pc.GetTrainerLeaderboard().Num() == 0, "previous scenario rows flashed");
    Reply(2, true, 500);
    pc.TrainerProgress.Scenario = 0;
    Require(pc.GetTrainerLeaderboard()[0].Score == 300, "scenario cache overwritten");
    ANCAimTrainerPlayerController network;
    network.NetMode = NM_Client;
    Require(!network.IsTrainerLeaderboardLocal(), "network default is not approved");
}
void Timing() {
    ANCAimTrainerPlayerController pc;
    pc.RefreshTrainerLeaderboard();
    FPlatformTime::Now = 107.;
    Reply(0, true);
    FPlatformTime::Now = 166.99;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 1, "success cache shorter than 60 seconds from response");
    FPlatformTime::Now = 167.;
    pc.RefreshTrainerLeaderboard();
    Reply(1, false);
    Require(pc.GetTrainerLeaderboard().Num() == 1, "failed refresh erased cached rows");
    Require(pc.GetTrainerLeaderboardStatus().find("previously loaded") != FString::npos,
            "stale cache not labeled");
    FPlatformTime::Now = 176.99;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 2, "failure cache shorter than 10 seconds");
    FPlatformTime::Now = 177.;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 3, "failure retry stayed pinned");
    Require(pc.OnlineStatus == "Score submission status", "board replaced submission status");
}
void Status() {
    ANCAimTrainerPlayerController pc;
    pc.RefreshTrainerLeaderboard();
    Require(pc.GetTrainerLeaderboardStatus().find("Loading") != FString::npos, "no loading state");
    Reply(0, false);
    Require(pc.GetTrainerLeaderboardStatus().find("unavailable") != FString::npos, "failure looks empty");
    FPlatformTime::Now += 10.;
    pc.RefreshTrainerLeaderboard();
    Reply(1, true, -1);
    Require(pc.GetTrainerLeaderboardStatus().find("No scores") != FString::npos, "empty board looks failed");
    TArray<FNCAimTrainerLeaderboardRow> stale(2);
    pc.ClientTrainerLeaderboard_Implementation(stale);
    Require(pc.GetTrainerLeaderboard().Num() == 0, "legacy rows overwrite scoped browser");
}
void Invalidation() {
    ANCAimTrainerPlayerController pc;
    pc.RefreshTrainerLeaderboard();
    pc.SelectTrainerLeaderboardSource(false);
    Reply(1, true, 900);
    pc.ClientTrainerLeaderboardSubmitted_Implementation(0, true, false);
    Reply(0, true, 100);
    Require(pc.GetTrainerLeaderboard()[0].Score == 900, "other source invalidated selected rows");
    pc.SelectTrainerLeaderboardSource(true);
    Require(Requests() == 3 && pc.GetTrainerLeaderboard().Num() == 0,
            "pre-submission response survived invalidation");
    Reply(2, true, 500);
    Require(pc.GetTrainerLeaderboard()[0].Score == 500, "fresh submitted score did not load");
    pc.ClientTrainerLeaderboardSubmitted_Implementation(2, true, false);
    Require(Requests() == 3, "inactive scenario was fetched eagerly");
    pc.TrainerProgress.Scenario = 2;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 4, "inactive scenario invalidation lost");
    pc.ClientTrainerLeaderboardSubmitted_Implementation(255, true, false);
    Require(Requests() == 4, "invalid notification requested a board");
}
void MovementBoards() {
    ANCAimTrainerPlayerController pc;
    pc.TrainerProgress.Scenario = 3;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 1 && FNCAimTrainerOnline::Requests[0].Scenario == 3
            && !FNCAimTrainerOnline::Requests[0].Movement, "fourth fixed board request incorrect");
    pc.TrainerProgress.bMovementPractice = true;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 2 && FNCAimTrainerOnline::Requests[1].Movement, "movement board request missing");
    Reply(0, true, 100);
    Require(pc.GetTrainerLeaderboard().Num() == 0, "fixed board flashed into movement board");
    Reply(1, true, 900);
    pc.ClientTrainerLeaderboardSubmitted_Implementation(3, true, false);
    Require(Requests() == 2 && pc.GetTrainerLeaderboard()[0].Score == 900,
            "fixed score invalidated movement board");
    pc.TrainerProgress.bMovementPractice = false;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 3 && !FNCAimTrainerOnline::Requests[2].Movement, "fixed invalidation was lost");
    Reply(2, true, 200);
    pc.TrainerProgress.bMovementPractice = true;
    pc.SelectTrainerLeaderboardSource(false);
    Require(Requests() == 4 && !FNCAimTrainerOnline::Requests[3].Local
            && FNCAimTrainerOnline::Requests[3].Movement, "approved movement source not independent");
    Require(pc.GetTrainerLeaderboard().Num() == 0, "local movement rows leaked into approved source");
}
void AllScenarioBoards() {
    ANCAimTrainerPlayerController pc;
    const char* slugs[] = {"strafe", "headshots", "instagib", "precision_popup", "sactf_headshots", "sactf_popup",
                          "strafe_hard", "airborne_ig", "airborne_sniper", "airborne_sactf", "airborne_rockets"};
    for (int movement = 0; movement < 2; ++movement) {
        pc.TrainerProgress.bMovementPractice = movement != 0;
        for (int local = 0; local < 2; ++local) {
            for (uint8 scenario = 0; scenario < NCAimTrainerScenarioId::ScenarioCount; ++scenario) {
                pc.TrainerProgress.Scenario = scenario;
                pc.SelectTrainerLeaderboardSource(local != 0);
                const int key = scenario + 11 * local + 22 * movement;
                Require(Requests() == key + 1, "scenario/source/movement cache keys collide");
                const auto& req = FNCAimTrainerOnline::Requests[key];
                Require(req.Scenario == scenario && req.Local == (local != 0)
                        && req.Movement == (movement != 0), "cache key decoded wrong request");
                Require(FNCAimTrainerOnline::PresetRevisionForScenario(req.Scenario) == (scenario == 1 || scenario == 4 ? 18 : scenario == 2 || scenario == 3 || scenario == 5 ? 19 : scenario >= 7 ? 16 : 14),
                        "airborne score reset selected the wrong scenario/source/movement board");
                Require(std::string(FNCAimTrainerOnline::ScenarioSlug(req.Scenario)) == slugs[scenario],
                        "scenario ID selected the wrong backend slug");
            }
        }
    }
    // Arrive in reverse order, with the current view on the airborne rocket board.
    for (int key = 43; key >= 0; --key) Reply(key, true, 1000 + key);
    Require(pc.GetTrainerLeaderboard()[0].Score == 1043, "late callbacks replaced selected rocket board");
    for (int movement = 0; movement < 2; ++movement) {
        pc.TrainerProgress.bMovementPractice = movement != 0;
        for (int local = 0; local < 2; ++local) {
            for (uint8 scenario = 0; scenario < NCAimTrainerScenarioId::ScenarioCount; ++scenario) {
                pc.TrainerProgress.Scenario = scenario;
                pc.SelectTrainerLeaderboardSource(local != 0);
                Require(pc.GetTrainerLeaderboard()[0].Score == 1000 + scenario + 11 * local + 22 * movement,
                        "one of 44 boards lost its own score");
            }
        }
    }
    Require(Requests() == 44, "browsing cached scenarios generated extra requests");
    pc.ClientTrainerLeaderboardSubmitted_Implementation(4, true, true);
    Require(Requests() == 44, "inactive SACTF headshot invalidation fetched eagerly");
    pc.TrainerProgress.Scenario = 4;
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 45 && FNCAimTrainerOnline::Requests[44].Scenario == 4,
            "SACTF headshot submission did not invalidate exact board");
}
void Lifecycle() {
    ANCAimTrainerPlayerController pc;
    pc.RefreshTrainerLeaderboard();
    pc.bLeaderboardEnded = true;
    Reply(0, true);
    Require(pc.GetTrainerLeaderboard().Num() == 0, "ended controller accepted callback");
    pc.RefreshTrainerLeaderboard();
    Require(Requests() == 1, "ended controller fetched");
    ANCAimTrainerPlayerController moved;
    moved.RefreshTrainerLeaderboard();
    moved.CurrentWorld = &moved.OtherWorld;
    Reply(1, true);
    Require(moved.GetTrainerLeaderboard().Num() == 0, "wrong-world callback accepted");
    ANCAimTrainerPlayerController server;
    server.Local = false; server.NetMode = NM_DedicatedServer;
    server.RefreshTrainerLeaderboard();
    server.SelectTrainerLeaderboardSource(true);
    Require(Requests() == 2 && !server.IsTrainerLeaderboardLocal(), "server PC fetched public board");
    ANCAimTrainerPlayerController running;
    running.TrainerProgress.Phase = 2;
    running.RefreshTrainerLeaderboard();
    running.SelectTrainerLeaderboardSource(false);
    Require(Requests() == 2 && running.IsTrainerLeaderboardLocal(), "active run browsed or fetched");
}
int main(int argc, char** argv) {
    Require(argc == 2, "missing case");
    const std::string name = argv[1];
    if (name == "sources") Sources();
    else if (name == "timing") Timing();
    else if (name == "status") Status();
    else if (name == "invalidation") Invalidation();
    else if (name == "lifecycle") Lifecycle();
    else if (name == "movement") MovementBoards();
    else if (name == "all_boards") AllScenarioBoards();
    else Require(false, "unknown case");
}
'''


class AimTrainerLeaderboardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-leaderboard-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerPlayerController.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "bool ANCAimTrainerPlayerController::IsTrainerLeaderboardLocal",
            "int32 ANCAimTrainerPlayerController::SelectedLeaderboardKey",
            "const TArray<FNCAimTrainerLeaderboardRow>& ANCAimTrainerPlayerController::GetTrainerLeaderboard",
            "FString ANCAimTrainerPlayerController::GetTrainerLeaderboardStatus",
            "void ANCAimTrainerPlayerController::SelectTrainerLeaderboardSource",
            "void ANCAimTrainerPlayerController::RefreshTrainerLeaderboard",
            "void ANCAimTrainerPlayerController::ClientTrainerLeaderboard_Implementation",
            "void ANCAimTrainerPlayerController::ClientTrainerLeaderboardSubmitted_Implementation",
        )
        source = directory / "trainer_leaderboard.cpp"
        online = (PLUGIN / "Source/Private/NCAimTrainerOnline.cpp").read_text(encoding="utf-8-sig")
        revision = native_function(online, "int32 FNCAimTrainerOnline::PresetRevisionForScenario")
        slugs = native_function(online, "const TCHAR* FNCAimTrainerOnline::ScenarioSlug").replace("const TCHAR*", "const char*")
        source.write_text("\n".join([f'#include "{(PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()}"', ADAPTER, revision, slugs] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_leaderboard.exe" if os.name == "nt" else "trainer_leaderboard")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_leaderboard.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Leaderboard adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_source_and_scenario_switches_isolate_late_responses(self): self.run_case("sources")
    def test_success_and_error_ttl_do_not_replace_submission_status(self): self.run_case("timing")
    def test_loading_error_empty_and_legacy_response_states(self): self.run_case("status")
    def test_submission_invalidates_exact_source_and_inflight_generation(self): self.run_case("invalidation")
    def test_world_controller_and_active_run_guards(self): self.run_case("lifecycle")
    def test_fourth_scenario_and_movement_boards_are_isolated(self): self.run_case("movement")
    def test_all_eleven_scenarios_keep_44_scoped_boards_separate(self): self.run_case("all_boards")
