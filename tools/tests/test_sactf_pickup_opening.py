"""Execute the production SACTF opening helper with stock-shaped pickup/timer adapters.

No editor/UBT. The adapter models Reset, StartSleeping, synchronous touch on WakeUp,
timer replacement, weak destruction and world cleanup. Rendered client countdowns
and cooked Blueprint overrides still require Jeremy's build/playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import PLUGIN, find_compiler, native_function
except ImportError:
    from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define TEXT(x) x
using TCHAR = char;
using int32 = int;
struct UClass;
struct UObject {
    bool Alive=true;
    UClass* Class=nullptr;
    std::map<std::string,UObject*> Objects;
    std::map<std::string,float> Floats;
    std::map<std::string,bool> Bools;
    virtual ~UObject() {}
    UClass* GetClass() const { return Class; }
};
struct UClass : UObject {
    std::string Path;
    UClass* Parent;
    UClass(const char* path,UClass* parent=nullptr) : Path(path),Parent(parent) { Class=this; }
    UClass* GetSuperClass() const { return Parent; }
    std::string GetPathName() const { return Path; }
    bool IsChildOf(const UClass* other) const {
        for(auto c=this;c;c=c->Parent) if(c==other) return true;
        return false;
    }
};
template<class T> T* Cast(UObject* o) { return dynamic_cast<T*>(o); }
struct UObjectPropertyBase {
    std::string Name;
    UObject* GetObjectPropertyValue_InContainer(UObject* o) { return o->Objects[Name]; }
};
struct UFloatProperty {
    std::string Name;
    float GetPropertyValue_InContainer(UObject* o) { return o->Floats[Name]; }
    void SetPropertyValue_InContainer(UObject* o,float f) { o->Floats[Name]=f; }
};
struct UBoolProperty {
    std::string Name;
    bool GetPropertyValue_InContainer(UObject* o) { return o->Bools[Name]; }
    void SetPropertyValue_InContainer(UObject* o,bool b) { o->Bools[Name]=b; }
};
template<class T> T* FindField(UClass*,const char* name) {
    static std::map<std::string,T> fields;
    auto& prop=fields[name];prop.Name=name;return &prop;
}
template<class T> struct TWeakObjectPtr {
    T* Ptr=nullptr;
    TWeakObjectPtr() {}
    TWeakObjectPtr(T* p) : Ptr(p) {}
    T* Get() const { return Ptr && Ptr->Alive ? Ptr : nullptr; }
    T* operator->() const { return Get(); }
    bool IsValid() const { return Get()!=nullptr; }
    bool operator==(const TWeakObjectPtr& o) const { return Ptr==o.Ptr; }
};
template<class T> struct TArray : std::vector<T> {
    int Num() const { return int(this->size()); }
    void Add(const T& t) { this->push_back(t); }
    void RemoveAtSwap(int i) { (*this)[i]=this->back();this->pop_back(); }
    template<class F> void RemoveAll(F f) {
        this->erase(std::remove_if(this->begin(),this->end(),f),this->end());
    }
};
struct FTimerHandle { int Id=0; };
struct FTimerDelegate {
    std::function<void()> Fn;
    template<class F,class P> static FTimerDelegate CreateStatic(F f,P p) { return {[f,p](){f(p);}}; }
};
struct FTimerManager {
    struct Timer { double At;FTimerDelegate Callback; };
    double Now=5000.;
    int Next=0;
    std::map<int,Timer> Timers;
    void ClearTimer(FTimerHandle& h) { Timers.erase(h.Id);h.Id=0; }
    void SetTimer(FTimerHandle& h,const FTimerDelegate& f,float delay,bool,float=-1.f) {
        ClearTimer(h);h.Id=++Next;Timers[h.Id]={Now+delay,f};
    }
    float GetTimerRemaining(const FTimerHandle& h) {
        auto t=Timers.find(h.Id);return t==Timers.end() ? -1.f : float(t->second.At-Now);
    }
    void Advance(double dt) {
        const double end=Now+dt;
        while(true) {
            auto best=Timers.end();
            for(auto it=Timers.begin();it!=Timers.end();++it)
                if(it->second.At<=end && (best==Timers.end() || it->second.At<best->second.At)) best=it;
            if(best==Timers.end()) break;
            const Timer timer=best->second;Timers.erase(best);Now=timer.At;timer.Callback.Fn();
        }
        Now=end;
    }
};
struct AUTPickupInventory;
struct UWorld : UObject {
    FTimerManager Timers;
    std::vector<AUTPickupInventory*> Pickups;
    FTimerManager& GetTimerManager() { return Timers; }
};
struct FDelegateHandle {};
struct CleanupDelegate {
    std::function<void(UWorld*,bool,bool)> Fn;
    template<class T> FDelegateHandle AddRaw(T* o,void(T::*f)(UWorld*,bool,bool)) {
        Fn=[o,f](UWorld* w,bool a,bool b){(o->*f)(w,a,b);};return {};
    }
    void Remove(FDelegateHandle) { Fn={}; }
    void Broadcast(UWorld* w) { if(Fn) Fn(w,false,false); }
};
struct FWorldDelegates { static CleanupDelegate OnWorldCleanup; };
CleanupDelegate FWorldDelegates::OnWorldCleanup;
struct AUTGameMode : UObject {
    UWorld* World=nullptr;
    bool Authority=true;
    bool HasAuthority() const { return Authority; }
    UWorld* GetWorld() const { return World; }
};
struct AUTArmor {
    static UClass* StaticClass() { static UClass c("/Script/UnrealTournament.UTArmor");return &c; }
};
struct AUTPickup : UObject {
    UWorld* World;
    FTimerHandle WakeUpTimerHandle;
    bool Active=true,PendingKill=false,TouchOnWake=false;
    int Wakes=0,NetUpdates=0;
    float SleepReplication=0,WakeReplication=0;
    AUTPickup(UWorld& w,UClass& cls,UClass& inventory,float respawn,bool delayed) : World(&w) {
        Class=&cls;Objects["InventoryType"]=&inventory;
        Floats["RespawnTime"]=respawn;Bools["bDelayedSpawn"]=delayed;Bools["bFixedRespawnInterval"]=false;
    }
    bool IsPendingKillPending() const { return PendingKill; }
    void ForceNetUpdate() { ++NetUpdates; }
    void StartSleeping() {
        Active=false;SleepReplication=Floats["RespawnTime"];
        World->Timers.SetTimer(WakeUpTimerHandle,{{[this](){ WakeUpTimer(); }}},Floats["RespawnTime"],false);
    }
    void WakeUpTimer() {
        Active=true;++Wakes;WakeReplication=Floats["RespawnTime"];
        World->Timers.ClearTimer(WakeUpTimerHandle);
        if(TouchOnWake) { TouchOnWake=false;StartSleeping(); }
    }
    void Reset() {
        World->Timers.ClearTimer(WakeUpTimerHandle);
        if(Bools["bDelayedSpawn"]) StartSleeping();
        else if(!Active) WakeUpTimer();
    }
};
struct AUTPickupInventory : AUTPickup {
    using AUTPickup::AUTPickup;
};
template<class T> struct TActorIterator {
    UWorld* World;size_t Index=0;
    TActorIterator(UWorld* w) : World(w) {}
    explicit operator bool() const { return Index<World->Pickups.size(); }
    TActorIterator& operator++() { ++Index;return *this; }
    T* operator*() const { return World->Pickups[Index]; }
};
namespace NCSACTFPickupOpening {
    void Prepare(AUTGameMode*);
    void Finish(AUTGameMode*);
    void Cancel(UWorld*);
}
'''


CASES = r'''
void Check(bool b,const char* message) { if(!b) { std::cerr<<message<<"\n";std::exit(1); } }
bool Near(float a,float b) { return std::abs(a-b)<.001f; }
const char* const ModePath="/Game/Blueprints/Netcode/SACTF+.SACTF+_C";
const char* const AmpPath="/Game/Blueprints/UT+/UT+/UTPlusNew/UT+SAmp.UT+SAmp_C";
struct Fixture {
    UWorld World;
    UClass Mode{ModePath}, PickupClass{"/Script/UnrealTournament.UTPickupInventory"},
        AmpType{AmpPath}, ArmorType{"/Game/Armor",AUTArmor::StaticClass()}, OtherAmp{"/Game/UDamage"};
    AUTGameMode Game;
    AUTPickupInventory Amp{World,PickupClass,AmpType,120.f,true};
    AUTPickupInventory Armor{World,PickupClass,ArmorType,27.5f,true};
    AUTPickupInventory Belt{World,PickupClass,ArmorType,55.f,true};
    AUTPickupInventory Other{World,PickupClass,OtherAmp,90.f,true};
    Fixture() { Game.World=&World;Game.Class=&Mode;World.Pickups={&Amp,&Armor,&Belt,&Other}; }
    ~Fixture() { FWorldDelegates::OnWorldCleanup.Broadcast(&World); }
    void ResetAll() { for(auto p:World.Pickups) p->Reset(); }
    void Start() { NCSACTFPickupOpening::Prepare(&Game);ResetAll();NCSACTFPickupOpening::Finish(&Game); }
};
int main() {
    {
        Fixture f;f.Amp.StartSleeping();f.World.Timers.Advance(87.);
        f.Armor.Active=f.Belt.Active=false;
        f.Start();
        Check(!f.Amp.Active && Near(f.World.Timers.GetTimerRemaining(f.Amp.WakeUpTimerHandle),30.f),
            "opening amp must wait30 live seconds regardless of map/warmup age");
        Check(f.Amp.SleepReplication==30.f,"stock sleeping state must replicate opening countdown30");
        Check(f.Armor.Active && f.Belt.Active,"armour and belt must be ready at live reset");
        f.Armor.StartSleeping();f.Belt.StartSleeping();
        Check(f.Armor.SleepReplication==27.5f && f.Belt.SleepReplication==55.f,"armour normal intervals changed");
        Check(f.Other.SleepReplication==90.f,"unrelated powerup changed");
        f.World.Timers.Advance(29.999);
        Check(!f.Amp.Active,"amp appeared early");
        f.Amp.TouchOnWake=true;f.World.Timers.Advance(.001);
        Check(f.Amp.Wakes==1 && !f.Amp.Active && f.Amp.SleepReplication==120.f && f.Amp.WakeReplication==120.f,
            "touching opening amp must immediately schedule120, never30");
        Check(Near(f.World.Timers.GetTimerRemaining(f.Amp.WakeUpTimerHandle),120.f),"second amp deadline incorrect");
        f.World.Timers.Advance(120.);
        Check(f.Amp.Active && f.Amp.Wakes==2 && f.Amp.Floats["RespawnTime"]==120.f,"ordinary amp recurrence changed");
        Check(OpeningState().Amps.Num()==0,"completed opening leaked saved state");
    }
    for(const char* mode:{"/Game/Blueprints/Netcode/NCPlusCTF.NCPlusCTF_C","/Game/Blueprints/Netcode/InstagibCTF.InstagibCTF_C"}) {
        Fixture f;f.Mode.Path=mode;f.Start();
        Check(f.Amp.SleepReplication==120.f && !f.Armor.Active && !f.Belt.Active,
            "NWCTF/iCTF acquired SACTF item timing");
    }
    {
        Fixture f;f.Mode.Path="/Game/OtherMode";
        UClass mutClass("/Game/Blueprints/Netcode/MutSaCTF.MutSaCTF_C"), child("/Game/Child",&mutClass);
        UObject mut;mut.Class=&child;f.Game.Objects["BaseMutator"]=&mut;f.Start();
        Check(f.Amp.SleepReplication==30.f,"exact SACTF mutator ancestry was not recognized");
    }
    {
        Fixture f;f.Game.Authority=false;f.Start();
        Check(f.Amp.SleepReplication==120.f,"client applied authority-only opening policy");
    }
    {
        Fixture f;f.Start();f.World.Timers.Advance(10.);
        NCSACTFPickupOpening::Cancel(&f.World); // BEFORE stock halftime reset.
        Check(f.Amp.Floats["RespawnTime"]==120.f && f.Amp.Bools["bDelayedSpawn"],"early intermission lost original settings");
        f.ResetAll();NCSACTFPickupOpening::Finish(&f.Game);
        Check(f.Amp.SleepReplication==120.f && Near(f.World.Timers.GetTimerRemaining(f.Amp.WakeUpTimerHandle),120.f),
            "later-half stock reset incorrectly retained30");
        f.World.Timers.Advance(20.);Check(f.Amp.Wakes==0,"stale first-half callback woke later-half amp");
        f.World.Timers.Advance(100.);Check(f.Amp.Wakes==1,"later-half amp did not use120");
    }
    {
        Fixture f;f.Start();f.World.Timers.Advance(10.);f.Start();
        Check(OpeningState().Amps.Num()==1,"repeated opening preparation duplicated saved state");
        f.World.Timers.Advance(30.);
        Check(f.Amp.Active && f.Amp.Floats["RespawnTime"]==120.f,"repeated start captured temporary30 as original");
    }
    {
        Fixture f;NCSACTFPickupOpening::Prepare(&f.Game);f.Amp.Alive=false;
        f.ResetAll();NCSACTFPickupOpening::Finish(&f.Game);
        Check(OpeningState().Amps.Num()==0,"destroyed pickup retained pending entry");
    }
    {
        Fixture f;f.Start();f.Amp.Alive=false;f.World.Timers.Advance(30.);
        Check(f.Amp.Wakes==0 && OpeningState().Amps.Num()==0,"opening callback used a destroyed pickup");
    }
    {
        Fixture f;f.Start();FWorldDelegates::OnWorldCleanup.Broadcast(&f.World);
        Check(OpeningState().Amps.Num()==0,"world cleanup retained pending opening state");
    }
    {
        Fixture a,b;a.Start();b.Start();NCSACTFPickupOpening::Cancel(&a.World);
        Check(a.Amp.Floats["RespawnTime"]==120.f && b.Amp.Floats["RespawnTime"]==30.f,
            "cancel affected another world or shared class defaults");
        b.World.Timers.Advance(30.);Check(b.Amp.Active,"other world lost opening callback");
    }
    {
        Fixture f;f.Amp.Bools["bFixedRespawnInterval"]=true;f.Start();
        Check(f.Amp.SleepReplication==120.f,"unexpected fixed-interval amp was retuned");
    }
    std::cout<<"SACTF opening pickup scenarios passed\n";
}
'''


class SACTFPickupOpeningTests(unittest.TestCase):
    def test_production_lifecycle(self):
        compiler, env, msvc = find_compiler()
        source = (PLUGIN / "Source/Private/NCSACTFPickupOpening.cpp").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        with tempfile.TemporaryDirectory(prefix="sactf-pickup-") as temp:
            path = Path(temp)
            cpp, exe = path / "test.cpp", path / ("test.exe" if os.name == "nt" else "test")
            cpp.write_text(ADAPTER + source + CASES)
            command = ([compiler, "/nologo", "/EHsc", "/std:c++14", str(cpp), f"/Fe:{exe}"] if msvc else
                       [compiler, "-std=c++14", "-Wall", "-Wextra", str(cpp), "-o", str(exe)])
            result = subprocess.run(command, cwd=path, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=path, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_first_live_reset_is_bracketed(self):
        source = (PLUGIN / "Source/Private/NCPlusCTFGameMode.cpp").read_text()
        start = native_function(source, "void ANCPlusCTFGameMode::HandleMatchHasStarted()")
        prepare = "if (bFirstLiveStart) { NCSACTFPickupOpening::Prepare(this); }"
        finish = "if (bFirstLiveStart) { NCSACTFPickupOpening::Finish(this); }"
        self.assertLess(start.index(prepare), start.index("Super::HandleMatchHasStarted();"))
        self.assertLess(start.index("Super::HandleMatchHasStarted();"), start.index(finish))
        for name in ("HandleMatchIntermission", "HandleExitingIntermission"):
            body = native_function(source, f"void ANCPlusCTFGameMode::{name}()")
            self.assertLess(body.index("NCSACTFPickupOpening::Cancel(GetWorld());"), body.index(f"Super::{name}();"))


if __name__ == "__main__":
    unittest.main()
