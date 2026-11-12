"""Run the real result/start/completion methods with asynchronous service sinks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using int32=int32_t; using uint8=uint8_t;
#define TEXT(x) x
#define UPROPERTY(...)
#define GENERATED_BODY()
#define UE_LOG(...)
struct FString : std::string {
    using std::string::string;
    bool IsEmpty() const { return empty(); }
};
template<class T> struct TArray : std::vector<T> { void Empty() { this->clear(); } };
template<class T> struct TWeakObjectPtr {
    T* Value;
    explicit TWeakObjectPtr(T* value):Value(value){}
    bool IsValid() const { return Value!=nullptr; }
    T* Get() const { return Value; }
    T* operator->() const { return Value; }
};
template<class T> struct TSharedPtr : std::shared_ptr<T> {
    TSharedPtr()=default;
    explicit TSharedPtr(T* value):std::shared_ptr<T>(value){}
    bool IsValid() const { return bool(*this); }
    void Reset() { this->reset(); }
};
template<class T,class U> T* Cast(U* value) { return static_cast<T*>(value); }
enum { NM_Standalone, NM_ListenServer };
struct AUTPlayerState { FString StatsID="account",PlayerName="Player"; };
struct World { float Now=0.f; float GetTimeSeconds() const { return Now; } };
struct FNCAimTrainerResult {
    int Scenario=0,Score=0,Shots=0,Hits=0,Headshots=0,TargetsExpired=0,TrackedMilliseconds=0,FiredMilliseconds=0;
    bool bMovementPractice=false;
    FString RunId,PlayerId,DisplayName;
};
struct FNCAimTrainerOnline {
    static int Submits;
    static std::function<void(bool,const FString&)> Completion;
    static void Submit(World*,const FNCAimTrainerResult&,std::function<void(bool,const FString&)> callback) {
        ++Submits; Completion=callback;
    }
};
int FNCAimTrainerOnline::Submits=0;
std::function<void(bool,const FString&)> FNCAimTrainerOnline::Completion;
struct FNCAimTrainerLocalSession {
    static std::function<void(TSharedPtr<FNCAimTrainerLocalSession>,const FString&)> StartCompletion;
    std::function<void(bool,const FString&,int32)> Completion;
    int Cancels=0,Starts=0;
    void Cancel() { ++Cancels; }
    void BeginRecording() { ++Starts; }
    void SetCompletion(std::function<void(bool,const FString&,int32)> callback) { Completion=callback; }
    static TSharedPtr<FNCAimTrainerLocalSession> Start(World*,uint8,const FString&,
        std::function<void(TSharedPtr<FNCAimTrainerLocalSession>,const FString&)> callback,bool) {
        StartCompletion=callback; return TSharedPtr<FNCAimTrainerLocalSession>(new FNCAimTrainerLocalSession);
    }
};
std::function<void(TSharedPtr<FNCAimTrainerLocalSession>,const FString&)> FNCAimTrainerLocalSession::StartCompletion;
namespace NCAimTrainerScoring {
    int TrackingMilliseconds(double value) { return int(value*1000.0); }
    float TrackingAccuracy(int tracked,int fired) { return fired?100.f*float(tracked)/float(fired):0.f; }
}
// PRODUCTION_PROGRESS
struct ANCAimTrainerPlayerController {
    AUTPlayerState State;
    AUTPlayerState* PlayerState=&State;
    FString Status;
    void SetTrainerOnlineStatus(const FString& status) { Status=status; }
};
struct Weapon { void StopFire(int) {} };
struct ANCAimTrainerGame {
    World TheWorld;
    World* GetWorld() { return &TheWorld; }
    int NetMode=NM_Standalone;
    int GetNetMode() const { return NetMode; }
    bool Standard=true,bRankedRun=false,bLocalRecording=false,bLocalAuthPending=false;
    bool IsStandardPreset() const { return Standard; }
    FNCAimTrainerProgress Progress;
    ANCAimTrainerPlayerController* Trainee=nullptr;
    bool IsTrainee(ANCAimTrainerPlayerController* pc) const { return pc && pc==Trainee; }
    TSharedPtr<FNCAimTrainerLocalSession> LocalSession;
    FString UnrankedReason,RunId="current";
    TArray<int> LocalEvents;
    double PhaseStartedAt=0.0;
    double GetTrainerTimeSeconds() const { return TheWorld.Now; }
    double TrackedSeconds=0.0,FiredSeconds=0.0;
    Weapon* RunWeapon=nullptr;
    int Publishes=0,Refreshes=0,FinalFlushes=0,Invalidations=0;
    void PublishProgress() { ++Publishes; }
    void RefreshLeaderboard(bool) { ++Refreshes; }
    void FlushLocalCheckpoint(bool final) { if(final)++FinalFlushes; }
    void InvalidateLocalRun() { ++Invalidations; }
    void UpdateShotCount() {}
    void FlushFlakHitFeedback() {}
    void HideAllTargets() {}
    void ClearTrainerProjectiles() {}
    void FinishRun();
    void StartLocalSession();
    void BeginLocalRecording();
};
void Check(bool value,const char* reason) { if(!value) { std::cerr<<reason;std::exit(1); } }
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    Fixture() {
        Game.Trainee=&Player; Game.Progress.Phase=2;
        Game.Progress.Scenario=7; Game.Progress.Score=100; Game.Progress.Shots=1; Game.Progress.Hits=1;
    }
    void Local() {
        Game.LocalSession=TSharedPtr<FNCAimTrainerLocalSession>(new FNCAimTrainerLocalSession);
        Game.BeginLocalRecording();
    }
};
'''

CASES = r'''
int main(int argc,char** argv) {
    Check(argc==2,"case required"); std::string name=argv[1]; Fixture f;
    if(name=="start_rejected") {
        f.Game.Progress.Phase=1; f.Game.StartLocalSession();
        FNCAimTrainerLocalSession::StartCompletion({},"Practice only: UT4Stats rate limit; new online runs available in 60 seconds.");
        Check(!f.Game.bLocalAuthPending&&!f.Game.LocalSession.IsValid(),"rejected start left active auth");
        f.Game.Progress.Phase=2; f.Game.FinishRun();
        Check(f.Game.Progress.SaveState==NCAimTrainerSaveState::NotSaved
            && f.Player.Status.find("60 seconds")!=std::string::npos
            && FNCAimTrainerOnline::Submits==0,"rejected start did not yield unsaved result and reason");
    } else if(name=="checkpoint_failed") {
        f.Local(); f.Game.LocalSession->Completion(false,"Practice only: checkpoint rejected",0);
        f.Game.FinishRun();
        Check(f.Game.Progress.SaveState==NCAimTrainerSaveState::NotSaved
            && f.Player.Status.find("checkpoint rejected")!=std::string::npos
            && f.Game.FinalFlushes==0,"failed checkpoint run advertised a pending or saved result");
    } else if(name=="local_saved"||name=="local_unconfirmed") {
        f.Local(); f.Game.FinishRun();
        Check(f.Game.Progress.SaveState==NCAimTrainerSaveState::Pending&&f.Game.FinalFlushes==1,
            "local result advertised a save before confirmation");
        const bool success=name=="local_saved";
        f.Game.LocalSession->Completion(success,success?"Score saved":"Upload not confirmed",success?95:0);
        Check(f.Game.Progress.SaveState==(success?NCAimTrainerSaveState::Saved:NCAimTrainerSaveState::Unconfirmed),
            "local completion misrepresented save outcome");
        Check(f.Game.Progress.Score==(success?95:100)&&f.Game.Refreshes==(success?1:0),
            "failed completion erased score or success omitted authoritative score/refresh");
    } else if(name=="server_saved"||name=="server_unconfirmed") {
        f.Game.bRankedRun=true; f.Game.NetMode=NM_ListenServer; f.Game.FinishRun();
        Check(f.Game.Progress.SaveState==NCAimTrainerSaveState::Pending&&FNCAimTrainerOnline::Submits==1,
            "server result advertised a save before confirmation");
        const bool success=name=="server_saved";
        FNCAimTrainerOnline::Completion(success,success?"Score saved":"Upload not confirmed");
        Check(f.Game.Progress.SaveState==(success?NCAimTrainerSaveState::Saved:NCAimTrainerSaveState::Unconfirmed)
            &&f.Game.Refreshes==(success?1:0),"server completion misrepresented save outcome");
    } else if(name=="stale_local"||name=="stale_server"||name=="stale_start") {
        if(name=="stale_start") { f.Game.Progress.Phase=1;f.Game.StartLocalSession(); }
        else if(name=="stale_local") { f.Local();f.Game.FinishRun(); }
        else { f.Game.bRankedRun=true;f.Game.FinishRun(); }
        f.Game.RunId="new-run"; f.Game.Progress=FNCAimTrainerProgress();
        f.Player.Status="new status"; const int published=f.Game.Publishes;
        if(name=="stale_start") FNCAimTrainerLocalSession::StartCompletion({},"old rejection");
        else if(name=="stale_local") f.Game.LocalSession->Completion(true,"old success",999);
        else FNCAimTrainerOnline::Completion(false,"old failure");
        Check(f.Game.Progress.SaveState==NCAimTrainerSaveState::None&&f.Game.Progress.Score==0
            &&f.Player.Status=="new status"&&f.Game.Publishes==published&&f.Game.Refreshes==0,
            "old callback clobbered the next run's result");
    } else Check(false,"unknown case");
}
'''


class AimTrainerSaveStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-save-status-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        controller = (PLUGIN / "Source/Public/NCAimTrainerPlayerController.h").read_text(encoding="utf-8-sig")
        progress = native_function(controller, "namespace NCAimTrainerSaveState") + "\n" + native_function(controller, "struct FNCAimTrainerProgress") + ";"
        functions = [native_function(game, "void ANCAimTrainerGame::" + name)
                     for name in ("FinishRun", "StartLocalSession", "BeginLocalRecording")]
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()
        source = directory / "save.cpp"
        source.write_text("\n".join([f'#include "{policy}"', ADAPTER.replace("// PRODUCTION_PROGRESS", progress)] + functions + [CASES]), encoding="utf-8")
        cls.executable = directory / ("save.exe" if os.name == "nt" else "save")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                    f"/Fe{cls.executable}", f"/Fo{directory / 'save.obj'}"] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("start_rejected", "checkpoint_failed", "local_saved", "local_unconfirmed", "server_saved",
              "server_unconfirmed", "stale_local", "stale_server", "stale_start"):
    setattr(AimTrainerSaveStatusTests, "test_" + _case, lambda self, case=_case: self.run_case(case))

if __name__ == "__main__":
    unittest.main()
