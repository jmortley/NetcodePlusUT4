"""Execute production Tick, event timestamps and checkpoint flush against an aged world.

The fixture accumulates a separate float UWorld clock to reproduce the original
rounding defect, while production Tick advances the new double trainer clock.
This is native method coverage; Jeremy compiles and playtests the UE release.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_aim_trainer_local_recording import ADAPTER as RECORDING_ADAPTER
from test_wipeout_healing import PLUGIN, find_compiler, native_function


EXTRA_TYPES = r'''
struct AUTCharacter {};
template<class T,class U> T* Cast(U* pointer) { return static_cast<T*>(pointer); }
struct BaseGame { void Tick(float) {} };
struct ANCAimTrainerTarget { bool IsPendingKillPending() const { return false; } };
struct ArenaType { bool IsPendingKillPending() const { return false; } };
struct Weapon { int Ammo=0,MaxAmmo=100; };
struct Settings { float GetEffectiveTimeDilation() const { return 1.f; } };
namespace NCAimTrainerLayout { enum { TargetCount=6 }; }
double RealTime=0.0;
std::vector<double> SendTimes;
std::vector<int> ExpectedEventTimes;
'''

EXTRA_GAME = r'''
    using Super=BaseGame;
    double TrainerTimeSeconds=5000.0,NextStatusTime=0.0,LastTraceTime=0.0,NextEventTime=0.0;
    double FinishedAt=0.0;
    bool bLocalAuthPending=false,bRankedRun=false,Finished=false;
    ArenaType ArenaStorage;
    ArenaType* Arena=&ArenaStorage;
    TArray<ANCAimTrainerTarget*> Targets;
    Weapon* RunWeapon=nullptr;
    Settings Options;
    Settings* GetWorldSettings() { return &Options; }
    bool IsInsidePracticeLane(AUTCharacter*) const { return true; }
    void AbortTraining(PC*) { std::abort(); }
    void BeginActiveRun() { Progress.Phase=2;PhaseStartedAt=GetTrainerTimeSeconds(); }
    void UpdateShotCount() {
        if (GetTrainerTimeSeconds()>=NextEventTime && GetTrainerTimeSeconds()-PhaseStartedAt<60.0) {
            RecordLocalSample(false,false);
            ExpectedEventTimes.push_back(int(RealTime*1000000.0));
            NextEventTime=GetTrainerTimeSeconds()+0.1;
        }
    }
    void UpdateTargets(double now) { CheckNow=now; }
    double CheckNow=0.0;
    void UpdateTrackingMovement(double) {}
    void UpdateTrackingSample(double) {}
    void FinishRun() { Finished=true;FinishedAt=GetTrainerTimeSeconds();Progress.Phase=3;FlushLocalCheckpoint(true); }
    void Tick(float DeltaSeconds);
'''

CASES = r'''
int main(int argc,char** argv) {
    Check(argc==2,"case required"); const std::string name=argv[1];
    if(name=="invalid_deltas") {
        for(float delta:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
            Check(NCAimTrainerClock::Advance(5000.0,delta)==5000.0,"invalid delta damaged monotonic clock");
        return 0;
    }
    const int fps=name=="700fps"?700:960;
    const float dt=1.f/float(fps);
    ANCAimTrainerGame game; PC player; ANCAimTrainerTarget targets[6];
    game.Trainee=&player; game.PhaseStartedAt=5000.0; game.Progress.Scenario=0;
    game.TheWorld.Now=5000.f;
    for(auto& target:targets) game.Targets.Add(&target);
    int frame=0;
    while(!game.Finished && frame<fps*60+2) {
        ++frame; RealTime=double(frame)/double(fps);
        game.TheWorld.Now+=dt; // Actual UE4.15 float accumulation defect.
        game.Tick(dt);
        Check(std::fabs(game.GetTrainerTimeSeconds()-5000.0-double(frame)*double(dt))<.001,
            "trainer elapsed drifted by more than 1 ms from supplied frame time");
        if(!game.Finished) Check(game.CheckNow==game.GetTrainerTimeSeconds(),"target scheduling used another clock");
    }
    Check(game.Finished && RealTime>=60.0-.000001 && RealTime<=60.0+double(dt)+.000001,
        "run did not end at 60 real seconds within one frame");
    Check(std::fabs(double(game.TheWorld.Now)-5000.0-RealTime)>.5,
        "fixture failed to reproduce material float world-clock drift");
    auto sink=game.LocalSession;
    Check(!sink->Cancelled && sink->Batches.size()==12 && SendTimes.size()==12,
        "aged session lost a checkpoint or invalidated an otherwise valid run");
    size_t eventIndex=0;
    for(size_t index=0;index<12;++index) {
        const auto& batch=sink->Batches[index];
        const double deadline=5.0*double(index+1);
        Check(SendTimes[index]>=deadline-.000001 && SendTimes[index]<=deadline+double(dt)+.000001,
            "checkpoint not sent on its real 5-second boundary (within one tick)");
        Check(std::fabs(double(batch.Elapsed)/1000000.0-SendTimes[index])<=double(dt)+.00001,
            "checkpoint elapsed does not match real elapsed");
        Check(batch.Final==(index==11),"final checkpoint marker changed");
        for(const auto& event:batch.Events) {
            Check(eventIndex<ExpectedEventTimes.size(),"unexpected fabricated event");
            Check(std::abs(event.TimeUs-ExpectedEventTimes[eventIndex])<=5,
                "event timestamps do not share the precise run/checkpoint clock");
            ++eventIndex;
        }
    }
    Check(eventIndex==ExpectedEventTimes.size(),"event lost while checkpointing");
    Check(SendTimes[8]>=45.0-.000001,"checkpoint 9 arrived before real 45 seconds");
}
'''


class AimTrainerClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-clock-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        controller = (PLUGIN / "Source/Public/NCAimTrainerPlayerController.h").read_text(encoding="utf-8-sig")
        adapter = RECORDING_ADAPTER.replace("#include <vector>", "#include <vector>\n#include <limits>")
        adapter = adapter.replace("struct PC {", "struct AUTCharacter;\nstruct PC { AUTCharacter* GetPawn() { return nullptr; }")
        adapter = adapter.replace("struct ANCAimTrainerGame {", EXTRA_TYPES + "\nstruct ANCAimTrainerGame : BaseGame {")
        adapter = adapter.replace("struct { int Scenario=1,Hits=0,Shots=0,Phase=2;", "struct { float RemainingSeconds=60.f; int Scenario=1,Hits=0,Shots=0,Phase=2;")
        adapter = adapter.replace("struct World { double Now=10; double GetTimeSeconds() const { return Now; } }", "struct World { float Now=5000.f; float GetTimeSeconds() const { return Now; } }")
        adapter = adapter.replace("return TheWorld.Now;", "return TrainerTimeSeconds;")
        adapter = adapter.replace("double PhaseStartedAt=10;", "double PhaseStartedAt=10;\n" + EXTRA_GAME)
        adapter = adapter.replace("static bool IsFinite(double x)", "template<class T> static T Max(T a,T b) { return std::max(a,b); }\n    static bool IsNearlyEqual(float a,float b) { return std::fabs(a-b)<.0001f; }\n    static bool IsFinite(double x)")
        adapter = adapter.replace("struct Sink {", "extern double RealTime; extern std::vector<double> SendTimes;\nstruct Sink {")
        adapter = adapter.replace("Batches.push_back({elapsed,events,final});", "SendTimes.push_back(RealTime); Batches.push_back({elapsed,events,final});")
        includes = "\n".join('#include "' + (PLUGIN / path).as_posix() + '"' for path in (
            "Source/Public/NCAimTrainerClock.h", "Source/Private/NCAimTrainerScenarioPolicy.h"))
        functions = [native_function(game, "void ANCAimTrainerGame::" + name) for name in (
            "Tick", "InvalidateLocalRun", "AddLocalEvent", "RecordLocalShotCount", "RecordLocalTarget",
            "RecordLocalSample", "FlushLocalCheckpoint")]
        save_states = native_function(controller, "namespace NCAimTrainerSaveState")
        source = directory / "clock.cpp"
        source.write_text("\n".join([includes, adapter, save_states] + functions + [CASES]), encoding="utf-8")
        cls.executable = directory / ("clock.exe" if os.name == "nt" else "clock")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                    f"/Fe{cls.executable}", f"/Fo{directory / 'clock.obj'}"] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_700fps_at_large_world_age(self): self.run_case("700fps")
    def test_960fps_at_large_world_age(self): self.run_case("960fps")
    def test_invalid_deltas_cannot_poison_or_rewind_clock(self): self.run_case("invalid_deltas")


if __name__ == "__main__":
    unittest.main()
