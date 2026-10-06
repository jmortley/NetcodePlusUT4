"""Exercise the actual hide resolver and beam/muzzle paths with UT type adapters.

This checks settings precedence and effect origins, not the rendered skeletal
mesh. The authored arms still require a packaged client playtest.
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
#include <map>
#include <string>
#include <vector>
#define TEXT(x) x
using int32 = int;
using uint8 = unsigned char;
constexpr int INDEX_NONE = -1;
constexpr int ROLE_Authority = 3;
struct FString {
    std::string Value;
    const char* operator*() const { return Value.c_str(); }
};
struct FName {
    std::string Value;
    explicit FName(const char* value) : Value(value) {}
    bool operator==(const FName& other) const { return Value == other.Value; }
    bool operator<(const FName& other) const { return Value < other.Value; }
};
struct Settings {
    std::map<FName, bool> Values;
    void Add(const FName& key, bool value) { Values[key] = value; }
    const bool* Find(const FName& key) const {
        auto found = Values.find(key);
        return found == Values.end() ? nullptr : &found->second;
    }
};
struct UClass {
    FString Name;
    FString GetName() const { return Name; }
};
struct FVector {
    float X, Y, Z;
    FVector(float x=0.f, float y=0.f, float z=0.f) : X(x), Y(y), Z(z) {}
    FVector operator+(FVector rhs) const { return FVector(X+rhs.X,Y+rhs.Y,Z+rhs.Z); }
    FVector operator*(float scalar) const { return FVector(X*scalar,Y*scalar,Z*scalar); }
};
struct FRotator { FVector Vector() const { return FVector(1.f,0.f,0.f); } };
struct Camera {
    FRotator GetComponentRotation() const { return FRotator(); }
    FVector GetComponentLocation() const { return FVector(100.f,200.f,300.f); }
};
struct ANCAimTrainerCharacter { static int StaticClass() { return 99; } };
struct AUTCharacter {
    bool Trainer = true;
    Camera View;
    Camera* CharacterCameraComponent = &View;
    void* Controller = nullptr;
    uint8 FireMode = 0;
    bool IsA(int klass) const { return Trainer && klass == 99; }
};
struct UParticleSystemComponent {};
struct Particles {
    std::vector<UParticleSystemComponent*> Values;
    bool IsValidIndex(int index) const { return index >= 0 && index < static_cast<int>(Values.size()); }
    UParticleSystemComponent*& operator[](int index) { return Values[index]; }
};
struct AUTWeapon {
    UClass Class;
    AUTCharacter* UTOwner = nullptr;
    Particles MuzzleFlash;
    int Role = ROLE_Authority;
    uint8 CurrentFireMode = 0;
    int SocketOrigins = 0, EffectCalls = 0;
    bool MuzzleVisibleToSuper = false;
    const UClass* GetClass() const { return &Class; }
    void GetImpactSpawnPosition(const FVector&, FVector& origin, FRotator&) {
        ++SocketOrigins; origin = FVector(9.f,8.f,7.f);
    }
    void PlayFiringEffects() {
        ++EffectCalls;
        MuzzleVisibleToSuper = MuzzleFlash.IsValidIndex(CurrentFireMode) && MuzzleFlash[CurrentFireMode] != nullptr;
    }
};
struct AUTWeaponFix : AUTWeapon {
    using Super = AUTWeapon;
    static Settings HiddenWeaponsByTag;
    static bool bClassicWeaponHide;
    static float HiddenBeamBackOffset, HiddenBeamDownOffset;
    static bool IsWeaponHiddenBySettings(const AUTWeapon*, const AUTCharacter*);
    void GetImpactSpawnPosition(const FVector&, FVector&, FRotator&);
    void PlayFiringEffects();
};
Settings AUTWeaponFix::HiddenWeaponsByTag;
bool AUTWeaponFix::bClassicWeaponHide = true;
float AUTWeaponFix::HiddenBeamBackOffset = 60.f;
float AUTWeaponFix::HiddenBeamDownOffset = 30.f;
'''

CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    Require(argc == 2, "choose a case");
    const std::string name = argv[1];
    AUTCharacter owner;
    AUTWeaponFix weapon;
    weapon.Class.Name.Value = "N+InstagibRifle_C";
    weapon.UTOwner = &owner;
    UParticleSystemComponent muzzle;
    weapon.MuzzleFlash.Values.push_back(&muzzle);
    AUTWeaponFix::HiddenWeaponsByTag.Add(FName("UTNPShockRifle_C"), true);
    bool expectedHidden = true;
    bool expectedCamera = true;
    if (name == "explicit_show") {
        AUTWeaponFix::HiddenWeaponsByTag.Add(FName("N+InstagibRifle_C"), false);
        expectedHidden = expectedCamera = false;
    } else if (name == "explicit_hide") {
        AUTWeaponFix::HiddenWeaponsByTag.Add(FName("UTNPShockRifle_C"), false);
        AUTWeaponFix::HiddenWeaponsByTag.Add(FName("N+InstagibRifle_C"), true);
    } else if (name == "ordinary_game") {
        owner.Trainer = false; expectedHidden = expectedCamera = false;
    } else if (name == "unrelated_weapon") {
        weapon.Class.Name.Value = "UTNPLightningGun_C"; expectedHidden = expectedCamera = false;
    } else if (name == "bp_parity") {
        AUTWeaponFix::bClassicWeaponHide = false; expectedCamera = false;
    } else if (name == "no_shock_preference") {
        AUTWeaponFix::HiddenWeaponsByTag.Values.clear(); expectedHidden = expectedCamera = false;
    } else if (name == "no_owner") {
        weapon.UTOwner = nullptr; expectedHidden = expectedCamera = false;
    } else { Require(name == "inherited_hide", "unknown case"); }
    const auto before = AUTWeaponFix::HiddenWeaponsByTag.Values;
    Require(!AUTWeaponFix::IsWeaponHiddenBySettings(nullptr, &owner), "null weapon did not fail open to visible");
    Require(AUTWeaponFix::IsWeaponHiddenBySettings(&weapon, weapon.UTOwner) == expectedHidden,
            "hide preference precedence/scope is wrong");
    FVector origin;
    FRotator rotation;
    weapon.GetImpactSpawnPosition(FVector(), origin, rotation);
    Require(weapon.SocketOrigins == (expectedCamera ? 0 : 1), "beam chose a different hide policy than mesh");
    if (expectedCamera) {
        Require(origin.X == 40.f && origin.Y == 200.f && origin.Z == 270.f,
                "classic hidden beam did not honor saved back/down offsets");
    } else {
        Require(origin.X == 9.f && origin.Y == 8.f && origin.Z == 7.f,
                "visible or BP-parity weapon lost its socket origin");
    }
    weapon.PlayFiringEffects();
    Require(weapon.EffectCalls == 1, "hide policy suppressed sound/animation effects");
    Require(weapon.MuzzleVisibleToSuper != expectedCamera,
            "classic hidden muzzle flash disagrees with relocated beam");
    Require(weapon.MuzzleFlash[0] == &muzzle, "temporary flash suppression was not restored");
    Require(AUTWeaponFix::HiddenWeaponsByTag.Values == before, "fallback wrote a persistent class override");
}
'''


class AimTrainerWeaponPresentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-presentation-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "bool AUTWeaponFix::IsWeaponHiddenBySettings",
            "void AUTWeaponFix::GetImpactSpawnPosition",
            "void AUTWeaponFix::PlayFiringEffects",
        )
        source = directory / "trainer_presentation.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_presentation.exe" if os.name == "nt" else "trainer_presentation")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_presentation.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Presentation adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_trainer_inherits_shock_hide_and_saved_beam_offsets(self): self.run_case("inherited_hide")
    def test_explicit_instagib_show_wins_over_shock_hide(self): self.run_case("explicit_show")
    def test_explicit_instagib_hide_wins_over_shock_show(self): self.run_case("explicit_hide")
    def test_ordinary_instagib_has_no_new_fallback(self): self.run_case("ordinary_game")
    def test_other_trainer_weapons_keep_independent_preferences(self): self.run_case("unrelated_weapon")
    def test_bp_parity_hide_keeps_socket_beam_origin(self): self.run_case("bp_parity")
    def test_no_saved_shock_choice_remains_visible(self): self.run_case("no_shock_preference")
    def test_missing_owner_does_not_use_trainer_fallback(self): self.run_case("no_owner")


if __name__ == "__main__":
    unittest.main()
