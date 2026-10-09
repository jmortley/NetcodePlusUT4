"""Native layout tests for support, cover and the trainer's fixed sightlines.

Head samples transform a conservative stock 184..212 unit standing band by
the real profile capsule/mesh seat/scale, not a claim of fixed animation.
A packaged playtest must verify the actual head pose.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function



ARENA_ADAPTER = r'''
#include <vector>
#include <cstdlib>
#include <cmath>
using int32=int;
#define TEXT(value) value
enum ENetMode { NM_Standalone,NM_Client,NM_ListenServer,NM_DedicatedServer };
enum { LOAD_NoWarn=1,LOAD_Quiet=2 };
struct UMaterialInterface {};
struct FBoxSphereBounds { FVector Origin,BoxExtent; };
struct UStaticMesh {
    FBoxSphereBounds Bounds;
    FBoxSphereBounds GetBounds() const { return Bounds; }
};
struct FTransform {
    FVector Location,Scale;
    FVector GetLocation() const { return Location; }
    FVector GetScale3D() const { return Scale; }
};
UMaterialInterface GridMaterial,DeckGooMaterial;
UStaticMesh CubeMesh={ {FVector(0),FVector(50)} };
UStaticMesh DeckSheetMesh={ {FVector(10,-20,3),FVector(250,250,0)} };
bool GooAvailable=true,GooMeshAvailable=true;
int GooLoadCalls=0;
template<class T> T* LoadObject(void*,const char*,void*,unsigned flags);
template<> UMaterialInterface* LoadObject(void*,const char*,void*,unsigned flags) {
    if(flags!=(LOAD_NoWarn|LOAD_Quiet))std::exit(6);
    ++GooLoadCalls;
    return GooAvailable?&DeckGooMaterial:nullptr;
}
template<> UStaticMesh* LoadObject(void*,const char*,void*,unsigned flags) {
    if(flags!=(LOAD_NoWarn|LOAD_Quiet))std::exit(6);
    ++GooLoadCalls;
    return GooMeshAvailable?&DeckSheetMesh:nullptr;
}
enum class ECollisionEnabled { NoCollision,QueryAndPhysics };
struct UStaticMeshComponent {
    bool Hidden=false;
    UMaterialInterface* Material=&GridMaterial;
    UStaticMesh* Mesh=&CubeMesh;
    FVector Scale=FVector(64,36,.1f),Location=FVector(0,0,15);
    ECollisionEnabled Collision=ECollisionEnabled::NoCollision;
    void SetMaterial(int,UMaterialInterface* material) { Material=material; }
    void SetStaticMesh(UStaticMesh* mesh) { Mesh=mesh; }
    UStaticMesh* GetStaticMesh() const { return Mesh; }
    FTransform GetRelativeTransform() const { return {Location,Scale}; }
    void SetRelativeScale3D(FVector scale) { Scale=scale; }
    void SetRelativeLocation(FVector location) { Location=location; }
    void SetHiddenInGame(bool hidden) { Hidden=hidden; }
    void SetCollisionEnabled(ECollisionEnabled collision) { Collision=collision; }
};
template<class T> struct TArray : std::vector<T> { int Num() const { return int(this->size()); } };
struct Actor { int BeginCalls=0;void BeginPlay() { ++BeginCalls; } };
struct ANCAimTrainerArena : Actor {
    using Super=Actor;
    ENetMode NetMode=NM_Standalone;
    ENetMode GetNetMode() const { return NetMode; }
    int Scenario=0;
    TArray<UStaticMeshComponent*> Cover,AirbornePlatforms,AirbornePadVisuals;
    UStaticMeshComponent* GooSurface=nullptr;
    void BeginPlay();
    void OnRep_Scenario();
};
'''
ARENA_CASE = r'''
void ArenaGooMaterial() {
    for(auto mode:{NM_Standalone,NM_Client,NM_ListenServer,NM_DedicatedServer}) {
        for(bool available:{true,false}) for(bool meshAvailable:{true,false}) for(int initialScenario:{3,4}) {
            GooAvailable=available;GooMeshAvailable=meshAvailable;GooLoadCalls=0;
            ANCAimTrainerArena arena;
            UStaticMeshComponent goo;
            arena.GooSurface=&goo;arena.Scenario=initialScenario;arena.NetMode=mode;
            // Scenario can replicate before BeginPlay replaces the cube with a sheet.
            arena.OnRep_Scenario();
            if(std::abs(goo.Location.Z+5.f-NCAimTrainerLayout::AirborneHazardHeight(initialScenario==4))>.001f)std::exit(14);
            arena.BeginPlay();
            const bool rendering=mode!=NM_DedicatedServer;
            const bool gooReady=rendering&&available&&meshAvailable;
            if(arena.BeginCalls!=1||GooLoadCalls!=(rendering?2:0))std::exit(7);
            if(goo.Material!=(gooReady?&DeckGooMaterial:&GridMaterial)
                ||goo.Mesh!=(gooReady?&DeckSheetMesh:&CubeMesh))std::exit(8);
            const auto bounds=goo.Mesh->GetBounds();
            if(std::abs(2*bounds.BoxExtent.X*goo.Scale.X-6400.f)>.001f
                ||std::abs(2*bounds.BoxExtent.Y*goo.Scale.Y-3600.f)>.001f
                ||std::abs(goo.Location.X+bounds.Origin.X*goo.Scale.X)>.001f
                ||std::abs(goo.Location.Y+bounds.Origin.Y*goo.Scale.Y)>.001f)std::exit(11);
            for(int scenario:{initialScenario,4,4,3,0,4,3}) {
                arena.Scenario=scenario;arena.OnRep_Scenario();
                const bool airborne=scenario==3||scenario==4;
                const float top=goo.Location.Z+(bounds.Origin.Z+bounds.BoxExtent.Z)*goo.Scale.Z;
                if(std::abs(top-NCAimTrainerLayout::AirborneHazardHeight(scenario==4))>.001f)std::exit(15);
                if(goo.Hidden==airborne||goo.Collision!=ECollisionEnabled::NoCollision)std::exit(9);
                if(GooLoadCalls!=(rendering?2:0)||goo.Material!=(gooReady?&DeckGooMaterial:&GridMaterial))std::exit(10);
            }
        }
    }
}
void ArenaScenarioReplication() {
    ANCAimTrainerArena arena;
    UStaticMeshComponent covers[8],platforms[3],pads[2],goo;
    for(auto& block:covers)arena.Cover.push_back(&block);
    for(auto& block:platforms)arena.AirbornePlatforms.push_back(&block);
    for(auto& pad:pads)arena.AirbornePadVisuals.push_back(&pad);
    arena.GooSurface=&goo;
    for(int scenario:{3,4,4,3,2,4,1,0,3,4,0}) {
        arena.Scenario=scenario;arena.OnRep_Scenario();
        const bool airborne=scenario==3||scenario==4,rockets=scenario==4;
        for(int index=0;index<8;++index) {
            const bool enabled=index<5?scenario==1:scenario==2;
            if(covers[index].Hidden==enabled||(covers[index].Collision==ECollisionEnabled::QueryAndPhysics)!=enabled)std::exit(2);
        }
        for(int index=0;index<3;++index) {
            const auto& block=platforms[index];
            if(block.Hidden==airborne||(block.Collision==ECollisionEnabled::QueryAndPhysics)!=airborne)std::exit(3);
            const auto geometry=index==0?NCAimTrainerLayout::AirborneFiringLedge(rockets)
                :NCAimTrainerLayout::AirborneJumpPad(index-1,rockets);
            if(block.Location.X!=geometry.CenterX||block.Location.Y!=geometry.CenterY||block.Location.Z!=geometry.Height*.5f
                ||std::abs(block.Scale.X*100.f-geometry.SizeX)>.001f||std::abs(block.Scale.Y*100.f-geometry.SizeY)>.001f
                ||std::abs(block.Scale.Z*100.f-geometry.Height)>.001f)std::exit(12);
        }
        for(int index=0;index<2;++index) {
            const auto& pad=pads[index];
            if(pad.Hidden==airborne||pad.Collision!=ECollisionEnabled::NoCollision)std::exit(4);
            const auto geometry=NCAimTrainerLayout::AirborneJumpPad(index,rockets);
            if(pad.Location.X!=geometry.CenterX||pad.Location.Y!=geometry.CenterY||pad.Location.Z!=geometry.Height+2.f
                ||pad.Scale.X!=(rockets?.5f:1.f)||pad.Scale.Y!=(rockets?.5f:1.f)||pad.Scale.Z!=1.f)std::exit(13);
        }
        if(goo.Hidden==airborne||goo.Collision!=ECollisionEnabled::NoCollision
            ||std::abs(goo.Location.Z+5.f-NCAimTrainerLayout::AirborneHazardHeight(rockets))>.001f)std::exit(5);
    }
}
'''

CASES = r'''
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace NCAimTrainerLayout;
bool InstagibGeometry = true;
int PopupVariant = 0;
FSeat TestPopupSeat(int index) { return PopupSeat(index, PopupVariant); }
NCAimTrainerCharacterProfile::FProfile Profile() {
    return InstagibGeometry ? NCAimTrainerCharacterProfile::Instagib() : NCAimTrainerCharacterProfile::TeamArena();
}
float TestRadius() { return Profile().CapsuleRadius; }
float TestHalfHeight() { return Profile().CapsuleHalfHeight; }
float TestHeadHeight(float OldHeight) { return Profile().CapsuleHalfHeight + Profile().MeshZ + Profile().MeshScale * OldHeight; }
struct Point { float X, Y, Z; };
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
float SliderMaximumTravel() {
    // Decoded Blueprint flat-ground slide starts at1100, lasts0.7s and
    // exits at40% speed. Allow two30Hz frames beyond the timer and a full
    // exit frame before direction friction14; ignore the walk-speed clamp.
    const float slideSpeed=1100.f, duration=.7f, exitFactor=.4f, groundFriction=14.f;
    const float exitSpeed=slideSpeed*exitFactor;
    return slideSpeed*(duration+2.f/30.f)+exitSpeed/30.f+exitSpeed/groundFriction;
}
FSeat PopupMovementSeat(int index) {
    FSeat seat=TestPopupSeat(index);
    if (index==PopupSliderSlot || index==0) seat.MinX-=SliderMaximumTravel();
    if (index==0) seat.WiggleRange=PopupLongStrafeRange;
    if (index==4 && PopupVariant!=2) {
        // A slide starts anywhere inside the initial wiggle, then preserves
        // that Y endpoint as the new wiggle center. Bound both appearances.
        const float extra=SliderMaximumTravel()+seat.WiggleRange+WiggleSafetyMargin;
        seat.CenterY+=(PopupVariant==1 ? -1.f : 1.f)*extra*.5f; seat.WiggleRange+=extra*.5f;
    }
    return seat;
}
FSeat PopupOcclusionSeat(int index) {
    // The inward-sliding near target may deliberately cross rear sightlines.
    // Its initial seat and the stationary/platform seats remain separated.
    return index==4 ? TestPopupSeat(index) : PopupMovementSeat(index);
}
std::vector<Point> Endpoints(const FSeat& seat) {
    const float reach = seat.SpawnJitterY + seat.WiggleRange + WiggleSafetyMargin;
    std::vector<Point> points;
    for (float fraction : {0.f,.25f,.5f,.75f,1.f}) {
        const float x=seat.MinX+(seat.MaxX-seat.MinX)*fraction;
        for (float y : {seat.CenterY-reach, seat.CenterY-reach*.5f, seat.CenterY,
                        seat.CenterY+reach*.5f, seat.CenterY+reach}) {
            points.push_back({x,y,seat.FloorZ});
        }
    }
    return points;
}
bool SegmentHitsBox(Point end, float minX, float maxX, float minY, float maxY, float minZ, float maxZ) {
    // Eye follows the selected character class, including the smaller IG seat.
    const float start[3] = {-1800.f,0.f,Profile().CapsuleHalfHeight + Profile().StandingEyeHeight};
    const float finish[3] = {end.X,end.Y,end.Z};
    const float low[3] = {minX,minY,minZ}, high[3] = {maxX,maxY,maxZ};
    float near = 0.f, far = 1.f;
    for (int axis=0; axis<3; ++axis) {
        const float dir = finish[axis]-start[axis];
        if (std::abs(dir) < .00001f) {
            if (start[axis] < low[axis] || start[axis] > high[axis]) return false;
        } else {
            float a=(low[axis]-start[axis])/dir, b=(high[axis]-start[axis])/dir;
            if (a>b) std::swap(a,b);
            near=std::max(near,a); far=std::min(far,b);
            if (near>far) return false;
        }
    }
    return near<=far;
}
bool HitsBlock(Point end, FBlock block) {
    return SegmentHitsBox(end,block.CenterX-block.SizeX*.5f,block.CenterX+block.SizeX*.5f,
        block.CenterY-block.SizeY*.5f,block.CenterY+block.SizeY*.5f,0.f,block.Height);
}
void PlatformSupport() {
    Require(TargetCount==6 && PopupSlotCount==5 && PopupSliderSlot==2 && PopupDodgerSlot==5
            && PopupDodgerSlot+1==TargetCount && HeadSlotCount==5 && PopupPlatformCount==3,
            "pool and layout counts disagree");
    Require(NCAimTrainerCharacterProfile::Acceleration*(1.f/30.f)*(1.f/30.f) < WiggleSafetyMargin,
        "predictive reversal margin cannot cover one frame of acceleration");
    for (int slot=0;slot<PopupSlotCount;++slot) {
        const FSeat seat=PopupMovementSeat(slot);
        for (Point point:Endpoints(seat)) {
            Require(point.X-TestRadius()>-3200.f && point.X+TestRadius()<3200.f
                    && point.Y-TestRadius()>-1800.f && point.Y+TestRadius()<1800.f,
                    "target swept capsule can leave room floor");
            if (slot<PopupPlatformCount) {
                const FBlock block=PopupPlatform(slot);
                Require(seat.FloorZ==block.Height, "spawn floor and platform top disagree");
                Require(point.X-TestRadius()>=block.CenterX-block.SizeX*.5f
                        && point.X+TestRadius()<=block.CenterX+block.SizeX*.5f
                        && point.Y-TestRadius()>=block.CenterY-block.SizeY*.5f
                        && point.Y+TestRadius()<=block.CenterY+block.SizeY*.5f,
                        "wiggle plus stopping margin can run off platform");
            } else {
                Require(seat.FloorZ==0.f, "floor seat is floating");
                for (int p=0;p<PopupPlatformCount;++p) {
                    const FBlock block=PopupPlatform(p);
                    const bool separate=point.X+TestRadius()<block.CenterX-block.SizeX*.5f
                        || point.X-TestRadius()>block.CenterX+block.SizeX*.5f
                        || point.Y+TestRadius()<block.CenterY-block.SizeY*.5f
                        || point.Y-TestRadius()>block.CenterY+block.SizeY*.5f;
                    Require(separate,"floor target overlaps a raised platform");
                }
            }
        }
    }
}
void PopupSightlines() {
    for (int slot=0;slot<PopupSlotCount;++slot) {
        for (Point point:Endpoints(PopupMovementSeat(slot))) {
            for (float headHeight:{184.f,198.f,212.f}) {
                const Point head={point.X,point.Y,point.Z+TestHeadHeight(headHeight)};
                for (int p=0;p<PopupPlatformCount;++p) {
                    Require(!HitsBlock(head,PopupPlatform(p)), "platform blocks a standing target head center");
                }
            }
            // This preserved block was authored for the shorter IG profile.
            // TeamArena exposes more shoulder above it, as before this change.
            if (slot==3 && InstagibGeometry) {
                Require(HitsBlock({point.X,point.Y,TestHeadHeight(170.f)},PopupPlatform(1)),
                        "rear target exposes its upper body instead of peeking over cover");
            }
        }
    }
}
void HeadCoverSightlines() {
    InstagibGeometry = false;
    for (int slot=0;slot<HeadSlotCount;++slot) {
        for (Point point:Endpoints(HeadSeat(slot))) {
            for (float headHeight:{184.f,198.f,212.f}) {
                for (int block=0;block<HeadSlotCount;++block) {
                    Require(!HitsBlock({point.X,point.Y,TestHeadHeight(headHeight)},HeadCover(block)),
                            "head station cover hides a valid standing head center");
                }
            }
            for (float bodySide:{-TestRadius(),0.f,TestRadius()}) {
                Require(HitsBlock({point.X,point.Y+bodySide,TestHeadHeight(170.f)},HeadCover(slot)),
                        "head station exposes an inside shoulder at its wiggle limit");
            }
        }
    }
}
void OtherTargetOcclusion() {
    // Boxes conservatively enclose full target capsules. Boundary/interior
    // pairs protect the center peek lane and the near-left angular separation.
    for (int slot=0;slot<PopupSlotCount;++slot) {
        for (Point point:Endpoints(PopupMovementSeat(slot))) {
            for (int other=0;other<PopupSlotCount;++other) {
                if (other==slot) continue;
                // The deep-left spawn is initially clear. A different left
                // target may subsequently slide/dodge across its sightline,
                // just as the existing foreground dodger can cross rear heads.
                const FSeat obstacleSeat=PopupVariant==2&&slot==4&&other==0
                    ? TestPopupSeat(other) : PopupOcclusionSeat(other);
                for (Point obstacle:Endpoints(obstacleSeat)) {
                    for (float headHeight:{184.f,212.f}) {
                        const bool obscured=SegmentHitsBox({point.X,point.Y,point.Z+TestHeadHeight(headHeight)},
                            obstacle.X-TestRadius(),obstacle.X+TestRadius(),
                            obstacle.Y-TestRadius(),obstacle.Y+TestRadius(),
                            obstacle.Z,obstacle.Z+2.f*TestHalfHeight());
                        if(obscured) {
                            std::cerr<<"variant="<<PopupVariant<<" slot="<<slot<<" other="<<other
                                <<" point="<<point.X<<","<<point.Y<<" obstacle="<<obstacle.X<<","<<obstacle.Y<<'\n';
                        }
                        Require(!obscured,"another target capsule can cover this target's head center");
                    }
                }
            }
        }
    }
}
void SliderRunway() {
    Require(PopupSliderSlot==2, "slide lane must use the elevated right platform");
    const FSeat seat=TestPopupSeat(PopupSliderSlot);
    const FBlock platform=PopupPlatform(PopupSliderSlot);
    Require(seat.MinX==1000.f && seat.MaxX==2200.f && seat.FloorZ==320.f,
        "upper-right spawn runway no longer matches the native slide preset");
    Require(seat.FloorZ==platform.Height && seat.CenterY==platform.CenterY,
        "slide lane is not centered on its supporting platform");
    const float minX=platform.CenterX-platform.SizeX*.5f;
    const float maxX=platform.CenterX+platform.SizeX*.5f;
    const float minY=platform.CenterY-platform.SizeY*.5f;
    const float maxY=platform.CenterY+platform.SizeY*.5f;
    for (Point spawn:Endpoints(seat)) {
        for (float fraction:{0.f,.25f,.5f,.75f,1.f}) {
            const Point slid={spawn.X-SliderMaximumTravel()*fraction,spawn.Y,spawn.Z};
            Require(slid.X-TestRadius()>minX && slid.X+TestRadius()<maxX,
                "native forward slide can carry its capsule off the platform");
            Require(slid.Y-TestRadius()>minY && slid.Y+TestRadius()<maxY,
                "wiggle envelope around the slide can leave a lateral edge");
            Require(slid.X-TestRadius()>-3200.f && slid.X+TestRadius()<3200.f
                && slid.Y-TestRadius()>-1800.f && slid.Y+TestRadius()<1800.f,
                "sliding capsule can leave the arena bounds");
        }
    }
}
void LeftMotionLanes() {
    for (bool ig:{false,true}) {
        InstagibGeometry=ig;
        PlatformSupport();
        const FSeat rear=PopupMovementSeat(0),near=PopupMovementSeat(4);
        const FBlock support=PopupPlatform(0);
        Require(TestPopupSeat(0).MinX>=1000.f&&TestPopupSeat(0).SpawnJitterY<=50.f
            &&rear.WiggleRange==180.f,"rear-left long strafe lost its required runway or safe lateral extent");
        for(Point point:Endpoints(rear)) {
            Require(point.Y-TestRadius()>support.CenterY-support.SizeY*.5f
                &&point.Y+TestRadius()<support.CenterY+support.SizeY*.5f,
                "rear-left long strafe leaves platform support");
        }
        for(Point point:Endpoints(near)) {
            Require(point.X-TestRadius()>PopupDodgerSeat().MaxX+TestRadius(),
                "near-left slide overlaps persistent dodger's X plane");
            if (PopupVariant!=2) {
                Require(point.X+TestRadius()<PopupPlatform(0).CenterX-PopupPlatform(0).SizeX*.5f,
                    "near-side inward slide clips the front of a platform");
            } else {
                Require(point.Y+TestRadius()<PopupPlatform(0).CenterY-PopupPlatform(0).SizeY*.5f,
                    "deep-left corridor clips the side of the low platform");
            }
            Require(point.X-TestRadius()>-1800.f+TestRadius(),
                "near-left slide can cross trainee's movement plane");
            Require(point.Y-TestRadius()>-1800.f&&point.Y+TestRadius()<1800.f,
                "near-left slide and subsequent wiggle can leave the room");
        }
    }
}
float DodgerMaximumReach() {
    // Stock UT ground dodge on the flat trainer floor: horizontal impulse1500,
    // vertical impulse500, project gravity2154. The target cannot jump again
    // in air. Ignore friction and reserve its full speed through the0.1s dodge
    // landing period plus two30Hz frames, then brake using target acceleration.
    // This conservative design check is not a replacement for engine physics.
    const float speed=1500.f, upwardSpeed=500.f, gravity=2154.f;
    const float flightSeconds=2.f*upwardSpeed/gravity;
    const float landingAndTickReserve=.1f+2.f/30.f;
    const float dodgeReach=500.f+speed*(flightSeconds+landingAndTickReserve)
        +speed*speed/(2.f*NCAimTrainerCharacterProfile::Acceleration);
    const float walkingSpeed=940.f;
    const float walkReach=PopupDodgerSeat().WiggleRange+walkingSpeed/30.f
        +walkingSpeed*walkingSpeed/(2.f*NCAimTrainerCharacterProfile::Acceleration);
    return std::max(dodgeReach,walkReach);
}
void DodgerLaneSupport() {
    const FSeat seat=PopupDodgerSeat();
    Require(seat.MinX==seat.MaxX && seat.MinX==-800.f && seat.CenterY==0.f
        && seat.SpawnJitterY==0.f && seat.FloorZ==0.f,
        "permanent dodger must use the clear foreground floor lane");
    Require(seat.WiggleRange==800.f, "dodger walking reversal threshold changed");
    for (float roll:{0.f,.49f,.5f,1.f}) {
        for (float offset:{500.f,800.f,DodgerMaximumReach()}) {
            Require(NCAimTrainerScenarioPolicy::DodgeDirection(offset,roll)==-1.f
                && NCAimTrainerScenarioPolicy::DodgeDirection(-offset,roll)==1.f,
                "outward dodge guard no longer protects the room boundary");
        }
    }
    Require(DodgerMaximumReach()+TestRadius()<1800.f,
        "native dodge plus conservative landing reserve can reach the side wall");
    Require(seat.MinX-TestRadius()>-3200.f && seat.MaxX+TestRadius()<3200.f,
        "dodger capsule leaves the floor on X");
    for (int index=0;index<PopupPlatformCount;++index) {
        const FBlock platform=PopupPlatform(index);
        Require(seat.MaxX+TestRadius()<platform.CenterX-platform.SizeX*.5f,
            "dodger capsule intersects a popup platform");
    }
    for (int index=0;index<PopupSlotCount;++index) {
        Require(seat.MaxX+TestRadius()<PopupMovementSeat(index).MinX-TestRadius(),
            "foreground dodger can collide with a popup target");
    }
    Require(seat.MinX-TestRadius()>-1800.f+TestRadius(),
        "dodger can collide with the trainee's movement plane");
}
void DodgerSightlines() {
    // Foreground movement can intentionally cross the sightline to another
    // target. The dodger itself must remain exposed, including at its widest
    // conservative landing overshoot, without touching platform geometry.
    const FSeat seat=PopupDodgerSeat();
    for (float fraction:{-1.f,-.5f,0.f,.5f,1.f}) {
        for (float lift:{0.f,500.f*500.f/(2.f*2154.f)}) {
            for (float bodyHeight:{40.f,108.f,184.f,212.f}) {
                const Point point={seat.MinX,seat.CenterY+fraction*DodgerMaximumReach(),lift+bodyHeight};
                for (int index=0;index<PopupPlatformCount;++index) {
                    Require(!HitsBlock(point,PopupPlatform(index)),
                        "platform blocks the foreground dodger");
                }
                for (int index=0;index<PopupSlotCount;++index) {
                    for (Point other:Endpoints(PopupMovementSeat(index))) {
                        Require(!SegmentHitsBox(point,other.X-TestRadius(),other.X+TestRadius(),
                            other.Y-TestRadius(),other.Y+TestRadius(),other.Z,other.Z+2.f*TestHalfHeight()),
                            "background target blocks the foreground dodger");
                    }
                }
            }
        }
    }
}
void TrackingSlideLane() {
    InstagibGeometry = false;
    const FSeat seat=PopupDodgerSeat(); // Link uses the same unobstructed X=-800 lane.
    const float travel=SliderMaximumTravel();
    const float walkingSpeed=940.f;
    const float walkReach=seat.WiggleRange+walkingSpeed/30.f
        +walkingSpeed*walkingSpeed/(2.f*NCAimTrainerCharacterProfile::Acceleration);
    const float slideReach=std::max(500.f+travel,walkReach);
    Require(slideReach+TestRadius()<1800.f,
        "lateral native slide or resumed strafe can leave the room");

    for (float offset:{-500.f,-499.99f,0.f,499.99f,500.f}) {
        for (float roll:{0.f,.49f,.5f,1.f}) {
            const float direction=NCAimTrainerScenarioPolicy::TrackingSlideDirection(offset,roll);
            for (float fraction:{0.f,.25f,.5f,.75f,1.f}) {
                const float y=seat.CenterY+offset+direction*travel*fraction;
                Require(std::abs(y)<=slideReach,
                    "direction guard does not bound outward slide travel");
                // Center of both standing and conservatively low slide capsules
                // remains within the shipped1800-unit Link range.
                // This is the fixed trainee position; movement practice may move
                // the player farther away and deliberately changes difficulty.
                for (float centerHeight:{40.f,69.f,108.f}) {
                    const float dx=seat.MinX+1800.f, dz=seat.FloorZ+centerHeight-191.f;
                    Require(dx*dx+y*y+dz*dz<1800.f*1800.f,
                        "outward tracking slide puts the target beyond actual beam range");
                }
            }
        }
    }
    // A prior dodge can place the pawn past the walking threshold. A subsequent
    // slide must head inward and never enlarge that already-established bound.
    for (float side:{-1.f,1.f}) {
        for (float offset:{800.f,DodgerMaximumReach()}) {
            for (float roll:{0.f,1.f}) {
                const float start=side*offset;
                const float direction=NCAimTrainerScenarioPolicy::TrackingSlideDirection(start,roll);
                Require(direction==-side,"post-dodge slide travels farther outward");
                const float finish=start+direction*travel;
                Require(std::abs(finish)<=std::abs(start),"post-dodge slide increases its outer excursion");
                Require(std::max(std::abs(start),std::abs(finish))+TestRadius()<1800.f,
                    "combined native dodge/slide envelope reaches the wall");
            }
        }
    }
}
void HardTrackingSlideLane() {
    const float scale=1.3f,travel=SliderMaximumTravel()*scale;
    const float reach=300.f+travel;
    Require(reach+40.f<1800.f,"hard slide capsule leaves the room");
    Require(1000.f*1000.f+reach*reach+151.f*151.f<1800.f*1800.f,
        "hard slide endpoint leaves1800-unit fixed-position Link beam range");
    const float dodgeFlight=2.f*500.f/2154.f;
    const float dodgeReach=500.f+1500.f*scale*dodgeFlight;
    Require(dodgeReach+40.f<1800.f
        &&1000.f*1000.f+dodgeReach*dodgeReach+151.f*151.f<1800.f*1800.f,
        "hard dodge500-unit inward guard exceeds the room or beam range");
}
void PreservedSeats() {
    for (int slot:{2,3}) {
        const FSeat original=PopupSeat(slot);
        for (int variant:{0,1,2}) {
            const FSeat seat=PopupSeat(slot,variant);
            Require(seat.MinX==original.MinX&&seat.MaxX==original.MaxX&&seat.CenterY==original.CenterY
                &&seat.SpawnJitterY==original.SpawnJitterY&&seat.WiggleRange==original.WiggleRange
                &&seat.FloorZ==original.FloorZ,"protected platform or head-peek target changed with variation");
        }
    }
    Require(PopupSeat(2).MinX==1000.f&&PopupSeat(2).MaxX==2200.f&&PopupSeat(2).CenterY==850.f
        &&PopupSeat(2).SpawnJitterY==85.f&&PopupSeat(2).WiggleRange==99.f&&PopupSeat(2).FloorZ==320.f,
        "high-right target no longer has its original movement envelope");
    Require(PopupSeat(3).MinX==2650.f&&PopupSeat(3).MaxX==2850.f&&PopupSeat(3).CenterY==0.f
        &&PopupSeat(3).SpawnJitterY==0.f&&PopupSeat(3).WiggleRange==60.5f&&PopupSeat(3).FloorZ==0.f,
        "rear-center head-peek no longer has its original movement envelope");
    Require(PopupSeatVariantCount(4)==3&&PopupSeatVariantCount(1)==2
        &&PopupSeatVariantCount(2)==1&&PopupSeatVariantCount(3)==1,"seat variation pool changed");
}
void AngledDodgePaths() {
    for (float radius:{38.f,40.f}) {
        Require(CanPopupDodgePath(0,1400.f,-850.f,100.f,-830.f,radius,120.f),
            "safe forward low-platform dodge rejected");
        Require(CanPopupDodgePath(0,1000.f,-850.f,2300.f,-880.f,radius,120.f),
            "safe backward low-platform dodge rejected");
        Require(CanPopupDodgePath(0,1000.f,-850.f,3000.f,-1200.f,radius,120.f),
            "backward dodge-slide cannot step off the one-unit platform onto clear floor");
        Require(!CanPopupDodgePath(0,1400.f,-850.f,3200.f,-850.f,radius,120.f),
            "backward dodge can reach the rear wall");
        Require(!CanPopupDodgePath(0,1400.f,-850.f,100.f,-1700.f,radius,120.f),
            "inherited lateral dodge momentum can reach the side wall");
        Require(CanPopupDodgePath(4,-500.f,-1450.f,1000.f,-1470.f,radius,100.f),
            "safe backward dodge in the left ground corridor rejected");
        Require(CanPopupDodgePath(4,1100.f,-1450.f,-400.f,-1470.f,radius,100.f),
            "safe forward dodge from deeper left seat rejected");
        Require(!CanPopupDodgePath(4,-500.f,-1450.f,-1700.f,-1470.f,radius,100.f),
            "near-side dodge can cross the foreground dodger plane");
        Require(!CanPopupDodgePath(4,-500.f,-1450.f,1000.f,-400.f,radius,100.f),
            "resumed strafe can clip central cover after landing");
        Require(!CanPopupDodgePath(4,-500.f,1450.f,1000.f,1470.f,radius,100.f),
            "far-right alternative unexpectedly enables the left-only dodge");
        for (int protectedSlot:{1,2,3,5}) {
            Require(!CanPopupDodgePath(protectedSlot,1400.f,-850.f,100.f,-830.f,radius,120.f),
                "an excluded target can use the new angled dodge");
        }
    }
    Require(!CanPopupDodgePath(0,std::numeric_limits<float>::quiet_NaN(),-850.f,100.f,-830.f,40.f,120.f),
        "nonfinite native motion is accepted");
    Require(!CanPopupDodgePath(0,1400.f,-850.f,100.f,-830.f,0.f,120.f),"empty capsule accepted");
}
Point NativeDodgeEnd(Point start,float degrees,float xSign,float ySign,float lateralVelocity,bool chain) {
    // Real profile impulse/cap, gravity, landing reset and slide defaults.
    // Include UT's perpendicular velocity carry, not merely the input angle.
    const float radians=degrees*3.14159265358979323846f/180.f;
    const float dx=xSign*std::cos(radians),dy=ySign*std::sin(radians);
    const float crossX=-dy,crossY=dx,carry=lateralVelocity*crossY;
    const float vx=1500.f*dx+carry*crossX,vy=1500.f*dy+carry*crossY;
    const float magnitude=std::sqrt(vx*vx+vy*vy),speed=std::min(magnitude,1700.f);
    float travel=speed*(2.f*500.f/2154.f+.06f);
    travel+=chain ? 1350.f*(.7f+.1f+.4f*(.35f+.1f)) : speed*(.35f+.1f);
    return {start.X+vx/magnitude*travel,start.Y+vy/magnitude*travel,0.f};
}
void NativeAngledDodgeOpportunity() {
    for(float radius:{38.f,40.f}) {
        for(float degrees:{12.f,20.f,28.f}) {
            for(float direction:{-1.f,1.f}) {
                const Point start={1100.f,-850.f,1.f};
                const Point end=NativeDodgeEnd(start,degrees,direction,-1.f,0.f,false);
                Require(CanPopupDodgePath(0,start.X,start.Y,end.X,end.Y,radius,140.f),
                    "normal 12-28 degree forward/back dodge has no safe low-left opportunity");
                const Point deep={1100.f,-1550.f,0.f};
                const Point deepEnd=NativeDodgeEnd(deep,degrees,direction,1.f,0.f,false);
                Require(CanPopupDodgePath(4,deep.X,deep.Y,deepEnd.X,deepEnd.Y,radius,100.f),
                    "normal forward/back angled dodge has no safe deep-left opportunity");
            }
        }
        for(Point start:std::vector<Point>{{900.f,-850.f,1.f},{-500.f,-1450.f,0.f}}) {
            const bool low=start.Z>0.f;
            const Point end=NativeDodgeEnd(start,12.f,1.f,low ? -1.f : 1.f,0.f,true);
            Require(CanPopupDodgePath(low ? 0 : 4,start.X,start.Y,end.X,end.Y,radius,low ? 140.f : 100.f),
                "native backward dodge-to-slide has no safe popup opportunity");
        }
        const Point start={1100.f,-850.f,1.f};
        const Point carried=NativeDodgeEnd(start,28.f,1.f,-1.f,-940.f,false);
        Require(!CanPopupDodgePath(0,start.X,start.Y,carried.X,carried.Y,radius,140.f),
            "full native perpendicular momentum is not guarded against side-wall contact");
    }
}
void AirborneGeometry() {
    for(bool rockets:{false,true}) {
    const FBlock ledge=AirborneFiringLedge(rockets);
    const float hazard=AirborneHazardHeight(rockets);
    Require(ledge.CenterX==(rockets?-800.f:-1800.f)&&ledge.CenterY==0.f&&ledge.SizeY==3600.f
        &&ledge.Height>hazard+TestHalfHeight(),"airborne player movement lane is not supported above goo");
    Require(ledge.SizeX>2.f*TestRadius()&&PracticeLaneX(rockets?10:7)==ledge.CenterX,
        "gameplay lane differs from a ledge supporting the full player capsule");
    for(int index=0;index<2;++index) {
        const FBlock pad=AirborneJumpPad(index,rockets);
        Require(pad.Height>hazard&&pad.SizeX>2.f*TestRadius()&&pad.SizeY>2.f*TestRadius(),
            "jump-pad platform cannot support a complete target capsule");
        Require(pad.CenterX-pad.SizeX*.5f>ledge.CenterX+ledge.SizeX*.5f
            &&pad.CenterX+pad.SizeX*.5f<3200.f
            &&pad.CenterY-pad.SizeY*.5f>-1800.f&&pad.CenterY+pad.SizeY*.5f<1800.f,
            "jump-pad platform intersects the firing ledge or room walls");
        Require(AirborneJumpApex(rockets)>pad.Height+TestHalfHeight()&&AirborneJumpApex(rockets)+TestHalfHeight()<2000.f,
            "jump-pad apex intersects the ceiling or sits below the launching capsule");
    }
    const FBlock pad0=AirborneJumpPad(0,rockets),pad1=AirborneJumpPad(1,rockets);
    Require(pad1.CenterX-pad0.CenterX==(rockets?500.f:1000.f)&&pad1.CenterY-pad0.CenterY==(rockets?1400.f:2800.f),
        "w00t-style jump-pad path must launch diagonally across both horizontal axes");
    for(int step=0;step<=20;++step) {
        const float t=float(step)/20.f;
        const float x=pad0.CenterX+(pad1.CenterX-pad0.CenterX)*t;
        const float y=pad0.CenterY+(pad1.CenterY-pad0.CenterY)*t;
        Require(x-TestRadius()>ledge.CenterX+ledge.SizeX*.5f&&x+TestRadius()<3200.f
            &&y-TestRadius()>-1800.f&&y+TestRadius()<1800.f,
            "diagonal flight capsule crosses the ledge or room walls");
    }
    for(int index=0;index<5;++index) for(bool sidewall:{false,true}) {
        const FSeat seat=AirborneDropSeat(index,sidewall,rockets);
        Require(AirborneTargetHeight(AirborneDropMinZ,rockets)>pad0.Height+2.f*TestHalfHeight()
            &&AirborneTargetHeight(AirborneDropMaxZ,rockets)+TestHalfHeight()<2000.f,
            "falling target spawn collides with floor or ceiling");
        Require(seat.FloorZ==hazard&&seat.MinX-TestRadius()>ledge.CenterX+ledge.SizeX*.5f
            &&seat.MaxX+TestRadius()<3200.f&&seat.CenterY-seat.SpawnJitterY-TestRadius()>-1800.f
            &&seat.CenterY+seat.SpawnJitterY+TestRadius()<1800.f,
            "falling capsule overlaps ledge or walls, or uses a different goo plane");
        for(int padIndex=0;padIndex<2;++padIndex) {
            const FBlock pad=AirborneJumpPad(padIndex,rockets);
            Require(seat.MinX-TestRadius()>pad.CenterX+pad.SizeX*.5f
                ||seat.MaxX+TestRadius()<pad.CenterX-pad.SizeX*.5f,
                "falling target can land on jump-pad support instead of goo");
        }
        if(sidewall&&(index==0||index==4)) {
            Require(seat.MinX==(rockets?-125.f:550.f)&&seat.MaxX==(rockets?-75.f:650.f)
                &&std::fabs(seat.CenterY)==(rockets?830.f:1660.f)
                &&seat.SpawnJitterY==(rockets?12.5f:25.f),"side appearance lost safe corridor geometry");
        } else {
            const FSeat usual=AirborneDropSeat(index,false,rockets);
            Require(seat.MinX==usual.MinX&&seat.MaxX==usual.MaxX&&seat.CenterY==usual.CenterY,
                "middle slots acquired unintended extra side-wall appearances");
            if(rockets) {
                Require(seat.MinX==(index%2==0?375.f:525.f)&&seat.MaxX==(index%2==0?475.f:650.f),
                    "rear rocket drops did not move to their closer X intervals");
                Require(seat.MinX-TestRadius()-(pad1.CenterX+pad1.SizeX*.5f)>=25.f,
                    "closer rear rocket drops lost full-capsule clearance from the right jump pad");
            }
        }
    }
    }
    for(int scenario=0;scenario<11;++scenario)
        Require(PracticeLaneX(scenario)==(scenario==10?-800.f:-1800.f),"rocket firing distance leaked to another scenario");
}
void RequireHalfRocketDistance(Point before,Point compact,float rearOffset=0.f) {
    // Compare to the previous rocket player's real stationary eye, not the
    // normal hitscan player's farther firing ledge or a target's foot height.
    const Point eye={-800.f,0.f,511.f};
    // Restore only the intentional extra rear-drop approach before comparing
    // against the half-size baseline; side lanes and pads have no offset.
    compact.X+=rearOffset;
    const float oldDistanceSquared=(before.X-eye.X)*(before.X-eye.X)+(before.Y-eye.Y)*(before.Y-eye.Y)
        +(before.Z-eye.Z)*(before.Z-eye.Z);
    const float compactDistanceSquared=(compact.X-eye.X)*(compact.X-eye.X)+(compact.Y-eye.Y)*(compact.Y-eye.Y)
        +(compact.Z-eye.Z)*(compact.Z-eye.Z);
    Require(std::abs(compactDistanceSquared/oldDistanceSquared-.25f)<.000001f
        &&std::abs((compact.X-eye.X)*2.f-(before.X-eye.X))<.001f
        &&std::abs((compact.Y-eye.Y)*2.f-(before.Y-eye.Y))<.001f
        &&std::abs((compact.Z-eye.Z)*2.f-(before.Z-eye.Z))<.001f,
        "compact rocket target center is not50% closer in3D along the same viewing direction");
}
void RocketHalfDistances() {
    const float halfHeight=NCAimTrainerCharacterProfile::TeamArena().CapsuleHalfHeight;
    Require(halfHeight==108.f&&AirborneFiringLedge(true).Height+halfHeight
        +NCAimTrainerCharacterProfile::TeamArena().StandingEyeHeight==511.f,
        "compact layout reference differs from the actual stationary rocket eye");
    for(int index=0;index<2;++index) {
        const auto original=AirborneJumpPad(index),compact=AirborneJumpPad(index,true);
        RequireHalfRocketDistance({original.CenterX,original.CenterY,original.Height+halfHeight+2.f},
            {compact.CenterX,compact.CenterY,compact.Height+halfHeight+2.f});
    }
    RequireHalfRocketDistance({600,0,AirborneJumpApex(false)},{-100,0,AirborneJumpApex(true)});
    for(int index=0;index<5;++index) for(bool sidewall:{false,true}) {
        const auto original=AirborneDropSeat(index,sidewall),compact=AirborneDropSeat(index,sidewall,true);
        const bool sideLane=sidewall&&(index==0||index==4);
        const float rearOffset=sideLane?0.f:(index%2==0?75.f:200.f);
        for(float position:{0.f,.5f,1.f}) for(float jitter:{-1.f,0.f,1.f})
            for(float height:{AirborneDropMinZ,AirborneDropMaxZ,AirborneHazardHeight(false)+halfHeight}) {
                const Point before={original.MinX+(original.MaxX-original.MinX)*position,
                    original.CenterY+jitter*original.SpawnJitterY,height};
                const float compactHeight=height==AirborneHazardHeight(false)+halfHeight
                    ?AirborneHazardHeight(true)+halfHeight:AirborneTargetHeight(height,true);
                const Point after={compact.MinX+(compact.MaxX-compact.MinX)*position,
                    compact.CenterY+jitter*compact.SpawnJitterY,compactHeight};
                RequireHalfRocketDistance(before,after,rearOffset);
            }
    }
}

int main(int argc,char**argv) {
    Require(argc==2,"choose a case"); const std::string name=argv[1];
    if(name=="preserved") { PreservedSeats(); return 0; }
    if(name=="dodge_paths") { AngledDodgePaths(); return 0; }
    if(name=="native_dodge") { NativeAngledDodgeOpportunity(); return 0; }
    if(name=="arena_replication") { ArenaScenarioReplication(); return 0; }
    if(name=="rocket_half_distance") { RocketHalfDistances(); return 0; }
    if(name=="arena_goo") { ArenaGooMaterial(); return 0; }
    if(name=="hard_tracking_slide") { HardTrackingSlideLane(); return 0; }
    for(bool instagib:{true,false}) {
    for(int variant:{0,1,2}) {
    InstagibGeometry=instagib;
    PopupVariant=variant;
    if(name=="support") PlatformSupport();
    else if(name=="popup") PopupSightlines();
    else if(name=="heads") HeadCoverSightlines();
    else if(name=="occlusion") OtherTargetOcclusion();
    else if(name=="slider_runway") SliderRunway();
    else if(name=="dodger_support") DodgerLaneSupport();
    else if(name=="dodger_sightlines") DodgerSightlines();
    else if(name=="tracking_slide") TrackingSlideLane();
    else if(name=="left_motion") LeftMotionLanes();
    else if(name=="airborne_geometry") AirborneGeometry();
    else Require(false,"unknown case");
    }
    }
}
'''


class AimTrainerLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-layout-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        layout = (PLUGIN / "Source/Private/NCAimTrainerLayout.h").read_text(encoding="utf-8-sig")
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()
        source = directory / "trainer_layout.cpp"
        profile = (PLUGIN / "Source/Private/NCAimTrainerCharacterProfile.h").read_text(encoding="utf-8-sig")
        vector_stub = "struct FVector { float X,Y,Z;explicit FVector(float v):X(v),Y(v),Z(v) {} FVector(float x,float y,float z):X(x),Y(y),Z(z) {} FVector operator/(float divisor) const { return FVector(X/divisor,Y/divisor,Z/divisor); } };\n"
        native = (PLUGIN / "Source/Private/NCAimTrainerTarget.cpp").read_text(encoding="utf-8-sig")
        source.write_text(vector_stub + profile.replace("#pragma once", "") + "\n" + layout.replace("#pragma once", "")
                          + f'\n#include "{policy}"\n' + ARENA_ADAPTER
                          + native_function(native, "void ANCAimTrainerArena::OnRep_Scenario")
                          + native_function(native, "void ANCAimTrainerArena::BeginPlay") + ARENA_CASE
                          + "\n" + CASES, encoding="utf-8")
        cls.executable = directory / ("trainer_layout.exe" if os.name == "nt" else "trainer_layout")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_layout.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Layout adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hard_tracking_slide_and_dodge_capsules_remain_in_room_and_beam_range(self): self.run_case("hard_tracking_slide")
    def test_airborne_spawn_lanes_pad_capsules_ledge_and_ceiling_are_clear(self): self.run_case("airborne_geometry")
    def test_replicated_scenario_switches_all_cover_hazard_and_pad_components(self): self.run_case("arena_replication")
    def test_rocket_half_distance_baseline_preserves_jumper_and_side_lanes_with_closer_rear_drops(self): self.run_case("rocket_half_distance")
    def test_goo_loads_on_rendering_peers_with_safe_missing_asset_fallback(self): self.run_case("arena_goo")
    def test_wiggle_and_stopping_capsule_remain_supported(self): self.run_case("support")
    def test_popup_heads_clear_platforms_and_rear_body_is_covered(self): self.run_case("popup")
    def test_five_head_stations_keep_heads_clear_and_shoulders_covered(self): self.run_case("heads")
    def test_popup_target_capsules_do_not_obscure_other_heads(self): self.run_case("occlusion")
    def test_right_platform_supports_native_slide_and_lateral_wiggle(self): self.run_case("slider_runway")
    def test_permanent_dodger_lane_supports_native_dodge_overshoot(self): self.run_case("dodger_support")
    def test_permanent_dodger_remains_visible_across_its_lane(self): self.run_case("dodger_sightlines")
    def test_tracking_slide_stays_in_lane_and_within_fixed_player_beam_range(self): self.run_case("tracking_slide")
    def test_both_left_slide_lanes_and_long_strafe_remain_clear_with_both_capsule_profiles(self): self.run_case("left_motion")
    def test_requested_platform_and_head_peek_seats_are_preserved(self): self.run_case("preserved")
    def test_angled_dodge_paths_keep_landing_strafe_and_capsules_clear(self): self.run_case("dodge_paths")
    def test_native_impulses_allow_forward_backward_and_landing_slide_opportunities(self): self.run_case("native_dodge")


if __name__ == "__main__":
    unittest.main()
