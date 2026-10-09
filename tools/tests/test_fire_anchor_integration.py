"""329 extracted timing/scope regressions. No UHT, actor replication or gameplay claim."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import find_compiler, native_function

PLUGIN = Path(__file__).resolve().parents[2]

ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include "NCFireAnchorPolicy.h"
using uint8 = uint8_t;
struct FMath {
    static float Max(float a,float b) { return std::max(a,b); }
    static float Clamp(float a,float b,float c) { return std::max(b,std::min(a,c)); }
};
template<class K,class V> struct TMap {
    std::map<K,V> Values;
    V* Find(K key) { auto i=Values.find(key); return i==Values.end()?nullptr:&i->second; }
    void Add(K key,V value) { Values[key]=value; }
    void Remove(K key) { Values.erase(key); }
};
struct AController { virtual ~AController(){} };
struct APlayerController : AController {
    bool Local=false, Confirmed=false, RTTValid=true; float RTT=60;
    bool IsLocalController() const { return Local; }
};
using AUTPlayerController=APlayerController;
struct AUTBot : AController {};
struct APlayerState { float ExactPing=100; };
struct Pawn { AController* Controller=nullptr; APlayerState* PlayerState=nullptr; };
template<class T,class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
template<class T> struct Weak {
    T* Value=nullptr;
    T* Get() const { return Value; }
};
struct FNCFireAnchor {
    bool bEnforce=false, bValid=true;
    float AcceptedAt=10.04f,ExtraAtAccept=.04f,BaseRewind=.02f,ObservedRTTMs=60,Cap=.125f;
};
namespace NCFireAnchor {
    int CurrentMode=1;
    int Mode() { return CurrentMode; }
    bool Dispatch(const FNCFireAnchor& a,float now,float& extra) {
        return a.bValid && NCFireAnchorPolicy::DispatchAge(now,a.AcceptedAt,a.ExtraAtAccept,a.BaseRewind,a.Cap,extra);
    }
}
namespace NCPlusVersionGate {
    bool IsProtocolConfirmed(APlayerController* pc) { return pc && pc->Confirmed; }
}
bool GetServerObservedRTTMs(const AUTPlayerController* pc,float& rtt) {
    if (!pc || !pc->RTTValid) return false;
    rtt=pc->RTT; return true;
}
const int ROLE_Authority=3;
struct AUTWeaponFix {
    int Role=ROLE_Authority;
    Pawn* UTOwner=nullptr;
    Weak<AController> FireProtocolController;
    float MaxRewindMs=250;
    bool Is329FireProtocolReady() const;
    float GetPredictionTimeWithFudgeMs(float) const;
    float GetHitValidationRenderTime(float,bool&) const;
};
struct FDeferredEquipFireContext {
    bool bRateObservedRTTValid=true,bDeferredRate=true;
    float RatePredictionTime=.02f,RateObservedRTTMs=60,ServerAcceptTime=10.04f;
};
// SCOPES
// METHODS
void Check(bool v,const char* msg) { if(!v){std::cerr<<msg<<'\n';std::exit(1);} }
void Near(float a,float b,const char* msg) { Check(std::fabs(a-b)<.00001f,msg); }
int main() {
    APlayerController pc; APlayerState ps; Pawn pawn{&pc,&ps}; AUTWeaponFix w; w.UTOwner=&pawn;
    Check(!w.Is329FireProtocolReady(),"pending remote blocked"); pc.Confirmed=true;
    Check(w.Is329FireProtocolReady(),"confirmed allowed"); pc.Confirmed=false; pc.Local=true;
    Check(w.Is329FireProtocolReady(),"listen host allowed"); pc.Local=false;
    AUTBot bot; pawn.Controller=&bot; Check(w.Is329FireProtocolReady(),"bot allowed");
    pawn.Controller=nullptr; Check(!w.Is329FireProtocolReady(),"unowned not trusted");
    pawn.Controller=&pc; pc.Confirmed=true;
    w.UTOwner=nullptr; w.FireProtocolController.Value=&pc;
    Check(w.Is329FireProtocolReady(),"same controller trade grace");
    pc.Confirmed=false; Check(!w.Is329FireProtocolReady(),"revoked cached controller");
    w.UTOwner=&pawn; pc.Confirmed=true;
    FNCFireAnchor a; a.bEnforce=true; bool valid=false;
    { FFireAnchorScope s(&w,a,10.052f);
      Check(!FindFireAnchor(&w),"shadow does not install timing scope");
      Near(w.GetPredictionTimeWithFudgeMs(20),.020f,"shadow legacy base"); }
    NCFireAnchor::CurrentMode=2;
    FDeferredEquipFireContext rate;
    { FServerRateDispatchScope rs(&w,&rate,10.052f);
    { FFireAnchorScope s(&w,a,10.052f);
      Check(FindFireAnchor(&w)==&s,"enforce scope installed");
      Near(w.GetPredictionTimeWithFudgeMs(20),.072f,"marker + queue exactly once");
      Near(w.GetHitValidationRenderTime(30,valid),.112f,"render shares original epoch");
      Check(valid,"anchor render timing valid");
      pc.RTT=240;
      Near(w.GetPredictionTimeWithFudgeMs(200),.072f,"frozen base survives changed ping/fudge");
      Near(w.GetHitValidationRenderTime(30,valid),.112f,"frozen RTT survives live change");
      AUTWeaponFix other; other.UTOwner=&pawn;
      Near(other.GetPredictionTimeWithFudgeMs(0),.120f,"other weapon isolated");
      FNCFireAnchor nested=a; nested.BaseRewind=.01f;
      { FFireAnchorScope inner(&w,nested,10.06f);
        Near(w.GetPredictionTimeWithFudgeMs(20),.070f,"nested dispatch snapshot"); }
      nested.bValid=false;
      { FServerRateDispatchScope innerRate(&w,nullptr,10.06f);
        FFireAnchorScope inner(&w,nested,10.06f);
        Check(!FindFireAnchor(&w),"invalid nested shot masks parent origin/time");
        Check(!FindServerRateDispatch(&w),"unreserved nested shot masks parent queue");
        Near(w.GetPredictionTimeWithFudgeMs(20),.110f,"nested fallback uses current own baseline");
        Near(w.GetHitValidationRenderTime(30,valid),.150f,"nested fallback render has no borrowed age"); }
      Check(FindFireAnchor(&w)==&s,"nested scope restores prior"); }
    Check(!FindFireAnchor(&w),"scope removed after dispatch");
    Near(w.GetPredictionTimeWithFudgeMs(0),.032f,"legacy queue restored");
    Near(w.GetHitValidationRenderTime(30,valid),.072f,"legacy render queue preserved");
    { FFireAnchorScope expired(&w,a,10.2f); Check(!FindFireAnchor(&w),"expired cannot install scope"); }
    } pc.RTTValid=false;
    Near(w.GetPredictionTimeWithFudgeMs(20),0,"missing RTT remains zero");
    Near(w.GetHitValidationRenderTime(30,valid),0,"missing render RTT remains invalid"); Check(!valid,"invalid flag");
    std::cout<<"329 extracted scope/timing checks passed\n";
}
'''


class FireAnchorIntegrationTests(unittest.TestCase):
    def test_production_scope_and_timing_methods(self):
        source = (PLUGIN / 'Source/Private/UTWeaponFix.cpp').read_text(encoding='utf-8-sig')
        start = source.index('struct FServerRateDispatchScope;')
        end = source.index('static uint32 AllocateDeferredEquipFireGeneration', start)
        scopes = source[start:end]
        methods = '\n'.join(native_function(source, name) for name in (
            'bool AUTWeaponFix::Is329FireProtocolReady',
            'float AUTWeaponFix::GetPredictionTimeWithFudgeMs',
            'float AUTWeaponFix::GetHitValidationRenderTime',
        ))
        compiler, environment, msvc = find_compiler()
        with tempfile.TemporaryDirectory(prefix='ncp-anchor-integration-') as tmp:
            tmp = Path(tmp)
            cpp = tmp / 'anchor.cpp'
            cpp.write_text(ADAPTER.replace('// SCOPES', scopes).replace('// METHODS', methods))
            executable = tmp / ('anchor.exe' if os.name == 'nt' else 'anchor')
            include = str(PLUGIN / 'Source/Public')
            if msvc:
                command = [compiler, '/nologo', '/EHsc', '/W4', '/WX', '/std:c++14', '/I'+include,
                           str(cpp), '/Fe'+str(executable), '/Fo'+str(tmp/'anchor.obj')]
            else:
                command = [compiler, '-std=c++14', '-Wall', '-Wextra', '-Werror', '-I'+include,
                           str(cpp), '-o', str(executable)]
            build = subprocess.run(command, cwd=tmp, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            run = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
            self.assertIn('checks passed', run.stdout)

    def test_transport_and_dispatch_contracts(self):
        source = (PLUGIN / 'Source/Private/UTWeaponFix.cpp').read_text(encoding='utf-8-sig')
        fire = native_function(source, 'void AUTWeaponFix::FireShot')
        self.assertLess(fire.index('anchor_expired_before_dispatch'), fire.index('LiveContext->bShotDispatched = true'))
        self.assertLess(fire.index('FFireAnchorScope AnchorScope'), fire.rindex('Super::FireShot();'))
        self.assertIn('ClientHeadOffset, ClientMoveTime, ClientFireLoc', fire)
        self.assertLess(fire.index('329_protocol_not_confirmed_at_dispatch'), fire.index('LiveContext->bShotDispatched = true'))
        retry = native_function(source, 'void AUTWeaponFix::ResendNextFireEventFixed')
        self.assertIn('Event.ClientMoveTime, Event.ClientFireLoc', retry)
        self.assertNotIn('GetCurrentSynchTime', retry)
        for name in ('Removed', 'Destroyed', 'DetachFromOwner_Implementation', 'BringUp', 'PutDown'):
            self.assertIn('NCFireAnchor::InvalidateWeapon(this)', native_function(source, ('bool' if name=='PutDown' else 'void')+' AUTWeaponFix::'+name))
        trace = native_function(source, 'void AUTWeaponFix::HitScanTrace')
        self.assertGreaterEqual(trace.count('GetHitValidationRenderTime('), 4)
        self.assertIn('AltRewindTime > AnchorDispatch->Anchor->Cap', trace)
        for name in ('ServerStartFire', 'ServerStartFireOffset', 'ResendServerStartFire', 'ResendServerStartFireOffset', 'ServerStopFire', 'ServerStopFireRecent', 'ServerStartFireFixed', 'ServerStopFireFixed'):
            rpc = native_function(source, 'void AUTWeaponFix::'+name+'_Implementation')
            self.assertIn('Is329FireProtocolReady()', rpc)
            self.assertIn('FireModeNum == 1 && Cast<AUTPlusWeap_RocketLauncher>(this)', rpc)


if __name__ == '__main__':
    unittest.main()
