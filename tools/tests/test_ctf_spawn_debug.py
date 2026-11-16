"""Execute production command/lifetime gates against a small native adapter.

Real rendering and network delivery still require an in-game playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_wipeout_healing import PLUGIN, find_compiler, native_function

ADAPTER = r'''
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using int32=int;
const int32 MAX_int32=2147483647;
using TCHAR=char;
#define TEXT(x) x
#define UE_LOG(...)
struct FString : std::string {
    using std::string::string;
    FString(const std::string& s) : std::string(s) {}
    const char* operator*() const { return c_str(); }
    bool IsEmpty() const { return empty(); }
    int Len() const { return int(size()); }
    FString ToLower() const { FString s=*this;for(char& c:s) c=char(std::tolower(c));return s; }
    bool IsNumeric() const { return !empty() && std::all_of(begin(),end(),[](char c){return std::isdigit(c);}); }
};
struct FCString { static int Atoi(const char* s) { return std::atoi(s); } };
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int Num() const { return int(this->size()); }
};
enum { NM_Standalone, NM_DedicatedServer, NM_Client };
namespace MatchState { const FString WaitingToStart="WaitingToStart"; }
struct AUTGameState {
    FString State=MatchState::WaitingToStart;
    FString GetMatchState() const { return State; }
};
struct UWorld;
struct AActor { virtual ~AActor()=default; };
struct ANCPlusCTFGameMode : AActor {};
struct APlayerState { bool Admin=false; };
struct APlayerController : AActor {
    APlayerState* PlayerState=nullptr;
    int Mode=NM_DedicatedServer;
    bool Local=false;
    int GetNetMode() const { return Mode; }
    bool IsLocalController() const { return Local; }
};
template<class T,class U> T* Cast(U* x) { return dynamic_cast<T*>(x); }
namespace NCPlusReflection {
    bool GetBool(APlayerState* ps,const TCHAR*) { return ps && ps->Admin; }
}
struct FActorSpawnParameters { AActor* Owner=nullptr;int SpawnCollisionHandlingOverride=0; };
namespace ESpawnActorCollisionHandlingMethod { const int AlwaysSpawn=1; }
struct ControllerRef { APlayerController* PC;APlayerController* Get() const { return PC; } };
struct FConstPlayerControllerIterator {
    std::vector<ControllerRef>* Values;size_t Index=0;
    explicit operator bool() const { return Index<Values->size(); }
    FConstPlayerControllerIterator& operator++() { ++Index;return *this; }
    ControllerRef* operator->() { return &(*Values)[Index]; }
};
struct LineComponent { int Clears=0;void Flush(){++Clears;} };
struct UTextRenderComponent { bool Visible=true;void SetVisibility(bool b){Visible=b;} };
struct BaseActor : AActor { void Tick(float) {} };
struct ANCCTFSpawnDebug : BaseActor {
    using Super=BaseActor;
    UWorld* World=nullptr;AActor* Owner=nullptr;
    bool Dead=false,Authority=true;
    int TargetPlayerId=-1,RefreshCount=0,RenderCount=0;
    float RefreshRemaining=0;
    LineComponent Storage;LineComponent* Lines=&Storage;
    TArray<UTextRenderComponent*> Labels;
    AActor* GetOwner(){return Owner;}
    UWorld* GetWorld(){return World;}
    bool HasAuthority()const{return Authority;}
    int GetNetMode()const;
    void Destroy(){Dead=true;}
    void RefreshMarkers(APlayerController*){++RefreshCount;}
    void RenderMarkers(APlayerController*){++RenderCount;}
    void Tick(float);
};
struct UWorld {
    int Mode=NM_DedicatedServer;
    AUTGameState* GS=nullptr;AActor* GM=nullptr;
    std::vector<ControllerRef> Players;
    std::vector<ANCCTFSpawnDebug*> Actors;
    ~UWorld(){for(auto a:Actors) delete a;}
    int GetNetMode()const{return Mode;}
    template<class T>T* GetGameState(){return static_cast<T*>(GS);}
    AActor* GetAuthGameMode(){return GM;}
    FConstPlayerControllerIterator GetPlayerControllerIterator(){return {&Players,0};}
    template<class T>T* SpawnActor(const FActorSpawnParameters& p){
        auto a=new T;a->World=this;a->Owner=p.Owner;Actors.push_back(a);return a;
    }
};
int ANCCTFSpawnDebug::GetNetMode()const{return World->Mode;}
template<class T>struct TActorIterator {
    UWorld* World;size_t Index=0;
    explicit TActorIterator(UWorld* w):World(w){Skip();}
    void Skip(){while(Index<World->Actors.size() && World->Actors[Index]->Dead)++Index;}
    explicit operator bool()const{return Index<World->Actors.size();}
    TActorIterator& operator++(){++Index;Skip();return *this;}
    T* operator->(){return World->Actors[Index];}
};
'''

CASES = r'''
void Check(bool b,const char* m){if(!b){std::cerr<<m<<"\n";std::exit(1);}}
int Active(UWorld& w){int n=0;for(auto a:w.Actors)if(!a->Dead)++n;return n;}
struct Fixture {
    UWorld World;AUTGameState GS;ANCPlusCTFGameMode GM;
    APlayerState Admin,Normal;APlayerController Viewer,Other;
    Fixture(){World.GS=&GS;World.GM=&GM;Admin.Admin=true;Viewer.PlayerState=&Admin;
        Other.PlayerState=&Normal;World.Players={{&Viewer},{&Other}};}
};
int main(){
    {
        Fixture f;SpawnBlocksCommand({},&f.World);
        Check(Active(f.World)==1 && f.World.Actors.back()->Owner==&f.Viewer,
            "only authenticated admin should own overlay");
        Check(f.World.Actors.back()->TargetPlayerId==-1,"default should follow viewer");
        SpawnBlocksCommand({"on","42"},&f.World);
        Check(Active(f.World)==1 && f.World.Actors.back()->TargetPlayerId==42,
            "repeat enable must replace overlay and apply explicit perspective");
        SpawnBlocksCommand({"bad"},&f.World);SpawnBlocksCommand({"on","oops"},&f.World);
        Check(Active(f.World)==1,"invalid command should leave existing overlay alone");
        SpawnBlocksCommand({"off"},&f.World);Check(Active(f.World)==0,"disable must remove overlays");
    }
    {
        Fixture f;f.World.Mode=NM_Client;SpawnBlocksCommand({"on"},&f.World);
        Check(Active(f.World)==0,"client-side command must not create overlays");
        f.World.Mode=NM_DedicatedServer;f.GS.State="InProgress";SpawnBlocksCommand({"on"},&f.World);
        Check(Active(f.World)==0,"live game must reject enable");
        f.GS.State=MatchState::WaitingToStart;f.World.GM=nullptr;SpawnBlocksCommand({"on"},&f.World);
        Check(Active(f.World)==0,"non-CTF game must reject enable");
    }
    {
        Fixture f;SpawnBlocksCommand({"on"},&f.World);auto a=f.World.Actors.back();
        a->Tick(.01f);a->Tick(.1f);Check(a->RefreshCount==1,"refresh should be bounded to 4Hz");
        a->Tick(.15f);Check(a->RefreshCount==2,"refresh did not update after quarter second");
        Check(a->RenderCount==0,"dedicated server must not render markers");
        f.Admin.Admin=false;a->Tick(.01f);Check(a->Dead,"revoked admin must lose overlay");
    }
    {
        Fixture f;SpawnBlocksCommand({"on"},&f.World);auto a=f.World.Actors.back();
        UTextRenderComponent label;a->Labels.push_back(&label);f.GS.State="InProgress";
        a->Tick(.01f);Check(a->Dead && a->Lines->Clears==1 && !label.Visible,
            "match start must clear geometry, hide labels and destroy server actor");
    }
    {
        Fixture f;SpawnBlocksCommand({"on"},&f.World);auto a=f.World.Actors.back();
        a->Authority=false;f.World.Mode=NM_Client;f.Viewer.Local=true;
        a->Tick(.01f);Check(a->RenderCount==1 && a->RefreshCount==0,"client must render replicated data only");
        UTextRenderComponent label;a->Labels.push_back(&label);f.GS.State="CountdownToBegin";
        a->Tick(.01f);Check(a->RenderCount==1 && !label.Visible && a->Lines->Clears==1,
            "client must hide markers when warmup ends before destroy replication arrives");
    }
    for(const char* invalid:{"","3.5","-1","+3","2147483648","999999999999999999"}) {
        int id=42;Check(!ParseSpawnDebugPlayerId(invalid,id) && id==42,"invalid ID accepted or output changed");
    }
    int maxId=-1;Check(ParseSpawnDebugPlayerId("2147483647",maxId) && maxId==2147483647,"largest valid ID rejected");
    SpawnBlocksCommand({},nullptr);
    Check(!IsSpawnDebugWarmup(nullptr) && !CanViewSpawnDebug(nullptr),"missing context should be safe");
    std::cout<<"spawn debug command and lifecycle passed\n";
}
'''


class CTFSpawnDebugTests(unittest.TestCase):
    def test_command_authority_visibility_and_lifecycle(self):
        compiler, env, msvc = find_compiler()
        source = (PLUGIN / 'Source/Private/NCCTFSpawnDebug.cpp').read_text()
        code = '\n'.join(native_function(source, signature) for signature in (
            'bool IsSpawnDebugWarmup(', 'bool CanViewSpawnDebug(', 'bool ParseSpawnDebugPlayerId(', 'void SpawnBlocksCommand(',
            'void ANCCTFSpawnDebug::Tick('))
        with tempfile.TemporaryDirectory(prefix='ctf-spawn-debug-') as temp:
            path=Path(temp);cpp=path/'test.cpp';exe=path/('test.exe' if os.name=='nt' else 'test')
            cpp.write_text(ADAPTER+code+CASES)
            cmd=([compiler,'/nologo','/EHsc','/std:c++14',str(cpp),f'/Fe:{exe}'] if msvc else
                 [compiler,'-std=c++14','-Wall','-Wextra',str(cpp),'-o',str(exe)])
            result=subprocess.run(cmd,cwd=path,env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe)],cwd=path,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=='__main__':
    unittest.main()
