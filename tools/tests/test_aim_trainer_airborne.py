"""Execute airborne scheduling, ballistic launch, hazard and rocket gates.

The actor adapter supplies state at the engine boundary. Production game
methods and geometry/timing policies are compiled unchanged; actual rendered
animations, collision sweeps and replicated movement still need a playtest.
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
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using int32 = int;
constexpr int ROLE_Authority = 3;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Abs(float value) { return std::fabs(value); }
};
struct FVector {
    float X, Y, Z;
    FVector(float x=0, float y=0, float z=0) : X(x), Y(y), Z(z) {}
    FVector operator+(const FVector& b) const { return FVector(X+b.X,Y+b.Y,Z+b.Z); }
    FVector operator-(const FVector& b) const { return FVector(X-b.X,Y-b.Y,Z-b.Z); }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
template<class T> struct TArray : std::vector<T> {
    int Num() const { return int(this->size()); }
    bool IsValidIndex(int index) const { return index>=0 && index<Num(); }
    void Add(const T& value) { this->push_back(value); }
};
struct UClass { int Kind; template<class T> const T* GetDefaultObject() const; };
UClass TeamClass{0}, IGClass{1}, RocketClass{2}, GrenadeClass{3}, SeekingClass{4}, SpiralClass{5}, OtherClass{6};
struct AActor { virtual ~AActor()=default; };
struct ANCAimTrainerPlayerController : AActor {
    AActor* Pawn=nullptr;
    AActor* GetPawn() const { return Pawn; }
};
struct AUTWeapon : AActor {};
struct FPlusRocketFireMode { UClass* ProjClass=nullptr; };
struct AUTPlusWeap_RocketLauncher : AUTWeapon {
    UClass* SeekingRocketClass=nullptr;
    UClass* SpiralRocketClass=nullptr;
    TArray<FPlusRocketFireMode> RocketFireModes;
};
struct AUTProjectile : AActor {
    int Role=ROLE_Authority, MyDamageType=42;
    float CreationTime=10.f;
    ANCAimTrainerPlayerController* InstigatorController=nullptr;
    AActor* Instigator=nullptr;
    UClass* Class=&RocketClass;
    AActor* GetInstigator() const { return Instigator; }
    UClass* GetClass() const { return Class; }
};
template<class T,class U> T* Cast(U* actor) { return dynamic_cast<T*>(actor); }
struct FDamageEvent { int DamageTypeClass=42; };
struct ANCAimTrainerTarget : AActor {
    struct Capsule {
        float HalfHeight=108.f;
        float GetScaledCapsuleHalfHeight() const { return HalfHeight; }
    } Shape;
    struct Movement {
        float GravityZ=-980.f;
        bool Grounded=false;
        float GetGravityZ() const { return GravityZ; }
        bool IsMovingOnGround() const { return Grounded; }
    } Move;
    bool Visible=false, HasCapsule=true, LaunchAllowed=true;
    int Activations=0, Launches=0, Hides=0;
    float HeadScale=1.f, Appearance=0.f, NativeGravityZ=-980.f, FlightRate=1.f;
    const float* WorldNow=nullptr;
    UClass* Class=&TeamClass;
    FVector Position, Velocity;
    const Capsule* GetCapsuleComponent() const { return HasCapsule?&Shape:nullptr; }
    Movement* GetCharacterMovement() { return &Move; }
    const Movement* GetCharacterMovement() const { return &Move; }
    FVector GetActorLocation() const { return Position; }
    UClass* GetClass() const { return Class; }
    bool IsAvailable() const { return Visible; }
    float GetAppearanceTime() const { return Appearance; }
    bool LaunchAirborneTarget(const FVector& velocity) {
        if(!LaunchAllowed || !Visible) return false;
        Velocity=velocity; Move.Grounded=false; ++Launches; return true;
    }
    void ActivateAirborneTarget(const FVector& position,const FVector& velocity,float rate=1.f) {
        Position=position; Visible=true; Appearance=WorldNow?*WorldNow:0.f;
        FlightRate=rate; Move.GravityZ=NativeGravityZ*rate*rate;
        ++Activations; LaunchAirborneTarget(FVector(velocity.X,velocity.Y,velocity.Z*rate));
    }
    void SetTrainerHeadshotScale(float scale) { HeadScale=scale; }
    void HideTarget() { Visible=false; ++Hides; }
};
template<class T> const T* UClass::GetDefaultObject() const {
    static ANCAimTrainerTarget team, instagib;
    instagib.Shape.HalfHeight=103.f;
    return static_cast<const T*>(Kind==1?&instagib:&team);
}
struct ANCAimTrainerGame {
    struct { int TargetsExpired=0, Phase=2, Scenario=9; } Progress;
    struct World { float Now=10.f; float GetTimeSeconds() const { return Now; } } TheWorld;
    struct {
        float Roll=.5f;
        int SlotChoice=0;
        float FRand() const { return Roll; }
        float FRandRange(float low,float high) const { return low+(high-low)*Roll; }
        int RandRange(int low,int high) const { return low+std::min(SlotChoice,high-low); }
    } Schedule;
    struct Terminal { int Slot, Appearance; bool Hit; };
    std::vector<Terminal> Terminals;
    ANCAimTrainerPlayerController* Trainee=nullptr;
    AUTWeapon* RunWeapon=nullptr;
    TArray<ANCAimTrainerTarget*> Targets;
    FVector ArenaOrigin=FVector(0.f,0.f,50000.f);
    float PhaseStartedAt=10.f, NextPopupTime=10.f;
    float TargetExpiry[6]={}, NextTargetTime[6]={};
    int LocalAppearances[6]={}, ScoreUpdates=0;
    const World* GetWorld() const { return &TheWorld; }
    void UpdateShotCount() { ++ScoreUpdates; }
    void RecordLocalTarget(int slot,bool hit) { Terminals.push_back({slot,LocalAppearances[slot],hit}); }
    bool IsCurrentRocketDamage(const ANCAimTrainerTarget*,const FDamageEvent&,AActor*) const;
    bool IsAtAirborneHazard(const ANCAimTrainerTarget*) const;
    bool LaunchAirborneJumper();
    void ActivateAirborneSlot(int32,float);
    void UpdateAirborneTargets(float);
};
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    AActor Pawn;
    AUTPlusWeap_RocketLauncher Launcher;
    AUTProjectile Projectile;
    FDamageEvent Event;
    ANCAimTrainerTarget Targets[6];
    Fixture() {
        Game.Trainee=&Player; Player.Pawn=&Pawn; Game.RunWeapon=&Launcher;
        Launcher.RocketFireModes.Add({&RocketClass}); Launcher.RocketFireModes.Add({&GrenadeClass});
        Launcher.SeekingRocketClass=&SeekingClass; Launcher.SpiralRocketClass=&SpiralClass;
        Projectile.InstigatorController=&Player; Projectile.Instigator=&Pawn;
        for(int i=0;i<6;++i) {
            Targets[i].WorldNow=&Game.TheWorld.Now;
            Targets[i].Appearance=Game.PhaseStartedAt;
            Game.Targets.Add(&Targets[i]);
        }
    }
    void At(float now) { Game.TheWorld.Now=now; Game.UpdateAirborneTargets(now); }
    bool RocketOkay() { return Game.IsCurrentRocketDamage(&Targets[0],Event,&Projectile); }
    int VisibleFalls() const {
        int count=0; for(int i=1;i<6;++i) count+=int(Targets[i].Visible); return count;
    }
};
void Require(bool value,const char* why) { if(!value) { std::cerr<<why<<'\n'; std::exit(1); } }
bool Near(float a,float b,float tolerance=.025f) { return std::fabs(a-b)<=tolerance; }
float FallSeconds(const ANCAimTrainerTarget& target,const FVector& origin,float hazard) {
    const float height=target.Position.Z-origin.Z-target.Shape.HalfHeight-hazard;
    const float gravity=-target.Move.GravityZ, vz=target.Velocity.Z;
    return (vz+std::sqrt(vz*vz+2.f*gravity*height))/gravity;
}
FVector BallisticPosition(const ANCAimTrainerTarget& target,const FVector& origin,float time) {
    const FVector start=target.Position-origin;
    return FVector(start.X+target.Velocity.X*time,start.Y+target.Velocity.Y*time,
        start.Z+target.Velocity.Z*time+.5f*target.Move.GravityZ*time*time);
}
void RequireHalfDistance(const FVector& original,const FVector& compact,float rearOffset=0.f,float rearRaise=0.f) {
    // Frozen pre-change rocket viewpoint. Test the three coordinates, not only
    // the floor-plane distance: fall height is part of the requested reduction.
    const FVector view(-800.f,0.f,511.f);
    // Rear falls move nearer and start higher. Undo those intentional spatial
    // translations before checking the otherwise unchanged native launch arc.
    const FVector oldDelta=original-view, newDelta=compact+FVector(rearOffset,0.f,-rearRaise)-view;
    Require(Near(newDelta.X,.5f*oldDelta.X) && Near(newDelta.Y,.5f*oldDelta.Y)
            && Near(newDelta.Z,.5f*oldDelta.Z),"compact rocket path is not half the original 3D displacement");
    const float oldDistance=std::sqrt(oldDelta.X*oldDelta.X+oldDelta.Y*oldDelta.Y+oldDelta.Z*oldDelta.Z);
    const float newDistance=std::sqrt(newDelta.X*newDelta.X+newDelta.Y*newDelta.Y+newDelta.Z*newDelta.Z);
    Require(Near(newDistance,.5f*oldDistance),"compact rocket target is not 50 percent as far from the firing view");
}
'''

CASES = r'''
int main(int argc,char** argv) {
    Require(argc==2,"case missing"); const std::string name(argv[1]);
    using namespace NCAimTrainerLayout;
    if(name=="falls") {
        for(int scenario:{7,8,9,10}) for(float roll:{0.f,.2f,.8f,1.f}) {
            Fixture f; f.Game.Progress.Scenario=scenario; f.Game.Schedule.Roll=roll;
            const bool rockets=scenario==10;
            for(int index=1;index<6;++index) {
                f.Targets[index].HeadScale=1.15f;
                f.Game.ActivateAirborneSlot(index,10.f);
                const FVector p=f.Targets[index].Position-f.Game.ArenaOrigin;
                const bool sideWall=NCAimTrainerAirbornePolicy::UseSideWallSeat(index-1,roll);
                const FSeat seat=AirborneDropSeat(index-1,sideWall,rockets);
                const float rate=NCAimTrainerAirbornePolicy::FlightRate(scenario);
                Require(Near(p.X,seat.MinX+(seat.MaxX-seat.MinX)*roll)
                        && Near(p.Y,seat.CenterY-seat.SpawnJitterY+2.f*seat.SpawnJitterY*roll)
                        && Near(p.Z,AirborneDropHeight(AirborneDropMinZ+(AirborneDropMaxZ-AirborneDropMinZ)*roll,rockets,sideWall)),
                        "fall position ignored the independently selected seat and random range");
                const float drift=sideWall?(index==1?1.f:-1.f)*(20.f+45.f*roll):-80.f+160.f*roll;
                const float launchScale=AirborneLaunchScale(rockets);
                Require(Near(f.Targets[index].Velocity.Y,drift*launchScale)
                        && Near(f.Targets[index].Velocity.Z,(150.f+150.f*roll)*rate*launchScale),"fall drift lost variation");
                const float apex=p.Z+f.Targets[index].Velocity.Z*f.Targets[index].Velocity.Z
                    /(-2.f*f.Targets[index].Move.GravityZ);
                Require(apex+f.Targets[index].Shape.HalfHeight<2000.f,
                        "fall target's full capsule reaches the ceiling at its native ballistic apex");
                Require(f.Targets[index].Activations==1 && f.Targets[index].Launches==1
                        && f.Game.LocalAppearances[index]==1 && f.Game.TargetExpiry[index]==70.f
                        && f.Targets[index].HeadScale==1.f && !f.Game.IsAtAirborneHazard(&f.Targets[index]),
                        "fall activation failed to reset hitbox, appearance or native flight");
                const float time=FallSeconds(f.Targets[index],f.Game.ArenaOrigin,AirborneHazardHeight(rockets));
                const float finalY=p.Y+f.Targets[index].Velocity.Y*time;
                Require(std::fabs(finalY)+40.f<1800.f && p.X+40.f<3200.f,
                        "native-gravity fall drift carries a full capsule outside the room before lava");
            }
        }
    } else if(name=="sidewall_mix") {
        for(int index=0;index<5;++index) for(float roll:{0.f,.44f,.45f,1.f}) {
            Require(NCAimTrainerAirbornePolicy::UseSideWallSeat(index,roll)
                    == ((index==0 || index==4) && roll<.45f),"side-wall chance leaked to middle seats or lost its bound");
        }
        for(int scenario:{9,10}) {
        Fixture f; f.Game.Progress.Scenario=scenario; f.Game.Schedule.Roll=.2f;
        const bool rockets=scenario==10;
        int left=0,right=0;
        for(int index=1;index<6;++index) {
            f.Game.ActivateAirborneSlot(index,10.f);
            const FVector p=f.Targets[index].Position-f.Game.ArenaOrigin;
            if(std::fabs(p.Y)>(rockets?750.f:1500.f)) {
                p.Y<0.f?++left:++right;
                Require(index==1 || index==5,"middle slot entered a side-wall lane");
                Require(p.X-40.f>AirborneJumpPad(0,rockets).CenterX+AirborneJumpPad(0,rockets).SizeX*.5f
                        && p.X+40.f<AirborneJumpPad(1,rockets).CenterX-AirborneJumpPad(1,rockets).SizeX*.5f,
                        "side-wall capsule falls onto a jump platform instead of goo");
                Require(p.Y*f.Targets[index].Velocity.Y<0.f,"side-wall drop drifted out of the room");
            }
        }
        Require(left==1 && right==1,"full target pool did not limit side-wall drops to one on each side");
        f.Game.Schedule.Roll=.8f;
        f.Game.ActivateAirborneSlot(1,10.f); f.Game.ActivateAirborneSlot(5,10.f);
        const float rearMin=rockets?375.f:1700.f;
        Require(f.Targets[1].Position.X-f.Game.ArenaOrigin.X>=rearMin
                && f.Targets[5].Position.X-f.Game.ArenaOrigin.X>=rearMin,
                "outer slots always use the side-wall lane instead of choosing the rear seats");
        }
    } else if(name=="flight_rates") {
        float baselineTime=0.f;
        for(int scenario:{9,7,8}) {
            Fixture f; f.Game.Progress.Scenario=scenario; f.Game.Schedule.Roll=.8f;
            f.Game.ActivateAirborneSlot(1,10.f);
            const auto& target=f.Targets[1];
            const float rate=scenario==7?.95f*.95f:scenario==8?.93f*.95f:1.f;
            Require(Near(target.FlightRate,rate) && Near(target.Move.GravityZ,-980.f*rate*rate)
                    && Near(target.Velocity.Z,270.f*rate),"fall received the wrong preset flight rate");
            const float z=target.Position.Z-f.Game.ArenaOrigin.Z;
            const float gravity=-target.Move.GravityZ, vz=target.Velocity.Z;
            const float flightTime=FallSeconds(target,f.Game.ArenaOrigin,AirborneHazardZ);
            const float apex=z+vz*vz/(2.f*gravity);
            Require(Near(apex,z+270.f*270.f/(2.f*980.f)),"slower flight changed the fall arc height");
            if(scenario==9) baselineTime=flightTime;
            Require(Near(flightTime,baselineTime/rate),"fall trajectory did not retain its shape at the slower rate");
            if(scenario==7 || scenario==8) Require(flightTime>baselineTime,"IG/sniper fall did not slow down");
            f.Game.ActivateAirborneSlot(0,10.f);
            Require(f.Targets[0].FlightRate==1.f && f.Targets[0].Move.GravityZ==-980.f,
                    "fall preset rate leaked into the diagonal jump-pad target");
        }
        Require(NCAimTrainerAirbornePolicy::FlightRate(10)==1.f,
                "compact rocket trajectory changed native gravity instead of its geometry");
    } else if(name=="rocket_compact_falls") {
        const float timeScale=std::sqrt(.5f);
        for(float gravity:{-980.f,-1960.f}) for(float roll:{0.f,.2f,.44f,.45f,.8f,1.f}) {
            for(int index=1;index<6;++index) {
                Fixture original,compact;
                // Scenario9 retains the old unscaled layout and native gravity.
                original.Game.Progress.Scenario=9; compact.Game.Progress.Scenario=10;
                original.Game.Schedule.Roll=compact.Game.Schedule.Roll=roll;
                for(float& next:compact.Game.NextTargetTime) next=1000.f;
                compact.Game.NextPopupTime=1000.f;
                auto& oldTarget=original.Targets[index]; auto& newTarget=compact.Targets[index];
                const bool sideWall=NCAimTrainerAirbornePolicy::UseSideWallSeat(index-1,roll);
                const float rearOffset=sideWall?0.f:((index-1)%2==0?75.f:200.f);
                oldTarget.NativeGravityZ=newTarget.NativeGravityZ=gravity;
                original.Game.ActivateAirborneSlot(index,10.f); compact.Game.ActivateAirborneSlot(index,10.f);
                Require(oldTarget.Shape.HalfHeight==108.f && newTarget.Shape.HalfHeight==108.f
                        && newTarget.Move.GravityZ==gravity && newTarget.FlightRate==1.f,
                        "rocket compaction changed target size or native gravity");
                Require(Near(newTarget.Velocity.Y,oldTarget.Velocity.Y*timeScale)
                        && Near(newTarget.Velocity.Z,oldTarget.Velocity.Z*timeScale),
                        "rocket fall did not scale all launch components by sqrt(0.5)");
                const float oldTime=FallSeconds(oldTarget,original.Game.ArenaOrigin,20.f);
                const float newTime=FallSeconds(newTarget,compact.Game.ArenaOrigin,211.5f);
                const float baselineHeight=930.5f+200.f*roll;
                const float rearRaise=sideWall?0.f:.6f*(baselineHeight-319.5f);
                Require(Near(newTarget.Position.Z-compact.Game.ArenaOrigin.Z,baselineHeight+rearRaise),
                        "rear rocket spawn did not gain60% clearance above goo or altered a side-wall spawn");
                Require(sideWall?Near(newTime,oldTime*timeScale):newTime>oldTime*timeScale,
                        "raised rear fall did not gain flight time or changed side-wall timing");
                for(int step=0;step<=20;++step) {
                    const float newSampleTime=newTime*float(step)/20.f;
                    RequireHalfDistance(BallisticPosition(oldTarget,original.Game.ArenaOrigin,newSampleTime/timeScale),
                        BallisticPosition(newTarget,compact.Game.ArenaOrigin,newSampleTime),rearOffset,rearRaise);
                }
                const FVector end=BallisticPosition(newTarget,compact.Game.ArenaOrigin,newTime);
                const FVector justAbove=BallisticPosition(newTarget,compact.Game.ArenaOrigin,newTime-.01f);
                if(!sideWall) {
                    newTarget.Position=compact.Game.ArenaOrigin+BallisticPosition(newTarget,compact.Game.ArenaOrigin,oldTime*timeScale);
                    compact.At(10.f+oldTime*timeScale);
                    Require(newTarget.Visible && compact.Game.Progress.TargetsExpired==0,
                            "raised rear fall expired at the previous lower spawn's landing time");
                }
                newTarget.Position=compact.Game.ArenaOrigin+justAbove;
                compact.At(10.f+newTime-.01f);
                Require(newTarget.Visible && compact.Game.Progress.TargetsExpired==0,
                        "rocket drop expired before physical goo contact");
                Require(Near(end.Z-newTarget.Shape.HalfHeight,211.5f),
                        "full-size rocket target did not reach the goo at its native ballistic endpoint");
                newTarget.Position=compact.Game.ArenaOrigin+FVector(end.X,end.Y,211.5f+108.f);
                compact.At(10.f+newTime);
                Require(!newTarget.Visible && compact.Game.Progress.TargetsExpired==1
                        && compact.Game.Terminals.size()==1 && compact.Game.Terminals[0].Appearance==1,
                        "rocket fall did not retire exactly once on actual goo contact");
            }
        }
    } else if(name=="rocket_compact_jumper") {
        const float timeScale=std::sqrt(.5f);
        for(float gravity:{-980.f,-1960.f}) for(int initialPad:{0,1}) {
            Fixture original,compact;
            original.Game.Progress.Scenario=9; compact.Game.Progress.Scenario=10;
            original.Game.Schedule.SlotChoice=compact.Game.Schedule.SlotChoice=initialPad;
            auto& oldTarget=original.Targets[0]; auto& newTarget=compact.Targets[0];
            oldTarget.NativeGravityZ=newTarget.NativeGravityZ=gravity;
            original.Game.ActivateAirborneSlot(0,10.f); compact.Game.ActivateAirborneSlot(0,10.f);
            const FBlock oldPad=AirborneJumpPad(1-initialPad,false), newPad=AirborneJumpPad(1-initialPad,true);
            const float oldTime=(oldPad.CenterY-oldTarget.Position.Y+original.Game.ArenaOrigin.Y)/oldTarget.Velocity.Y;
            const float newTime=(newPad.CenterY-newTarget.Position.Y+compact.Game.ArenaOrigin.Y)/newTarget.Velocity.Y;
            Require(Near(newTime,oldTime*timeScale) && newTarget.Move.GravityZ==gravity
                    && newTarget.Shape.HalfHeight==108.f,"rocket jumper changed gravity/capsule or lost half-size flight time");
            Require(Near(newTarget.Velocity.X,oldTarget.Velocity.X*timeScale)
                    && Near(newTarget.Velocity.Y,oldTarget.Velocity.Y*timeScale)
                    && Near(newTarget.Velocity.Z,oldTarget.Velocity.Z*timeScale),
                    "rocket jumper launch does not follow compact native-gravity arc");
            for(int step=0;step<=20;++step) {
                const float time=oldTime*float(step)/20.f;
                RequireHalfDistance(BallisticPosition(oldTarget,original.Game.ArenaOrigin,time),
                    BallisticPosition(newTarget,compact.Game.ArenaOrigin,time*timeScale));
            }
            const FVector end=BallisticPosition(newTarget,compact.Game.ArenaOrigin,newTime);
            Require(Near(end.X,newPad.CenterX) && Near(end.Y,newPad.CenterY)
                    && Near(end.Z,newPad.Height+108.f+2.f),"compact jumper missed the full-size capsule landing seat");
        }
    } else if(name=="stagger") {
        Fixture f; f.Game.Schedule.SlotChoice=4; f.Game.Schedule.Roll=.8f;
        f.At(10.f);
        Require(f.Targets[0].Visible && f.Targets[5].Visible && f.VisibleFalls()==1,
                "airborne start did not randomly select just one falling slot plus jumper");
        Require(Near(f.Game.NextPopupTime,10.65f),"next fall did not use varied spawn interval");
        f.At(10.1f); Require(f.VisibleFalls()==1,"fall spawned before cadence elapsed");
        f.At(30.f); Require(f.VisibleFalls()==2,"slow frame spawned a catch-up burst");
        f.At(30.f); Require(f.VisibleFalls()==2,"same-tick retry bypassed fall cadence");
        f.Game.Schedule.SlotChoice=0; f.At(31.f);
        Require(f.Targets[1].Visible && f.VisibleFalls()==3,"eligible slot randomization always preferred one station");
    } else if(name=="hazard_reuse") {
        for(int scenario:{9,10}) {
        Fixture f;
        f.Game.Progress.Scenario=scenario;
        for(float& next:f.Game.NextTargetTime) next=1000.f;
        f.Game.ActivateAirborneSlot(2,10.f);
        f.Targets[2].Position.Z=f.Game.ArenaOrigin.Z+AirborneHazardHeight(scenario==10)+f.Targets[2].Shape.HalfHeight;
        f.At(11.f);
        Require(f.Game.Progress.TargetsExpired==1 && f.Targets[2].Hides==1 && !f.Targets[2].Visible
                && f.Game.ScoreUpdates==1 && f.Game.Terminals.size()==1
                && f.Game.Terminals[0].Slot==2 && f.Game.Terminals[0].Appearance==1 && !f.Game.Terminals[0].Hit,
                "hazard did not expire exactly the active appearance");
        f.At(11.f); Require(f.Game.Progress.TargetsExpired==1 && f.Game.Terminals.size()==1,"hidden target expired twice");
        f.At(11.1f); Require(!f.Targets[2].Visible,"fall respawn ignored short retirement interval");
        f.At(11.3f);
        Require(f.Targets[2].Visible && f.Game.LocalAppearances[2]==2 && Near(f.Targets[2].Appearance,11.3f)
                && f.Game.Progress.TargetsExpired==1,"pooled fall did not obtain a fresh appearance");
        Require(!f.Game.IsCurrentRocketDamage(&f.Targets[2],f.Event,&f.Projectile),
                "projectile from before reused target's appearance received credit");
        }
    } else if(name=="landed_fall") {
        Fixture f;
        for(float& next:f.Game.NextTargetTime) next=1000.f;
        f.Game.ActivateAirborneSlot(3,10.f);
        f.Targets[3].Position.Z=f.Game.ArenaOrigin.Z+1000.f; f.Targets[3].Move.Grounded=true;
        f.At(11.f);
        Require(!f.Targets[3].Visible && f.Game.Progress.TargetsExpired==1,
                "fall target survived unexpected contact with a platform above lava");
    } else if(name=="hazard_bounds") {
        for(int scenario:{7,8,9,10}) {
        Fixture f; auto& target=f.Targets[0];
        f.Game.Progress.Scenario=scenario;
        const float hazard=scenario==10?211.5f:20.f;
        target.Position=f.Game.ArenaOrigin+FVector(0,0,hazard+target.Shape.HalfHeight+.5f);
        Require(!f.Game.IsAtAirborneHazard(&target),"target above hazard was expired early");
        target.Position.Z-=.5f; Require(f.Game.IsAtAirborneHazard(&target),"feet exactly on hazard remained hittable");
        target.Position.Z-=1.f; Require(f.Game.IsAtAirborneHazard(&target),"feet below hazard remained hittable");
        target.Position=f.Game.ArenaOrigin+FVector(3201,0,1000);
        Require(f.Game.IsAtAirborneHazard(&target),"target beyond back wall survived");
        target.Position=f.Game.ArenaOrigin+FVector(0,-1801,1000);
        Require(f.Game.IsAtAirborneHazard(&target),"target beyond side wall survived");
        target.Position.Z=std::numeric_limits<float>::quiet_NaN();
        Require(f.Game.IsAtAirborneHazard(&target),"nonfinite vertical position remained hittable");
        target.HasCapsule=false; Require(f.Game.IsAtAirborneHazard(&target),"missing capsule was treated as safe");
        Require(f.Game.IsAtAirborneHazard(nullptr),"missing target was treated as safe");
        }
    } else if(name=="jumper_arc") {
        for(float gravity:{-980.f,-1960.f}) for(int scenario:{7,8,9,10}) for(int initialPad:{0,1}) {
            Fixture f; auto& target=f.Targets[0];
            f.Game.Progress.Scenario=scenario;
            const bool instagib=scenario==7, rockets=scenario==10;
            f.Game.Schedule.SlotChoice=initialPad;
            target.Class=instagib?&IGClass:&TeamClass; target.Shape.HalfHeight=instagib?103.f:108.f;
            target.NativeGravityZ=gravity; target.Move.GravityZ=gravity;
            for(float& next:f.Game.NextTargetTime) next=1000.f;
            f.Game.ActivateAirborneSlot(0,10.f);
            for(int landingPad:{1-initialPad,initialPad}) {
                const FVector start=target.Position-f.Game.ArenaOrigin;
                const FVector velocity=target.Velocity;
                const FBlock pad=AirborneJumpPad(landingPad,rockets);
                Require(landingPad?velocity.X>0.f && velocity.Y>0.f:velocity.X<0.f && velocity.Y<0.f,
                        "jump pad direction did not travel diagonally to opposite platform");
                const float time=(pad.CenterY-start.Y)/velocity.Y;
                const float endZ=pad.Height+target.Shape.HalfHeight+2.f;
                Require(time>0.f && Near(start.X+velocity.X*time,pad.CenterX)
                        && Near(start.Z+velocity.Z*time+.5f*gravity*time*time,endZ),
                        "native-gravity ballistic arc misses destination pad");
                const float apexTime=-velocity.Z/gravity;
                Require(Near(start.Z+velocity.Z*apexTime+.5f*gravity*apexTime*apexTime,AirborneJumpApex(rockets)),
                        "jump arc did not honor native gravity and selected apex");
                const int launches=target.Launches;
                f.At(f.Game.TheWorld.Now+.05f);
                Require(target.Launches==launches,"airborne jumper relaunched before landing");
                target.Position=f.Game.ArenaOrigin+FVector(pad.CenterX,pad.CenterY,endZ);
                target.Move.Grounded=true;
                f.At(f.Game.TheWorld.Now+time);
                Require(target.Launches==launches+1 && !target.Move.Grounded && target.Visible
                        && f.Game.Progress.TargetsExpired==0 && f.Game.LocalAppearances[0]==1,
                        "jumper landing expired/reused target instead of launching back");
            }
        }
    } else if(name=="deadline") {
        for(int phase:{0,1,3}) {
            Fixture f; f.Game.Progress.Phase=phase;
            f.Game.ActivateAirborneSlot(1,10.f); f.At(10.f);
            Require(!f.Targets[0].Visible && !f.Targets[1].Visible && f.VisibleFalls()==0,
                    "airborne targets spawned outside the active run phase");
        }
        Fixture f; f.Game.ActivateAirborneSlot(1,10.f);
        f.Targets[1].Position.Z=f.Game.ArenaOrigin.Z;
        f.Game.ActivateAirborneSlot(2,70.f); f.At(70.f);
        Require(!f.Targets[2].Visible && f.Game.Progress.TargetsExpired==0 && f.Game.Terminals.empty(),
                "run deadline allowed another spawn or scored a post-run expiry");
        Fixture invalid; invalid.Game.ActivateAirborneSlot(-1,10.f);
        invalid.Game.ActivateAirborneSlot(6,10.f);
        invalid.Game.Targets[2]=nullptr; invalid.Game.ActivateAirborneSlot(2,10.f);
        Require(invalid.Game.LocalAppearances[2]==0,"invalid target slot gained an appearance");
    } else if(name=="jumper_failure") {
        Fixture f;
        Require(!f.Game.LaunchAirborneJumper(),"hidden jumper launched");
        f.Game.ActivateAirborneSlot(0,10.f);
        f.Targets[0].Move.GravityZ=0.f;
        Require(!f.Game.LaunchAirborneJumper(),"zero gravity produced a valid jump arc");
        f.Targets[0].Move.GravityZ=-980.f; f.Targets[0].LaunchAllowed=false;
        Require(!f.Game.LaunchAirborneJumper(),"target launch failure was swallowed");
        f.Game.Targets[0]=nullptr; Require(!f.Game.LaunchAirborneJumper(),"missing jumper launched");
        f.Game.Targets.clear(); Require(!f.Game.LaunchAirborneJumper(),"empty target pool launched");
    } else if(name=="rocket_classes") {
        Fixture f;
        for(UClass* allowed:{&RocketClass,&GrenadeClass,&SeekingClass,&SpiralClass}) {
            f.Projectile.Class=allowed; Require(f.RocketOkay(),"configured NCP projectile class was rejected");
        }
        f.Projectile.Class=&OtherClass; Require(!f.RocketOkay(),"unconfigured projectile class scored");
        f.Projectile.Class=nullptr; Require(!f.RocketOkay(),"missing projectile class scored");
        f.Projectile.Class=&RocketClass;
        AUTWeapon stock; f.Game.RunWeapon=&stock; Require(!f.RocketOkay(),"non-NCP launcher supplied rocket credit");
        f.Game.RunWeapon=&f.Launcher;
        Require(!f.Game.IsCurrentRocketDamage(&f.Targets[0],f.Event,&f.Pawn),"nonprojectile causer scored");
        Require(!f.Game.IsCurrentRocketDamage(&f.Targets[0],f.Event,nullptr),"missing causer scored");
        Require(!f.Game.IsCurrentRocketDamage(nullptr,f.Event,&f.Projectile),"missing target scored");
    } else if(name=="rocket_owner_type_role") {
        Fixture f; ANCAimTrainerPlayerController other; AActor otherPawn;
        f.Projectile.Role=2; Require(!f.RocketOkay(),"client fake projectile scored");
        f.Projectile.Role=ROLE_Authority; f.Projectile.InstigatorController=&other;
        Require(!f.RocketOkay(),"another controller's projectile scored");
        f.Projectile.InstigatorController=&f.Player; f.Projectile.Instigator=&otherPawn;
        Require(!f.RocketOkay(),"previous pawn's projectile scored");
        f.Projectile.Instigator=&f.Pawn; f.Projectile.MyDamageType=0;
        Require(!f.RocketOkay(),"missing damage type scored");
        f.Projectile.MyDamageType=41; Require(!f.RocketOkay(),"mismatched damage event scored");
        f.Projectile.MyDamageType=42; Require(f.RocketOkay(),"current authority projectile identity rejected");
    } else if(name=="rocket_age") {
        Fixture f; f.Game.TheWorld.Now=15.f;
        Require(f.RocketOkay(),"projectile exactly five seconds old rejected");
        f.Game.TheWorld.Now=15.01f; Require(!f.RocketOkay(),"projectile outside evidence window scored");
        f.Game.TheWorld.Now=12.f; f.Projectile.CreationTime=9.99f;
        Require(!f.RocketOkay(),"projectile from before run scored");
        f.Projectile.CreationTime=10.f; f.Targets[0].Appearance=10.01f;
        Require(!f.RocketOkay(),"projectile older than appearance scored");
        f.Targets[0].Appearance=10.f; f.Projectile.CreationTime=12.01f;
        Require(!f.RocketOkay(),"future projectile scored");
        f.Projectile.CreationTime=std::numeric_limits<float>::quiet_NaN();
        Require(!f.RocketOkay(),"nonfinite projectile birth scored");
        f.Projectile.CreationTime=10.f; f.Game.TheWorld.Now=std::numeric_limits<float>::infinity();
        Require(!f.RocketOkay(),"nonfinite server clock scored");
    } else Require(false,"unknown case");
}
'''


class AimTrainerAirborneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-airborne-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        headers = [f'#include "{(PLUGIN / "Source/Private" / name).as_posix()}"' for name in (
            "NCAimTrainerLayout.h", "NCAimTrainerAirbornePolicy.h", "NCAimTrainerScenarioPolicy.h")]
        signatures = (
            "bool ANCAimTrainerGame::IsCurrentRocketDamage",
            "bool ANCAimTrainerGame::IsAtAirborneHazard",
            "bool ANCAimTrainerGame::LaunchAirborneJumper",
            "void ANCAimTrainerGame::ActivateAirborneSlot",
            "void ANCAimTrainerGame::UpdateAirborneTargets",
        )
        source = directory / "airborne.cpp"
        source.write_text("\n".join([ADAPTER] + headers + [native_function(game, item) for item in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("airborne.exe" if os.name == "nt" else "airborne")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                    f"/Fe{cls.executable}", f"/Fo{directory / 'airborne.obj'}"] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("falls", "sidewall_mix", "flight_rates", "rocket_compact_falls", "rocket_compact_jumper", "stagger", "hazard_reuse", "landed_fall", "hazard_bounds", "jumper_arc", "jumper_failure", "deadline",
              "rocket_classes", "rocket_owner_type_role", "rocket_age"):
    setattr(AimTrainerAirborneTests, "test_" + _case, lambda self, case=_case: self.run_case(case))


if __name__ == "__main__":
    unittest.main()
