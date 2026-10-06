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
    Require(TargetCount==5 && PopupSlotCount==5 && HeadSlotCount==5 && PopupPlatformCount==3,
            "pool and layout counts disagree");
    const float stopDistance = WiggleSpeed*WiggleSpeed/(2.f*WiggleAcceleration);
    Require(stopDistance+WiggleSpeed/30.f < WiggleSafetyMargin, "wiggle reserve does not cover stopping plus a30Hz frame");
    for (int slot=0;slot<PopupSlotCount;++slot) {
        const FSeat seat=PopupSeat(slot);
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
        for (Point point:Endpoints(PopupSeat(slot))) {
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
        for (Point point:Endpoints(PopupSeat(slot))) {
            for (int other=0;other<PopupSlotCount;++other) {
                if (other==slot) continue;
                for (Point obstacle:Endpoints(PopupSeat(other))) {
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
int main(int argc,char**argv) {
    Require(argc==2,"choose a case"); const std::string name=argv[1];
    if(name=="support") PlatformSupport();
    else if(name=="popup") PopupSightlines();
    else if(name=="heads") HeadCoverSightlines();
    else if(name=="occlusion") OtherTargetOcclusion();
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
        source = directory / "trainer_layout.cpp"
        source.write_text(layout.replace("#pragma once", "") + "\n" + CASES, encoding="utf-8")
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


if __name__ == "__main__":
    unittest.main()
