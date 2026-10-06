"""Native layout tests for support, cover and the trainer's fixed sightlines.

Head samples use a conservative 184..212 unit standing band, not a claim that
animation is fixed there. A packaged playtest must verify the actual head pose.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler


CASES = r'''
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using namespace NCAimTrainerLayout;
struct Point { float X, Y, Z; };
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
float SliderMaximumTravel() {
    // Native UT flat-ground slide from a 220-speed wiggle starts at900, lasts
    // 0.7s, and exits at40% speed. Allow two30Hz frames beyond the timer and a
    // full exit frame before applying direction friction. With lateral-only
    // input, X decays through GroundFriction10.5, not the7000 Y acceleration.
    // Ignoring the native220 walking-speed clamp further overestimates travel.
    const float slideSpeed=900.f, duration=.7f, exitFactor=.4f, groundFriction=10.5f;
    const float exitSpeed=slideSpeed*exitFactor;
    return slideSpeed*(duration+2.f/30.f)+exitSpeed/30.f+exitSpeed/groundFriction;
}
FSeat PopupMovementSeat(int index) {
    FSeat seat=PopupSeat(index);
    if (index==PopupSliderSlot) seat.MinX-=SliderMaximumTravel();
    return seat;
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
    // Actual native trainee eye: capsule half-height108 + BaseEyeHeight83.
    const float start[3] = {-1800.f,0.f,191.f};
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
    const float stopDistance = WiggleSpeed*WiggleSpeed/(2.f*WiggleAcceleration);
    Require(stopDistance+WiggleSpeed/30.f < WiggleSafetyMargin, "wiggle reserve does not cover stopping plus a30Hz frame");
    for (int slot=0;slot<PopupSlotCount;++slot) {
        const FSeat seat=PopupMovementSeat(slot);
        for (Point point:Endpoints(seat)) {
            Require(point.X-CapsuleRadius>-3200.f && point.X+CapsuleRadius<3200.f
                    && point.Y-CapsuleRadius>-1800.f && point.Y+CapsuleRadius<1800.f,
                    "target swept capsule can leave room floor");
            if (slot<PopupPlatformCount) {
                const FBlock block=PopupPlatform(slot);
                Require(seat.FloorZ==block.Height, "spawn floor and platform top disagree");
                Require(point.X-CapsuleRadius>=block.CenterX-block.SizeX*.5f
                        && point.X+CapsuleRadius<=block.CenterX+block.SizeX*.5f
                        && point.Y-CapsuleRadius>=block.CenterY-block.SizeY*.5f
                        && point.Y+CapsuleRadius<=block.CenterY+block.SizeY*.5f,
                        "wiggle plus stopping margin can run off platform");
            } else {
                Require(seat.FloorZ==0.f, "floor seat is floating");
                for (int p=0;p<PopupPlatformCount;++p) {
                    const FBlock block=PopupPlatform(p);
                    const bool separate=point.X+CapsuleRadius<block.CenterX-block.SizeX*.5f
                        || point.X-CapsuleRadius>block.CenterX+block.SizeX*.5f
                        || point.Y+CapsuleRadius<block.CenterY-block.SizeY*.5f
                        || point.Y-CapsuleRadius>block.CenterY+block.SizeY*.5f;
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
                const Point head={point.X,point.Y,point.Z+headHeight};
                for (int p=0;p<PopupPlatformCount;++p) {
                    Require(!HitsBlock(head,PopupPlatform(p)), "platform blocks a standing target head center");
                }
            }
            if (slot==3) {
                Require(HitsBlock({point.X,point.Y,170.f},PopupPlatform(1)),
                        "rear target exposes its upper body instead of peeking over cover");
            }
        }
    }
}
void HeadCoverSightlines() {
    for (int slot=0;slot<HeadSlotCount;++slot) {
        for (Point point:Endpoints(HeadSeat(slot))) {
            for (float headHeight:{184.f,198.f,212.f}) {
                for (int block=0;block<HeadSlotCount;++block) {
                    Require(!HitsBlock({point.X,point.Y,headHeight},HeadCover(block)),
                            "head station cover hides a valid standing head center");
                }
            }
            for (float bodySide:{-CapsuleRadius,0.f,CapsuleRadius}) {
                Require(HitsBlock({point.X,point.Y+bodySide,170.f},HeadCover(slot)),
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
                for (Point obstacle:Endpoints(PopupMovementSeat(other))) {
                    for (float headHeight:{184.f,212.f}) {
                        Require(!SegmentHitsBox({point.X,point.Y,point.Z+headHeight},
                            obstacle.X-CapsuleRadius,obstacle.X+CapsuleRadius,
                            obstacle.Y-CapsuleRadius,obstacle.Y+CapsuleRadius,
                            obstacle.Z,obstacle.Z+2.f*CapsuleHalfHeight),
                            "another target capsule can cover this target's head center");
                    }
                }
            }
        }
    }
}
void SliderRunway() {
    Require(PopupSliderSlot==2, "slide lane must use the elevated right platform");
    const FSeat seat=PopupSeat(PopupSliderSlot);
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
            Require(slid.X-CapsuleRadius>minX && slid.X+CapsuleRadius<maxX,
                "native forward slide can carry its capsule off the platform");
            Require(slid.Y-CapsuleRadius>minY && slid.Y+CapsuleRadius<maxY,
                "wiggle envelope around the slide can leave a lateral edge");
            Require(slid.X-CapsuleRadius>-3200.f && slid.X+CapsuleRadius<3200.f
                && slid.Y-CapsuleRadius>-1800.f && slid.Y+CapsuleRadius<1800.f,
                "sliding capsule can leave the arena bounds");
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
        +speed*speed/(2.f*WiggleAcceleration);
    const float walkingSpeed=500.f;
    const float walkReach=PopupDodgerSeat().WiggleRange+walkingSpeed/30.f
        +walkingSpeed*walkingSpeed/(2.f*WiggleAcceleration);
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
    Require(DodgerMaximumReach()+CapsuleRadius<1800.f,
        "native dodge plus conservative landing reserve can reach the side wall");
    Require(seat.MinX-CapsuleRadius>-3200.f && seat.MaxX+CapsuleRadius<3200.f,
        "dodger capsule leaves the floor on X");
    for (int index=0;index<PopupPlatformCount;++index) {
        const FBlock platform=PopupPlatform(index);
        Require(seat.MaxX+CapsuleRadius<platform.CenterX-platform.SizeX*.5f,
            "dodger capsule intersects a popup platform");
    }
    for (int index=0;index<PopupSlotCount;++index) {
        Require(seat.MaxX+CapsuleRadius<PopupMovementSeat(index).MinX-CapsuleRadius,
            "foreground dodger can collide with a popup target");
    }
    Require(seat.MinX-CapsuleRadius>-1800.f+CapsuleRadius,
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
                        Require(!SegmentHitsBox(point,other.X-CapsuleRadius,other.X+CapsuleRadius,
                            other.Y-CapsuleRadius,other.Y+CapsuleRadius,other.Z,other.Z+2.f*CapsuleHalfHeight),
                            "background target blocks the foreground dodger");
                    }
                }
            }
        }
    }
}
int main(int argc,char**argv) {
    Require(argc==2,"choose a case"); const std::string name=argv[1];
    if(name=="support") PlatformSupport();
    else if(name=="popup") PopupSightlines();
    else if(name=="heads") HeadCoverSightlines();
    else if(name=="occlusion") OtherTargetOcclusion();
    else if(name=="slider_runway") SliderRunway();
    else if(name=="dodger_support") DodgerLaneSupport();
    else if(name=="dodger_sightlines") DodgerSightlines();
    else Require(false,"unknown case");
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
        policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").read_text(encoding="utf-8-sig")
        source = directory / "trainer_layout.cpp"
        source.write_text(layout.replace("#pragma once", "") + "\n" + policy.replace("#pragma once", "")
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

    def test_wiggle_and_stopping_capsule_remain_supported(self): self.run_case("support")
    def test_popup_heads_clear_platforms_and_rear_body_is_covered(self): self.run_case("popup")
    def test_five_head_stations_keep_heads_clear_and_shoulders_covered(self): self.run_case("heads")
    def test_popup_target_capsules_do_not_obscure_other_heads(self): self.run_case("occlusion")
    def test_right_platform_supports_native_slide_and_lateral_wiggle(self): self.run_case("slider_runway")
    def test_permanent_dodger_lane_supports_native_dodge_overshoot(self): self.run_case("dodger_support")
    def test_permanent_dodger_remains_visible_across_its_lane(self): self.run_case("dodger_sightlines")


if __name__ == "__main__":
    unittest.main()
