"""Execute the game's actual checkpoint recording helpers with a transport sink.

The sink records submitted batches; protocol/adversarial validation is covered
by the native HTTP tests and Django tests rather than reimplemented here.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function

ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#define TEXT(x) x
using int32 = int32_t;
struct FString : std::string {
    using std::string::string;
    bool IsEmpty() const { return empty(); }
};
struct FMath {
    static bool IsFinite(double x) { return std::isfinite(x); }
    static double Clamp(double x,double a,double b) { return std::max(a,std::min(x,b)); }
};
template<class T> struct TArray : std::vector<T> {
    int Num() const { return int(this->size()); }
    void Empty() { this->clear(); }
    void Add(const T& value) { this->push_back(value); }
};
template<class T> struct TSharedPtr : std::shared_ptr<T> {
    TSharedPtr() : std::shared_ptr<T>(std::make_shared<T>()) {}
    bool IsValid() const { return bool(*this); }
    void Reset() { this->reset(); }
};
struct FNCAimTrainerLocalEvent {
    enum EType { Shot, Hit, Expire, Sample };
    EType Type=Shot;
    int32 TimeUs=0,Target=0,Appearance=0;
    bool bHead=false,bFiring=false,bContact=false;
};
struct Sink {
    struct Batch { int Elapsed; TArray<FNCAimTrainerLocalEvent> Events; bool Final; };
    std::vector<Batch> Batches;
    bool Healthy=true,Cancelled=false,Ready=false;
    bool IsHealthy() const { return Healthy && !Cancelled; }
    bool IsRecordingReady() const { return Ready && IsHealthy(); }
    void Cancel() { Cancelled=true; }
    FString GetFailureReason() const { return "transport failed"; }
    bool QueueCheckpoint(int elapsed,const TArray<FNCAimTrainerLocalEvent>& events,bool final) {
        if (!IsHealthy()) return false;
        Batches.push_back({elapsed,events,final}); return true;
    }
};
struct PC { void SetTrainerOnlineStatus(const FString&) {} };
struct ANCAimTrainerGame {
    struct { int Scenario=1,Hits=0,Shots=0; } Progress;
    struct World { double Now=10; double GetTimeSeconds() const { return Now; } } TheWorld;
    World* GetWorld() { return &TheWorld; }
    double PhaseStartedAt=10;
    TSharedPtr<Sink> LocalSession;
    TArray<FNCAimTrainerLocalEvent> LocalEvents;
    int LocalAppearances[6]={1,1,1,1,1,1};
    int LocalNextCheckpoint=1,LocalLastShotCount=0;
    bool bLocalRecording=true,Standard=true;
    FString UnrankedReason;
    PC* Trainee=nullptr;
    bool IsTrainee(PC* pc) const { return pc!=nullptr; }
    bool IsStandardPreset() const { return Standard; }
    int RecordingStarts=0;
    void BeginLocalRecording() { ++RecordingStarts; bLocalRecording=true; }
    bool PrepareLocalRecording();
    void InvalidateLocalRun();
    void RecordLocalShotCount();
    void RecordLocalTarget(int32,bool,bool=false);
    void RecordLocalSample(bool,bool);
    void AddLocalEvent(FNCAimTrainerLocalEvent);
    void FlushLocalCheckpoint(bool=false);
};
void Check(bool ok,const char* why) { if(!ok) {std::cerr<<why; std::exit(1);} }
'''

CASES = r'''
int main(int argc,char** argv) {
    Check(argc==2,"case required"); std::string name=argv[1]; ANCAimTrainerGame game;
    if(name=="anchor_ack") {
        PC player; game.Trainee=&player; game.bLocalRecording=false;
        Check(!game.PrepareLocalRecording(),"run started before anchor acknowledged");
        Check(!game.PrepareLocalRecording() && game.RecordingStarts==1,"repeated anchor start while waiting");
        game.LocalSession->Ready=true;
        Check(game.PrepareLocalRecording(),"acknowledged run did not start");
        game.LocalSession->Healthy=false;
        Check(game.PrepareLocalRecording(),"unavailable service blocked practice");
    } else if(name=="precision") {
        game.TheWorld.Now=10.5; game.Progress.Shots=1; game.RecordLocalShotCount();
        game.RecordLocalTarget(2,true,true); game.RecordLocalShotCount();
        Check(game.LocalEvents.Num()==2,"duplicate shot or missing hit");
        Check(game.LocalEvents[0].Type==FNCAimTrainerLocalEvent::Shot,"shot must precede hit");
        Check(game.LocalEvents[1].bHead && game.LocalEvents[1].Target==2
            && game.LocalEvents[1].Appearance==1,"headshot appearance lost");
        Check(game.LocalEvents[0].TimeUs==500000 && game.LocalEvents[1].TimeUs==500000,"timestamps drifted");
        game.TheWorld.Now=11; game.LocalAppearances[2]=2; game.RecordLocalTarget(2,false);
        Check(game.LocalEvents[2].Type==FNCAimTrainerLocalEvent::Expire
            && game.LocalEvents[2].Appearance==2,"expiry reused prior appearance");
        game.Progress.Scenario=3; game.TheWorld.Now=11.5;
        game.RecordLocalTarget(3,true,false); game.RecordLocalTarget(4,true,true);
        Check(!game.LocalEvents[3].bHead && game.LocalEvents[4].bHead,
            "popup body/head observations collapsed into one event");
    } else if(name=="counter_jump") {
        auto sink=game.LocalSession; game.Progress.Shots=2; game.RecordLocalShotCount();
        Check(!game.bLocalRecording && sink->Cancelled && game.LocalEvents.Num()==0,"invented times for accumulated shots");
    } else if(name=="counter_reset") {
        auto sink=game.LocalSession; game.LocalLastShotCount=2; game.Progress.Shots=1; game.RecordLocalShotCount();
        Check(!game.bLocalRecording && sink->Cancelled,"counter reset accepted");
    } else if(name=="tracking") {
        game.Progress.Scenario=0; game.TheWorld.Now=10.04; game.RecordLocalSample(false,false);
        game.TheWorld.Now=10.08; game.RecordLocalSample(true,false);
        game.TheWorld.Now=10.12; game.RecordLocalSample(true,true); game.RecordLocalTarget(0,true);
        Check(game.LocalEvents.Num()==3,"tracking emitted precision event");
        Check(!game.LocalEvents[0].bFiring && game.LocalEvents[1].bFiring && !game.LocalEvents[1].bContact
            && game.LocalEvents[2].bContact,"idle/off-target/contact collapsed");
    } else if(name=="boundaries") {
        auto sink=game.LocalSession;
        for(int sequence=1;sequence<=11;++sequence) {
            game.TheWorld.Now=10+sequence*5-.01; game.FlushLocalCheckpoint();
            Check(int(sink->Batches.size())==sequence-1,"early checkpoint");
            game.TheWorld.Now=10+sequence*5+.01; game.RecordLocalTarget(0,false); game.FlushLocalCheckpoint();
            Check(int(sink->Batches.size())==sequence && game.LocalEvents.Num()==0,"checkpoint lost or buffer uncleared");
            Check(!sink->Batches.back().Final && sink->Batches.back().Events.size()==1,"wrong batch");
        }
        game.TheWorld.Now=70.01; game.FlushLocalCheckpoint(true);
        Check(sink->Batches.size()==12 && sink->Batches.back().Final && sink->Batches.back().Elapsed==60000000,"bad final");
    } else if(name=="no_catchup") {
        auto sink=game.LocalSession; game.TheWorld.Now=16; game.FlushLocalCheckpoint();
        Check(sink->Cancelled && sink->Batches.empty(),"synthesized delayed checkpoint");
    } else if(name=="no_missing_final") {
        auto sink=game.LocalSession; game.TheWorld.Now=70; game.FlushLocalCheckpoint(true);
        Check(sink->Cancelled && sink->Batches.empty(),"final accepted without prior windows");
    } else if(name=="preset_change") {
        auto sink=game.LocalSession; game.Standard=false; game.FlushLocalCheckpoint();
        Check(sink->Cancelled,"changed preset remained eligible");
    } else if(name=="event_bounds") {
        game.TheWorld.Now=9; game.RecordLocalSample(false,false);
        game.TheWorld.Now=70; game.RecordLocalSample(false,false);
        Check(game.LocalEvents.Num()==0,"outside-run event recorded");
        game.TheWorld.Now=11;
        for(int i=0;i<256;++i) game.RecordLocalSample(false,false);
        Check(game.LocalEvents.Num()==256,"buffer limit too small");
        auto sink=game.LocalSession; game.RecordLocalSample(false,false);
        Check(sink->Cancelled && game.LocalEvents.Num()==0,"unbounded event buffer");
    } else if(name=="transport_failure") {
        auto sink=game.LocalSession; sink->Healthy=false; game.TheWorld.Now=15;
        game.RecordLocalSample(false,false); game.FlushLocalCheckpoint();
        Check(sink->Cancelled && !game.bLocalRecording && sink->Batches.empty(),"failed transport published");
    } else Check(false,"unknown case");
}
'''

class LocalRecordingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-local-recording-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        functions = [native_function(game, "void ANCAimTrainerGame::" + name) for name in (
            "InvalidateLocalRun", "AddLocalEvent", "RecordLocalShotCount", "RecordLocalTarget",
            "RecordLocalSample", "FlushLocalCheckpoint")]
        functions.append(native_function(game, "bool ANCAimTrainerGame::PrepareLocalRecording"))
        source = directory / "recording.cpp"
        source.write_text("\n".join([ADAPTER] + functions + [CASES]), encoding="utf-8")
        cls.executable = directory / ("recording.exe" if os.name == "nt" else "recording")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                    f"/Fe{cls.executable}", f"/Fo{directory / 'recording.obj'}"] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("anchor_ack", "precision", "counter_jump", "counter_reset", "tracking", "boundaries", "no_catchup",
              "no_missing_final", "preset_change", "event_bounds", "transport_failure"):
    setattr(LocalRecordingTests, "test_" + _case, lambda self, case=_case: self.run_case(case))

if __name__ == "__main__":
    unittest.main()
