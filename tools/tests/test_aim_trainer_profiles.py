"""Verify decoded trainer profiles and real engine posture callbacks natively.

The profile header is compiled unchanged. Stock OnStart/OnEndCrouch and UT eye
height recalculation are extracted unchanged. Capsule collision, replicated
movement and the actual spawned subclass still require the normal UE playtest.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
struct FVector {
    float X, Y, Z;
    explicit FVector(float xyz=0.f) : X(xyz), Y(xyz), Z(xyz) {}
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
};
#include "NCAimTrainerCharacterProfile.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLayout.h"
struct MeshComponent {
    FVector RelativeLocation, RelativeScale3D{1.f};
    int AnimClass = 47;
    void SetRelativeLocation(FVector value, bool=false) { RelativeLocation=value; }
    void SetRelativeScale3D(FVector value) { RelativeScale3D=value; }
};
struct Capsule {
    float Radius=40.f, HalfHeight=108.f;
    void InitCapsuleSize(float radius, float halfHeight) { Radius=radius; HalfHeight=halfHeight; }
};
struct Movement {
    float MaxWalkSpeed=940.f, MaxWalkSpeedCrouched=315.f, MaxAcceleration=3200.f;
    float DefaultBrakingDecelerationWalking=520.f, BrakingDecelerationWalking=520.f;
    float GroundFriction=10.5f, DodgeAirControl=.41f, CrouchedHalfHeight=69.f;
    float NetworkSimulatedSmoothLocationTime=.1f;
    float EasyImpactImpulse=1100.f, EasyImpactDamage=25.f, FullImpactImpulse=1600.f;
    float FullImpactDamage=40.f, ImpactMaxHorizontalVelocity=1500.f;
    float MaxInitialFloorSlideSpeed=1350.f, MaxFloorSlideSpeed=900.f;
    float MaxFastAccelSpeed=200.f, MaxStepHeight=51.f, NetworkMaxSmoothUpdateDistance=92.f;
    // Inherited movement and live prediction state must remain independent.
    float JumpZVelocity=730.f, DodgeImpulseHorizontal=1500.f, DodgeImpulseVertical=500.f;
    float MaxPositionErrorSquared=324.f, Velocity=67.f, SavedMoveTimestamp=10.5f;
    bool bIsFloorSliding=false;
};
struct ACharacter;
struct UClass { ACharacter* CDO=nullptr; };
template<class T> T* GetDefault(UClass* type) { return static_cast<T*>(type->CDO); }
struct ACharacter {
    virtual ~ACharacter() = default;
    MeshComponent Body, Camera;
    MeshComponent* Mesh=&Body;
    FVector BaseTranslationOffset;
    UClass* Class=nullptr;
    bool bIsCrouched=false;
    UClass* GetClass() const { return Class; }
    virtual void RecalculateBaseEyeHeight() {}
    void K2_OnStartCrouch(float, float) {}
    void K2_OnEndCrouch(float, float) {}
    void OnStartCrouch(float, float);
    void OnEndCrouch(float, float);
};
struct AUTCharacter : ACharacter {
    Capsule BodyCapsule;
    Movement Move;
    Movement* UTCharacterMovement=&Move;
    MeshComponent* CharacterCameraComponent=&Camera;
    float BaseEyeHeight=83.f, DefaultBaseEyeHeight=83.f, CrouchedEyeHeight=45.f;
    float DefaultCrouchedEyeHeight=40.f, FloorSlideEyeHeight=1.f, SlideTargetHeight=55.f;
    Capsule* GetCapsuleComponent() { return &BodyCapsule; }
    MeshComponent* GetMesh() { return Mesh; }
    void RecalculateBaseEyeHeight() override;
};
// ENGINE_CALLBACKS
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void Near(float value, float expected, const char* message) {
    Require(std::abs(value-expected)<.00001f, message);
}
void CharacterDefaults() {
    using namespace NCAimTrainerCharacterProfile;
    AUTCharacter normal, ig;
    ApplyCharacter(normal, TeamArena()); ApplyCharacter(ig, Instagib());
    Near(normal.BodyCapsule.Radius,40.f,"Team capsule radius changed");
    Near(normal.BodyCapsule.HalfHeight,108.f,"Team capsule height changed");
    Near(ig.BodyCapsule.Radius,38.f,"IG capsule radius did not match content");
    Near(ig.BodyCapsule.HalfHeight,103.f,"IG capsule height did not match content");
    Near(normal.Body.RelativeScale3D.X,1.f,"Team body scale changed");
    Near(ig.Body.RelativeScale3D.X,.95f,"IG body scale did not match content");
    Near(normal.Body.RelativeLocation.Z,-110.f,"Team body seat changed");
    Near(ig.Body.RelativeLocation.Z,-110.f,"IG body seat changed");
    Near(normal.BaseEyeHeight+normal.BodyCapsule.HalfHeight,191.f,"Team standing eye changed");
    Near(ig.BaseEyeHeight+ig.BodyCapsule.HalfHeight,183.f,"IG standing eye changed");
    Near(normal.CrouchedEyeHeight,45.f,"Team initial crouched eye changed");
    Near(ig.CrouchedEyeHeight,43.f,"IG initial crouched eye changed");
    Near(ig.DefaultCrouchedEyeHeight,40.f,"IG inherited crouch eye was overwritten");
    Require(normal.Body.AnimClass==47 && ig.Body.AnimClass==47,"profile replaced authored animation");
}
void MovementDefaults() {
    using namespace NCAimTrainerCharacterProfile;
    for (bool instagib : {false,true}) {
        Movement move; move.MaxWalkSpeed=500.f; move.MaxWalkSpeedCrouched=220.f;
        move.MaxAcceleration=7000.f;
        if (instagib) ApplyInstagibMovement(move); else ApplyTeamArenaMovement(move);
        Near(move.MaxWalkSpeed,940.f,"exercise speed limit survived");
        Near(move.MaxWalkSpeedCrouched,315.f,"exercise crouch limit survived");
        Near(move.MaxAcceleration,5000.f,"authored acceleration missing");
        Near(move.BrakingDecelerationWalking,2000.f,"authored braking missing");
        Near(move.DefaultBrakingDecelerationWalking,instagib?2100.f:2000.f,"landing braking baseline wrong");
        Near(move.GroundFriction,14.f,"authored friction missing");
        Near(move.CrouchedHalfHeight,72.f,"authored crouch capsule wrong");
        Near(move.DodgeAirControl,instagib?.6f:.55f,"authored dodge control missing");
        Near(move.MaxFastAccelSpeed,instagib?220.f:200.f,"fast acceleration threshold wrong");
        Near(move.MaxStepHeight,instagib?53.f:51.f,"authored step height wrong");
        Near(move.NetworkSimulatedSmoothLocationTime,instagib?.05f:.07f,"authored smoothing time wrong");
        Near(move.NetworkMaxSmoothUpdateDistance,instagib?284.f:92.f,"authored smoothing distance wrong");
        Near(move.EasyImpactImpulse,850.f,"authored impact impulse missing");
        Near(move.EasyImpactDamage,10.f,"authored impact damage missing");
        Near(move.FullImpactImpulse,2000.f,"authored full impact impulse missing");
        Near(move.FullImpactDamage,25.f,"authored full impact damage missing");
        Near(move.ImpactMaxHorizontalVelocity,2300.f,"authored impact speed missing");
        Near(move.MaxInitialFloorSlideSpeed,1350.f,"BeginPlay initial slide speed missing");
        Near(move.MaxFloorSlideSpeed,1100.f,"BeginPlay sustained slide speed missing");
        Near(move.JumpZVelocity,730.f,"profile changed inherited jump");
        Near(move.DodgeImpulseHorizontal,1500.f,"profile changed inherited horizontal dodge");
        Near(move.DodgeImpulseVertical,500.f,"profile changed inherited vertical dodge");
        Near(move.MaxPositionErrorSquared,324.f,"profile changed NCP correction policy");
        Near(move.Velocity,67.f,"profile changed live velocity");
        Near(move.SavedMoveTimestamp,10.5f,"profile changed prediction history");
    }
}
void Posture() {
    using namespace NCAimTrainerCharacterProfile;
    for (bool instagib : {false,true}) {
        const auto profile=instagib?Instagib():TeamArena();
        AUTCharacter cdo, pawn;
        ApplyCharacter(cdo,profile); ApplyCharacter(pawn,profile);
        UClass type; type.CDO=&cdo; cdo.Class=&type; pawn.Class=&type;
        const float heightAdjust=profile.CapsuleHalfHeight-CrouchedHalfHeight;
        for (int cycle=0; cycle<20; ++cycle) {
            pawn.bIsCrouched=true;
            pawn.OnStartCrouch(heightAdjust,heightAdjust);
            Near(pawn.Body.RelativeLocation.Z,-110.f+heightAdjust,"crouch did not use class-specific seat");
            Near(pawn.BaseEyeHeight,40.f,"native crouch eye no longer matches inherited default");
            pawn.bIsCrouched=false;
            pawn.OnEndCrouch(heightAdjust,heightAdjust);
            Near(pawn.Body.RelativeLocation.Z,-110.f,"uncrouch accumulated body seat drift");
            Near(pawn.BaseEyeHeight,profile.StandingEyeHeight,"uncrouch restored wrong class eye");
            Near(pawn.Body.RelativeScale3D.Z,profile.MeshScale,"posture changed authored model scale");
        }
        pawn.Move.bIsFloorSliding=true; pawn.RecalculateBaseEyeHeight();
        Near(pawn.BaseEyeHeight,profile.SlideEyeHeight,"native slide eye changed");
        pawn.Move.bIsFloorSliding=false; pawn.RecalculateBaseEyeHeight();
        Near(pawn.BaseEyeHeight,profile.StandingEyeHeight,"slide ending lost standing eye");
        Near(cdo.Body.RelativeLocation.Z,-110.f,"live posture mutated the class default");
    }
}
void Repeat() {
    using namespace NCAimTrainerCharacterProfile;
    AUTCharacter pawn;
    for (int n=0;n<10;++n) {
        ApplyCharacter(pawn,Instagib()); ApplyInstagibMovement(pawn.Move);
        Near(pawn.Body.RelativeScale3D.Z,.95f,"reapplication compounded mesh scale");
        Near(pawn.BodyCapsule.HalfHeight,103.f,"reapplication compounded capsule scale");
        Near(pawn.Move.DodgeAirControl,.6f,"reapplication compounded movement");
    }
}
void StrafeBraking() {
    using NCAimTrainerScenarioPolicy::BoundedStrafeDirection;
    Near(BoundedStrafeDirection(80.f,500.f,5000.f,90.f,1.f,1.f/60.f),-1.f,
         "outward target did not brake before the edge");
    Near(BoundedStrafeDirection(-80.f,-500.f,5000.f,90.f,-1.f,1.f/60.f),1.f,
         "negative outward target did not brake before the edge");
    Near(BoundedStrafeDirection(0.f,0.f,5000.f,90.f,-1.f,1.f/60.f),-1.f,
         "safe requested reversal was suppressed");
    // Conservative one-dimensional integration omits helpful ground friction.
    // Full gameplay acceleration/speed must stay bounded without a 220 cap.
    for (float hz : {30.f,60.f,120.f,700.f}) for (float range : {44.f,80.f,90.f,99.f,800.f}) {
        const float dt=1.f/hz;
        float position=0.f, velocity=0.f, direction=1.f, peakSpeed=0.f;
        for (int step=0;step<int(hz*20.f);++step) {
            if (step % int(std::max(1.f,hz*.17f))==0) direction=-direction;
            direction=BoundedStrafeDirection(position,velocity,5000.f,range,direction,dt);
            velocity=std::max(-940.f,std::min(940.f,velocity+direction*5000.f*dt));
            position+=velocity*dt;
            peakSpeed=std::max(peakSpeed,std::abs(velocity));
            if (std::abs(position)>range+NCAimTrainerLayout::WiggleSafetyMargin) {
                std::cerr << "hz=" << hz << " range=" << range << " step=" << step
                          << " position=" << position << " velocity=" << velocity << '\n';
                Require(false,"full-speed strafe exceeded its reserved stopping margin");
            }
        }
        Require(peakSpeed>220.f,"strafe test never exceeded the old exercise speed cap");
    }
}
int main(int argc,char** argv) {
    Require(argc==2,"one test case required");
    const std::string name(argv[1]);
    if(name=="characters") CharacterDefaults();
    else if(name=="movement") MovementDefaults();
    else if(name=="posture") Posture();
    else if(name=="repeat") Repeat();
    else if(name=="strafe") StrafeBraking();
    else Require(false,"unknown test case");
}
'''


class AimTrainerProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-profiles-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        engine = PLUGIN.parents[2] / "Engine/Source/Runtime/Engine/Private/Character.cpp"
        character = PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTCharacter.cpp"
        definitions = [native_function(engine.read_text(encoding="utf-8-sig"), signature)
                       for signature in ("void ACharacter::OnStartCrouch", "void ACharacter::OnEndCrouch")]
        definitions.append(native_function(character.read_text(encoding="utf-8-sig"),
                                           "void AUTCharacter::RecalculateBaseEyeHeight"))
        source = directory / "profiles.cpp"
        source.write_text(ADAPTER.replace("// ENGINE_CALLBACKS", "\n".join(definitions)), encoding="utf-8")
        cls.executable = directory / ("profiles.exe" if os.name == "nt" else "profiles")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                       f"/I{PLUGIN / 'Source/Private'}", str(source), f"/Fe{cls.executable}",
                       f"/Fo{directory / 'profiles.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-I", str(PLUGIN / "Source/Private"), str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_distinct_authored_character_size_and_camera_defaults(self):
        self.run_case("characters")

    def test_content_movement_and_beginplay_values_preserve_prediction_state(self):
        self.run_case("movement")

    def test_native_posture_callbacks_restore_each_class_default_without_drift(self):
        self.run_case("posture")

    def test_repeated_constructor_profile_application_does_not_compound_scale(self):
        self.run_case("repeat")

    def test_full_speed_strafe_brakes_inside_short_lanes_at_multiple_frame_rates(self):
        self.run_case("strafe")


if __name__ == "__main__":
    unittest.main()
