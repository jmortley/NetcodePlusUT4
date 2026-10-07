"""Compile trainer target actions against a small actor/movement adapter.

Native target functions and the real scenario direction policy are exercised.
The adapter records calls to UT Dodge and head-pose lookup; actual animation,
collision, physics and networking still require a packaged playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdlib>
#include <cmath>
#include <limits>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(value) value
using int32 = int;
struct FLinearColor { static const FLinearColor Transparent; };
const FLinearColor FLinearColor::Transparent;
struct UMaterialInstanceDynamic { void SetVectorParameterValue(const char*, FLinearColor) {} };
constexpr int ROLE_Authority = 3;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Clamp(float value,float low,float high) { return value<low?low:value>high?high:value; }
    static float Min(float a,float b) { return a<b?a:b; }
    static float Max(float a,float b) { return a>b?a:b; }
};
enum MovementMode { MOVE_None, MOVE_Walking, MOVE_Falling, MOVE_Flying };
struct FVector {
    float X, Y, Z;
    FVector(float x=0.f, float y=0.f, float z=0.f) : X(x), Y(y), Z(z) {}
    float Size2D() const { return std::sqrt(X*X+Y*Y); }
    FVector operator*(float scale) const { return FVector(X*scale,Y*scale,Z*scale); }
    float operator|(const FVector& other) const { return X*other.X+Y*other.Y+Z*other.Z; }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
FVector operator*(float scale,const FVector& vector) { return vector*scale; }
struct FRotator { float Pitch,Yaw,Roll; FRotator(float p,float y,float r):Pitch(p),Yaw(y),Roll(r){} };
enum class ETeleportType { TeleportPhysics };
struct UCharacterMovementComponent {
    virtual ~UCharacterMovementComponent() = default;
    MovementMode Mode = MOVE_Walking;
    float Speed = 0.f, MaxWalkSpeed = 500.f, MaxWalkSpeedCrouched = 240.f, MaxAcceleration = 6000.f;
    int Stops = 0;
    bool IsMovingOnGround() const { return Mode == MOVE_Walking; }
    void StopMovementImmediately() { Speed = 0.f; ++Stops; }
    void SetMovementMode(MovementMode mode) { Mode = mode; }
    void DisableMovement() { Mode = MOVE_None; }
};
struct ATeamArenaCharacter;
struct AUTCharacter;
struct UUTCharacterMovement : UCharacterMovementComponent {
    ATeamArenaCharacter* Owner = nullptr;
    AUTCharacter* CharacterOwner = nullptr;
    bool bIsDodging = false, DodgeInput = false, bIsDodgeLanding = false, FallingFlags = false;
    bool bWantsToCrouch = false, CrouchAllowed = true, StandAllowed = true;
    int Crouches = 0, Uncrouches = 0;
    float HalfHeight = 108.f, StandingHalfHeight = 108.f, CrouchedHalfHeight = 40.f;
    float DodgeResetTime = 0.f, DodgeLandingTimeAdjust = -.25f, MovementTime = 0.f;
    bool bIsFloorSliding=false,bWasFloorSliding=false,bWantsFloorSlide=false,bPressedSlide=false,DodgeAllowed=true;
    float FloorSlideTapTime=0.f,FloorSlideEndTime=0.f,FloorSlideDuration=.7f,FloorSlideAcceleration=400.f;
    float MaxFloorSlideSpeed=900.f,MaxInitialFloorSlideSpeed=1350.f,FloorSlideSlopeBraking=2.7f,DodgeResetInterval=.35f;
    int TimerResets=0;
    FVector Velocity,Acceleration;
    struct {
        bool Walkable=true;
        struct { FVector ImpactNormal=FVector(0.f,0.f,1.f); } HitResult;
        bool IsWalkableFloor() const { return Walkable; }
    } CurrentFloor;
    void ClearDodgeInput() { DodgeInput = false; bPressedSlide=false; }
    void ClearFloorSlideTap() { bWantsFloorSlide=false; }
    void ResetTimers() { DodgeResetTime=FloorSlideTapTime=FloorSlideEndTime=0.f; ++TimerResets; }
    void ClearFallingStateFlags() { bIsDodging = false; FallingFlags = false; bIsFloorSliding=false; }
    float GetCurrentMovementTime() const { return MovementTime; }
    bool CanDodge() const { return DodgeAllowed&&!bIsFloorSliding&&MovementTime>=DodgeResetTime; }
    void PerformFloorSlide(const FVector&,const FVector&);
    void Crouch(bool);
    void UnCrouch(bool);
};
template<class T,class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct World { float Time = 42.f; float GetTimeSeconds() const { return Time; } };
enum { EME_Slide=1,NAME_NumFloorSlides=2 };
struct AUTPlayerState { virtual ~AUTPlayerState()=default; void ModifyStatsValue(int,float) {} };
struct AUTCharacter {
    std::vector<UMaterialInstanceDynamic*> BodyMIs;
    void SetBodyColorFlash(const void*, bool) {}
    int Role=ROLE_Authority,SlideEvents=0,EyeUpdates=0;
    bool bRepFloorSliding=false,bPressedJump=false,SlideAllowed=true;
    FVector SlideDirection;
    AUTPlayerState* PlayerState=nullptr;
    bool bIsCrouched = false, Dead = false;
    bool IsDead() const { return Dead; }
    bool CanSlide() const { return SlideAllowed&&!bIsCrouched; }
    void MovementEventUpdated(int type,FVector direction) { if(type==EME_Slide) {++SlideEvents;SlideDirection=direction;} }
    void UpdateCrouchedEyeHeight() { ++EyeUpdates; }
    int PoseQueries = 0, HelmetNotifications = 0;
    float LastPosePrediction = -1.f;
    virtual FVector GetHeadLocation(float prediction) {
        ++PoseQueries; LastPosePrediction = prediction; return FVector(100.f - prediction, 2.f, 200.f);
    }
    virtual void NotifyBlockedHeadShot(AUTCharacter*) { ++HelmetNotifications; }
};
struct ATeamArenaCharacter : AUTCharacter {
    int CapsuleHeadQueries = 0, SuperTicks = 0, DodgeCalls = 0, Teleports = 0, NetUpdates = 0, InputCalls = 0;
    bool DodgeAllowed = true, Hidden = false, Collision = true, HasPendingInput = false;
    FVector Position, LastDodgeDirection, LastDodgeCross, LastInput;
    UUTCharacterMovement Move;
    World TheWorld;
    UCharacterMovementComponent* GetCharacterMovement() { Move.Owner=this; Move.CharacterOwner=this; return &Move; }
    UCharacterMovementComponent* GetCharacterMovement() const { return const_cast<UUTCharacterMovement*>(&Move); }
    FVector GetActorLocation() const { return Position; }
    FVector GetVelocity() const { return Move.Velocity; }
    World* GetWorld() { return &TheWorld; }
    FVector GetHeadLocation(float) override { ++CapsuleHeadQueries; return FVector(0,0,188); }
    void Tick(float) { ++SuperTicks; }
    bool Dodge(FVector direction, FVector cross) {
        ++DodgeCalls; LastDodgeDirection=direction; LastDodgeCross=cross;
        return DodgeAllowed&&!bIsCrouched&&Move.CanDodge();
    }
    FVector ConsumeMovementInputVector() { HasPendingInput=false; return FVector(); }
    void SetActorLocationAndRotation(FVector position, FRotator, bool, void*, ETeleportType) {
        ++Teleports; Position=position;
    }
    void SetActorHiddenInGame(bool hidden) { Hidden=hidden; }
    void SetActorEnableCollision(bool enabled) { Collision=enabled; }
    void ForceNetUpdate() { ++NetUpdates; }
    void AddMovementInput(FVector direction, float, bool) { ++InputCalls; LastInput=direction; }
};
// Engine posture calls are observed here; real collision/animation is verified
// separately in a packaged run. Mirror grounded capsule-base preservation so
// the native activation method cannot accidentally accumulate vertical drift.
void UUTCharacterMovement::Crouch(bool) {
    ++Crouches;
    if (!Owner || !CrouchAllowed) return;
    if (!Owner->bIsCrouched && IsMovingOnGround()) Owner->Position.Z -= StandingHalfHeight-CrouchedHalfHeight;
    Owner->bIsCrouched=true; HalfHeight=CrouchedHalfHeight;
}
void UUTCharacterMovement::UnCrouch(bool) {
    ++Uncrouches;
    if (!Owner || !StandAllowed) return;
    if (Owner->bIsCrouched && IsMovingOnGround()) Owner->Position.Z += StandingHalfHeight-CrouchedHalfHeight;
    Owner->bIsCrouched=false; HalfHeight=StandingHalfHeight;
}
struct ANCAimTrainerTarget : ATeamArenaCharacter {
    using Super = ATeamArenaCharacter;
    bool bTrainerVisible=false, bTrainerStrafe=false, bTrainerWiggle=false;
    float StrafeDirection=1.f, StrafeRange=800.f, SpawnProtectionStartTime=0.f, AppearanceTime=0.f;
    float WiggleRange=0.f, PopupLongStrafeEndTime=0.f;
    bool bRecenterWiggleAfterSlide=false;
    FVector StrafeCenter,TrainerSlideDirection;
    struct History { int Count=7; void Reset() { Count=0; } } SavedPositions, SavedCapsulePostures;
    void OnRep_TrainerVisible();
    void ActivateTarget(const FVector&,bool);
    void StartWiggle(float);
    bool StartPopupLongStrafe(float,float,float);
    bool IsTrainerLongStrafing() const { return PopupLongStrafeEndTime>0.f; }
    bool SetTrainerCrouched(bool);
    void HideTarget();
    void ResetTargetMovement();
    void ReverseStrafe();
    bool TryTrainerDodge(float);
    bool TryTrainerSlideForward();
    bool TryTrainerPopupSlide(int32);
    bool TryTrainerTrackingSlide(float);
    bool StartTrainerSlide(const FVector&);
    bool IsTrainerSliding() const;
    void Tick(float);
    FVector GetHeadLocation(float) override;
    void NotifyBlockedHeadShot(AUTCharacter*) override;
};
void Require(bool condition,const char* why) { if(!condition) { std::cerr<<why<<'\n'; std::exit(1); } }
ANCAimTrainerTarget Active() {
    ANCAimTrainerTarget target; target.bTrainerVisible=true; target.bTrainerStrafe=true; return target;
}
'''

CASES = r'''
int main(int argc,char**argv) {
    Require(argc==2,"case required"); const std::string name(argv[1]);
    if(name=="reverse") {
        for(int guard=0;guard<4;++guard) {
            auto target=Active();
            if(guard==0) target.Role=1;
            if(guard==1) target.bTrainerVisible=false;
            if(guard==2) target.bTrainerStrafe=false;
            if(guard==3) target.Move.Mode=MOVE_Falling;
            target.ReverseStrafe();
            Require(target.StrafeDirection==1.f,"reverse escaped authority/visibility/ground guard");
        }
        auto target=Active(); target.StrafeCenter.Y=100.f;
        target.Position.Y=100.f; target.ReverseStrafe();
        Require(target.StrafeDirection==-1.f,"center reversal did not alternate");
        target.Position.Y=-700.f; target.ReverseStrafe();
        Require(target.StrafeDirection==1.f,"left edge did not turn inward");
        target.Position.Y=900.f; target.ReverseStrafe();
        Require(target.StrafeDirection==-1.f,"right edge did not turn inward");
    } else if(name=="dodge") {
        for(int guard=0;guard<5;++guard) {
            auto target=Active();
            if(guard==0) target.Role=1;
            if(guard==1) target.bTrainerVisible=false;
            if(guard==2) target.bTrainerStrafe=false;
            if(guard==3) target.Move.Mode=MOVE_Falling;
            if(guard==4) target.bTrainerWiggle=true;
            Require(!target.TryTrainerDodge(0.f)&&target.DodgeCalls==0,"invalid target invoked native dodge");
        }
        auto target=Active(); target.DodgeAllowed=false;
        Require(!target.TryTrainerDodge(0.f)&&target.StrafeDirection==1.f,"failed native dodge changed direction");
        target.DodgeAllowed=true;
        for(float side : {-1.f,1.f}) {
            target.Position.Y=side*600.f;
            Require(target.TryTrainerDodge(side<0.f?0.f:1.f),"valid native dodge rejected");
            Require(target.StrafeDirection==-side&&target.LastDodgeDirection.Y==-side
                    &&target.LastDodgeDirection.X==0.f&&target.LastDodgeDirection.Z==0.f,
                    "edge dodge did not invoke native inward lateral impulse");
            Require(target.LastDodgeCross.X==1.f&&target.LastDodgeCross.Y==0.f&&target.Teleports==0,
                    "dodge teleported or used wrong cross axis");
        }
    } else if(name=="lifecycle") {
        ANCAimTrainerTarget target;
        target.Move.Speed=500; target.Move.bIsDodging=true;
        target.Move.bIsDodgeLanding=true; target.Move.FallingFlags=true;
        target.Move.DodgeInput=true; target.Move.DodgeResetTime=999; target.HasPendingInput=true;
        const FVector location(900,650,50108);
        target.ActivateTarget(location,true);
        Require(target.bTrainerVisible&&target.bTrainerStrafe&&!target.Hidden&&target.Collision
                &&target.Move.Mode==MOVE_Walking&&target.Position.Y==650.f,"activation did not expose correct target");
        Require(target.AppearanceTime==42.f&&target.SavedPositions.Count==0&&target.SavedCapsulePostures.Count==0,
                "activation retained a previous appearance's history/epoch");
        Require(target.Move.Speed==0&&!target.Move.bIsDodging&&!target.Move.DodgeInput
                &&!target.Move.bIsDodgeLanding&&!target.Move.FallingFlags
                &&target.Move.DodgeResetTime==0&&!target.HasPendingInput,"activation retained dodge/input momentum");
        target.Move.Speed=500; target.Move.bIsDodging=true; target.HasPendingInput=true;
        target.Move.bIsDodgeLanding=true; target.Move.FallingFlags=true;
        target.SavedPositions.Count=4; target.SavedCapsulePostures.Count=4;
        target.HideTarget();
        Require(!target.bTrainerVisible&&!target.bTrainerStrafe&&target.Hidden&&!target.Collision
                &&target.Move.Mode==MOVE_None,"hidden target remained active or collidable");
        Require(target.Move.Speed==0&&!target.Move.bIsDodging&&!target.HasPendingInput
                &&!target.Move.bIsDodgeLanding&&!target.Move.FallingFlags
                &&target.SavedPositions.Count==0&&target.SavedCapsulePostures.Count==0,"hide retained motion/history");
        target.ActivateTarget(location,false);
        Require(target.Move.Mode==MOVE_Flying&&!target.bTrainerStrafe&&target.StrafeDirection==1.f,
                "static popup inherited moving target behavior");
    } else if(name=="client_lifecycle") {
        auto target=Active(); target.Role=1; target.HasPendingInput=true; target.Move.Speed=300;
        target.ActivateTarget(FVector(99,88,77),false); target.HideTarget();
        Require(target.bTrainerVisible&&target.bTrainerStrafe&&target.Teleports==0&&target.NetUpdates==0
                &&target.Move.Speed==300&&target.HasPendingInput,"client changed authoritative lifecycle");
    } else if(name=="airborne_tick") {
        auto target=Active(); target.Move.Mode=MOVE_Falling;
        target.Tick(.016f);
        Require(target.InputCalls==0&&target.SuperTicks==1,"airborne target countersteered a native dodge");
        target.Move.Mode=MOVE_Walking; target.Position.Y=850;
        target.Tick(.016f);
        Require(target.InputCalls==1&&target.LastInput.Y==-1.f&&target.SuperTicks==2,"grounded target stopped strafing");
    } else if(name=="wiggle_guards") {
        for(float width : {0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            auto target=Active(); target.StartWiggle(width);
            Require(!target.bTrainerWiggle&&target.StrafeRange==800.f&&target.Move.MaxWalkSpeed==500.f,
                    "invalid wiggle width mutated target");
        }
        auto target=Active(); target.Role=1; target.StartWiggle(80.f);
        Require(!target.bTrainerWiggle,"client activated wiggle");
        target.Role=ROLE_Authority; target.bTrainerVisible=false; target.StartWiggle(80.f);
        Require(!target.bTrainerWiggle,"hidden target activated wiggle");
    } else if(name=="wiggle_boundaries") {
        ANCAimTrainerTarget target; target.ActivateTarget(FVector(0,300,108),false);
        target.StartWiggle(5.f);
        Require(target.bTrainerWiggle&&target.bTrainerStrafe&&target.StrafeRange==20.f
                &&target.Move.MaxWalkSpeed==500.f&&target.Move.Mode==MOVE_Walking,"wiggle changed profile speed or movement mode");
        target.StartWiggle(1000.f);
        Require(target.StrafeRange==110.f,"wiggle exceeded maximum range");
        target.Position.Y=410.f; target.Tick(.016f);
        Require(target.LastInput.Y==-1.f,"wiggle did not reverse at right edge");
        target.Position.Y=190.f; target.ReverseStrafe();
        Require(target.StrafeDirection==1.f,"wiggle reverse ignored local left edge");
        target.Tick(.016f); Require(target.LastInput.Y==1.f,"wiggle tick escaped local left edge");
        Require(!target.TryTrainerDodge(.8f)&&target.DodgeCalls==0,"headshot wiggle performed a dodge");
        target.HideTarget(); Require(!target.bTrainerWiggle,"hidden target retained wiggle");
        target.ActivateTarget(FVector(0,0,108),true);
        Require(!target.bTrainerWiggle&&target.StrafeRange==800.f&&target.Move.MaxWalkSpeed==500.f,
                "new tracking appearance changed profile speed or retained narrow range");
    } else if(name=="profile_lifecycle") {
        for(bool instagib:{false,true}) {
            ANCAimTrainerTarget target;
            auto& move=target.Move;
            // Distinct sentinels prove lifecycle methods preserve the selected
            // class defaults; profile constructors are checked independently.
            const float speed=instagib?940.f:930.f, crouch=instagib?245.f:235.f;
            const float acceleration=instagib?4900.f:4800.f;
            const float standing=instagib?103.f:108.f, crouched=instagib?38.f:40.f;
            move.MaxWalkSpeed=speed; move.MaxWalkSpeedCrouched=crouch; move.MaxAcceleration=acceleration;
            move.HalfHeight=move.StandingHalfHeight=standing; move.CrouchedHalfHeight=crouched;
            for(bool strafe:{false,true}) {
                target.ActivateTarget(FVector(100,300,standing),strafe);
                target.StartWiggle(60.f);
                Require(target.SetTrainerCrouched(true)&&move.HalfHeight==crouched&&target.Position.Z==crouched,
                        "selected profile did not use its native crouched capsule");
                Require(target.SetTrainerCrouched(false)&&move.HalfHeight==standing&&target.Position.Z==standing,
                        "standing failed to restore selected class height");
                target.HideTarget();
                target.ActivateTarget(FVector(200,400,standing),strafe);
                Require(move.MaxWalkSpeed==speed&&move.MaxWalkSpeedCrouched==crouch&&move.MaxAcceleration==acceleration,
                        "target appearance or wiggle replaced profile movement defaults");
                Require(move.HalfHeight==standing&&target.Position.Z==standing,
                        "new appearance changed selected profile standing height");
            }
        }
    } else if(name=="braking_lane") {
        for(float sign:{-1.f,1.f}) {
            auto target=Active();
            target.Move.MaxWalkSpeed=940.f; target.Move.MaxAcceleration=5000.f;
            target.StartWiggle(80.f); target.StrafeDirection=sign;
            target.Move.Velocity=FVector(0.f,sign*940.f,0.f);
            target.Position=target.StrafeCenter;
            const int teleports=target.Teleports;
            target.Tick(.016f);
            Require(target.LastInput.Y==-sign&&target.Move.MaxWalkSpeed==940.f&&target.Teleports==teleports,
                    "narrow lane failed to brake before edge or replaced native motion with a speed cap/teleport");
            target.bTrainerWiggle=false; target.StrafeRange=800.f; target.StrafeDirection=sign;
            target.Position.Y=target.StrafeCenter.Y+sign*100.f; target.Tick(.016f);
            Require(target.LastInput.Y==sign,"wide lane reversed while stopping room remained");
            target.Position.Y=target.StrafeCenter.Y+sign*710.f; target.Tick(.016f);
            Require(target.LastInput.Y==-sign,"wide lane waited until boundary to brake native speed");
        }
    } else if(name=="crouch_guards") {
        for(bool wiggle:{false,true}) for(int guard=0;guard<6;++guard) {
            auto target=Active(); target.bTrainerWiggle=wiggle;
            if(guard==0) target.Role=1;
            if(guard==1) target.bTrainerVisible=false;
            if(guard==2) target.bTrainerStrafe=false;
            if(guard==3) target.Dead=true;
            if(guard==4) target.Move.Mode=MOVE_Falling;
            if(guard==5) target.Move.bIsFloorSliding=true;
            Require(!target.SetTrainerCrouched(true)&&target.Move.Crouches==0&&!target.Move.bWantsToCrouch,
                    "invalid target entered crouch");
        }
        auto target=Active(); target.bTrainerWiggle=true; target.Move.CrouchAllowed=false;
        Require(!target.SetTrainerCrouched(true)&&!target.Move.bWantsToCrouch,
                "failed crouch left a deferred request outside its scheduled hold");
        target.Role=1; target.bIsCrouched=true; target.Move.bWantsToCrouch=true;
        Require(!target.SetTrainerCrouched(false)&&target.Move.bWantsToCrouch&&target.Move.Uncrouches==0,
                "client uncrouched authoritative target");
    } else if(name=="tracking_crouch") {
        for(float standing:{108.f,103.f}) {
            ANCAimTrainerTarget target;
            target.Move.HalfHeight=target.Move.StandingHalfHeight=standing;
            target.Move.CrouchedHalfHeight=72.f;
            target.Move.MaxWalkSpeed=940.f; target.Move.MaxWalkSpeedCrouched=315.f;
            target.Move.MaxAcceleration=5000.f;
            target.ActivateTarget(FVector(-800.f,0.f,50000.f+standing),true);
            const int teleports=target.Teleports,updates=target.NetUpdates;
            Require(!target.bTrainerWiggle&&target.SetTrainerCrouched(true)
                    &&target.bIsCrouched&&target.Move.bWantsToCrouch
                    &&target.Move.HalfHeight==72.f&&target.Position.Z==50072.f,
                    "full tracking target could not crouch with its actual capsule profile");
            target.SetTrainerCrouched(true); target.ReverseStrafe(); target.Tick(.016f);
            Require(target.Move.Crouches==1&&target.NetUpdates==updates+1&&target.Teleports==teleports
                    &&target.LastInput.Y==-1.f&&target.LastInput.X==0.f,
                    "tracking crouch repeated posture changes, teleported or stopped native A/D input");
            Require(target.Move.MaxWalkSpeed==940.f&&target.Move.MaxWalkSpeedCrouched==315.f
                    &&target.Move.MaxAcceleration==5000.f,
                    "tracking crouch changed the selected movement profile");
            Require(!target.TryTrainerDodge(.5f)&&!target.TryTrainerTrackingSlide(.5f)&&target.SlideEvents==0,
                    "dodge or slide interrupted a crouched tracking target");
            target.Move.StandAllowed=false;
            Require(!target.SetTrainerCrouched(false)&&target.bIsCrouched,
                    "blocked tracking uncrouch falsely reported clearance");
            target.Move.StandAllowed=true;
            Require(target.SetTrainerCrouched(false)&&!target.bIsCrouched&&!target.Move.bWantsToCrouch
                    &&target.Move.HalfHeight==standing&&target.Position.Z==50000.f+standing,
                    "tracking recovery failed to restore the selected standing capsule");
            Require(target.TryTrainerDodge(.5f),"native dodge did not resume after tracking crouch");
            Require(target.SetTrainerCrouched(true),"tracking target could not crouch again");
            target.HideTarget();
            Require(!target.bTrainerVisible&&!target.bIsCrouched&&!target.Move.bWantsToCrouch
                    &&target.Move.HalfHeight==standing&&target.Move.Mode==MOVE_None,
                    "hiding a tracking crouch retained posture or active physics");
        }
    } else if(name=="crouch_posture") {
        ANCAimTrainerTarget target; target.ActivateTarget(FVector(100,300,284),false); target.StartWiggle(60.f);
        const int teleports=target.Teleports, updates=target.NetUpdates;
        target.Move.Speed=150.f;
        Require(target.SetTrainerCrouched(true)&&target.bIsCrouched&&target.Move.bWantsToCrouch
                &&target.Move.HalfHeight==40.f&&target.Position.Z==216.f,"crouch did not use native capsule posture");
        target.SetTrainerCrouched(true); target.Tick(.016f);
        Require(target.Move.Crouches==1&&target.NetUpdates==updates+1&&target.Teleports==teleports,
                "held crouch teleported or repeated native transitions");
        Require(target.Move.MaxWalkSpeed==500.f&&target.Move.Speed==150.f&&target.InputCalls==1,
                "crouch changed standing wiggle speed or stopped normal strafe input");
        Require(target.SetTrainerCrouched(false)&&!target.bIsCrouched&&!target.Move.bWantsToCrouch
                &&target.Move.HalfHeight==108.f&&target.Position.Z==284.f&&target.Teleports==teleports,
                "standing did not restore native base-preserving posture");
    } else if(name=="crouch_reset") {
        ANCAimTrainerTarget target; target.ActivateTarget(FVector(100,300,284),false); target.StartWiggle(60.f);
        target.SetTrainerCrouched(true); target.Move.Mode=MOVE_Falling;
        target.HideTarget();
        Require(!target.bIsCrouched&&!target.Move.bWantsToCrouch&&target.Move.HalfHeight==108.f,
                "hidden airborne target retained crouch");
        target.ActivateTarget(FVector(100,300,428),false); target.StartWiggle(60.f); target.SetTrainerCrouched(true);
        target.Move.StandAllowed=false;
        Require(!target.SetTrainerCrouched(false)&&target.bIsCrouched&&!target.Move.bWantsToCrouch,
                "blocked native standing was falsely reported successful");
        target.Move.StandAllowed=true;
        target.ActivateTarget(FVector(200,-500,108),false);
        Require(!target.bIsCrouched&&!target.Move.bWantsToCrouch&&target.Position.Z==108.f,
                "new seat inherited crouched height or uncrouch shifted its standing anchor");
        target.StartWiggle(60.f); target.SetTrainerCrouched(true); target.Move.StandAllowed=false;
        target.ActivateTarget(FVector(200,-500,108),false);
        Require(!target.bTrainerVisible&&!target.Collision,
                "a blocked standing reset exposed an invalid new appearance");
    } else if(name=="landing_recovery") {
        auto target=Active(); target.Move.bIsDodgeLanding=true; target.Move.DodgeResetTime=10.f;
        target.Move.MovementTime=9.749f; target.Tick(.001f);
        Require(target.Move.bIsDodgeLanding,"landing acceleration retired before stock deadline");
        target.Move.MovementTime=9.75f; target.Tick(.001f);
        Require(!target.Move.bIsDodgeLanding,"controllerless target retained landing slowdown past deadline");
        target.Move.bIsDodgeLanding=true; target.Role=1; target.Move.MovementTime=11.f;
        target.Tick(.016f);
        Require(target.Move.bIsDodgeLanding,"client overwrote authoritative landing state");
    } else if(name=="slide_guards") {
        for(int guard=0;guard<10;++guard) {
            auto target=Active(); target.bTrainerWiggle=true;
            if(guard==0) target.Role=1;
            if(guard==1) target.bTrainerVisible=false;
            if(guard==2) target.bTrainerWiggle=false;
            if(guard==3) target.Dead=true;
            if(guard==4) target.Move.Mode=MOVE_Falling;
            if(guard==5) target.Move.CurrentFloor.Walkable=false;
            if(guard==6) target.SlideAllowed=false;
            if(guard==7) target.Move.DodgeAllowed=false;
            if(guard==8) target.bIsCrouched=true;
            if(guard==9) target.Move.bIsFloorSliding=true;
            Require(!target.TryTrainerSlideForward()&&target.SlideEvents==0&&target.NetUpdates==0&&target.Teleports==0,
                    "ineligible target invoked native floor slide");
        }
    } else if(name=="slide_native") {
        auto target=Active(); target.bTrainerWiggle=true; target.Move.MovementTime=10.f;
        target.HasPendingInput=true; target.Move.bWantsToCrouch=true;
        target.Move.Velocity=FVector(0.f,220.f,0.f); target.Position=FVector(1000.f,850.f,50428.f);
        Require(target.TryTrainerSlideForward()&&target.IsTrainerSliding()&&target.bRepFloorSliding,
                "valid controllerless slide did not start or replicate");
        Require(target.SlideEvents==1&&target.SlideDirection.X==-1.f&&target.SlideDirection.Y==0.f
                &&target.Move.Velocity.X==-900.f&&target.Move.Velocity.Y==0.f
                &&target.Move.Acceleration.X==-400.f&&target.Move.FloorSlideEndTime==10.7f,
                "slide bypassed actual UT impulse, forward direction or duration");
        Require(target.bIsCrouched&&target.Move.Crouches==1&&!target.Move.bWantsToCrouch
                &&target.Position.Z==50360.f&&target.Position.X==1000.f&&target.Teleports==0,
                "native slide capsule failed or forward movement was faked by teleport");
        Require(!target.HasPendingInput&&target.LastInput.X==-1.f&&target.LastInput.Y==0.f,
                "lateral input contaminated initial forward slide");
        const int crouches=target.Move.Crouches,uncrouches=target.Move.Uncrouches;
        const float direction=target.StrafeDirection;
        Require(!target.SetTrainerCrouched(true)&&!target.SetTrainerCrouched(false),
                "scheduled crouch/stand interrupted native slide posture");
        target.ReverseStrafe();
        Require(target.StrafeDirection==direction&&target.Move.Crouches==crouches&&target.Move.Uncrouches==uncrouches,
                "wiggle reversal or crouch changed an active slide");
        Require(!target.TryTrainerSlideForward()&&target.SlideEvents==1,"active slide restarted its lifetime");
        target.Tick(.016f);
        Require(target.LastInput.X==-1.f&&target.LastInput.Y==0.f&&target.SuperTicks==1&&target.Teleports==0,
                "active slide countersteered or bypassed real actor/movement ticking");
    } else if(name=="slide_end") {
        auto target=Active(); target.bTrainerWiggle=true; target.Move.MovementTime=10.f;
        target.Position=FVector(1000.f,860.f,50428.f); target.StrafeCenter=FVector(1000.f,850.f,50428.f);
        Require(target.TryTrainerSlideForward(),"fixture slide failed");
        target.Move.bWasFloorSliding=true; target.Move.bWantsFloorSlide=true;
        target.Position.X=370.f; // Native physics moved the capsule; the trainer must retain this X.
        target.Move.MovementTime=target.Move.FloorSlideEndTime-.001f; target.Tick(.016f);
        Require(target.IsTrainerSliding()&&target.bIsCrouched,"slide retired before its native deadline");
        target.Move.MovementTime=target.Move.FloorSlideEndTime; target.Tick(.016f);
        Require(!target.IsTrainerSliding()&&!target.bRepFloorSliding&&!target.bIsCrouched
                &&!target.Move.bWantsFloorSlide&&!target.Move.bWantsToCrouch,
                "controllerless slide never restored standing replicated posture");
        Require(target.Move.bWasFloorSliding&&target.EyeUpdates==1&&target.Move.Uncrouches==1,
                "ending skipped UT slowdown history or repeated native posture changes");
        Require(target.Position.X==370.f&&target.Position.Z==50428.f&&target.StrafeCenter.Y==850.f
                &&target.LastInput.X==0.f&&target.LastInput.Y==1.f&&target.Teleports==0,
                "slide ending snapped target back or did not resume original lateral lane");
        target.Tick(.016f);
        Require(target.Move.Uncrouches==1&&target.EyeUpdates==1,"ended slide repeated restoration");
        auto client=Active(); client.Role=1; client.Move.bIsFloorSliding=true;
        client.bRepFloorSliding=true; client.Move.FloorSlideEndTime=1.f; client.Move.MovementTime=10.f;
        client.Tick(.016f);
        Require(client.IsTrainerSliding()&&client.bRepFloorSliding&&client.InputCalls==0,
                "simulated client locally retired authoritative slide");
    } else if(name=="slide_reset") {
        for(bool activate:{false,true}) {
            auto target=Active(); target.bTrainerWiggle=true; target.Move.MovementTime=10.f;
            Require(target.TryTrainerSlideForward(),"fixture slide failed");
            target.Move.bWasFloorSliding=true; target.Move.bWantsFloorSlide=true;
            target.Move.bPressedSlide=true; target.bPressedJump=true;
            target.Move.FloorSlideTapTime=10.f;
            if(activate) target.ActivateTarget(FVector(1200.f,900.f,50428.f),false);
            else target.HideTarget();
            Require(!target.IsTrainerSliding()&&!target.bRepFloorSliding&&!target.bIsCrouched
                    &&!target.Move.bWasFloorSliding&&!target.Move.bWantsFloorSlide
                    &&!target.Move.bPressedSlide&&!target.bPressedJump&&!target.Move.bWantsToCrouch,
                    "appearance reset retained slide flags/posture/input");
            Require(target.Move.DodgeResetTime==0.f&&target.Move.FloorSlideTapTime==0.f
                    &&target.Move.FloorSlideEndTime==0.f&&target.Move.TimerResets==1,
                    "appearance reset retained native slide timing or pending tap timer");
            if(activate) Require(target.Position.X==1200.f&&target.Position.Z==50428.f&&target.bTrainerVisible,
                                 "new appearance did not restore its authored standing seat");
            else Require(!target.bTrainerVisible&&!target.Collision&&target.Move.Mode==MOVE_None,
                         "hidden slide remained in physics/collision");
        }
    } else if(name=="tracking_slide_guards") {
        for(int guard=0;guard<12;++guard) {
            auto target=Active(); target.Move.MovementTime=10.f;
            if(guard==0) target.Role=1;
            if(guard==1) target.bTrainerVisible=false;
            if(guard==2) target.bTrainerStrafe=false;
            if(guard==3) target.bTrainerWiggle=true;
            if(guard==4) target.Dead=true;
            if(guard==5) target.Move.Mode=MOVE_Falling;
            if(guard==6) target.Move.CurrentFloor.Walkable=false;
            if(guard==7) target.SlideAllowed=false;
            if(guard==8) target.Move.DodgeAllowed=false;
            if(guard==9) target.bIsCrouched=true;
            if(guard==10) target.Move.bIsFloorSliding=true;
            if(guard==11) target.Move.DodgeResetTime=11.f;
            Require(!target.TryTrainerTrackingSlide(0.f)&&target.SlideEvents==0&&target.NetUpdates==0
                &&target.Teleports==0&&target.StrafeDirection==1.f,
                "tracking slide escaped native posture, cooldown, authority or target scope");
        }
    } else if(name=="tracking_slide_direction") {
        for(float offset:{-1600.f,-800.f,-500.f,-499.f,0.f,499.f,500.f,800.f,1600.f}) {
            for(float roll:{0.f,.49f,.5f,1.f}) {
                auto target=Active(); target.Move.MovementTime=10.f;
                target.StrafeCenter=FVector(-800.f,100.f,50108.f);
                target.Position=FVector(-800.f,100.f+offset,50108.f);
                target.Move.Velocity=FVector(0.f,500.f,0.f);
                target.HasPendingInput=true;
                const float expected=offset>=500.f?-1.f:offset<=-500.f?1.f:roll<.5f?-1.f:1.f;
                Require(target.TryTrainerTrackingSlide(roll),"eligible native tracking slide rejected");
                Require(target.Move.Velocity.X==0.f&&target.Move.Velocity.Y==900.f*expected
                    &&target.SlideDirection.X==0.f&&target.SlideDirection.Y==expected
                    &&target.TrainerSlideDirection.X==0.f&&target.TrainerSlideDirection.Y==expected
                    &&target.StrafeDirection==expected&&target.bRepFloorSliding&&target.bIsCrouched,
                    "tracking slide changed X or bypassed native lateral impulse and posture");
                Require(target.Move.FloorSlideEndTime==10.7f&&target.Move.DodgeResetTime==11.05f
                    &&target.Position.X==-800.f&&target.Teleports==0&&!target.HasPendingInput,
                    "tracking slide changed native timing, teleported or retained pending input");
                target.Position.Y=100.f+expected*900.f;
                target.ReverseStrafe(); target.Tick(.016f);
                Require(target.LastInput.X==0.f&&target.LastInput.Y==expected&&target.StrafeDirection==expected,
                    "scheduled reversal countersteered an in-progress native slide");
            }
        }
    } else if(name=="tracking_slide_end_and_reset") {
        for(float direction:{-1.f,1.f}) {
            auto target=Active(); target.Move.MovementTime=10.f;
            target.Position=FVector(-800.f,0.f,50108.f);
            target.StrafeCenter=target.Position;
            Require(target.TryTrainerTrackingSlide(direction<0.f?0.f:1.f),"tracking slide fixture failed");
            Require(!target.TryTrainerDodge(0.f)&&!target.TryTrainerTrackingSlide(0.f),
                "dodge or second slide interrupted the active slide");
            target.Move.bWasFloorSliding=true; target.Move.MovementTime=target.Move.FloorSlideEndTime;
            target.Position.Y=direction*950.f;
            target.Tick(.016f);
            Require(!target.IsTrainerSliding()&&!target.bRepFloorSliding&&!target.bIsCrouched
                &&target.Move.bWasFloorSliding&&target.TrainerSlideDirection.Size2D()==0.f
                &&target.LastInput.Y==-direction&&target.LastInput.X==0.f,
                "slide exit lost native slowdown or failed to resume inward strafe");
            Require(!target.TryTrainerDodge(.5f)&&!target.TryTrainerTrackingSlide(.5f),
                "native post-slide cooldown was bypassed");
            target.Move.MovementTime=target.Move.DodgeResetTime+.001f;
            auto slideAgain=target;
            Require(target.TryTrainerDodge(.5f),"dodge did not recover after native slide cooldown");
            Require(slideAgain.TryTrainerTrackingSlide(.5f),"tracking slide did not recover after cooldown");
            target=slideAgain;
            target.HideTarget();
            Require(target.TrainerSlideDirection.Size2D()==0.f&&!target.IsTrainerSliding()
                &&!target.bRepFloorSliding&&!target.bIsCrouched,"hidden target retained a lateral slide");
            target.ActivateTarget(FVector(1000.f,850.f,50428.f),false); target.StartWiggle(99.f);
            Require(target.TryTrainerSlideForward()&&target.TrainerSlideDirection.X==-1.f
                &&target.TrainerSlideDirection.Y==0.f&&target.Move.Velocity.X==-900.f,
                "reusing a tracking target changed the instagib forward slide");
        }
    } else if(name=="tracking_slide_policy") {
        for(float roll:{-1.f,0.f,.25f,.5f,1.f,2.f}) {
            const float delay=NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(roll);
            Require(delay>=4.f&&delay<=7.f,"tracking slide delay escaped intended frequency");
        }
        Require(NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(0.f)==4.f
            &&NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(1.f)==7.f
            &&NCAimTrainerScenarioPolicy::TrackingSlideDelaySeconds(.5f)==5.5f,
            "tracking slide policy lost its independent random delay");
    } else if(name=="popup_slide_slots") {
        for(int slot:{0,2,4}) {
            ANCAimTrainerTarget target;
            target.ActivateTarget(FVector(1000.f,-850.f,103.f),false); target.StartWiggle(99.f);
            const auto original=target.StrafeCenter;
            Require(target.TryTrainerPopupSlide(slot),"eligible popup slide was rejected");
            Require(target.SlideDirection.X==(slot==4?0.f:-1.f)
                &&target.SlideDirection.Y==(slot==4?1.f:0.f),"popup slide used wrong safe lane");
            target.Position.X=slot==4?1000.f:370.f;
            target.Position.Y=slot==4?-220.f:-835.f;
            target.Move.MovementTime=target.Move.FloorSlideEndTime; target.Tick(.016f);
            Require(target.StrafeCenter.X==target.Position.X
                &&target.StrafeCenter.Y==(slot==4?target.Position.Y:original.Y)
                &&!target.bRecenterWiggleAfterSlide,"slide endpoint did not retain its intended wiggle center");
            Require(target.Teleports==1,"slide exit teleported to a new center");
        }
        for(int slot:{-1,1,3,5,6}) {
            auto target=Active();target.bTrainerWiggle=true;
            Require(!target.TryTrainerPopupSlide(slot)&&target.SlideEvents==0,"non-slide slot accepted a popup slide");
        }
        auto target=Active();target.bTrainerWiggle=true;
        Require(target.TryTrainerPopupSlide(4),"slide reset fixture failed");
        target.HideTarget();
        Require(!target.bRecenterWiggleAfterSlide,"hidden target retained a deferred slide recenter");
    } else if(name=="popup_long_guards") {
        for(int guard=0;guard<11;++guard) {
            ANCAimTrainerTarget target;
            target.ActivateTarget(FVector(1000.f,-850.f,103.f),false); target.StartWiggle(99.f);
            float width=180.f,hold=.6f;
            if(guard==0)target.Role=1;
            if(guard==1)target.bTrainerVisible=false;
            if(guard==2)target.bTrainerWiggle=false;
            if(guard==3)target.Dead=true;
            if(guard==4)target.bIsCrouched=true;
            if(guard==5)target.Move.bIsFloorSliding=true;
            if(guard==6)target.Move.Mode=MOVE_Falling;
            if(guard==7)width=99.f;
            if(guard==8)width=std::numeric_limits<float>::quiet_NaN();
            if(guard==9)hold=0.f;
            if(guard==10)hold=std::numeric_limits<float>::infinity();
            Require(!target.StartPopupLongStrafe(width,hold,0.f)&&!target.IsTrainerLongStrafing()
                &&target.StrafeRange==99.f,"invalid long strafe mutated target motion");
        }
    } else if(name=="popup_long_motion") {
        for(float side:{-1.f,1.f}) {
            ANCAimTrainerTarget target;
            target.ActivateTarget(FVector(1000.f,-850.f,103.f),false);target.StartWiggle(99.f);
            target.Position.Y+=side*70.f;
            Require(target.StartPopupLongStrafe(180.f,.6f,side<0.f?0.f:1.f),"valid long strafe rejected");
            Require(target.StrafeDirection==-side&&target.StrafeRange==180.f
                &&target.PopupLongStrafeEndTime==42.6f,"long strafe did not sweep across its current lane");
            target.ReverseStrafe();target.Tick(.016f);
            Require(target.LastInput.Y==-side&&target.Teleports==1&&target.Move.MaxWalkSpeed==500.f,
                "short wiggle interrupted long hold or motion was replaced with a speed cap/teleport");
            Require(!target.StartPopupLongStrafe(180.f,.8f,0.f)&&!target.TryTrainerPopupSlide(0)
                &&!target.SetTrainerCrouched(true),"another scheduled action interrupted long native strafe");
            target.Position.Y=target.StrafeCenter.Y-side*170.f;
            target.TheWorld.Time=target.PopupLongStrafeEndTime-.001f;target.Tick(.001f);
            Require(target.IsTrainerLongStrafing(),"long hold ended before deadline");
            target.TheWorld.Time=target.PopupLongStrafeEndTime;target.Tick(.001f);
            Require(!target.IsTrainerLongStrafing()&&target.StrafeRange==99.f&&target.LastInput.Y==side
                &&target.Position.Y==target.StrafeCenter.Y-side*170.f&&target.Teleports==1,
                "long strafe did not return inside original wiggle range with native motion");
            Require(target.StartPopupLongStrafe(999.f,999.f,0.f)&&target.StrafeRange==180.f
                &&target.PopupLongStrafeEndTime<=target.TheWorld.Time+.801f,"long move exceeded safe range or hold");
            target.HideTarget();
            Require(!target.IsTrainerLongStrafing()&&target.WiggleRange==0.f,"hidden appearance retained long hold");
            target.ActivateTarget(FVector(1200.f,-850.f,103.f),false);target.StartWiggle(99.f);
            Require(!target.IsTrainerLongStrafing()&&target.StrafeRange==99.f,"new appearance inherited long strafe");
        }
    } else if(name=="popup_motion_policy") {
        int slides[6]={},longs[6]={};
        for(int slot=0;slot<6;++slot)for(int roll=0;roll<100;++roll) {
            slides[slot]+=NCAimTrainerScenarioPolicy::ShouldPopupSlide(slot,roll/100.f)?1:0;
            longs[slot]+=NCAimTrainerScenarioPolicy::ShouldPopupLongStrafe(slot,roll/100.f)?1:0;
        }
        Require(slides[0]==45&&slides[4]==45&&slides[2]==100&&slides[1]==0&&slides[3]==0&&slides[5]==0,
            "random slide frequency or eligible lanes changed");
        Require(longs[0]==65&&longs[1]+longs[2]+longs[3]+longs[4]+longs[5]==0,
            "long strafe probability leaked outside rear-left target");
        for(float roll:{-1.f,0.f,.5f,1.f,2.f}) {
            const float slide=NCAimTrainerScenarioPolicy::PopupSlideDelaySeconds(roll);
            const float delay=NCAimTrainerScenarioPolicy::PopupLongStrafeDelaySeconds(roll);
            const float hold=NCAimTrainerScenarioPolicy::PopupLongStrafeHoldSeconds(roll);
            Require(delay>slide+.8f&&hold>=.5f&&hold<=.75f,
                "long strafe can coincide with initial slide or is not a perceptible long hold");
        }
    } else if(name=="head_feedback") {
        ANCAimTrainerTarget target; AUTCharacter shooter;
        const FVector head=target.GetHeadLocation(.125f);
        Require(target.PoseQueries==1&&target.CapsuleHeadQueries==0&&target.LastPosePrediction==.125f
                &&head.X==99.875f&&head.Z==200.f,"trainer did not delegate exact epoch to visible head pose");
        target.NotifyBlockedHeadShot(&shooter);
        Require(target.HelmetNotifications==0&&shooter.HelmetNotifications==0,
                "immortal target still announced a helmet block");
    } else Require(false,"unknown case");
}
'''


class AimTrainerTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-targets-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerTarget.cpp").read_text(encoding="utf-8-sig")
        movement = (PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTCharacterMovement.cpp").read_text(encoding="utf-8-sig")
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()
        layout = (PLUGIN / "Source/Private/NCAimTrainerLayout.h").as_posix()
        signatures = (
            "void ANCAimTrainerTarget::OnRep_TrainerVisible",
            "void ANCAimTrainerTarget::ActivateTarget",
            "void ANCAimTrainerTarget::StartWiggle",
            "bool ANCAimTrainerTarget::StartPopupLongStrafe",
            "bool ANCAimTrainerTarget::SetTrainerCrouched",
            "void ANCAimTrainerTarget::HideTarget",
            "void ANCAimTrainerTarget::ResetTargetMovement",
            "void ANCAimTrainerTarget::ReverseStrafe",
            "bool ANCAimTrainerTarget::TryTrainerDodge",
            "bool ANCAimTrainerTarget::TryTrainerSlideForward",
            "bool ANCAimTrainerTarget::TryTrainerPopupSlide",
            "bool ANCAimTrainerTarget::TryTrainerTrackingSlide",
            "bool ANCAimTrainerTarget::StartTrainerSlide",
            "bool ANCAimTrainerTarget::IsTrainerSliding",
            "void ANCAimTrainerTarget::Tick",
            "FVector ANCAimTrainerTarget::GetHeadLocation",
            "void ANCAimTrainerTarget::NotifyBlockedHeadShot",
        )
        source = directory / "trainer_targets.cpp"
        source.write_text("\n".join([ADAPTER, f'#include "{policy}"', f'#include "{layout}"']
            + [native_function(movement,"void UUTCharacterMovement::PerformFloorSlide")]
            + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_targets.exe" if os.name == "nt" else "trainer_targets")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_targets.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Target adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reversals_require_authority_visible_grounded_strafe_target(self): self.run_case("reverse")
    def test_dodge_uses_native_action_and_inward_direction_policy(self): self.run_case("dodge")
    def test_appearance_lifecycle_clears_motion_and_prior_history(self): self.run_case("lifecycle")
    def test_client_cannot_change_target_lifecycle(self): self.run_case("client_lifecycle")
    def test_airborne_dodges_are_not_countersteered_by_tracking_input(self): self.run_case("airborne_tick")
    def test_controllerless_landing_acceleration_expires_at_stock_deadline(self): self.run_case("landing_recovery")
    def test_wiggle_rejects_invalid_width_hidden_target_and_client_requests(self): self.run_case("wiggle_guards")
    def test_wiggle_boundaries_speed_and_no_dodge_reset_on_next_appearance(self): self.run_case("wiggle_boundaries")
    def test_target_lifecycle_preserves_selected_movement_and_capsule_profile(self): self.run_case("profile_lifecycle")
    def test_strafe_brakes_with_native_speed_without_teleporting_or_speed_caps(self): self.run_case("braking_lane")
    def test_crouch_guards_and_failed_request_rollback(self): self.run_case("crouch_guards")
    def test_tracking_crouch_preserves_profile_strafing_and_native_movement_conflicts(self): self.run_case("tracking_crouch")
    def test_crouch_uses_real_posture_once_without_teleports(self): self.run_case("crouch_posture")
    def test_crouch_reset_handles_hidden_airborne_and_blocked_postures(self): self.run_case("crouch_reset")
    def test_visible_head_pose_and_no_false_helmet_feedback(self): self.run_case("head_feedback")
    def test_slide_requires_authoritative_standing_grounded_wiggle_target_and_native_guards(self): self.run_case("slide_guards")
    def test_forward_slide_invokes_real_ut_physics_and_preserves_its_input_and_posture(self): self.run_case("slide_native")
    def test_controllerless_slide_retires_at_native_deadline_and_resumes_wiggle(self): self.run_case("slide_end")
    def test_slide_lifecycle_clears_posture_native_timing_and_queued_inputs(self): self.run_case("slide_reset")
    def test_tracking_slide_respects_target_scope_and_native_guards(self): self.run_case("tracking_slide_guards")
    def test_tracking_slide_uses_guarded_lateral_native_impulse_and_keeps_direction(self): self.run_case("tracking_slide_direction")
    def test_tracking_slide_cooldown_exit_and_reuse_preserve_normal_movement(self): self.run_case("tracking_slide_end_and_reset")
    def test_tracking_slide_has_random_four_to_seven_second_delay(self): self.run_case("tracking_slide_policy")
    def test_popup_slide_lanes_and_post_slide_center_are_native_and_reset(self): self.run_case("popup_slide_slots")
    def test_long_strafe_requires_valid_visible_standing_wiggle_target(self): self.run_case("popup_long_guards")
    def test_long_strafe_holds_direction_then_returns_to_wiggle_without_teleporting(self): self.run_case("popup_long_motion")
    def test_popup_slide_and_long_strafe_frequency_and_sequencing(self): self.run_case("popup_motion_policy")


if __name__ == "__main__":
    unittest.main()
