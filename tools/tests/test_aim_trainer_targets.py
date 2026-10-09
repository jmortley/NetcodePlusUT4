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
constexpr int NM_DedicatedServer = 1;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Clamp(float value,float low,float high) { return value<low?low:value>high?high:value; }
    static float Min(float a,float b) { return a<b?a:b; }
    static float Max(float a,float b) { return a>b?a:b; }
};
enum MovementMode { MOVE_None, MOVE_Walking, MOVE_Falling, MOVE_Flying, MOVE_Swimming };
struct FVector {
    float X, Y, Z;
    FVector(float x=0.f, float y=0.f, float z=0.f) : X(x), Y(y), Z(z) {}
    bool ContainsNaN() const { return !std::isfinite(X)||!std::isfinite(Y)||!std::isfinite(Z); }
    float SizeSquared() const { return X*X+Y*Y+Z*Z; }
    float Size() const { return std::sqrt(SizeSquared()); }
    float Size2D() const { return std::sqrt(X*X+Y*Y); }
    FVector GetSafeNormal2D() const { const float size=Size2D(); return size>0.f?FVector(X/size,Y/size,0.f):FVector(); }
    FVector operator*(float scale) const { return FVector(X*scale,Y*scale,Z*scale); }
    FVector operator+(const FVector& other) const { return FVector(X+other.X,Y+other.Y,Z+other.Z); }
    FVector operator-(const FVector& other) const { return FVector(X-other.X,Y-other.Y,Z-other.Z); }
    FVector& operator*=(float scale) { X*=scale;Y*=scale;Z*=scale;return *this; }
    bool IsNearlyZero() const { return std::abs(X)+std::abs(Y)+std::abs(Z)<.00001f; }
    FVector GetClampedToMaxSize(float maximum) const {
        const float size=std::sqrt(X*X+Y*Y+Z*Z);return size>maximum?*this*(maximum/size):*this;
    }
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
    float BrakingDecelerationWalking=2000.f;
    float GravityScale = 1.f;
    FVector PendingLaunchVelocity;
    float GetMaxAcceleration() const { return MaxAcceleration; }
    int Stops = 0;
    bool IsMovingOnGround() const { return Mode == MOVE_Walking; }
    bool IsFalling() const { return Mode == MOVE_Falling; }
    void StopMovementImmediately() { Speed = 0.f; ++Stops; }
    void SetMovementMode(MovementMode mode) { Mode = mode; }
    void DisableMovement() { Mode = MOVE_None; }
};
struct ATeamArenaCharacter;
struct AUTCharacter;
struct FHitResult { FVector ImpactNormal=FVector(0,0,1); };
struct UUTCharacterMovement : UCharacterMovementComponent {
    using Super=UCharacterMovementComponent;
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
    float DodgeImpulseHorizontal=1500.f,DodgeImpulseVertical=500.f,DodgeMaxHorizontalVelocity=1700.f;
    float DodgeLandingSpeedFactor=1.f,FloorSlideEndingSpeedFactor=.4f,Gravity=-2154.f;
    float DodgeJumpResetInterval=.35f,DodgeJumpLandingSpeedFactor=1.f;
    float DefaultBrakingDecelerationWalking=2000.f;
    float FastInitialAcceleration=12000.f,MaxFastAccelSpeed=200.f,DodgeLandingAcceleration=1000.f;
    float MaxFallingAcceleration=3200.f,MaxSwimmingAcceleration=1000.f;
    float MaxRelativeSwimmingAccelNumerator=1.f,MaxRelativeSwimmingAccelDenominator=1.f;
    bool bIsAgainstWall=false,bFallingInWater=false,bCountWallSlides=false,bHasPlayedWallHitSound=false;
    bool bJumpAssisted=false,bExplicitJump=false;
    int CurrentMultiJumpCount=0,CurrentWallDodgeCount=0;
    int TimerResets=0;
    FVector Velocity,Acceleration;
    struct {
        bool Walkable=true;
        struct { FVector ImpactNormal=FVector(0.f,0.f,1.f); } HitResult;
        bool IsWalkableFloor() const { return Walkable; }
    } CurrentFloor;
    void ClearDodgeInput() { DodgeInput = false; bPressedSlide=false; }
    void ClearFloorSlideTap() { bWantsFloorSlide=false; }
    void UpdateFloorSlide(bool wants) { bWantsFloorSlide=wants; }
    float GetGravityZ() const { return Gravity * GravityScale; }
    float GetMaxAcceleration() const;
    void ClearRestrictedJump() {}
    void SetPostLandedPhysics(const FHitResult&) { Mode=MOVE_Walking; }
    void StartNewPhysics(float,int32) {}
    void ResetTimers() { DodgeResetTime=FloorSlideTapTime=FloorSlideEndTime=0.f; ++TimerResets; }
    void ClearFallingStateFlags() { bIsDodging = false; FallingFlags = false; bIsFloorSliding=false; }
    float GetCurrentMovementTime() const { return MovementTime; }
    bool CanDodge() const { return DodgeAllowed&&!bIsFloorSliding&&MovementTime>=DodgeResetTime; }
    void PerformFloorSlide(const FVector&,const FVector&);
    void ProcessLanded(const FHitResult&,float,int32);
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
    bool bApplyWallSlide=false;
    bool ShouldNotifyLanded(const FHitResult&) const { return false; }
    void Landed(const FHitResult&) {}
    FVector SlideDirection;
    AUTPlayerState* PlayerState=nullptr;
    bool bIsCrouched = false, Dead = false;
    bool IsDead() const { return Dead; }
    float GetWalkMovementReductionPct() const { return 0.f; }
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
    struct Attachment { void SetActorHiddenInGame(bool) {} };
    Attachment* WeaponAttachment = nullptr;
    int CapsuleHeadQueries = 0, SuperTicks = 0, DodgeCalls = 0, Teleports = 0, NetUpdates = 0, InputCalls = 0;
    int Launches = 0;
    bool LaunchXYOverride = false, LaunchZOverride = false;
    void LaunchCharacter(FVector velocity, bool xyOverride, bool zOverride) {
        ++Launches; Move.PendingLaunchVelocity=velocity; LaunchXYOverride=xyOverride; LaunchZOverride=zOverride;
    }
    bool DodgeAllowed = true, Hidden = false, Collision = true, HasPendingInput = false;
    bool SimulateNativeDodge=false;
    int GetNetMode() const { return NM_DedicatedServer; }
    float HeadRadius=18.f,HeadScale=1.f;
    float HeadScaleUsed=0.f,HeadPrediction=-1.f;
    bool IsHeadShot(FVector,FVector,float scale,AUTCharacter*,float prediction) {
        HeadScaleUsed=scale;HeadPrediction=prediction;return HeadRadius*HeadScale*scale>20.5f;
    }
    float MaxSpeedPctModifier=1.f;
    struct Capsule { float Radius=38.f;float GetScaledCapsuleRadius() const { return Radius; } } CapsuleComponent;
    Capsule* GetCapsuleComponent() { return &CapsuleComponent; }
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
        const bool success=DodgeAllowed&&!bIsCrouched&&Move.CanDodge();
        if(success&&SimulateNativeDodge) {
            Move.Mode=MOVE_Falling;Move.bIsDodging=true;
            Move.Velocity=Move.DodgeImpulseHorizontal*direction+(Move.Velocity|cross)*cross;
            const float speed=FMath::Min(Move.Velocity.Size2D(),Move.DodgeMaxHorizontalVelocity)*MaxSpeedPctModifier;
            Move.Velocity=speed*Move.Velocity.GetSafeNormal2D();Move.Velocity.Z=Move.DodgeImpulseVertical;
        }
        return success;
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
    bool bTrainerVisible=false, bTrainerStrafe=false, bTrainerWiggle=false, bTrainerAirborne=false;
    struct ActorClass {
        ANCAimTrainerTarget* Defaults=nullptr;
        template<class T> const T* GetDefaultObject() const { return static_cast<const T*>(Defaults); }
    } Class;
    ActorClass* GetClass() { return &Class; }
    float StrafeDirection=1.f, StrafeRange=800.f, SpawnProtectionStartTime=0.f, AppearanceTime=0.f;
    float WiggleRange=0.f, PopupLongStrafeEndTime=0.f;
    bool bRecenterWiggleAfterSlide=false,bRecenterWiggleAfterDodge=false,bTrainerDodgeSlidePending=false;
    FVector StrafeCenter,TrainerSlideDirection,StrafeAxis=FVector(0.f,1.f,0.f);
    FVector PopupMoveMinimum,PopupMoveMaximum,PopupMoveDirection;
    bool bPopupEvasion=false,bPopupNeedsDecision=false;
    float TrainerHeadshotScale=1.f,NextTrainerTintTime=0.f,TrainerFlightRate=1.f;
    void UpdateTrainerTint() {}
    void SetTrainerHeadshotScale(float);
    void OnRep_TrainerHeadshotScale();
    struct History { int Count=7; void Reset() { Count=0; } } SavedPositions, SavedCapsulePostures;
    void OnRep_TrainerVisible();
    void ActivateTarget(const FVector&,bool);
    void ActivateAirborneTarget(const FVector&,const FVector&,float=1.f);
    void OnRep_TrainerFlightRate();
    bool LaunchAirborneTarget(const FVector&);
    void SetTrainerSpeedScale(float);
    void StartWiggle(float);
    bool StartPopupLongStrafe(float,float,float);
    bool IsTrainerLongStrafing() const { return PopupLongStrafeEndTime>0.f; }
    bool SetTrainerCrouched(bool);
    void HideTarget();
    void ResetTargetMovement();
    void ReverseStrafe();
    void ConfigurePopupStrafe(const FVector&,float,float);
    void SetPopupStrafeAxis(const FVector&);
    bool SetPopupMovement(const FVector&,const FVector&,const FVector&);
    bool NeedsPopupMovementDecision() const;
    bool TryTrainerDodge(float);
    bool TryTrainerPopupDodge(int32,const FVector&,const FVector&,bool=false);
    bool TryTrainerSlideForward();
    bool TryTrainerPopupSlide(int32,int32=0);
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
ANCAimTrainerTarget ActivePopup() {
    auto target=Active();target.StartWiggle(120.f);
    target.Move.MaxWalkSpeed=940.f;target.Move.MaxAcceleration=5000.f;target.Move.MaxFloorSlideSpeed=1100.f;
    target.StrafeCenter=target.Position=FVector(1200,-1045,103);return target;
}
'''

CASES = r'''
int main(int argc,char**argv) {
    Require(argc==2,"case required"); const std::string name(argv[1]);
    if(name=="popup_travel") {
        for(float dt:{.008f,.016f,.032f}) for(int slot:{0,1,4,5}) for(int variant:{0,1,2}) {
            const auto area=NCAimTrainerLayout::PopupEvasionArea(slot,variant);
            const FVector minimum(area.MinX,area.MinY,0),maximum(area.MaxX,area.MaxY,0);
            auto target=ActivePopup(); target.Position=(minimum+maximum)*.5f; target.Position.Z=428.f;
            const int teleports=target.Teleports;
            unsigned seed=unsigned(1+slot*17+variant*31);
            auto roll=[&]() { seed=1664525u*seed+1013904223u; return float(seed>>8)/16777216.f; };
            float decision=0.f,lastHold=0.f,minX=target.Position.X,maxX=minX,minY=target.Position.Y,maxY=minY;
            int decisions=0,fastFrames=0;
            for(int frame=0;frame<4000;++frame) {
                const float now=frame*dt;
                if(now>=decision) {
                    const float angle=roll()*6.28318530718f;
                    Require(target.SetPopupMovement(minimum,maximum,FVector(std::cos(angle),std::sin(angle),0)),
                        "valid popup input rejected");
                    lastHold=NCAimTrainerScenarioPolicy::PopupMoveHoldSeconds(roll(),roll(),lastHold);
                    decision=now+lastHold; ++decisions;
                }
                const int inputs=target.InputCalls; const FVector before=target.Move.Velocity;
                target.Tick(dt);
                Require(target.InputCalls==inputs+1,"popup stopped at a waypoint instead of driving native movement");
                target.Move.Velocity=target.Move.Velocity+target.LastInput*(target.Move.MaxAcceleration*dt);
                target.Move.Velocity=target.Move.Velocity.GetClampedToMaxSize(target.Move.MaxWalkSpeed);
                target.Position=target.Position+(before+target.Move.Velocity)*(.5f*dt);
                Require(target.Position.X>=area.MinX-20.f && target.Position.X<=area.MaxX+20.f
                    &&target.Position.Y>=area.MinY-20.f &&target.Position.Y<=area.MaxY+20.f,
                    "predictive steering left its capsule-safe area");
                minX=std::min(minX,target.Position.X); maxX=std::max(maxX,target.Position.X);
                minY=std::min(minY,target.Position.Y); maxY=std::max(maxY,target.Position.Y);
                fastFrames+=int(target.Move.Velocity.Size2D()>400.f);
            }
            Require(decisions>20 && fastFrames>2400 && target.Teleports==teleports,"evasion barely moved or teleported");
            Require(maxX-minX>150.f && maxY-minY>150.f,"target settled onto a fixed strafe line");
            target.HideTarget(); target.ActivateTarget(FVector(-800,0,108),true);
            Require(!target.bPopupEvasion && !target.bPopupNeedsDecision && target.StrafeAxis.Y==1.f,
                "pooled tracking target inherited popup motion");
        }
        const FVector minimum(100,-1500,0),maximum(2400,-450,0),direction(1,0,0);
        auto target=ActivePopup(); target.Role=1;
        Require(!target.SetPopupMovement(minimum,maximum,direction),"client selected authority movement");
        target.Role=ROLE_Authority; target.Move.Mode=MOVE_Falling;
        Require(!target.SetPopupMovement(minimum,maximum,direction),"ground input countersteered a dodge");
        target.Move.Mode=MOVE_Walking; target.Move.bIsFloorSliding=true;
        Require(!target.SetPopupMovement(minimum,maximum,direction),"ground input interrupted a native slide");
        target.Move.bIsFloorSliding=false;
        Require(!target.SetPopupMovement(maximum,minimum,direction),"inverted area accepted");
        Require(!target.SetPopupMovement(minimum,maximum,FVector()),"zero direction accepted");
        Require(!target.SetPopupMovement(minimum,maximum,FVector(1,0,1)),"vertical walk input accepted");
        Require(!target.SetPopupMovement(minimum,maximum,FVector(std::numeric_limits<float>::quiet_NaN(),0,0)),"NaN accepted");
    }
    else if(name=="popup_evasion_dodges") {
        for(float gravity : {-2154.f,-980.f}) for(int slot : {0,1,4,5}) for(int variant : {0,1,2}) {
            const auto area=NCAimTrainerLayout::PopupEvasionArea(slot,variant);
            const FVector minimum(area.MinX,area.MinY,0),maximum(area.MaxX,area.MaxY,0);
            unsigned seed=unsigned(1+slot*17+variant*31);
            auto roll=[&]() { seed=1664525u*seed+1013904223u; return float(seed>>8)/16777216.f; };
            int dodges=0,rejected=0,forward=0,backward=0,left=0,right=0,chains=0;
            for(int sample=0;sample<4000;++sample) {
                auto target=Active(); target.SimulateNativeDodge=true;
                target.Move.Gravity=gravity; target.Move.MaxWalkSpeed=940.f; target.Move.MaxAcceleration=5000.f;
                target.Move.MaxFloorSlideSpeed=1100.f;
                target.Position=FVector(area.MinX+(area.MaxX-area.MinX)*roll(),area.MinY+(area.MaxY-area.MinY)*roll(),428.f);
                const float heading=roll()*6.28318530718f;
                target.Move.Velocity=FVector(std::cos(heading),std::sin(heading),0)*(940.f*roll());
                Require(target.SetPopupMovement(minimum,maximum,FVector(1,0,0))&&!target.bTrainerWiggle,
                    "persistent popup did not enter evasion without a wiggle state");
                const bool chain=sample%5==0 && slot!=1;
                const float degrees=slot==1 ? 3.f+6.f*roll()
                    : slot==5 || (slot==4 && variant==1) ? 55.f+70.f*roll() : 8.f+70.f*roll();
                const float angle=degrees*.01745329252f;
                const float x=chain ? std::abs(std::cos(angle)) : (roll()<.5f ? -1.f : 1.f)*std::cos(angle);
                const FVector direction(x,(roll()<.5f ? -1.f : 1.f)*std::sin(angle),0);
                if(!target.TryTrainerPopupDodge(slot,direction,FVector::ZeroVector,chain)) {
                    ++rejected; Require(target.DodgeCalls==0,"unsafe trajectory reached native Dodge"); continue;
                }
                ++dodges; forward+=int(direction.X<0); backward+=int(direction.X>0);
                left+=int(direction.Y<0); right+=int(direction.Y>0); chains+=int(chain);
                Require(target.Move.bIsDodging&&target.Move.IsFalling()&&target.bPopupNeedsDecision
                    &&!target.NeedsPopupMovementDecision(),"new evasion action did not defer decisions through native flight");
                const int inputs=target.InputCalls; target.Tick(.016f);
                Require(target.InputCalls==inputs+int(chain),"ordinary ground input interrupted native flight");
                Require(!target.SetPopupMovement(minimum,maximum,FVector(0,1,0)),"fresh decision interrupted native dodge");
                // The normal dodge recovery must request a new decision rather
                // than returning to the old direction or the old spawn anchor.
                if(!chain) {
                    target.Move.MovementTime=10.f; target.Move.ProcessLanded(FHitResult(),0.f,0);
                    target.Tick(.016f);
                    Require(!target.NeedsPopupMovementDecision(),"landing cooldown was bypassed");
                    target.Move.MovementTime=target.Move.DodgeResetTime+.001f; target.Tick(.016f);
                    Require(target.NeedsPopupMovementDecision()&&target.InputCalls==inputs,
                        "dodge recovery reused old ground input or lost the next decision");
                    Require(target.SetPopupMovement(minimum,maximum,FVector(0,1,0))&&!target.NeedsPopupMovementDecision(),
                        "recovered target could not accept a new independent decision");
                }
                Require(target.Teleports==0,"evasion dodge teleported a target");
            }
            if(dodges<10 || rejected<10 || !left || !right || !forward || !backward
                || (gravity==-2154.f && (slot==0 || (slot==4 && variant==0)) && !chains)) {
                std::cerr<<"slot="<<slot<<" variant="<<variant<<" gravity="<<gravity<<" dodges="<<dodges
                    <<" rejected="<<rejected<<" forward="<<forward<<" backward="<<backward
                    <<" left="<<left<<" right="<<right<<" chains="<<chains<<'\n';
                Require(false,"an evasion area offered no varied native dodge opportunities");
            }
        }
    }
    else if(name=="popup_left_dodge") {
        int accepted=0;
        for(float angle:{-.13962634f,0.f,.13962634f}) for(float offset:{-1500.f,-799.f,-499.f,0.f,499.f,799.f,1500.f})
        for(float gravity:{-980.f,-2154.f}) for(float roll:{0.f,1.f}) {
            auto target=Active(); const FVector center(1100,-850,108),axis(std::cos(angle),std::sin(angle),0);
            target.Position=center+axis*offset; target.ConfigurePopupStrafe(center,800.f,roll); target.SetPopupStrafeAxis(axis);
            target.Move.Gravity=gravity;
            const FVector start=target.Position;
            const bool dodged=target.TryTrainerDodge(roll);
            if(!dodged) { Require(target.DodgeCalls==0,"unsafe predicted path invoked a dodge anyway"); continue; }
            ++accepted;
            Require(target.DodgeCalls==1 && std::fabs(target.LastDodgeDirection.X)>.99f,"left dodger used lateral tracking direction");
            const float time=-2.f*target.Move.DodgeImpulseVertical/gravity+.06f
                +target.Move.DodgeLandingSpeedFactor*(target.Move.DodgeResetInterval+.1f);
            const FVector end=start+target.Move.Velocity.GetSafeNormal2D()*target.Move.Velocity.Size2D()*time;
            Require(NCAimTrainerLayout::CanPopupDodgePath(0,start.X,start.Y,end.X,end.Y,target.GetCapsuleComponent()->GetScaledCapsuleRadius(),0.f),
                "left dodge crosses central cover, room wall or foreground lane");
        }
        Require(accepted>50,"safe second-dodger lane cannot perform frequent native dodges");
    }
    else if(name=="popup_anchor") {
        ANCAimTrainerTarget target;
        target.ActivateTarget(FVector(-1000,450,103),true);
        const int teleports=target.Teleports;
        target.ConfigurePopupStrafe(FVector(-1000,0,103),800.f,.1f);
        Require(target.StrafeCenter.Y==0.f && target.Position.Y==450.f && target.StrafeDirection==-1.f
            &&target.StrafeRange==800.f && target.Teleports==teleports,"random start shifted the safe lane or teleported after configuration");
        target.Position.Y=805.f; target.ConfigurePopupStrafe(FVector(-1000,0,103),800.f,.9f); target.Tick(.016f);
        Require(target.LastInput.Y==-1.f,"random direction overrode native boundary braking");
        target.ActivateTarget(FVector(1500,-850,103),false); target.StartWiggle(400.f);
        target.ConfigurePopupStrafe(FVector(1500,-850,103),350.f,.9f);
        Require(target.WiggleRange==350.f && target.StrafeRange==350.f && target.Move.MaxWalkSpeed==500.f,
            "varied walking band changed native speed or did not survive posture recovery");
        target.Role=1; target.ConfigurePopupStrafe(FVector(),40.f,0.f);
        Require(target.StrafeRange==350.f,"client reconfigured authority target movement");
    } else if(name=="reverse") {
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
        Require(target.StrafeRange==400.f,"wiggle exceeded maximum range or clipped wider left lanes");
        target.Position.Y=700.f; target.Tick(.016f);
        Require(target.LastInput.Y==-1.f,"wiggle did not reverse at right edge");
        target.Position.Y=-100.f; target.ReverseStrafe();
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
            for(float width:{150.f,180.f}) {
                target.StartWiggle(width); target.StrafeDirection=sign;
                target.Position.Y=target.StrafeCenter.Y+sign*30.f;
                target.Tick(.016f);
                Require(target.StrafeRange==width&&target.LastInput.Y==sign&&target.Move.MaxWalkSpeed==940.f
                        &&target.Teleports==teleports,"wider left lane still reverses inside the old narrow range");
                target.Position.Y=target.StrafeCenter.Y+sign*(width-30.f); target.Tick(.016f);
                Require(target.LastInput.Y==-sign,"wider left lane failed to brake before its authored boundary");
            }
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
            target.ActivateTarget(FVector(1000.f,-850.f,103.f),false);target.StartWiggle(180.f);
            target.Position.Y+=side*150.f;
            Require(target.StartPopupLongStrafe(220.f,.9f,side<0.f?0.f:1.f),"valid long strafe rejected");
            Require(target.StrafeDirection==-side&&target.StrafeRange==220.f
                &&target.PopupLongStrafeEndTime==42.9f,"long strafe did not sweep across its wider left lane");
            target.ReverseStrafe();target.Tick(.016f);
            Require(target.LastInput.Y==-side&&target.Teleports==1&&target.Move.MaxWalkSpeed==500.f,
                "short wiggle interrupted long hold or motion was replaced with a speed cap/teleport");
            Require(!target.StartPopupLongStrafe(220.f,.9f,0.f)&&!target.TryTrainerPopupSlide(0)
                &&!target.SetTrainerCrouched(true),"another scheduled action interrupted long native strafe");
            target.Position.Y=target.StrafeCenter.Y-side*210.f;
            target.TheWorld.Time=target.PopupLongStrafeEndTime-.001f;target.Tick(.001f);
            Require(target.IsTrainerLongStrafing(),"long hold ended before deadline");
            target.TheWorld.Time=target.PopupLongStrafeEndTime;target.Tick(.001f);
            Require(!target.IsTrainerLongStrafing()&&target.StrafeRange==180.f&&target.LastInput.Y==side
                &&target.Position.Y==target.StrafeCenter.Y-side*210.f&&target.Teleports==1,
                "long strafe did not return inside original wiggle range with native motion");
            Require(target.StartPopupLongStrafe(999.f,999.f,0.f)&&target.StrafeRange==220.f
                &&target.PopupLongStrafeEndTime==target.TheWorld.Time+1.f,"long move exceeded safe range or lost the one-second hold");
            target.HideTarget();
            Require(!target.IsTrainerLongStrafing()&&target.WiggleRange==0.f,"hidden appearance retained long hold");
            target.ActivateTarget(FVector(1200.f,-850.f,103.f),false);target.StartWiggle(99.f);
            Require(!target.IsTrainerLongStrafing()&&target.StrafeRange==99.f,"new appearance inherited long strafe");
        }
    } else if(name=="popup_dodge_guards") {
        const FVector diagonal(.9797959f,.2f,0.f);
        for(int guard=0;guard<11;++guard) {
            auto target=ActivePopup();
            if(guard==0)target.Role=1;
            if(guard==1)target.bTrainerVisible=false;
            if(guard==2)target.bTrainerWiggle=false;
            if(guard==3)target.Dead=true;
            if(guard==4)target.bIsCrouched=true;
            if(guard==5)target.Move.bIsFloorSliding=true;
            if(guard==6)target.PopupLongStrafeEndTime=43.f;
            if(guard==7)target.bRecenterWiggleAfterDodge=true;
            if(guard==8)target.Move.Mode=MOVE_Falling;
            if(guard==9)target.Move.bIsDodging=true;
            if(guard==10)target.Move.bIsDodgeLanding=true;
            Require(!target.TryTrainerPopupDodge(0,diagonal,FVector::ZeroVector)&&target.DodgeCalls==0&&target.NetUpdates==0,
                "invalid popup target reached native dodge or changed replication");
        }
        for(int reject=0;reject<3;++reject) {
            auto target=ActivePopup();target.HasPendingInput=true;
            if(reject==0)target.DodgeAllowed=false;
            if(reject==1)target.Move.DodgeAllowed=false;
            if(reject==2)target.Move.DodgeResetTime=100.f;
            Require(!target.TryTrainerPopupDodge(0,diagonal,FVector::ZeroVector)&&target.DodgeCalls==1
                &&!target.bRecenterWiggleAfterDodge&&target.StrafeDirection==1.f
                &&target.HasPendingInput&&target.NetUpdates==0,
                "native dodge rejection changed ordinary target movement or bypassed cooldown");
        }
    } else if(name=="popup_dodge_direction") {
        const float nan=std::numeric_limits<float>::quiet_NaN();
        const float inf=std::numeric_limits<float>::infinity();
        for(const auto& direction : {FVector(),FVector(.1f,.1f,0.f),FVector(1.f,1.f,0.f),
            FVector(1.f,0.f,.001f),FVector(nan,0.f,0.f),FVector(0.f,nan,0.f),FVector(1.f,0.f,nan),
            FVector(inf,0.f,0.f),FVector(0.f,-inf,0.f),FVector(1.f,0.f,inf),FVector(1.e30f,1.e30f,0.f)}) {
            auto target=ActivePopup();
            Require(!target.TryTrainerPopupDodge(0,direction,FVector::ZeroVector)&&target.DodgeCalls==0&&!target.bRecenterWiggleAfterDodge,
                "invalid direction reached native dodge");
        }
        for(float x : {-1.f,1.f}) for(float y : {-1.f,1.f}) {
            auto target=ActivePopup();target.HasPendingInput=true;
            const FVector direction(.9797959f*x,.2f*y,0.f);
            Require(target.TryTrainerPopupDodge(0,direction,FVector::ZeroVector)&&target.DodgeCalls==1,
                "valid front/back diagonal native dodge was rejected");
            Require(std::abs(target.LastDodgeDirection.X-direction.X)<.00001f
                &&std::abs(target.LastDodgeDirection.Y-direction.Y)<.00001f
                &&std::abs(target.LastDodgeDirection|target.LastDodgeCross)<.00001f
                &&std::abs(target.LastDodgeCross.Size2D()-1.f)<.00001f
                &&target.LastDodgeDirection.Z==0.f&&target.LastDodgeCross.Z==0.f,
                "popup dodge changed selected quadrant or did not use unit perpendicular directions");
            Require(target.bRecenterWiggleAfterDodge&&!target.HasPendingInput&&target.StrafeDirection==y
                &&target.NetUpdates==1&&target.Teleports==0&&target.InputCalls==0,
                "popup dodge did not defer recentering or bypassed native impulse/replication");
        }
        auto target=ActivePopup();
        Require(target.TryTrainerPopupDodge(0,FVector(.9798f,.2001f,0.f),FVector::ZeroVector)
            &&std::abs(target.LastDodgeDirection.Size2D()-1.f)<.00001f,
            "near-unit direction was not normalized before the native impulse");
    } else if(name=="popup_dodge_recovery") {
        auto target=ActivePopup();target.StrafeCenter=FVector(1200,-800,103);
        target.Position=target.StrafeCenter;target.Move.MovementTime=10.f;
        Require(target.TryTrainerPopupDodge(0,FVector(-.9797959f,-.2f,0.f),FVector::ZeroVector),"dodge recovery fixture failed");
        target.Move.Mode=MOVE_Falling;target.Move.bIsDodging=true;
        target.Position=FVector(600,-1250,250);target.Tick(.016f);
        Require(target.bRecenterWiggleAfterDodge&&target.StrafeCenter.X==1200.f
            &&target.StrafeCenter.Y==-800.f&&target.InputCalls==0,"airborne dodge recentered or countersteered");
        // Native landing clears bIsDodging and sets its slowdown and cooldown.
        target.Move.Mode=MOVE_Walking;target.Move.bIsDodging=false;target.Move.bIsDodgeLanding=true;
        target.Move.DodgeResetTime=11.35f;target.Move.MovementTime=11.f;
        target.Position=FVector(450,-1360,103);target.Tick(.016f);
        target.ReverseStrafe();
        Require(target.StrafeDirection==-1.f&&!target.StartPopupLongStrafe(180.f,.6f,0.f)
            &&!target.TryTrainerPopupSlide(4)&&!target.TryTrainerPopupDodge(0,FVector(.9797959f,.2f,0.f),FVector::ZeroVector)
            &&!target.SetTrainerCrouched(true)&&target.bRecenterWiggleAfterDodge&&target.InputCalls==0,
            "a scheduled action interrupted native popup dodge landing");
        target.Move.MovementTime=11.349f;target.Tick(.016f);
        Require(!target.Move.bIsDodgeLanding&&target.bRecenterWiggleAfterDodge&&target.InputCalls==0,
            "popup movement resumed before native cooldown or retained landing slowdown");
        target.Move.MovementTime=11.35f;target.Tick(.016f);
        Require(!target.bRecenterWiggleAfterDodge&&target.StrafeCenter.X==450.f&&target.StrafeCenter.Y==-1360.f
            &&target.StrafeCenter.Z==103.f&&target.LastInput.Y==-1.f&&target.InputCalls==1
            &&target.Teleports==0&&target.StrafeRange==120.f,"landing did not resume wiggle at the native endpoint");
        target.ReverseStrafe();Require(target.StrafeDirection==1.f,"wiggle did not recover after landing");
        Require(target.SetTrainerCrouched(true),"scheduled crouch did not recover after landing");
    } else if(name=="popup_dodge_reuse") {
        for(bool hide : {false,true}) {
            auto target=ActivePopup();
            Require(target.TryTrainerPopupDodge(0,FVector(-.9797959f,.2f,0.f),FVector::ZeroVector),"reuse fixture failed");
            target.Move.Mode=MOVE_Falling;target.Move.bIsDodging=true;
            target.Move.DodgeResetTime=999.f;target.HasPendingInput=true;
            if(hide) {
                target.HideTarget();
                Require(!target.bRecenterWiggleAfterDodge&&!target.HasPendingInput
                    &&!target.Move.bIsDodging&&target.Move.DodgeResetTime==0.f,"hide retained diagonal dodge state");
            }
            target.ActivateTarget(FVector(1200,-900,103),false);target.StartWiggle(99.f);target.Tick(.016f);
            Require(!target.bRecenterWiggleAfterDodge&&!target.Move.bIsDodging&&target.Move.DodgeResetTime==0.f
                &&target.StrafeCenter.X==1200.f&&target.StrafeCenter.Y==-900.f&&target.LastInput.Y==1.f,
                "reused target inherited prior appearance dodge endpoint or blocked movement");
        }
        auto target=ActivePopup();target.bRecenterWiggleAfterDodge=true;
        target.Role=1;target.Position=FVector(400,500,100);target.Tick(.016f);
        Require(target.bRecenterWiggleAfterDodge&&target.StrafeCenter.X==1200.f&&target.StrafeCenter.Y==-1045.f,
            "remote target overwrote authority dodge lifecycle");
    } else if(name=="popup_dodge_geometry") {
        for(int slot : {0,4}) for(float sign : {-1.f,1.f}) {
            auto target=ActivePopup();target.Move.Velocity=FVector(0.f,sign*500.f,0.f);
            const FVector origin(20000.f,-40000.f,50000.f);
            target.Position=target.Position+origin;
            Require(target.TryTrainerPopupDodge(slot,FVector(sign*.9797959f,-sign*.2f,0.f),origin),
                "real-profile angled dodge was unavailable with perpendicular strafe momentum or translated arena");
        }
        for(int guard=0;guard<9;++guard) {
            auto target=ActivePopup();FVector origin;
            int slot=0;bool chain=false;FVector direction(.9797959f,.2f,0.f);
            if(guard==0)target.Position.X=3000.f;
            if(guard==1)target.Position.Y=-500.f;
            if(guard==2)slot=2;
            if(guard==3)slot=3;
            if(guard==4)target.Move.Gravity=0.f;
            if(guard==5)origin.X=std::numeric_limits<float>::quiet_NaN();
            if(guard==6)target.Move.Velocity.Y=10000.f;
            if(guard==7){chain=true;direction.X=-.9797959f;}
            if(guard==8){chain=true;target.Position.X=2000.f;}
            Require(!target.TryTrainerPopupDodge(slot,direction,origin,chain)&&target.DodgeCalls==0,
                "unsafe world bounds, protected target, momentum or chain escaped prediction guard");
        }
    } else if(name=="popup_dodge_slide_chain") {
        for(bool successfulSlide : {false,true}) for(bool evasion : {false,true}) {
            auto target=ActivePopup();target.SimulateNativeDodge=true;
            target.Position=target.StrafeCenter=FVector(-400.f,-1300.f,103.f);
            const auto area=NCAimTrainerLayout::PopupEvasionArea(4,0);
            const FVector minimum(area.MinX,area.MinY,0),maximum(area.MaxX,area.MaxY,0);
            if(evasion) Require(target.SetPopupMovement(minimum,maximum,FVector(1,0,0)),"evasion chain setup failed");
            const FVector direction(.9797959f,.2f,0.f);
            Require(target.TryTrainerPopupDodge(4,direction,FVector::ZeroVector,true)
                &&target.bTrainerDodgeSlidePending&&target.Move.bWantsFloorSlide
                &&target.Move.IsFalling()&&target.Move.bIsDodging&&!target.bRepFloorSliding,
                "backward angled dodge failed to arm the native landing-slide request");
            const int inputs=target.InputCalls;target.Tick(.016f);
            Require(target.InputCalls==inputs+1&&std::abs((target.LastInput|direction)-1.f)<.0001f,
                "armed landing slide lost its native airborne movement intent");
            target.Move.MovementTime=10.f;
            // Native movement consumes the queued input before ProcessLanded.
            target.Move.Acceleration=target.LastInput*target.Move.MaxAcceleration;
            if(!successfulSlide)target.Move.Velocity=target.Move.Velocity.GetSafeNormal2D()*100.f;
            target.Move.ProcessLanded(FHitResult(),0.f,0);
            Require(!target.Move.bIsDodging&&target.Move.IsMovingOnGround(),"native landing fixture did not land");
            target.Tick(.016f);
            if(successfulSlide) {
                Require(target.SlideEvents==1&&target.IsTrainerSliding()&&target.bRepFloorSliding&&target.bIsCrouched
                    &&!target.bTrainerDodgeSlidePending&&!target.Move.bWantsFloorSlide
                    &&std::abs((target.LastInput|direction)-1.f)<.0001f,
                    "native ProcessLanded slide lacked movement event, replicated posture or direction");
                Require(!target.SetTrainerCrouched(false)&&!target.StartPopupLongStrafe(180.f,.6f,0.f),
                    "scheduled posture or strafe interrupted a native chained slide");
                target.Position=FVector(1500.f,-900.f,target.Move.HalfHeight);
                target.Move.bWasFloorSliding=true;
                target.Move.MovementTime=target.Move.FloorSlideEndTime;target.Tick(.016f);
                Require(!target.IsTrainerSliding()&&!target.bRepFloorSliding&&!target.bIsCrouched
                    &&target.Move.bWasFloorSliding&&target.bRecenterWiggleAfterDodge,
                    "chained slide did not keep native ending slowdown and dodge cooldown");
            } else {
                Require(target.SlideEvents==0&&!target.IsTrainerSliding()&&!target.bRepFloorSliding
                    &&!target.bTrainerDodgeSlidePending&&!target.Move.bWantsFloorSlide
                    &&target.bRecenterWiggleAfterDodge,"failed native landing slide left a future slide request armed");
            }
            target.Position=FVector(1510.f,-895.f,103.f);
            target.Move.MovementTime=target.Move.DodgeResetTime+.001f;target.Tick(.016f);
            Require(!target.bRecenterWiggleAfterDodge&&target.StrafeCenter.X==1510.f&&target.StrafeCenter.Y==-895.f
                &&(evasion ? target.NeedsPopupMovementDecision() : target.LastInput.X==0.f&&target.LastInput.Y==1.f)
                &&target.Teleports==0,
                "native dodge-slide chain did not resume bounded walking at its real endpoint");
            if(evasion) {
                Require(target.SetPopupMovement(minimum,maximum,FVector(0,-1,0)),"recovered chain rejected fresh movement");
                target.Tick(.016f);
                Require(target.LastInput.Y==-1.f&&!target.NeedsPopupMovementDecision(),"slide retained its old movement pattern");
            }
        }
    } else if(name=="popup_dodge_slide_reuse") {
        for(bool nativeSlide : {false,true}) {
            auto target=ActivePopup();target.SimulateNativeDodge=true;
            target.Position=target.StrafeCenter=FVector(-400.f,-1300.f,103.f);
            Require(target.TryTrainerPopupDodge(4,FVector(.9797959f,.2f,0.f),FVector::ZeroVector,true),
                "pending slide reset fixture failed");
            if(nativeSlide) {
                target.Move.Acceleration=target.LastInput*target.Move.MaxAcceleration;
                target.Move.ProcessLanded(FHitResult(),0.f,0);target.Tick(.016f);
            }
            target.HideTarget();
            Require(!target.bTrainerDodgeSlidePending&&!target.bRecenterWiggleAfterDodge
                &&!target.Move.bWantsFloorSlide&&!target.Move.bIsDodging&&!target.IsTrainerSliding()
                &&!target.bRepFloorSliding&&!target.bIsCrouched,"hidden appearance retained native chained slide state");
            target.ActivateTarget(FVector(2000,-1000,103),false);target.StartWiggle(120.f);target.Tick(.016f);
            Require(!target.bTrainerDodgeSlidePending&&!target.bRecenterWiggleAfterDodge
                &&target.StrafeCenter.X==2000.f&&target.StrafeCenter.Y==-1000.f&&target.LastInput.Y==1.f,
                "new appearance inherited native landing slide motion");
        }
    } else if(name=="popup_slide_variants") {
        for(int variant : {0,1,2,3,-1}) {
            auto target=ActivePopup();
            const bool allowed=variant==0||variant==1;
            Require(target.TryTrainerPopupSlide(4,variant)==allowed,"popup slide ignored the selected spawn variant");
            if(allowed)Require(target.TrainerSlideDirection.Y==(variant==1?-1.f:1.f)
                &&target.TrainerSlideDirection.X==0.f,"far-right slide did not turn inward away from arena wall");
            else Require(target.SlideEvents==0,"deep-left or invalid variant crossed a raised platform");
        }
    } else if(name=="airborne_launch") {
        auto target=Active();
        target.Move.PendingLaunchVelocity=FVector(1,2,3);
        target.ActivateAirborneTarget(FVector(100,-1400,288),FVector(500,1300,2300));
        Require(target.bTrainerAirborne&&target.bTrainerVisible&&!target.bTrainerStrafe&&!target.bTrainerWiggle
            &&target.Move.Mode==MOVE_Falling&&target.Launches==1&&target.LaunchXYOverride&&target.LaunchZOverride
            &&target.Move.PendingLaunchVelocity.X==500.f&&target.Move.PendingLaunchVelocity.Y==1300.f
            &&target.Move.PendingLaunchVelocity.Z==2300.f,
            "airborne activation did not invoke native launch with full override");
        const int teleports=target.Teleports;
        target.SavedPositions.Count=4;
        target.TheWorld.Time+=2.f;
        target.Move.Mode=MOVE_Walking;
        Require(target.LaunchAirborneTarget(FVector(-500,-1300,2300))&&target.Teleports==teleports
            &&target.SavedPositions.Count==4&&target.AppearanceTime==42.f,
            "repeat jumppad launch recycled identity or teleported instead of native launching");
        target.Tick(.016f);
        Require(target.InputCalls==0,"airborne flight was countersteered by A/D movement");
        target.HideTarget();
        Require(!target.bTrainerAirborne&&target.Move.PendingLaunchVelocity.IsNearlyZero(),
            "hidden target retained a queued native launch");
        target.ActivateAirborneTarget(FVector(2000,0,1700),FVector::ZeroVector);
        Require(target.Move.Mode==MOVE_Falling&&target.Move.PendingLaunchVelocity.IsNearlyZero(),
            "zero-impulse falling target did not enter gravity physics");
        target.ActivateTarget(FVector(900,0,108),true);
        Require(!target.bTrainerAirborne&&target.bTrainerStrafe&&target.Move.Mode==MOVE_Walking,
            "pooled target carried airborne state into normal training");
    } else if(name=="airborne_flight_rate") {
        for(float baseGravityScale:{.8f,1.f,1.25f}) for(float rate:{.93f,.95f,1.f}) {
            ANCAimTrainerTarget defaults;defaults.Move.GravityScale=baseGravityScale;
            for(float initialZ:{-300.f,0.f,900.f}) {
                auto target=Active();target.Class.Defaults=&defaults;
                const FVector initialVelocity(350.f,-220.f,initialZ);
                target.ActivateAirborneTarget(FVector(2000,0,1700),initialVelocity,rate);
                const FVector actualVelocity=target.Move.PendingLaunchVelocity;
                Require(target.TrainerFlightRate==rate&&target.Move.Mode==MOVE_Falling&&target.Launches==1
                    &&actualVelocity.X==initialVelocity.X&&actualVelocity.Y==initialVelocity.Y,
                    "fall-rate control changed horizontal launch or bypassed native falling");
                for(float normalTime:{.125f,.5f,1.2f}) {
                    const float slowedTime=normalTime/rate;
                    const float normalHeight=1700.f+initialZ*normalTime+.5f*defaults.Move.GetGravityZ()*normalTime*normalTime;
                    const float slowedHeight=target.Position.Z+actualVelocity.Z*slowedTime
                        +.5f*target.Move.GetGravityZ()*slowedTime*slowedTime;
                    const float normalSpeed=initialZ+defaults.Move.GetGravityZ()*normalTime;
                    const float slowedSpeed=actualVelocity.Z+target.Move.GetGravityZ()*slowedTime;
                    Require(std::abs(normalHeight-slowedHeight)<.002f&&std::abs(slowedSpeed-normalSpeed*rate)<.002f,
                        "vertical rate does not preserve matched height with the requested7%/5% speed reduction");
                }
                // A replicated pawn extrapolates with the same gravity and its
                // normal movement-replicated velocity, including late joining.
                auto client=Active();client.Class.Defaults=&defaults;client.Role=1;
                client.TrainerFlightRate=target.TrainerFlightRate;client.OnRep_TrainerFlightRate();
                Require(client.Move.GetGravityZ()==target.Move.GetGravityZ()&&client.Launches==0&&client.Teleports==0,
                    "replicated rate disagrees with authority or relaunches the client pawn");
            }
        }
    } else if(name=="airborne_flight_rate_reset") {
        ANCAimTrainerTarget defaults;defaults.Move.GravityScale=1.25f;
        auto target=Active();target.Class.Defaults=&defaults;
        const FVector location(2000,0,1700),velocity(150,200,-300);
        for(float rate:{.93f,.95f,.93f}) {
            target.ActivateAirborneTarget(location,velocity,rate);
            target.ActivateAirborneTarget(location,velocity,rate);
            Require(std::abs(target.Move.GravityScale-1.25f*rate*rate)<.00001f,
                "reactivation compounded a previous fall's gravity scale");
            target.HideTarget();
            Require(target.TrainerFlightRate==1.f&&target.Move.GravityScale==1.25f
                &&target.Move.Mode==MOVE_None&&target.Move.PendingLaunchVelocity.IsNearlyZero(),
                "hidden pooled target retained reduced gravity or a queued launch");
            target.ActivateAirborneTarget(location,velocity,rate);
            target.ActivateTarget(FVector(900,0,108),true);
            Require(target.TrainerFlightRate==1.f&&target.Move.GravityScale==1.25f&&target.Move.Mode==MOVE_Walking,
                "grounded target inherited the airborne drop's reduced gravity");
            target.ActivateAirborneTarget(location,velocity,rate);
            target.ActivateAirborneTarget(FVector(100,-1400,288),FVector(500,1300,2300));
            Require(target.TrainerFlightRate==1.f&&target.Move.GravityScale==1.25f
                &&target.Move.PendingLaunchVelocity.Z==2300.f,
                "jump-pad appearance inherited a falling target's vertical rate");
            Require(target.LaunchAirborneTarget(FVector(-500,-1300,2100))
                &&target.Move.PendingLaunchVelocity.Z==2100.f&&target.Move.PendingLaunchVelocity.Y==-1300.f,
                "repeat pad launch changed its explicitly calculated ballistic velocity");
        }
        auto client=Active();client.Class.Defaults=&defaults;client.Role=1;
        for(float rate:{.93f,1.f,.95f,1.f}) {
            client.TrainerFlightRate=rate;client.OnRep_TrainerFlightRate();
            Require(std::abs(client.Move.GravityScale-defaults.Move.GravityScale*rate*rate)<.00001f,
                "client retained or compounded gravity after a replicated appearance change");
        }
    } else if(name=="airborne_flight_rate_guards") {
        ANCAimTrainerTarget defaults;defaults.Move.GravityScale=1.25f;
        for(float rate:{0.f,-.1f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            auto target=Active();target.Class.Defaults=&defaults;target.Move.GravityScale=1.25f;
            target.ActivateAirborneTarget(FVector(2000,0,1700),FVector(100,200,300),rate);
            Require(target.Teleports==0&&target.Launches==0&&target.TrainerFlightRate==1.f&&target.Move.GravityScale==1.25f,
                "invalid fall rate mutated native target state");
        }
        auto target=Active();target.Class.Defaults=&defaults;target.Role=1;
        target.ActivateAirborneTarget(FVector(2000,0,1700),FVector(100,200,300),.93f);
        Require(target.Teleports==0&&target.Launches==0&&target.TrainerFlightRate==1.f,
            "client changed the authoritative fall rate or appearance");
    } else if(name=="airborne_guards") {
        for(int condition=0;condition<4;++condition) {
            auto target=Active();target.bTrainerAirborne=true;
            FVector velocity(0,1000,2000);
            if(condition==0)target.Role=1;
            if(condition==1)target.bTrainerVisible=false;
            if(condition==2)target.bTrainerAirborne=false;
            if(condition==3)velocity.Y=std::numeric_limits<float>::infinity();
            Require(!target.LaunchAirborneTarget(velocity)&&target.Launches==0,
                "native launch accepted invalid authority, lifecycle, or velocity");
        }
        auto target=Active();target.ActivateAirborneTarget(FVector(0,0,1700),FVector::ZeroVector);
        target.StartWiggle(50.f);
        Require(!target.bTrainerWiggle&&!target.bTrainerStrafe&&target.Move.Mode==MOVE_Falling,
            "popup movement took over airborne target");
        const int teleports=target.Teleports;
        target.ActivateAirborneTarget(FVector(std::numeric_limits<float>::quiet_NaN(),0,0),FVector::ZeroVector);
        Require(target.Teleports==teleports,"nonfinite airborne spawn teleported target");
    } else if(name=="speed_scale") {
        ANCAimTrainerTarget defaults;defaults.Move.MaxWalkSpeed=940.f;defaults.Move.MaxWalkSpeedCrouched=315.f;
        defaults.Move.MaxFloorSlideSpeed=900.f;
        auto target=Active();target.Class.Defaults=&defaults;
        target.SetTrainerSpeedScale(1.3f);target.SetTrainerSpeedScale(1.3f);
        Require(std::abs(target.Move.MaxWalkSpeed-1222.f)<.01f&&std::abs(target.Move.MaxWalkSpeedCrouched-409.5f)<.01f
            &&std::abs(target.Move.DodgeImpulseHorizontal-1950.f)<.01f
            &&std::abs(target.Move.DodgeMaxHorizontalVelocity-2210.f)<.01f
            &&std::abs(target.Move.MaxInitialFloorSlideSpeed-1755.f)<.01f
            &&std::abs(target.Move.MaxFloorSlideSpeed-1170.f)<.01f,
            "hard tracking speeds compounded or ignored the target variant defaults");
        Require(target.Move.DodgeImpulseVertical==defaults.Move.DodgeImpulseVertical
            &&target.Move.Gravity==defaults.Move.Gravity,
            "horizontal speed boost changed gravity or jump impulse");
        Require(std::abs(target.Move.MaxAcceleration-defaults.Move.MaxAcceleration*1.3f)<.01f
            &&std::abs(target.Move.FastInitialAcceleration-15600.f)<.01f
            &&std::abs(target.Move.MaxFastAccelSpeed-260.f)<.01f
            &&std::abs(target.Move.DodgeLandingAcceleration-1300.f)<.01f
            &&std::abs(target.Move.FloorSlideAcceleration-520.f)<.01f
            &&std::abs(target.Move.DefaultBrakingDecelerationWalking-2600.f)<.01f
            &&std::abs(target.Move.BrakingDecelerationWalking-2600.f)<.01f,
            "short reversals, landing, or slide acceleration did not receive the speed scale");
        for(float speed:{0.f,100.f,199.f,250.f,900.f}) {
            defaults.Move.Velocity=FVector(0,speed,0);target.Move.Velocity=FVector(0,speed*1.3f,0);
            Require(std::abs(target.Move.GetMaxAcceleration()-defaults.Move.GetMaxAcceleration()*1.3f)<.01f,
                "UT's initial acceleration blend breaks30% normalized short-strafe speed");
        }
        target.SetTrainerSpeedScale(std::numeric_limits<float>::quiet_NaN());
        target.SetTrainerSpeedScale(0.f);
        Require(std::abs(target.Move.MaxWalkSpeed-1222.f)<.01f,"invalid scale mutated movement");
        target.HideTarget();
        Require(target.Move.MaxWalkSpeed==940.f&&target.Move.MaxFloorSlideSpeed==900.f
            &&target.Move.MaxAcceleration==defaults.Move.MaxAcceleration&&target.Move.FastInitialAcceleration==12000.f
            &&target.Move.BrakingDecelerationWalking==2000.f&&target.Move.FloorSlideAcceleration==400.f,
            "target reuse leaked hard tracking speed into a different preset");
        target.Role=1;target.SetTrainerSpeedScale(1.3f);
        Require(target.Move.MaxWalkSpeed==940.f,"client changed target movement speed");
    } else if(name=="hard_slide_range") {
        ANCAimTrainerTarget defaults;defaults.Move.MaxFloorSlideSpeed=1100.f;
        for(float scale:{1.f,1.3f}) {
            const float threshold=scale>1.f?300.f:500.f;
            for(float offset:{-500.f,-300.f,-299.99f,0.f,299.99f,300.f,500.f}) {
                for(float roll:{0.f,1.f}) {
                    auto target=Active();target.Class.Defaults=&defaults;target.SetTrainerSpeedScale(scale);
                    target.Position.Y=offset;
                    Require(target.TryTrainerTrackingSlide(roll),"valid hard/normal tracking slide failed");
                    const float direction=offset>=threshold?-1.f:offset<=-threshold?1.f:roll<.5f?-1.f:1.f;
                    Require(target.LastInput.Y==direction&&target.TrainerSlideDirection.Y==direction,
                        "tracking slide did not use the speed-specific inward threshold");
                    const float travel=target.Move.MaxFloorSlideSpeed*(.7f+2.f/30.f)
                        +target.Move.MaxFloorSlideSpeed*.4f/30.f+target.Move.MaxFloorSlideSpeed*.4f/14.f;
                    const float endpoint=offset+direction*travel;
                    Require(1000.f*1000.f+endpoint*endpoint+151.f*151.f<1800.f*1800.f,
                        "faster lateral slide leaves the fixed trainee's Link beam range");
                }
            }
        }
    } else if(name=="headshot_scale") {
        ANCAimTrainerTarget defaults;
        auto target=Active();target.Class.Defaults=&defaults;
        Require(!target.IsHeadShot(FVector(),FVector(),1.f,nullptr,.125f)
            &&target.HeadScaleUsed==1.f&&target.HeadPrediction==.125f,
            "default popup target received HS-only enlargement or lost rewind prediction");
        target.SetTrainerHeadshotScale(1.15f);
        Require(target.IsHeadShot(FVector(),FVector(),1.f,nullptr,.2f)&&target.HeadScaleUsed==1.f
            &&std::abs(target.HeadRadius-20.7f)<.001f&&target.HeadScale==1.f
            &&target.HeadPrediction==.2f,"HS target did not enlarge native radius without changing visible head scale");
        auto client=Active();client.Class.Defaults=&defaults;client.Role=1;
        client.TrainerHeadshotScale=target.TrainerHeadshotScale;client.OnRep_TrainerHeadshotScale();
        const float inlineClaimRadius=client.HeadRadius*client.HeadScale*1.f;
        Require(std::abs(inlineClaimRadius-20.7f)<.001f
            &&client.IsHeadShot(FVector(),FVector(),1.f,nullptr,0.f),
            "client-claimed inline head geometry and normal headshot validation disagree");
        target.SetTrainerHeadshotScale(1.15f);
        Require(std::abs(target.HeadRadius-20.7f)<.001f,"headshot radius compounded across appearances");
        target.SetTrainerHeadshotScale(2.f);target.SetTrainerHeadshotScale(std::numeric_limits<float>::quiet_NaN());
        Require(target.TrainerHeadshotScale==1.15f,"unbounded headshot multiplier accepted");
        target.SetTrainerHeadshotScale(1.f);target.Role=1;target.SetTrainerHeadshotScale(1.15f);
        Require(target.TrainerHeadshotScale==1.f&&target.HeadRadius==18.f&&target.HeadScale==1.f,
            "client expanded authoritative geometry or next normal preset kept enlarged radius");
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
            "void ANCAimTrainerTarget::ActivateAirborneTarget",
            "void ANCAimTrainerTarget::OnRep_TrainerFlightRate",
            "bool ANCAimTrainerTarget::LaunchAirborneTarget",
            "void ANCAimTrainerTarget::SetTrainerSpeedScale",
            "void ANCAimTrainerTarget::StartWiggle",
            "bool ANCAimTrainerTarget::StartPopupLongStrafe",
            "bool ANCAimTrainerTarget::SetTrainerCrouched",
            "void ANCAimTrainerTarget::HideTarget",
            "void ANCAimTrainerTarget::ResetTargetMovement",
            "void ANCAimTrainerTarget::ReverseStrafe",
            "void ANCAimTrainerTarget::ConfigurePopupStrafe",
            "void ANCAimTrainerTarget::SetPopupStrafeAxis",
            "bool ANCAimTrainerTarget::SetPopupMovement",
            "bool ANCAimTrainerTarget::NeedsPopupMovementDecision",
            "bool ANCAimTrainerTarget::TryTrainerDodge",
            "bool ANCAimTrainerTarget::TryTrainerPopupDodge",
            "bool ANCAimTrainerTarget::TryTrainerSlideForward",
            "bool ANCAimTrainerTarget::TryTrainerPopupSlide",
            "bool ANCAimTrainerTarget::TryTrainerTrackingSlide",
            "bool ANCAimTrainerTarget::StartTrainerSlide",
            "bool ANCAimTrainerTarget::IsTrainerSliding",
            "void ANCAimTrainerTarget::Tick",
            "FVector ANCAimTrainerTarget::GetHeadLocation",
            "void ANCAimTrainerTarget::SetTrainerHeadshotScale",
            "void ANCAimTrainerTarget::OnRep_TrainerHeadshotScale",
            "void ANCAimTrainerTarget::NotifyBlockedHeadShot",
        )
        source = directory / "trainer_targets.cpp"
        source.write_text("\n".join([ADAPTER, f'#include "{policy}"', f'#include "{layout}"']
            + [native_function(movement,"float UUTCharacterMovement::GetMaxAcceleration").replace("MovementMode", "Mode"),
               native_function(movement,"void UUTCharacterMovement::PerformFloorSlide"),
               native_function(movement,"void UUTCharacterMovement::ProcessLanded")]
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

    def test_popup_evasion_keeps_moving_in_two_dimensions_inside_safe_bounds(self): self.run_case("popup_travel")
    def test_popup_evasion_areas_allow_varied_native_dodges_and_defer_decisions_through_recovery(self): self.run_case("popup_evasion_dodges")
    def test_left_popup_dodger_checks_full_native_landing_path(self): self.run_case("popup_left_dodge")

    def test_hard_tracking_slide_turns_early_enough_to_stay_within_beam_range(self): self.run_case("hard_slide_range")
    def test_headshot_only_scale_preserves_native_weapon_scale_and_prediction(self): self.run_case("headshot_scale")
    def test_native_airborne_launch_fall_relaunch_and_pooled_identity(self): self.run_case("airborne_launch")
    def test_airborne_rate_scales_vertical_time_and_client_gravity(self): self.run_case("airborne_flight_rate")
    def test_airborne_rate_resets_on_hide_grounded_and_jumppad_reuse(self): self.run_case("airborne_flight_rate_reset")
    def test_airborne_rate_rejects_invalid_values_and_client_activation(self): self.run_case("airborne_flight_rate_guards")
    def test_airborne_authority_input_and_nonfinite_guards(self): self.run_case("airborne_guards")
    def test_hard_tracking_speed_uses_variant_defaults_and_resets_on_reuse(self): self.run_case("speed_scale")

    def test_reversals_require_authority_visible_grounded_strafe_target(self): self.run_case("reverse")
    def test_dodge_uses_native_action_and_inward_direction_policy(self): self.run_case("dodge")
    def test_appearance_lifecycle_clears_motion_and_prior_history(self): self.run_case("lifecycle")
    def test_client_cannot_change_target_lifecycle(self): self.run_case("client_lifecycle")
    def test_airborne_dodges_are_not_countersteered_by_tracking_input(self): self.run_case("airborne_tick")
    def test_controllerless_landing_acceleration_expires_at_stock_deadline(self): self.run_case("landing_recovery")
    def test_wiggle_rejects_invalid_width_hidden_target_and_client_requests(self): self.run_case("wiggle_guards")
    def test_wiggle_boundaries_speed_and_no_dodge_reset_on_next_appearance(self): self.run_case("wiggle_boundaries")
    def test_random_popup_start_preserves_fixed_lane_and_native_movement(self): self.run_case("popup_anchor")
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
    def test_popup_dodge_guards_and_native_rejection(self): self.run_case("popup_dodge_guards")
    def test_popup_dodge_validates_horizontal_direction_and_native_perpendicular(self): self.run_case("popup_dodge_direction")
    def test_popup_dodge_preserves_native_landing_and_recenters_without_teleport(self): self.run_case("popup_dodge_recovery")
    def test_popup_dodge_state_cannot_leak_into_reused_targets_or_client_tick(self): self.run_case("popup_dodge_reuse")
    def test_popup_dodge_checks_actual_profile_momentum_and_world_lane(self): self.run_case("popup_dodge_geometry")
    def test_popup_dodge_slide_runs_native_landing_physics_and_recovery(self): self.run_case("popup_dodge_slide_chain")
    def test_popup_dodge_slide_request_clears_when_hidden_or_reused(self): self.run_case("popup_dodge_slide_reuse")
    def test_popup_slide_variants_turn_inward_and_reject_deep_lane(self): self.run_case("popup_slide_variants")


if __name__ == "__main__":
    unittest.main()
