"""Execute the real saCTF respawn policy and stock countdown methods without UBT.

Checks mode isolation, Blueprint/mutator inheritance, warmup -> live -> death,
and server authority. Network delivery and the cooked BP still need a playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import PLUGIN, find_compiler, native_function
    from .test_sactf_pickup_opening import ADAPTER as PICKUP_ADAPTER
except ImportError:
    from test_wipeout_healing import PLUGIN, find_compiler, native_function
    from test_sactf_pickup_opening import ADAPTER as PICKUP_ADAPTER


ADAPTER = PICKUP_ADAPTER.replace('struct UWorld : UObject {',
    'struct AUTGameState;\nstruct UWorld : UObject {\n'
    '    AUTGameState* GameState=nullptr;\n'
    '    template<class T> T* GetGameState() { return static_cast<T*>(GameState); }')
if 'struct FFloatProperty' in ADAPTER:
    ADAPTER += '\nusing UFloatProperty = FFloatProperty;\n#define FindField FindFProperty\n'
ADAPTER += r'''
#define UE_LOG(...)
struct FMath {
    template<class T> static T Max(T a,T b) { return std::max(a,b); }
};
namespace NCPlusReflection {
    template<class T> T* FindCachedProperty(UObject* o,const TCHAR* n) {
        return o ? FindField<T>(o->GetClass(),n) : nullptr;
    }
}
struct APlayerState : UObject {};
struct AUTPlayerState : APlayerState {
    UWorld* World;
    float& RespawnWaitTime=Floats["RespawnWaitTime"];
    float RespawnTime=0.f,ForceRespawnTime=0.f;
    int NetUpdates=0;
    explicit AUTPlayerState(UWorld& w) : World(&w) {}
    UWorld* GetWorld() { return World; }
    void OnRespawnWaitReceived();
    void OnDeathsReceived();
    void ForceNetUpdate() { ++NetUpdates; }
};
struct AUTGameState : UObject {
    std::vector<APlayerState*> PlayerArray;
    float RespawnWaitTime=2.f,ForceRespawnTime=5.f;
    int NetUpdates=0;
    void SetRespawnWaitTime(float f) { RespawnWaitTime=f; }
    float GetRespawnWaitTimeFor(AUTPlayerState*);
    void ForceNetUpdate() { ++NetUpdates; }
};
struct AGameSession { int MaxPlayers=10; };
struct ANCPlusCTFGameMode : AUTGameMode {
    AUTGameState* CTFGameState=nullptr;
    AGameSession* GameSession=nullptr;
    int CTFSmallGameMaxPlayers=2;
    float CTFRespawnWait=1.5f,CTFRespawnWaitSmall=1.f,RespawnWaitTime=2.f;
    bool bIsInstagib=false;
    void ApplyRegulationRespawnWait();
};
'''

CASES = r'''
void Check(bool b,const char* message) { if(!b) { std::cerr<<message<<"\n";std::exit(1); } }
struct Fixture {
    UWorld World;
    UClass SA{"/Game/Blueprints/Netcode/SACTF+.SACTF+_C"};
    UClass CT{"/Game/Blueprints/Netcode/NCPlusCTF.NCPlusCTF_C"};
    UClass IC{"/Game/Blueprints/Netcode/InstagibCTF.InstagibCTF_C"};
    UClass Mut{"/Game/Blueprints/Netcode/MutSaCTF.MutSaCTF_C"};
    UClass DerivedSA{"/Game/CustomSA",&SA},DerivedMut{"/Game/CustomMut",&Mut};
    ANCPlusCTFGameMode Game;
    AUTGameState State;
    AGameSession Session;
    AUTPlayerState Warm{World},Fresh{World};
    APlayerState Foreign;
    UObject OtherMutator,SaMutator;
    Fixture() {
        World.GameState=&State;Game.World=&World;Game.Class=&SA;
        Game.CTFGameState=&State;Game.GameSession=&Session;
        State.PlayerArray={&Warm,&Fresh,&Foreign,nullptr};
        Warm.RespawnWaitTime=2.f;Warm.OnRespawnWaitReceived();
        SaMutator.Class=&DerivedMut;OtherMutator.Class=&CT;
    }
};
int main() {
    for(int capacity:{2,4,10,32}) {
        Fixture f;f.Session.MaxPlayers=capacity;
        f.Game.CTFRespawnWait=4.5f;f.Game.CTFRespawnWaitSmall=3.f;
        Check(f.Warm.RespawnTime==2.f,"fixture must reproduce stock warmup delay");
        f.Game.ApplyRegulationRespawnWait();
        Check(f.Game.RespawnWaitTime==1.f && f.State.RespawnWaitTime==1.f,
            "saCTF must override match-size and configured regulation delay");
        Check(f.Warm.RespawnWaitTime==0.f && f.Warm.RespawnTime==1.f && f.Warm.ForceRespawnTime==6.f,
            "warmup floor and current countdown must use live timing");
        Check(f.Warm.NetUpdates==1 && f.State.NetUpdates==1,"changed timing must request replication");
        Check(f.Fresh.RespawnWaitTime==0.f && f.Fresh.NetUpdates==0,"fresh players must stay unchanged");
        f.Warm.OnDeathsReceived();f.Fresh.OnDeathsReceived();
        Check(f.Warm.RespawnTime==1.f && f.Fresh.RespawnTime==1.f,"next death must allow both players at 1s");
        f.State.SetRespawnWaitTime(2.5f);f.Warm.OnDeathsReceived();
        Check(f.Warm.RespawnTime==2.5f,"overtime must retain its existing ability to raise the delay");
    }
    for(bool instagib:{false,true}) for(int capacity:{2,4,10}) {
        Fixture f;f.Game.Class=instagib ? &f.IC : &f.CT;f.Game.bIsInstagib=instagib;f.Session.MaxPlayers=capacity;
        f.Game.CTFRespawnWaitSmall=.75f;f.Game.CTFRespawnWait=1.75f;
        f.Game.ApplyRegulationRespawnWait();
        const float expected=instagib ? 1.f : (capacity<=2 ? .75f : 1.75f);
        Check(f.Game.RespawnWaitTime==expected && f.State.RespawnWaitTime==expected,
            "iCTF must use 1s while ordinary CTF retains its configured waits");
        Check(f.Warm.RespawnWaitTime==(instagib ? 0.f : 2.f)
            && f.Warm.RespawnTime==(instagib ? 1.f : 2.f) && f.Warm.NetUpdates==(instagib ? 1 : 0),
            "iCTF must clear warmup delays while ordinary CTF leaves them untouched");
        f.Warm.OnDeathsReceived();
        Check(f.Warm.RespawnTime==(instagib ? 1.f : 2.f),"iCTF next death retained warmup floor");
    }
    {
        Fixture f;f.Game.Class=&f.DerivedSA;f.Game.ApplyRegulationRespawnWait();
        Check(f.State.RespawnWaitTime==1.f && f.Warm.RespawnWaitTime==0.f,"derived saCTF BP not recognized");
    }
    {
        Fixture f;f.Game.Class=&f.CT;
        f.Game.Objects["BaseMutator"]=&f.OtherMutator;f.OtherMutator.Objects["NextMutator"]=&f.SaMutator;
        f.Game.ApplyRegulationRespawnWait();
        Check(f.State.RespawnWaitTime==1.f && f.Warm.RespawnWaitTime==0.f,"derived saCTF mutator in chain not recognized");
    }
    {
        Fixture f;f.Game.Authority=false;f.Game.ApplyRegulationRespawnWait();
        Check(f.State.RespawnWaitTime==2.f && f.Warm.RespawnWaitTime==2.f && f.State.NetUpdates==0,
            "non-authority call changed timing");
    }
    {
        Fixture f;f.Game.GameSession=nullptr;f.Game.ApplyRegulationRespawnWait();
        Check(f.State.RespawnWaitTime==1.f,"saCTF without a session must still use 1s");
    }
    {
        Fixture f;f.Game.Class=&f.CT;f.Game.GameSession=nullptr;f.Game.ApplyRegulationRespawnWait();
        Check(f.State.RespawnWaitTime==1.5f,"other modes' absent-session fallback changed");
    }
    {
        Fixture f;f.Game.CTFGameState=nullptr;f.Game.ApplyRegulationRespawnWait();
        Check(f.Game.RespawnWaitTime==1.f,"absent game state must not crash or skip mode timing");
    }
    Check(!NCSACTFPickupOpening::IsSACTF(nullptr),"null game classified as saCTF");
    std::cout<<"saCTF respawn scenarios passed\n";
}
'''


class SACTFRespawnTests(unittest.TestCase):
    def test_production_policy_and_stock_countdown(self):
        compiler, env, msvc = find_compiler()
        pickup = (PLUGIN / 'Source/Private/NCSACTFPickupOpening.cpp').read_text()
        mode = (PLUGIN / 'Source/Private/NCPlusCTFGameMode.cpp').read_text()
        stock = PLUGIN.parents[1] / 'Source/UnrealTournament/Private'
        ps = (stock / 'UTPlayerState.cpp').read_text()
        gs = (stock / 'UTGameState.cpp').read_text()
        code = '\n'.join([
            native_function(pickup, 'bool HasClassPath('),
            native_function(pickup, 'UObject* ReadObject('),
            native_function(pickup, 'bool NCSACTFPickupOpening::IsSACTF('),
            native_function(gs, 'float AUTGameState::GetRespawnWaitTimeFor('),
            native_function(ps, 'void AUTPlayerState::OnRespawnWaitReceived()'),
            native_function(ps, 'void AUTPlayerState::OnDeathsReceived()'),
            native_function(mode, 'void ANCPlusCTFGameMode::ApplyRegulationRespawnWait()'),
        ])
        with tempfile.TemporaryDirectory(prefix='sactf-respawn-') as temp:
            path = Path(temp)
            cpp, exe = path / 'test.cpp', path / ('test.exe' if os.name == 'nt' else 'test')
            cpp.write_text(ADAPTER + code + CASES)
            cmd = ([compiler, '/nologo', '/EHsc', '/std:c++14', str(cpp), f'/Fe:{exe}'] if msvc else
                   [compiler, '-std=c++14', '-Wall', '-Wextra', str(cpp), '-o', str(exe)])
            result = subprocess.run(cmd, cwd=path, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=path, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_policy_runs_once_after_first_live_reset(self):
        source = (PLUGIN / 'Source/Private/NCPlusCTFGameMode.cpp').read_text()
        start = native_function(source, 'void ANCPlusCTFGameMode::HandleMatchHasStarted()')
        first_live = native_function(start, 'if (HasAuthority() && !RatingSystem.IsValid())')
        call = 'ApplyRegulationRespawnWait();'
        self.assertEqual(source.count(call), 1)
        self.assertIn(call, first_live)
        self.assertLess(start.index('Super::HandleMatchHasStarted();'), start.index(call))
        self.assertLess(first_live.index('RatingSystem = MakeUnique'), first_live.index(call))


if __name__ == '__main__':
    unittest.main()
