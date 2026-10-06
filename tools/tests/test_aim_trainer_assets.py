"""Compile actual target initialization against UT's content/pawn separation.

CharacterContent intentionally has a mesh with no animation Blueprint. The
stock pawn Blueprint supplies that class and capsule-relative mesh transform.
The adapter models ApplyCharacterData's class-default scale rule, not an Unreal
asset loader; a packaged playtest is still required for the real cooked assets.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdlib>
#include <iostream>
#include <string>
#define TEXT(x) x
struct FObjectInitializer {};
struct AUTCharacter;
struct UClass {
    AUTCharacter* Object = nullptr;
    template<class T> const T* GetDefaultObject() const { return static_cast<const T*>(Object); }
};
struct Transform { float Z = 0.f, Yaw = 0.f, Scale = 1.f; };
enum class EMeshComponentUpdateFlag { OnlyTickPoseWhenRendered, AlwaysTickPoseAndRefreshBones };
struct Mesh {
    Transform Relative;
    UClass* AnimClass = nullptr;
    void* SkeletalMesh = nullptr;
    EMeshComponentUpdateFlag MeshComponentUpdateFlag = EMeshComponentUpdateFlag::OnlyTickPoseWhenRendered;
    bool bEnableUpdateRateOptimizations = true;
    void SetRelativeTransform(Transform value) { Relative = value; }
    Transform GetRelativeTransform() const { return Relative; }
    void SetAnimInstanceClass(UClass* value) { AnimClass = value; }
};
struct AUTCharacterContent { Mesh Body; };
struct Movement {
    bool bRunPhysicsWithNoController = false, bOrientRotationToMovement = true;
    bool bUseControllerDesiredRotation = true;
    float MaxWalkSpeed = 0.f, MaxAcceleration = 0.f;
};
enum class EAutoPossessAI { Disabled, Enabled };
struct AUTCharacter {
    Mesh Body;
    Movement Move;
    AUTCharacterContent* CharacterData = nullptr;
    int PostInitCalls = 0, BeginCalls = 0, Applies = 0;
    float ClassDefaultMeshScale = 1.f;
    bool bAlwaysRelevant = false;
    float NetUpdateFrequency = 0.f, MinNetUpdateFrequency = 0.f;
    int Health = 0, HealthMax = 0, ArmorAmount = 0;
    EAutoPossessAI AutoPossessAI = EAutoPossessAI::Enabled;
    AUTCharacter() = default;
    explicit AUTCharacter(const FObjectInitializer&) {}
    Mesh* GetMesh() const { return const_cast<Mesh*>(&Body); }
    Movement* GetCharacterMovement() { return &Move; }
    void PostInitializeComponents() { ++PostInitCalls; }
    void BeginPlay() { ++BeginCalls; }
    void ApplyCharacterData(AUTCharacterContent* data) {
        ++Applies;
        Body.SkeletalMesh = data->Body.SkeletalMesh;
        Body.Relative.Scale = ClassDefaultMeshScale * data->Body.Relative.Scale;
        Body.Relative.Yaw = data->Body.Relative.Yaw;
        // Component reregistration can recreate the animation instance. Its
        // class comes from the pawn; CharacterContent does not supply it.
        Body.AnimClass = nullptr;
    }
};
UClass* AvailableTemplate = nullptr;
namespace ConstructorHelpers {
template<class T> struct FClassFinder {
    UClass* Class;
    explicit FClassFinder(const char*) : Class(AvailableTemplate) {}
};
}
struct ANCAimTrainerTarget : AUTCharacter {
    using Super = AUTCharacter;
    int VisibilityUpdates = 0;
    explicit ANCAimTrainerTarget(const FObjectInitializer&);
    void PostInitializeComponents();
    void BeginPlay();
    bool HasCharacterAssets() const;
    void OnRep_TrainerVisible() { ++VisibilityUpdates; }
};
'''

CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    Require(argc == 2, "choose a case");
    const std::string name = argv[1];
    FObjectInitializer initializer;
    UClass animation;
    AUTCharacter stock;
    stock.Body.AnimClass = &animation;
    stock.Body.Relative = {-108.f, -90.f, 1.25f};
    UClass templateClass;
    templateClass.Object = &stock;
    AvailableTemplate = name == "missing_template" ? nullptr : &templateClass;
    AUTCharacterContent skin;
    int meshResource = 1;
    skin.Body.SkeletalMesh = name == "missing_mesh" ? nullptr : &meshResource;
    skin.Body.Relative.Scale = 0.8f;
    skin.Body.Relative.Yaw = -90.f;
    Require(!skin.Body.AnimClass, "fixture must reproduce CharacterContent without AnimClass");
    ANCAimTrainerTarget target(initializer);
    target.ClassDefaultMeshScale = target.Body.Relative.Scale;
    target.CharacterData = &skin;
    target.PostInitializeComponents();
    if (name == "spawn_ready") {
        Require(target.HasCharacterAssets(), "native target rejected the valid stock skin");
        Require(target.Body.AnimClass == &animation, "lost pawn animation class to skin's null AnimClass");
        Require(target.PostInitCalls == 1 && target.BeginCalls == 0, "assets depend on BeginPlay");
        Require(target.Body.Relative.Z == -108.f && target.Body.Relative.Yaw == -90.f,
                "mesh did not inherit stock capsule-relative placement");
        Require(target.Body.Relative.Scale == 1.f, "skin scale did not use pawn class defaults");
        target.BeginPlay();
        Require(target.Applies == 1 && target.BeginCalls == 1 && target.VisibilityUpdates == 1,
                "BeginPlay reapplied assets or lost visibility initialization");
    } else if (name == "scale_stable") {
        target.PostInitializeComponents();
        Require(target.Body.Relative.Scale == 1.f && target.Body.Relative.Z == -108.f,
                "reapplication compounded skin scale or lost authored offset");
        Require(target.Body.AnimClass == &animation, "reapplication lost pawn animation");
    } else if (name == "dedicated_pose") {
        Require(target.Body.MeshComponentUpdateFlag == EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones,
                "unrendered head bones stop updating on the dedicated server");
        Require(!target.Body.bEnableUpdateRateOptimizations && target.Move.bRunPhysicsWithNoController,
                "target cannot animate/move independently of a bot controller");
    } else if (name == "missing_template" || name == "missing_mesh") {
        Require(!target.HasCharacterAssets(), "unusable target incorrectly accepted");
    } else { Require(false, "unknown case"); }
}
'''


class AimTrainerAssetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-assets-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerTarget.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "ANCAimTrainerTarget::ANCAimTrainerTarget",
            "void ANCAimTrainerTarget::PostInitializeComponents",
            "void ANCAimTrainerTarget::BeginPlay",
            "bool ANCAimTrainerTarget::HasCharacterAssets",
        )
        source = directory / "trainer_assets.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_assets.exe" if os.name == "nt" else "trainer_assets")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_assets.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Asset adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_valid_skin_without_anim_class_is_ready_before_begin_play(self): self.run_case("spawn_ready")
    def test_skin_scale_does_not_compound(self): self.run_case("scale_stable")
    def test_dedicated_server_pose_refresh_is_preserved(self): self.run_case("dedicated_pose")
    def test_missing_pawn_template_fails_closed(self): self.run_case("missing_template")
    def test_missing_skin_mesh_fails_closed(self): self.run_case("missing_mesh")


if __name__ == "__main__":
    unittest.main()
