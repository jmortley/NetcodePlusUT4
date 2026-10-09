"""Compile the actual four-team palette and body-capability methods, without UBT.

The adapter supplies material parameter lookup and HSV conversion. It cannot
validate cooked shaders, animation lifecycles, or the rendered result.
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


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(x) x
using int32 = int;
struct FString : std::string {
    using std::string::string;
    bool IsEmpty() const { return empty(); }
};
using FName = std::string;
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    bool IsValidIndex(int i) const { return i >= 0 && i < int(this->size()); }
};
struct FLinearColor {
    float R, G, B, A;
    FLinearColor(float r=0, float g=0, float b=0, float a=1) : R(r), G(g), B(b), A(a) {}
    static const FLinearColor Red, Blue, Green, Yellow, White;
    FLinearColor LinearRGBToHSV() const {
        const float lo=std::min(R,std::min(G,B)), hi=std::max(R,std::max(G,B));
        const float d=hi-lo;
        float h=0;
        if (d>0) {
            if (hi==R) h=std::fmod((G-B)/d,6.f);
            else if (hi==G) h=(B-R)/d+2.f;
            else h=(R-G)/d+4.f;
            h*=60.f;
            if(h<0) h+=360.f;
        }
        return {h,hi>0?d/hi:0,hi,A};
    }
    FLinearColor HSVToLinearRGB() const {
        const float c=B*G, x=c*(1-std::fabs(std::fmod(R/60.f,2.f)-1)), m=B-c;
        FLinearColor out;
        if(R<60) out={c,x,0}; else if(R<120) out={x,c,0};
        else if(R<180) out={0,c,x}; else if(R<240) out={0,x,c};
        else if(R<300) out={x,0,c}; else out={c,0,x};
        return {out.R+m,out.G+m,out.B+m,A};
    }
};
const FLinearColor FLinearColor::Red(1,0,0), FLinearColor::Blue(0,0,1),
    FLinearColor::Green(0,1,0), FLinearColor::Yellow(1,1,0), FLinearColor::White(1,1,1);
enum class ENCPlusSkinStyle { TeamEnemy, RedBlue, EnemyOnly };
enum class ENCPlusArmourMode { MatchSkin, Complimentary };
struct FNCPlusModelSettings {
    FString ContentPath;
    float H=0,S=1,V=1,Brightness=1;
    bool bTint=false,bComplimentary=false;
    ENCPlusArmourMode ArmourMode=ENCPlusArmourMode::MatchSkin;
};
struct FNCPlusForceModelsConfig {
    ENCPlusSkinStyle Style=ENCPlusSkinStyle::TeamEnemy;
    FNCPlusModelSettings Red,Blue,Team,Enemy;
} Config;
struct UWorld { bool Four; };
struct UObject { virtual ~UObject() {} };
struct UClass {
    bool MaterialClass=true;
    bool IsChildOf(const UClass*) const { return MaterialClass; }
};
template<class T> T* Cast(UObject* Object) { return dynamic_cast<T*>(Object); }
struct UMaterialInterface : UObject {
    FString Name;
    bool HasColour;
    UMaterialInterface(const char* name,bool has) : Name(name),HasColour(has) {}
    static UClass* StaticClass() { static UClass Class; return &Class; }
    FString GetName() const { return Name; }
    bool GetVectorParameterValue(const FName& Param,FLinearColor& Out) const {
        return HasColour && Param=="TeamColor";
    }
};
struct USkeletalMeshComponent {
    bool SkeletalMesh=true;
    TArray<UMaterialInterface*> Materials;
    int GetNumMaterials() const { return int(Materials.size()); }
    UMaterialInterface* GetMaterial(int i) const { return Materials[i]; }
};
struct UArrayProperty;
struct AUTCharacterContent {
    USkeletalMeshComponent* GetMesh() const { return Mesh; }
    UClass* GetClass() const { static UClass Class; return &Class; }
    void SetMesh(USkeletalMeshComponent* value) { Mesh=value; }
    void SetTeamMaterials(TArray<UMaterialInterface*> value) { TeamMaterials=value; }
    void SetArmMaterials(TArray<UMaterialInterface*> value) { TeamMaterials1p=value; }
protected:
    // Match UTCharacterContent's access restriction: production code cannot read these directly.
    friend struct UArrayProperty;
    USkeletalMeshComponent* Mesh=nullptr;
    TArray<UMaterialInterface*> TeamMaterials;
    TArray<UMaterialInterface*> TeamMaterials1p;
};
struct UObjectProperty : UObject {
    UClass* PropertyClass=UMaterialInterface::StaticClass();
    UObject* GetObjectPropertyValue(const void* raw) const {
        return *static_cast<UMaterialInterface* const*>(raw);
    }
} MaterialProperty;
struct UArrayProperty : UObject {
    UObject* Inner=&MaterialProperty;
    template<class T> const T* ContainerPtrToValuePtr(const AUTCharacterContent* Data) const {
        return static_cast<const T*>(&Data->TeamMaterials);
    }
} TeamMaterialsProperty;
bool HasTeamMaterialsProperty=true;
template<class T> T* FindField(UClass*,const char*) {
    return HasTeamMaterialsProperty ? static_cast<T*>(&TeamMaterialsProperty) : nullptr;
}
struct FScriptArrayHelper {
    const TArray<UMaterialInterface*>* Items;
    FScriptArrayHelper(UArrayProperty*,const void* data) : Items(static_cast<const TArray<UMaterialInterface*>*>(data)) {}
    int Num() const { return int(Items->size()); }
    const void* GetRawPtr(int i) const { return &(*Items)[i]; }
};
template<class T> struct TSubclassOf {
    T* Object;
    TSubclassOf(T* object=nullptr) : Object(object) {}
    T* GetDefaultObject() const { return Object; }
};
namespace NCPlusForceModels {
    const FNCPlusForceModelsConfig& Get() { return Config; }
    bool IsFourTeamGame(UWorld* World) { return World && World->Four; }
    FLinearColor GetFourTeamColour(int32);
    FNCPlusModelSettings GetModelSettings(int32,bool,UWorld*);
    FLinearColor GetSkinColour(const FNCPlusModelSettings&);
    bool CanTintBodyContent(TSubclassOf<AUTCharacterContent>);
    bool IsRecolorSkippedMaterial(const FString& Name,bool configured) { return Name=="head"; }
    bool IsBakedMaterial(const FString& Name) { return Name=="baked"; }
    const TArray<FName>& TeamColourParamNames(bool configured) {
        static TArray<FName> Params={"TeamColor"}; return Params;
    }
}
int Failures=0,Checks=0;
void Check(bool ok,const char* message) {
    ++Checks; if(!ok) { ++Failures; std::cerr<<message<<'\n'; }
}
bool Near(float a,float b) { return std::fabs(a-b)<0.0001f; }
'''

CASES = r'''
int main() {
    using namespace NCPlusForceModels;
    UWorld Four{true}, Two{false};
    Config.Team.ContentPath="team"; Config.Enemy.ContentPath="enemy";
    Config.Team.H=15; Config.Enemy.H=310;
    for(auto style : {ENCPlusSkinStyle::TeamEnemy,ENCPlusSkinStyle::RedBlue,ENCPlusSkinStyle::EnemyOnly}) {
        Config.Style=style;
        for(bool friendly : {false,true}) {
            const auto side=GetModelSettings(1,friendly,&Four);
            const auto rgb=GetSkinColour(side);
            Check(Near(rgb.R,.2f)&&Near(rgb.G,.5f)&&Near(rgb.B,1.f),"Blue keeps HUD RGB through HSV resolution");
            Check(side.bTint&&!side.bComplimentary,"Four-team identity overrides personal hue mode");
        }
    }
    Config.Style=ENCPlusSkinStyle::RedBlue;
    const auto stock=GetModelSettings(1,false,&Two);
    Check(Near(stock.H,240)&&Near(stock.S,.9f),"Two-team RedBlue keeps prior hue and saturation");
    Check(GetModelSettings(2,true,&Four).ContentPath=="team","Green friendly keeps chosen model");
    Check(GetModelSettings(3,false,&Four).ContentPath=="enemy","Yellow enemy keeps chosen model");
    UMaterialInterface flat{"body",false}, tint{"body",true}, face{"head",true}, baked{"baked",true};
    USkeletalMeshComponent mesh; mesh.Materials={&flat};
    AUTCharacterContent content; content.SetMesh(&mesh); content.SetArmMaterials({&tint});
    Check(!CanTintBodyContent(&content),"Tintable arms cannot qualify an untintable body");
    content.SetTeamMaterials({&tint});
    Check(CanTintBodyContent(&content),"Tintable authored team override qualifies the body");
    content.SetTeamMaterials({&baked});
    Check(!CanTintBodyContent(&content),"Known baked parameters require fallback despite parameter presence");
    content.SetTeamMaterials({&face});
    Check(!CanTintBodyContent(&content),"Tintable face cannot qualify an untintable body");
    content.SetTeamMaterials({}); mesh.Materials={&tint,&face};
    Check(CanTintBodyContent(&content),"Tintable mesh fallback ignores natural face materials");
    HasTeamMaterialsProperty=false;
    Check(!CanTintBodyContent(&content),"Missing reflected array fails closed");
    HasTeamMaterialsProperty=true;
    UObject WrongInner; TeamMaterialsProperty.Inner=&WrongInner;
    Check(!CanTintBodyContent(&content),"Non-object array fails closed");
    TeamMaterialsProperty.Inner=&MaterialProperty;
    UClass WrongClass; WrongClass.MaterialClass=false; MaterialProperty.PropertyClass=&WrongClass;
    Check(!CanTintBodyContent(&content),"Non-material object array fails closed");
    MaterialProperty.PropertyClass=UMaterialInterface::StaticClass();
    mesh.SkeletalMesh=false;
    Check(!CanTintBodyContent(&content),"Missing skeletal mesh fails closed");
    Check(!CanTintBodyContent(nullptr),"Missing content fails closed");
    std::cout<<Checks<<" appearance assertions\n";
    return Failures?1:0;
}
'''


class FourTeamAppearanceTests(unittest.TestCase):
    def test_native_palette_and_body_capabilities(self):
        compiler, environment, msvc = find_compiler()
        source = (PLUGIN / "Source/Private/NCPlusForceModels.cpp").read_text(encoding="utf-8-sig")
        methods = "\n".join(native_function(source, signature) for signature in (
            "FLinearColor NCPlusForceModels::GetFourTeamColour(",
            "FNCPlusModelSettings NCPlusForceModels::GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly, UWorld* World)",
            "FLinearColor NCPlusForceModels::GetSkinColour(",
            "bool NCPlusForceModels::CanTintBodyContent(",
        ))
        with tempfile.TemporaryDirectory(prefix="ncp-appearance-") as temporary:
            directory = Path(temporary)
            native = directory / "appearance.cpp"
            native.write_text(ADAPTER + methods + CASES, encoding="utf-8")
            executable = directory / ("appearance.exe" if os.name == "nt" else "appearance")
            command = ([compiler, "/nologo", "/EHsc", "/std:c++14", str(native), "/Fe:" + str(executable)]
                       if msvc else [compiler, "-std=c++14", str(native), "-o", str(executable)])
            built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("25 appearance assertions", ran.stdout)


if __name__ == "__main__":
    unittest.main()
