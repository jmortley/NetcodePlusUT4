"""Compile the actual Darken Bodies corpse helpers against a material adapter, without UBT.

The colour math is the engine's own FLinearColor HSV code (Core/Private/Math/Color.cpp) and the
parameter/skip lists are production's, so only the material instance itself is an adapter. It
records parameter reads and writes, proving which parameters a corpse writes and that a repeat
writes nothing. Cooked shaders, the rendered look and the death/ragdoll lifecycle still need an
in-game check; the wiring tests below only pin the call sites.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import PLUGIN, find_compiler, native_function
except ImportError:
    from test_wipeout_healing import PLUGIN, find_compiler, native_function

ENGINE_COLOR = PLUGIN.parents[2] / "Engine/Source/Runtime/Core/Private/Math/Color.cpp"


ADAPTER = r'''
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <math.h>   // the engine HSV body calls bare floorf
#include <iostream>
#include <map>
#include <string>
#include <vector>
#define TEXT(x) x
#define KINDA_SMALL_NUMBER (1.e-4f)
using uint32 = uint32_t;
// GenericPlatformMath.h equivalents for the members the engine HSV bodies use.
struct FMath {
    template<class T> static T Min(T a, T b) { return std::min(a, b); }
    template<class T> static T Max(T a, T b) { return std::max(a, b); }
    template<class T> static T Min3(T a, T b, T c) { return Min(Min(a, b), c); }
    template<class T> static T Max3(T a, T b, T c) { return Max(Max(a, b), c); }
    static float Abs(float v) { return std::fabs(v); }
    static float TruncToFloat(float v) { return std::trunc(v); }
    static float Fmod(float X, float Y) {
        if (std::fabs(Y) <= 1.e-8f) return 0.f;
        const float Quotient = TruncToFloat(X / Y);
        float IntPortion = Y * Quotient;
        if (std::fabs(IntPortion) > std::fabs(X)) IntPortion = X;
        return X - IntPortion;
    }
};
namespace ESearchCase { enum Type { CaseSensitive, IgnoreCase }; }
static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
struct FString : std::string {
    FString() = default;
    FString(const char* s) : std::string(s) {}
    FString(const std::string& s) : std::string(s) {}
    bool Contains(const FString& Sub, ESearchCase::Type Case) const {
        return Case == ESearchCase::IgnoreCase ? Lower(*this).find(Lower(Sub)) != std::string::npos
                                               : find(Sub) != std::string::npos;
    }
};
struct FName : std::string {
    FName() = default;
    FName(const char* s) : std::string(s) {}
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int Num() const { return int(this->size()); }
};
struct FLinearColor {
    float R, G, B, A;
    FLinearColor(float r=0, float g=0, float b=0, float a=1) : R(r), G(g), B(b), A(a) {}
    FLinearColor LinearRGBToHSV() const;
    FLinearColor HSVToLinearRGB() const;
    bool Equals(const FLinearColor& O, float Tolerance=KINDA_SMALL_NUMBER) const {
        return FMath::Abs(R-O.R)<Tolerance && FMath::Abs(G-O.G)<Tolerance
            && FMath::Abs(B-O.B)<Tolerance && FMath::Abs(A-O.A)<Tolerance;
    }
};
struct UMaterialInterface {
    FString Name;
    explicit UMaterialInterface(const char* name) : Name(name) {}
    FString GetName() const { return Name; }
};
// Only parameters present in the maps exist on the material; reads of anything else fail.
struct UMaterialInstanceDynamic {
    const UMaterialInterface* Parent = nullptr;
    FString Name = "MID_Body";
    std::map<std::string, FLinearColor> Vectors;
    std::map<std::string, float> Scalars;
    int VectorWrites = 0, ScalarWrites = 0;
    FString GetName() const { return Name; }
    bool GetVectorParameterValue(FName Param, FLinearColor& Out) const {
        const auto Found = Vectors.find(Param);
        if (Found == Vectors.end()) return false;
        Out = Found->second; return true;
    }
    bool GetScalarParameterValue(FName Param, float& Out) const {
        const auto Found = Scalars.find(Param);
        if (Found == Scalars.end()) return false;
        Out = Found->second; return true;
    }
    void SetVectorParameterValue(FName Param, FLinearColor Value) { ++VectorWrites; Vectors[Param] = Value; }
    void SetScalarParameterValue(FName Param, float Value) { ++ScalarWrites; Scalars[Param] = Value; }
};
struct FNCPlusForceModelsConfig {
    TArray<FName> RecolorParams;
    TArray<FString> SkipMaterialSubstrings;
} Config;
namespace NCPlusForceModels {
    const FNCPlusForceModelsConfig& Get() { return Config; }
    const TArray<FName>& TeamColourParamNames(bool bUseConfiguredOverrides);
    bool IsRecolorSkippedMaterial(const FString& MaterialName, bool bUseConfiguredOverrides);
    FLinearColor GetCorpseColour(const FLinearColor& Live, float Value);
    void DarkenCorpseMaterial(UMaterialInstanceDynamic* MID, float Value, bool bOverlay);
}
'''

CASES = r'''
int Failures=0, Checks=0;
void Check(bool ok, const char* message) {
    ++Checks; if(!ok) { ++Failures; std::cerr<<message<<'\n'; }
}
bool Near(float a, float b, float tolerance=0.0005f) { return std::fabs(a-b)<tolerance; }

int main() {
    using namespace NCPlusForceModels;
    const float Value = 0.1f;

    const FLinearColor Glow = GetCorpseColour(FLinearColor(0.f, 2.75f, 0.f, 1.f), Value);
    const FLinearColor GlowHSV = Glow.LinearRGBToHSV();
    Check(Near(GlowHSV.R, 120.f, 0.5f), "Corpse colour keeps the live hue");
    Check(GlowHSV.G <= 0.35f + 0.0001f, "Corpse colour caps saturation");
    Check(Near(GlowHSV.B, Value), "A Glow-overbright colour drops to the absolute value");
    const FLinearColor Dim = GetCorpseColour(FLinearColor(0.02f, 0.01f, 0.01f, 1.f), Value);
    Check(Near(Dim.LinearRGBToHSV().B, 0.02f), "A colour darker than the target is never brightened");
    Check(Near(GetCorpseColour(FLinearColor(0.2f, 0.4f, 1.f, 0.5f), Value).A, 0.5f), "Alpha is preserved");
    const FLinearColor Black = GetCorpseColour(FLinearColor(1.f, 0.f, 0.f, 1.f), -1.f);
    Check(Near(Black.R, 0.f) && Near(Black.G, 0.f) && Near(Black.B, 0.f), "A negative value clamps to black");
    const FLinearColor Gray = GetCorpseColour(FLinearColor(3.f, 3.f, 3.f, 1.f), Value);
    Check(Near(Gray.R, Value) && Near(Gray.G, Value) && Near(Gray.B, Value), "Gray stays gray at the target value");

    // Engine math over a grid including HDR: idempotent and never brighter, so a repeat
    // application (every refresh of a settled corpse) can never write or creep.
    bool bIdempotent = true, bNeverBrighter = true;
    const float Steps[] = {0.f, 0.001f, 0.05f, 0.1f, 0.3f, 0.7f, 1.f, 2.75f, 10.f, 50.f};
    for (float R : Steps) for (float G : Steps) for (float B : Steps)
    {
        const FLinearColor Live(R, G, B, 1.f);
        const FLinearColor Once = GetCorpseColour(Live, Value);
        bIdempotent = bIdempotent && GetCorpseColour(Once, Value).Equals(Once);
        bNeverBrighter = bNeverBrighter
            && FMath::Max3(Once.R, Once.G, Once.B) <= FMath::Max3(R, G, B) + 1.e-5f;
    }
    Check(bIdempotent, "Engine HSV round trip is idempotent across the grid");
    Check(bNeverBrighter, "No grid colour gets brighter");

    // Every production team parameter is darkened once; a settled repeat writes nothing.
    const TArray<FName>& Names = TeamColourParamNames(false);
    Check(Names.Num() == 42, "Production lists 42 team colour parameters");
    UMaterialInterface BodyParent("M_Skaarj_Body_Inst");
    UMaterialInstanceDynamic Full; Full.Parent = &BodyParent;
    for (const FName& Name : Names) Full.Vectors[Name] = FLinearColor(0.f, 2.75f, 0.f, 1.f);
    Full.Vectors["Color"] = FLinearColor(1.f, 1.f, 1.f, 1.f);
    Full.Scalars["Emissive Max"] = 2.19f;
    Full.Scalars["Emission Power"] = 0.f;
    DarkenCorpseMaterial(&Full, Value, false);
    bool bAllDark = true;
    for (const FName& Name : Names) bAllDark = bAllDark && Near(Full.Vectors[Name].LinearRGBToHSV().B, Value);
    Check(Full.VectorWrites == 42 && bAllDark, "All 42 team parameters are darkened");
    Check(Full.ScalarWrites == 1 && Near(Full.Scalars["Emissive Max"], 0.f), "Only the non-zero highlight scalar is written");
    Check(Full.Vectors["Color"].Equals(FLinearColor(1.f, 1.f, 1.f, 1.f)), "A body's non-team Color is untouched");
    DarkenCorpseMaterial(&Full, Value, false);
    Check(Full.VectorWrites == 42 && Full.ScalarWrites == 1, "A repeat on a settled corpse writes nothing");

    UMaterialInstanceDynamic Sparse; Sparse.Parent = &BodyParent;
    Sparse.Vectors["Red Team Color"] = FLinearColor(1.f, 0.05f, 0.f, 1.f);
    DarkenCorpseMaterial(&Sparse, Value, false);
    Check(Sparse.VectorWrites == 1 && Sparse.Vectors.size() == 1, "Absent parameters are never created");

    // Personal overrides narrow the live recolour; they must not exempt a corpse channel.
    Config.RecolorParams = {"TeamColor"};
    Config.SkipMaterialSubstrings = {"body"};
    UMaterialInstanceDynamic Personal; Personal.Parent = &BodyParent;
    Personal.Vectors["Red Team Color"] = FLinearColor(1.f, 0.05f, 0.f, 1.f);
    DarkenCorpseMaterial(&Personal, Value, false);
    Check(Personal.VectorWrites == 1, "Personal RecolorParams/SkipMaterials cannot exempt a corpse channel");
    Config.RecolorParams.clear();
    Config.SkipMaterialSubstrings.clear();

    UMaterialInterface EyeParent("M_Necris_Eye_Inst");
    UMaterialInstanceDynamic Eye; Eye.Parent = &EyeParent;
    Eye.Vectors["TeamColor"] = FLinearColor(1.f, 0.f, 0.f, 1.f);
    Eye.Scalars["Emissive Max"] = 5.f;
    DarkenCorpseMaterial(&Eye, Value, false);
    Check(Eye.ScalarWrites == 1 && Eye.VectorWrites == 0, "Eyes drop their glow but keep their colour");
    UMaterialInstanceDynamic Orphan; Orphan.Name = "MID_Hair_0";
    Orphan.Vectors["TeamColor"] = FLinearColor(1.f, 0.f, 0.f, 1.f);
    DarkenCorpseMaterial(&Orphan, Value, false);
    Check(Orphan.VectorWrites == 0, "A parent-less MID is skip-checked by its own name");

    UMaterialInterface OverlayParent("M_ShieldBelt_Head_Overlay");
    UMaterialInstanceDynamic Overlay; Overlay.Parent = &OverlayParent;
    Overlay.Vectors["Color"] = FLinearColor(1.f, 1.f, 0.f, 0.6f);
    Overlay.Vectors["TeamColor"] = FLinearColor(1.f, 0.f, 0.f, 1.f);
    DarkenCorpseMaterial(&Overlay, Value, true);
    Check(Overlay.VectorWrites == 2, "An overlay darkens Color and TeamColor, ignoring the face skip list");
    Check(Near(Overlay.Vectors["Color"].LinearRGBToHSV().B, Value) && Near(Overlay.Vectors["Color"].A, 0.6f),
        "The overlay emissive Color is dimmed with its alpha kept");

    DarkenCorpseMaterial(nullptr, Value, false);
    std::cout<<Checks<<" corpse darkening assertions\n";
    return Failures?1:0;
}
'''


def read(relative):
    return (PLUGIN / relative).read_text(encoding="utf-8-sig")


class CorpseDarkenNativeTests(unittest.TestCase):
    def test_native_corpse_colour_and_material_writes(self):
        if not ENGINE_COLOR.is_file():
            self.skipTest("Engine source (Core/Private/Math/Color.cpp) is required for the exact HSV code")
        compiler, environment, msvc = find_compiler()
        engine = ENGINE_COLOR.read_text(encoding="utf-8-sig", errors="replace")
        source = read("Source/Private/NCPlusForceModels.cpp")
        methods = "\n".join(
            [native_function(engine, signature) for signature in (
                "FLinearColor FLinearColor::LinearRGBToHSV() const",
                "FLinearColor FLinearColor::HSVToLinearRGB() const",
            )] + [native_function(source, signature) for signature in (
                "const TArray<FName>& NCPlusForceModels::TeamColourParamNames(bool bUseConfiguredOverrides)",
                "bool NCPlusForceModels::IsRecolorSkippedMaterial(const FString& MaterialName, bool bUseConfiguredOverrides)",
                "FLinearColor NCPlusForceModels::GetCorpseColour(",
                "void NCPlusForceModels::DarkenCorpseMaterial(",
            )])
        with tempfile.TemporaryDirectory(prefix="ncp-corpse-") as temporary:
            directory = Path(temporary)
            native = directory / "corpse.cpp"
            native.write_text(ADAPTER + methods + CASES, encoding="utf-8")
            executable = directory / ("corpse.exe" if os.name == "nt" else "corpse")
            command = ([compiler, "/nologo", "/EHsc", "/std:c++14", str(native), "/Fe:" + str(executable)]
                       if msvc else [compiler, "-std=c++14", str(native), "-o", str(executable)])
            built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("20 corpse darkening assertions", ran.stdout)


class CorpseDarkenWiringTests(unittest.TestCase):
    def setUp(self):
        self.character = read("Source/Private/TeamArenaCharacter.cpp")
        self.models = read("Source/Private/NCPlusForceModels.cpp")

    def test_death_darkens_after_stock_dying(self):
        body = native_function(self.character, "void ATeamArenaCharacter::PlayDying()")
        self.assertLess(body.index("Super::PlayDying();"), body.index("DarkenCorpse();"))
        self.assertLess(body.index("DarkenCorpse();"), body.index("SpawnSkeletonDissolve();"))

    def test_darken_is_client_only_gated_and_one_shot(self):
        body = native_function(self.character, "void ATeamArenaCharacter::DarkenCorpse()")
        for token in ("NM_DedicatedServer", "IsPendingKillPending()", "C.bEnabled", "C.bDarkenBodies"):
            self.assertIn(token, body)
        self.assertLess(body.index("C.bDarkenBodies"), body.index("bCorpseDarkened = true;"))
        self.assertLess(body.index("bCorpseDarkened = true;"), body.index("DarkenCorpseMaterials();"))
        self.assertIn("RefreshForcedArmourOverlay();", body)

    def test_repaint_paths_keep_a_darkened_corpse_dark(self):
        apply = native_function(self.character, "void ATeamArenaCharacter::ApplyForcedModel(bool bForceReapply)")
        guard = apply.index("if (bCorpseDarkened)")
        for write in ("ApplyCharacterData(", "SetScalarParameterValue(", "SetVectorParameterValue(",
                      "AUTCharacter::NotifyTeamChanged()", "UpdateCosmeticStrip("):
            self.assertLess(guard, apply.index(write), write)
        overlay = native_function(self.character, "void ATeamArenaCharacter::RefreshForcedArmourOverlay()")
        corpse = overlay.index("if (bCorpseDarkened)")
        self.assertLess(corpse, overlay.index("C.bArmour"))
        self.assertLess(corpse, overlay.index("SetVectorParameterValue("))
        # The own pawn's early return must not hide its corpse from that branch.
        self.assertIn("(IsLocalPlayerPawn() && !bCorpseDarkened)", overlay)
        # A body-material rebuild re-queues the darken pass in every mode, not only four-team.
        rebuild = native_function(self.character, "void ATeamArenaCharacter::ApplyCharacterData(")
        self.assertIn("(bCorpseDarkened || NCPlusForceModels::IsFourTeamGame(GetWorld()))", rebuild)
        reapply = native_function(self.models, "void NCPlusForceModels::ReapplyAll(UWorld* World)")
        self.assertIn("!C->IsDead()", reapply)

    def test_darken_no_longer_hides_bodies(self):
        body = native_function(self.character, "void ATeamArenaCharacter::SpawnSkeletonDissolve()")
        self.assertNotIn("bDarkenBodies", body)
        self.assertIn("const bool bFadeHide = !bShowRagdoll;", body)

    def test_menu_label_says_what_checking_does(self):
        self.assertIn('MakeFlagCheck(TEXT("Darken Bodies"), &FMConfig.bDarkenBodies)',
                      read("Source/Private/SUTNCPlusMenu.cpp"))


if __name__ == "__main__":
    unittest.main()
