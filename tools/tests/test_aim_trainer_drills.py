"""Execute production drill lifecycle, pellet aggregation and shot scoring.

The adapter supplies actors and the native weapon's shot counter; Unreal
collision, animation and packaged network play still require a playtest.
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
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(x) x
using int32=int;
constexpr int ROLE_Authority=3;
constexpr float PI=3.14159265f;
struct FMath {
    static float Min(float a,float b){return std::min(a,b);}
    static float Max(float a,float b){return std::max(a,b);}
    static float Abs(float x){return std::abs(x);}
    static float Cos(float x){return std::cos(x);}
    static float Sin(float x){return std::sin(x);}
    static float Atan2(float y,float x){return std::atan2(y,x);}
    static bool IsFinite(float x){return std::isfinite(x);}
    static int RoundToInt(float x){return int(std::round(x));}
};
struct FVector {
    float X,Y,Z;
    FVector(float x=0,float y=0,float z=0):X(x),Y(y),Z(z){}
    FVector operator+(FVector v) const{return {X+v.X,Y+v.Y,Z+v.Z};}
    FVector operator-(FVector v) const{return {X-v.X,Y-v.Y,Z-v.Z};}
};
struct FRotator { FRotator(float,float,float){} };
struct FString:std::string {
    using std::string::string; using std::string::operator=;
    static FString Printf(const char* text,int a,int b,int c){char out[180];std::snprintf(out,sizeof(out),text,a,b,c);return out;}
};
template<class T> struct TArray:std::vector<T> {
    using std::vector<T>::vector;
    void Add(T x){this->push_back(x);}
    void Empty(){this->clear();}
    int Num()const{return int(this->size());}
    bool IsValidIndex(int i)const{return i>=0&&i<Num();}
    bool Contains(T x)const{return std::find(this->begin(),this->end(),x)!=this->end();}
};
struct AActor { virtual ~AActor()=default; };
struct AUTPlayerState:AActor{};
struct AController:AActor{};
struct ANCAimTrainerPlayerController:AController {
    AUTPlayerState* PlayerState=nullptr;
    AActor Pawn; int Notifications=0; FString Status;
    AActor* GetPawn(){return &Pawn;}
    void NotifyTrainerHit(float){++Notifications;}
    void SetTrainerOnlineStatus(const FString& s){Status=s;}
};
struct AUTWeapon:AActor {
    float Shots=0;
    float GetWeaponShotsStats(AUTPlayerState*) const{return Shots;}
};
struct AUTPlusFlakCannon:AUTWeapon { TArray<int> ProjClass{10},MultiShotProjClass{11}; };
struct AUTPlusShockRifle:AUTWeapon {
    struct Info { int DamageType=20; };
    TArray<Info> InstantHitInfo=TArray<Info>(1);
    int Mode=0;float Rewind=0;
    int GetCurrentFireMode(){return Mode;}
    float GetHitValidationPredictionTime()const{return Rewind;}
};
struct AUTProjectile:AActor {
    int Role=ROLE_Authority,Type=10,MyDamageType=10;
    AController* InstigatorController=nullptr; AActor* Instigator=nullptr;
    float CreationTime=1.f;
    int GetClass()const{return Type;}
    AActor* GetInstigator()const{return Instigator;}
};
template<class T,class U>T* Cast(U* v){return dynamic_cast<T*>(v);}
struct FDamageEvent{int DamageTypeClass=10;bool IsOfType(int)const{return true;}};
struct FUTPointDamageEvent{static const int ClassID=101;};
struct MovementBase{virtual ~MovementBase()=default;};
struct UUTCharacterMovement:MovementBase {
    float DodgeImpulseHorizontal=1500,DodgeImpulseVertical=500;
    float GetGravityZ()const{return -2154;}
};
float WorldNow=1.f;
struct ANCAimTrainerTarget:AActor {
    bool Visible=false,Crouched=false;float Appearance=0;
    FVector Position; UUTCharacterMovement Movement;
    int Impulses=0,Dodges=0,Reversals=0,Feints=0;
    bool DodgeAllowed=true,ActionReady=true;
    struct Capsule{float GetScaledCapsuleHalfHeight()const{return 108.f;}} Shape;
    struct Class {template<class T>const T* GetDefaultObject()const{static T Target;return &Target;}} Type;
    Class* GetClass(){return &Type;}
    const Capsule* GetCapsuleComponent()const{return &Shape;}
    MovementBase* GetCharacterMovement(){return &Movement;}
    bool IsAvailable()const{return Visible;}
    float GetAppearanceTime()const{return Appearance;}
    FVector GetActorLocation()const{return Position;}
    void ActivateTarget(FVector p,bool){Visible=true;Position=p;Appearance=WorldNow;}
    void HideTarget(){Visible=false;}
    void SetActorRotation(FRotator){}
    void ConfigureDrillMovement(bool,FVector){}
    void ChooseDrillStrafe(float,float){++Feints;}
    bool CanStartTrainerDrillAction()const{return ActionReady;}
    bool SetTrainerCrouched(bool value){Crouched=value;return true;}
    bool TryTrainerDrillDodge(float){++Dodges;return DodgeAllowed;}
    bool TryTrainerDrillWallDodge(){return false;}
    void ReverseStrafe(){++Reversals;}
    void ApplyTrainerShockMomentum(const FDamageEvent&){++Impulses;}
};
struct ANCAimTrainerGame {
    struct {int Phase=2,Scenario=11,Shots=0,Hits=0,TargetsExpired=0,Headshots=0,Score=0;float Accuracy=0;} Progress;
    struct World{float GetTimeSeconds(){return WorldNow;}} TheWorld;
    struct Random{float Roll=.5f; float FRand(){return Roll;} float FRandRange(float a,float b){return a+(b-a)*Roll;} int RandRange(int a,int b){return a+int(float(b-a)*Roll);}} Schedule;
    ANCAimTrainerPlayerController* Trainee=nullptr;
    AUTWeapon* RunWeapon=nullptr;
    TArray<ANCAimTrainerTarget*> Targets;
    FVector ArenaOrigin;
    NCAimTrainerDrillPolicy::FAttempt Drill;
    int FlakAttemptShot=0;
    TArray<AActor*> FlakPellets;
    float NextDrillAction=0,PhaseStartedAt=0,ShotStatBaseline=0;
    bool bDrillDodgeQueued=false;
    float NextWiggleTime[6]={},NextCrouchTime[6]={},CrouchEndTime[6]={},TargetExpiry[6]={},NextTargetTime[6]={};
    int LocalAppearances[6]={},HitsRecorded=0,ExpiresRecorded=0,ProjectilesCleared=0;
    bool bRankedRun=true;std::string UnrankedReason;
    World* GetWorld(){return &TheWorld;}
    void InvalidateLocalRun(){}
    void RecordLocalShotCount(){}
    void RecordLocalTarget(int,bool hit){if(hit)++HitsRecorded;else ++ExpiresRecorded;}
    void ClearTrainerProjectiles(){++ProjectilesCleared;}
    void PublishDrillStatus();void UpdateDrillShots();void ActivateDrillTarget(float);
    void RetireDrillTarget(bool,float);void UpdateDrillTargets(float);void UpdateShotCount();
    float RecordDrillHit(ANCAimTrainerTarget*,float,const FDamageEvent&,AActor*);
};
void Require(bool condition,const char* why){if(!condition){std::cerr<<why;std::exit(1);}}
struct Fixture {
    ANCAimTrainerGame G;ANCAimTrainerTarget T;ANCAimTrainerPlayerController P;AUTPlayerState PS;
    AUTPlusFlakCannon Flak;AUTPlusShockRifle Shock;
    Fixture(bool shock=false){WorldNow=1;P.PlayerState=&PS;G.Trainee=&P;G.Targets.Add(&T);G.RunWeapon=shock?static_cast<AUTWeapon*>(&Shock):&Flak;G.Progress.Scenario=shock?12:11;G.UpdateDrillTargets(WorldNow);}
    AUTProjectile Pellet(){AUTProjectile p;p.Instigator=P.GetPawn();p.InstigatorController=&P;p.CreationTime=WorldNow;return p;}
    float Hit(AActor* causer,float damage=20){FDamageEvent e;e.DamageTypeClass=G.Progress.Scenario==11?10:20;return G.RecordDrillHit(&T,damage,e,causer);}
    void Shot(){++G.RunWeapon->Shots;G.UpdateShotCount();}
};
'''

CASES = r'''
int main(int argc,char** argv){
    Require(argc==2,"case missing");const std::string name(argv[1]);
    if(name=="flak_threshold"){
        Fixture f;f.Shot();AUTProjectile pellets[6];
        for(int i=0;i<5;++i){pellets[i]=f.Pellet();Require(f.Hit(&pellets[i],19.9f)>0,"valid pellet rejected");}
        Require(f.G.Progress.Hits==0&&f.T.Visible,"less than 100 damage killed");
        Require(f.Hit(&pellets[0],100)==0,"same pellet counted twice");
        pellets[5]=f.Pellet();f.Hit(&pellets[5],.5f);
        Require(f.G.Progress.Hits==1&&!f.T.Visible&&f.G.Progress.Score==100&&f.G.HitsRecorded==1,"100 damage did not resolve exactly one lethal volley");
    } else if(name=="flak_attempts"){
        Fixture f;f.Shot();auto old=f.Pellet();f.Hit(&old,50);
        WorldNow=1.36f;f.G.UpdateDrillTargets(WorldNow);
        Require(!f.T.Visible&&f.G.Progress.TargetsExpired==1&&f.G.Progress.Hits==0,"partial volley was not a failed attempt");
        WorldNow=1.55f;f.G.UpdateDrillTargets(WorldNow);
        Require(f.T.Visible&&f.G.LocalAppearances[0]==2&&f.G.ProjectilesCleared==2,"next target did not start clean");
        Require(f.Hit(&old,100)==0,"old pellet damaged new appearance");
        auto early=f.Pellet();Require(f.Hit(&early,100)==0,"prior shot reused on new target");
        WorldNow=2.f;f.Shot();auto fresh=f.Pellet();f.Hit(&fresh,100);
        Require(f.G.Progress.Hits==1&&f.G.Progress.Shots==2,"next volley did not score");
    } else if(name=="flak_identity"){
        for(int guard=0;guard<5;++guard){Fixture f;f.Shot();auto p=f.Pellet();if(guard==0)p.Role=1;if(guard==1)p.InstigatorController=nullptr;if(guard==2)p.Instigator=nullptr;if(guard==3)p.Type=99;if(guard==4)p.MyDamageType=99;Require(f.Hit(&p,100)==0&&f.T.Visible,"foreign/alternate pellet accepted");}
        Fixture f;f.Shot();WorldNow=1.36f;auto late=f.Pellet();Require(f.Hit(&late,100)==0,"late flak pellet scored");
    } else if(name=="shock_streak"){
        Fixture f(true);
        for(int i=0;i<4;++i){WorldNow=1.f+float(i)*.7f;f.Shot();f.Hit(&f.Shock);Require(f.Hit(&f.Shock)==0,"duplicate beam advanced streak");}
        Require(f.G.Drill.Streak==4&&f.T.Visible,"carrier reset before five hits");
        WorldNow+=.7f;f.Shot(); // miss
        WorldNow+=.7f;f.Shot();f.Hit(&f.Shock);Require(f.G.Drill.Streak==1&&f.T.Visible,"miss did not break consecutive hits");
        for(int i=0;i<4;++i){WorldNow+=.7f;f.Shot();f.Hit(&f.Shock);}
        Require(!f.T.Visible&&f.G.Drill.Stops==1&&f.G.Progress.Hits==9&&f.T.Impulses==9,"five-hit reset or native impulse count wrong");
        Require(f.G.Progress.Score==875,"shock score differs from server formula");
    } else if(name=="shock_capture"){
        Fixture f(true);f.Shot();f.Hit(&f.Shock);f.T.Position=FVector(-1400,0,108);f.G.UpdateDrillTargets(WorldNow);f.G.UpdateShotCount();
        Require(!f.T.Visible&&f.G.Progress.TargetsExpired==1&&f.G.Drill.Streak==0&&f.G.Progress.Score==0,"capture did not retire/reset carrier");
        WorldNow+=.2f;f.G.UpdateDrillTargets(WorldNow);Require(f.Hit(&f.Shock)==0,"capture reused preceding shot");
        f.Shock.Mode=1;f.Shot();Require(f.Hit(&f.Shock)==0,"shock alternate fire counted");
    } else if(name=="spawn_geometry"){
        Fixture f;
        for(int i=0;i<=100;++i){f.G.Schedule.Roll=float(i)/100.f;f.G.ActivateDrillTarget(WorldNow);float radius=std::sqrt(f.T.Position.X*f.T.Position.X+f.T.Position.Y*f.T.Position.Y);Require(radius<=320.01f&&radius<=1500.f*500.f/2154.f,"flak spawn farther than half a dodge");}
        Fixture shock(true);Require(shock.T.Position.X==1800.f,"carrier did not start at far end");
    } else if(name=="evasive_sequence"){
        Fixture f(true);f.G.Schedule.Roll=.2f;f.T.ActionReady=false;
        WorldNow=f.G.NextDrillAction;f.G.UpdateDrillTargets(WorldNow);
        Require(!f.T.Crouched&&!f.G.bDrillDodgeQueued&&f.T.Dodges==0,"landing recovery consumed the pending crouch choice");
        f.T.ActionReady=true;WorldNow=f.G.NextDrillAction;f.G.UpdateDrillTargets(WorldNow);
        Require(f.T.Crouched&&f.G.bDrillDodgeQueued&&f.T.Dodges==0,"carrier omitted selected crouch feint");
        WorldNow=f.G.CrouchEndTime[0];f.G.UpdateDrillTargets(WorldNow);
        Require(!f.T.Crouched&&f.T.Dodges==0,"carrier dodged before completing crouch feint");
        WorldNow=f.G.NextDrillAction;f.G.UpdateDrillTargets(WorldNow);
        Require(f.T.Dodges==1&&!f.T.Crouched&&!f.G.bDrillDodgeQueued,"crouch feint did not lead into dodge");
        f.T.DodgeAllowed=false;f.G.Schedule.Roll=.8f;WorldNow=f.G.NextDrillAction;f.G.UpdateDrillTargets(WorldNow);
        Require(f.G.bDrillDodgeQueued&&f.G.NextDrillAction-WorldNow<=.161f,"native cooldown caused a long skipped dodge");
        f.T.DodgeAllowed=true;f.G.Schedule.Roll=.1f;WorldNow=f.G.NextDrillAction;f.G.UpdateDrillTargets(WorldNow);
        Require(!f.G.bDrillDodgeQueued&&!f.T.Crouched&&f.T.Dodges==3,"cooldown retry restarted crouch loop instead of dodging");
        Require(f.T.Feints>1,"carrier did not refresh lateral approach");
        Fixture flak;flak.G.Schedule.Roll=.2f;WorldNow=flak.G.NextDrillAction;
        Require(WorldNow<=1.26f,"flak stood idle too long after spawn");
        flak.G.UpdateDrillTargets(WorldNow);
        Require(flak.T.Dodges==1&&!flak.T.Crouched&&flak.G.NextDrillAction-WorldNow<=.65f,"flak did not prioritize frequent dodges");
    } else return 2;
}
'''


class AimTrainerDrillTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-drills-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        headers = [PLUGIN / p for p in ("Source/Public/NCAimTrainerDrillPolicy.h", "Source/Private/NCAimTrainerScenarioPolicy.h", "Source/Private/NCAimTrainerScoring.h")]
        drills = (PLUGIN / "Source/Private/NCAimTrainerDrills.cpp").read_text()
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text()
        signatures = ["void ANCAimTrainerGame::" + name for name in ("PublishDrillStatus", "UpdateDrillShots", "ActivateDrillTarget", "RetireDrillTarget", "UpdateDrillTargets")]
        source = directory / "drills.cpp"
        source.write_text("\n".join([*(f'#include "{p.as_posix()}"' for p in headers), ADAPTER,
            *(native_function(drills, s) for s in signatures), native_function(drills, "float ANCAimTrainerGame::RecordDrillHit"),
            native_function(game, "void ANCAimTrainerGame::UpdateShotCount"), CASES]))
        cls.executable = directory / ("drills.exe" if os.name == "nt" else "drills")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source), "/Fe:" + str(cls.executable)] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.executable), case], env=self.environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("flak_threshold", "flak_attempts", "flak_identity", "shock_streak", "shock_capture", "spawn_geometry", "evasive_sequence"):
    setattr(AimTrainerDrillTests, "test_" + _case, lambda self, case=_case: self.run_case(case))
