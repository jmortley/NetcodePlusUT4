"""Compile target and trainee initialization against UT's content/pawn separation.

CharacterContent intentionally has a mesh with no animation Blueprint. The
stock pawn Blueprint supplies that class and capsule-relative mesh transform.
The adapter models ApplyCharacterData's class-default scale rule and UT's local
cosmetic attachment lifecycle, not an Unreal asset loader; a packaged playtest
is still required for the real cooked assets and animation Blueprint.
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
#include <vector>
#include <map>
#include <cmath>
#define TEXT(x) x
using int32=int;
using FName=std::string;
using FString=std::string;
constexpr int NM_DedicatedServer=1;
template<class T> using TSubclassOf=T*;
template<class T> struct TArray : std::vector<T> {
    void Reset() { this->clear(); }
    void Add(const T& value) { this->push_back(value); }
    int Num() const { return int(this->size()); }
};
template<class K,class V> struct TMap {
    struct Pair { K Key; V Value; };
    std::vector<Pair> Values;
    void Add(const K& key,const V& value) { Values.push_back({key,value}); }
    int Num() const { return int(Values.size()); }
    auto begin() const -> decltype(Values.begin()) { return Values.begin(); }
    auto end() const -> decltype(Values.end()) { return Values.end(); }
};
template<class T> struct TWeakObjectPtr {
    T* Value=nullptr;
    TWeakObjectPtr& operator=(T* value) { Value=value;return *this; }
    T* Get() const { return Value; }
};
struct FMath {
    static float Clamp(float x,float low,float high) { return x<low?low:x>high?high:x; }
    static float Min(float a,float b) { return a<b?a:b; }
};
struct FObjectInitializer {
    mutable int MovementType = 0;
    template<class T> const FObjectInitializer& SetDefaultSubobjectClass(int) const {
        MovementType = T::Type; return *this;
    }
};
struct ACharacter { static constexpr int CharacterMovementComponentName = 1; };
struct UNCAimTrainerMovement { static constexpr int Type = 23; };
struct FVector {
    float X, Y, Z;
    explicit FVector(float value) : X(value), Y(value), Z(value) {}
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}
};
struct AUTCharacter;
struct UAnimInstance {};
struct UClass {
    AUTCharacter* Object = nullptr;
    template<class T> const T* GetDefaultObject() const { return static_cast<const T*>(Object); }
};
struct Transform { float Z = 0.f, Yaw = 0.f, Scale = 1.f, X = 0.f; };
enum class EMeshComponentUpdateFlag { OnlyTickPoseWhenRendered, AlwaysTickPoseAndRefreshBones };
struct Mesh {
    Transform Relative;
    UClass* AnimClass = nullptr;
    void* SkeletalMesh = nullptr;
    EMeshComponentUpdateFlag MeshComponentUpdateFlag = EMeshComponentUpdateFlag::OnlyTickPoseWhenRendered;
    bool bEnableUpdateRateOptimizations = true;
    bool bCastHiddenShadow = true;
    void SetRelativeTransform(Transform value) { Relative = value; }
    Transform GetRelativeTransform() const { return Relative; }
    void SetAnimInstanceClass(UClass* value) { AnimClass = value; }
    void SetRelativeLocation(FVector value) { Relative.Z = value.Z; Relative.X = value.X; }
    void SetRelativeScale3D(FVector value) { Relative.Scale = value.X; }
};
struct AUTWeaponAttachment {
    struct Mesh Body;
    struct Mesh* Mesh = &Body;
    bool Hidden = false, Collision = true;
    void SetActorHiddenInGame(bool value) { Hidden = value; }
    void SetActorEnableCollision(bool value) { Collision = value; }
};
struct FLinearColor {
    float R=0.f,G=0.f,B=0.f,A=1.f;
    FLinearColor operator*(float x) const { return {R*x,G*x,B*x,A*x}; }
    static const FLinearColor Transparent;
};
const FLinearColor FLinearColor::Transparent;
struct UMaterialInterface {
    std::string Name="body";
    std::string GetName() const { return Name; }
};
struct UMaterialInstanceDynamic : UMaterialInterface {
    UMaterialInterface* Parent=nullptr;
    std::map<std::string,FLinearColor> Vectors;
    std::map<std::string,float> Scalars;
    void SetVectorParameterValue(const std::string& name, FLinearColor value) { Vectors[name]=value; }
    void SetScalarParameterValue(const std::string& name, float value) { Scalars[name]=value; }
    bool GetVectorParameterValue(const std::string& name,FLinearColor& value) {
        auto found=Vectors.find(name);if(found==Vectors.end())return false;value=found->second;return true;
    }
    bool GetScalarParameterValue(const std::string& name,float& value) {
        auto found=Scalars.find(name);if(found==Scalars.end())return false;value=found->second;return true;
    }
};
struct AUTCharacterContent { Mesh Body; };
struct Movement {
    bool bRunPhysicsWithNoController = false, bOrientRotationToMovement = true;
    bool bUseControllerDesiredRotation = true;
    float MaxWalkSpeed = 0.f, MaxWalkSpeedCrouched = 0.f, MaxAcceleration = 0.f;
    float DefaultBrakingDecelerationWalking = 0.f, BrakingDecelerationWalking = 0.f, GroundFriction = 0.f;
    float DodgeAirControl = 0.f, CrouchedHalfHeight = 0.f, NetworkSimulatedSmoothLocationTime = 0.f;
    float EasyImpactImpulse = 0.f, EasyImpactDamage = 0.f, FullImpactImpulse = 0.f, FullImpactDamage = 0.f;
    float ImpactMaxHorizontalVelocity = 0.f, MaxInitialFloorSlideSpeed = 0.f, MaxFloorSlideSpeed = 0.f;
    float MaxFastAccelSpeed = 0.f, MaxStepHeight = 0.f, NetworkMaxSmoothUpdateDistance = 0.f;
    struct NavProperties { bool bCanCrouch = false; } Nav;
    NavProperties& GetNavAgentPropertiesRef() { return Nav; }
};
enum class EAutoPossessAI { Disabled, Enabled };
struct AUTCharacter {
    Mesh Body, Hands;
    Mesh* FirstPersonMesh = &Hands;
    Movement Move;
    Movement* UTCharacterMovement = &Move;
    struct Capsule {
        float Radius = 0.f, HalfHeight = 0.f;
        void InitCapsuleSize(float radius, float halfHeight) { Radius=radius; HalfHeight=halfHeight; }
    } Shape;
    float BaseEyeHeight = 0.f, DefaultBaseEyeHeight = 0.f, CrouchedEyeHeight = 0.f;
    float DefaultCrouchedEyeHeight = 0.f, FloorSlideEyeHeight = 0.f, SlideTargetHeight = 0.f;
    int MovementType = 0;
    AUTCharacterContent* CharacterData = nullptr;
    int PostInitCalls = 0, BeginCalls = 0, Applies = 0;
    int VisibilityUpdates = 0, AttachmentUpdates = 0, AttachmentSpawns = 0;
    bool Hidden = false, Collision = true, DedicatedServer = false;
    bool DeferAttachmentCreation = false, AttachmentSpawnedAfterBegin = false;
    UClass* WeaponAttachmentClass = nullptr;
    AUTWeaponAttachment LocalAttachment;
    AUTWeaponAttachment* WeaponAttachment = nullptr;
    TArray<UMaterialInstanceDynamic*> BodyMIs;
    const TArray<UMaterialInstanceDynamic*>& GetBodyMIs() const { return BodyMIs; }
    AUTCharacter* DefaultActor=nullptr;
    UClass Class;
    UClass* GetClass() { Class.Object=DefaultActor?DefaultActor:this;return &Class; }
    int GetNetMode() const { return DedicatedServer?NM_DedicatedServer:0; }
    void* GetWorld() const { return nullptr; }
    float ClassDefaultMeshScale = 1.f;
    bool bAlwaysRelevant = false;
    float NetUpdateFrequency = 0.f, MinNetUpdateFrequency = 0.f;
    int Health = 0, HealthMax = 0, ArmorAmount = 0;
    EAutoPossessAI AutoPossessAI = EAutoPossessAI::Enabled;
    AUTCharacter() = default;
    explicit AUTCharacter(const FObjectInitializer& init) : MovementType(init.MovementType) {}
    Mesh* GetMesh() const { return const_cast<Mesh*>(&Body); }
    Movement* GetCharacterMovement() { return &Move; }
    Movement* GetUTCharacterMovement() { return &Move; }
    Capsule* GetCapsuleComponent() { return &Shape; }
    void PostInitializeComponents() { ++PostInitCalls; }
    void BeginPlay() { ++BeginCalls; }
    void SetActorHiddenInGame(bool value) { Hidden = value; ++VisibilityUpdates; }
    void SetActorEnableCollision(bool value) { Collision = value; }
    void SetBodyColorFlash(void*, bool) {}
    void UpdateWeaponAttachment() {
        ++AttachmentUpdates;
        // Stock UT creates the attachment locally, after actor BeginPlay, and
        // intentionally omits it from dedicated-server worlds.
        if (!DedicatedServer && !DeferAttachmentCreation && WeaponAttachmentClass && !WeaponAttachment) {
            WeaponAttachment = &LocalAttachment;
            ++AttachmentSpawns;
            AttachmentSpawnedAfterBegin = BeginCalls > 0;
        }
    }
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
UClass* AvailableBaseTemplate = nullptr;
UClass* AvailablePlayerTemplate = nullptr;
UClass* AvailableRifleTemplate = nullptr;
UClass* AvailableUT3Animation = nullptr;
namespace ConstructorHelpers {
template<class T> struct FClassFinder {
    UClass* Class;
    explicit FClassFinder(const char* path) : Class(nullptr) {
        const std::string asset = path;
        if (asset == "/Game/RestrictedAssets/Blueprints/BaseUTCharacter") Class = AvailableBaseTemplate;
        else if (asset == "/Game/RestrictedAssets/Blueprints/DefaultCharacter") Class = AvailablePlayerTemplate;
        else if (asset == "/Game/RestrictedAssets/Weapons/ShockRifle/ShockAttachment") Class = AvailableRifleTemplate;
        else if (asset == "/Game/RestrictedAssets/Character/Base/Blueprints/Base_3p_AnimBP_UT3") Class = AvailableUT3Animation;
    }
};
}
struct FNCPlusModelSettings { bool bTint=true;float Brightness=2.f; };
namespace NCPlusForceModels {
    bool Enabled=true;
    bool ModelSelected=false;
    int ViewerTeam=0,EnemyTeam=-1;
    FNCPlusModelSettings Side;
    AUTCharacterContent Selected;
    bool IsEnabled() { return Enabled; }
    int GetViewerTeam(void*) { return ViewerTeam; }
    FNCPlusModelSettings GetModelSettings(int team,bool friendly,void*) {
        if(friendly)std::exit(3);EnemyTeam=team;return Side;
    }
    AUTCharacterContent* GetModelClass(const FNCPlusModelSettings&) { return ModelSelected?&Selected:nullptr; }
    bool IsModelAllowed(AUTCharacterContent*) { return true; }
    FLinearColor GetSkinColour(const FNCPlusModelSettings&) { return {.1f,.5f,.2f,1.f}; }
    const TArray<FName>& TeamColourParamNames() {
        static TArray<FName> params; if(params.empty())params.Add("TeamColor");return params;
    }
    bool IsRecolorSkippedMaterial(const std::string& name) { return name=="head"; }
    bool IsBakedMaterial(const std::string& name) { return name=="baked"; }
}
struct ANCAimTrainerTarget : AUTCharacter {
    using Super = AUTCharacter;
    bool bTrainerVisible = false;
    explicit ANCAimTrainerTarget(const FObjectInitializer&);
    void PostInitializeComponents();
    void ApplyCharacterData(TSubclassOf<AUTCharacterContent>);
    struct FTrainerMaterialTint {
        TWeakObjectPtr<UMaterialInstanceDynamic> Material;
        TMap<FName,FLinearColor> Vectors;
        TMap<FName,float> Scalars;
    };
    TArray<FTrainerMaterialTint> TrainerTintMaterials;
    float NextTrainerTintTime=0.f;
    void UpdateTrainerTint();
    void BeginPlay();
    void UpdateWeaponAttachment();
    bool HasCharacterAssets() const;
    void UpdateDrillFlag() {}
    void OnRep_TrainerVisible();
};
struct ANCAimTrainerCharacter : AUTCharacter {
    using Super = AUTCharacter;
    explicit ANCAimTrainerCharacter(const FObjectInitializer&);
};
struct ANCAimTrainerInstagibCharacter : ANCAimTrainerCharacter {
    using Super = ANCAimTrainerCharacter;
    explicit ANCAimTrainerInstagibCharacter(const FObjectInitializer&);
};
struct ANCAimTrainerInstagibTarget : ANCAimTrainerTarget {
    using Super = ANCAimTrainerTarget;
    explicit ANCAimTrainerInstagibTarget(const FObjectInitializer&);
};
struct ANCAimTrainerSACTFTarget : ANCAimTrainerTarget {
    using Super = ANCAimTrainerTarget;
    explicit ANCAimTrainerSACTFTarget(const FObjectInitializer&);
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
    UClass ut3Animation;
    AvailableUT3Animation = name == "missing_template" || name == "missing_ut3" ? nullptr : &ut3Animation;
    UClass handsAnimation;
    UClass rifleTemplate;
    AvailableRifleTemplate = name == "missing_rifle" ? nullptr : &rifleTemplate;
    AUTCharacter stock;
    stock.Body.AnimClass = &animation;
    stock.Body.Relative = {-108.f, -90.f, 1.25f};
    // Actual authored distinction: BaseUTCharacter has no first-person
    // AnimClass and parks arms at (-30,0,10), yaw 0. DefaultCharacter supplies
    // Base_1p_AnimBP and (-15,0,0), yaw -90. A generic template stub hid this bug.
    stock.Hands.Relative = {10.f, 0.f, 1.f, -30.f};
    UClass templateClass;
    templateClass.Object = &stock;
    AvailableBaseTemplate = (name == "missing_template") ? nullptr : &templateClass;
    AUTCharacter playable;
    playable.Body.AnimClass = &animation;
    playable.Body.Relative = {-110.f, -90.f, 1.f};
    playable.Hands.AnimClass = &handsAnimation;
    playable.Hands.Relative = {0.f, -90.f, 1.f, -15.f};
    if (name == "trainee_missing_arms") playable.FirstPersonMesh = nullptr;
    UClass playerTemplateClass;
    playerTemplateClass.Object = &playable;
    AvailablePlayerTemplate = (name == "trainee_missing_template") ? nullptr : &playerTemplateClass;
    if (name.find("trainee_") == 0) {
        ANCAimTrainerCharacter trainee(initializer);
        Require(trainee.MovementType == UNCAimTrainerMovement::Type,
                "authored asset setup replaced the trainer movement selection");
        Require(trainee.Applies == 0, "constructor prematurely applied skin before possession");
        if (name == "trainee_missing_template") {
            Require(!trainee.Body.AnimClass && !trainee.Hands.AnimClass,
                    "missing template manufactured animation defaults");
        } else {
            Require(trainee.Body.AnimClass == &animation && trainee.Body.Relative.Z == -110.f
                    && trainee.Body.Relative.Yaw == -90.f && trainee.Body.Relative.Scale == 1.f,
                    "native trainee lost body animation or capsule-relative placement");
            if (name == "trainee_defaults") {
                Require(trainee.Hands.AnimClass == &handsAnimation && trainee.Hands.Relative.Z == 0.f
                        && trainee.Hands.Relative.Yaw == -90.f && trainee.Hands.Relative.Scale == 1.f
                        && trainee.Hands.Relative.X == -15.f,
                        "native trainee copied incomplete base arms instead of playable weapon socket pose");
            } else {
                Require(name == "trainee_missing_arms" && !trainee.Hands.AnimClass,
                        "missing optional template arms were dereferenced or invented");
            }
        }
        return 0;
    }
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
        Require(target.Body.AnimClass == &ut3Animation, "lost pawn animation class to skin's null AnimClass");
        Require(target.PostInitCalls == 1 && target.BeginCalls == 0, "assets depend on BeginPlay");
        Require(target.Body.Relative.Z == -110.f && target.Body.Relative.Yaw == -90.f,
                "mesh did not inherit stock capsule-relative placement");
        Require(target.Body.Relative.Scale == .8f, "skin scale did not use pawn class defaults");
        target.BeginPlay();
        Require(target.Applies == 1 && target.BeginCalls == 1 && target.VisibilityUpdates == 1,
                "BeginPlay reapplied assets or lost visibility initialization");
    } else if (name == "explicit_ut3") {
        Require(target.HasCharacterAssets()&&target.Body.AnimClass==&ut3Animation,
                "targets still inherited the unspecified stock animation instead of explicit UT3");
        target.PostInitializeComponents();
        Require(target.Body.AnimClass==&ut3Animation,
                "skin reapplication replaced explicit UT3 animation");
        ANCAimTrainerInstagibTarget igTarget(initializer);
        igTarget.CharacterData=&skin;igTarget.PostInitializeComponents();
        Require(igTarget.Body.AnimClass==&ut3Animation,
                "instagib target constructor lost shared UT3 animation");
        ANCAimTrainerSACTFTarget saTarget(initializer);
        saTarget.CharacterData=&skin;saTarget.PostInitializeComponents();
        Require(saTarget.Body.AnimClass==&ut3Animation,
                "SACTF target constructor lost shared UT3 animation");
        ANCAimTrainerCharacter trainee(initializer);
        Require(trainee.Body.AnimClass==&animation&&trainee.Hands.AnimClass==&handsAnimation,
                "target animation selection changed trainee body or first-person arms");
    } else if (name == "locked_model_tint") {
        ANCAimTrainerTarget defaults(initializer);defaults.CharacterData=&skin;target.DefaultActor=&defaults;
        AUTCharacterContent alternate;
        int alternateMesh=2;alternate.Body.SkeletalMesh=&alternateMesh;
        UClass* oldAnimation=target.Body.AnimClass;
        target.ApplyCharacterData(&alternate);
        Require(target.Body.SkeletalMesh==&meshResource&&target.Body.AnimClass==oldAnimation,
                "forced model replaced training geometry or its animation");
        UMaterialInstanceDynamic body,head,baked;
        body.Vectors["TeamColor"]={.2f,.3f,.4f,1.f};body.Vectors["HitFlashColor"]={.8f,0.f,0.f,1.f};
        body.Scalars["TeamSelect"]=0.f;body.Scalars["Team Color Blend Max"]=.4f;
        body.Scalars["Emissive Max"]=.2f;
        head.Name="head";head.Vectors["TeamColor"]={.3f,.2f,.1f,1.f};
        baked.Name="baked";baked.Vectors["TeamColor"]={.3f,.2f,.1f,1.f};
        target.BodyMIs={};target.BodyMIs.Add(&body);target.BodyMIs.Add(&head);target.BodyMIs.Add(&baked);
        const int applies=target.Applies;
        target.UpdateTrainerTint();
        Require(NCPlusForceModels::EnemyTeam==1&&body.Vectors["TeamColor"].G==1.f
                &&body.Scalars["TeamSelect"]==255.f&&body.Scalars["Team Color Blend Max"]==1.f,
                "teamless trainer did not use the local enemy color preferences");
        Require(head.Vectors["TeamColor"].R==.3f&&baked.Vectors["TeamColor"].R==.3f
                &&body.Vectors["HitFlashColor"].R==.8f&&target.Applies==applies,
                "tint changed excluded materials, hit flash, or rebuilt the target model");
        NCPlusForceModels::ViewerTeam=1;NCPlusForceModels::Side.Brightness=8.f;
        target.UpdateTrainerTint();
        Require(NCPlusForceModels::EnemyTeam==0&&body.Vectors["TeamColor"].G==1.75f
                &&body.Scalars["Emissive Max"]==2.5f,"tint compounded or exceeded native glow caps");
        NCPlusForceModels::Enabled=false;target.UpdateTrainerTint();
        Require(body.Vectors["TeamColor"].G==.3f&&body.Scalars["TeamSelect"]==0.f
                &&body.Scalars["Team Color Blend Max"]==.4f&&body.Scalars["Emissive Max"]==.2f
                &&target.Applies==applies&&target.Body.AnimClass==oldAnimation,
                "disabling color failed to restore authored material without animation restart");
        NCPlusForceModels::Enabled=true;target.DedicatedServer=true;target.UpdateTrainerTint();
        Require(body.Vectors["TeamColor"].G==.3f,"dedicated server applied local render preferences");
    } else if (name == "rifle_lifecycle") {
        Require(target.WeaponAttachmentClass == &rifleTemplate,
                "target did not select the stock third-person shock rifle attachment");
        Require(!target.WeaponAttachment && target.AttachmentSpawns == 0,
                "cosmetic rifle spawned before actor BeginPlay");
        target.BeginPlay();
        AUTWeaponAttachment* rifle = target.WeaponAttachment;
        Require(rifle && target.AttachmentSpawns == 1 && target.AttachmentSpawnedAfterBegin,
                "standalone target did not initialize its rifle after Super::BeginPlay");
        Require(target.Hidden && !target.Collision && rifle->Hidden && !rifle->Collision,
                "initially hidden target left a visible or colliding rifle");
        Require(!rifle->Mesh->bCastHiddenShadow,
                "hidden training rifle can leave a detached shadow");
        target.bTrainerVisible = true;
        target.OnRep_TrainerVisible();
        Require(!target.Hidden && target.Collision && !rifle->Hidden && !rifle->Collision,
                "target appearance did not reveal its noncolliding rifle");
        target.bTrainerVisible = false;
        target.OnRep_TrainerVisible();
        Require(target.Hidden && !target.Collision && rifle->Hidden,
                "recycled target left its separate rifle actor visible");
        target.bTrainerVisible = true;
        target.OnRep_TrainerVisible();
        target.UpdateWeaponAttachment();
        Require(target.WeaponAttachment == rifle && target.AttachmentSpawns == 1 && !rifle->Hidden,
                "target reuse duplicated or failed to reveal its rifle");
    } else if (name == "rifle_late_attachment") {
        target.DeferAttachmentCreation = true;
        target.BeginPlay();
        Require(!target.WeaponAttachment && target.Hidden,
                "fixture must reproduce visibility arriving before the attachment");
        target.DeferAttachmentCreation = false;
        target.UpdateWeaponAttachment();
        Require(target.WeaponAttachment && target.WeaponAttachment->Hidden
                && !target.WeaponAttachment->Collision,
                "late attachment ignored the target's already-replicated hidden state");
        target.bTrainerVisible = true;
        target.OnRep_TrainerVisible();
        target.WeaponAttachment = nullptr;
        target.LocalAttachment.Hidden = true;
        target.LocalAttachment.Collision = true;
        target.UpdateWeaponAttachment();
        Require(target.WeaponAttachment && !target.WeaponAttachment->Hidden
                && !target.WeaponAttachment->Collision && target.AttachmentSpawns == 2,
                "replacement attachment ignored the target's visible state");
    } else if (name == "missing_rifle") {
        target.BeginPlay();
        Require(target.HasCharacterAssets() && !target.WeaponAttachmentClass && !target.WeaponAttachment,
                "optional cosmetic rifle failure prevented valid training targets");
        target.bTrainerVisible = true;
        target.OnRep_TrainerVisible();
        Require(!target.Hidden && target.Collision,
                "missing optional rifle prevented the target from appearing");
    } else if (name == "rifle_dedicated") {
        target.DedicatedServer = true;
        target.BeginPlay();
        target.bTrainerVisible = true;
        target.OnRep_TrainerVisible();
        Require(target.HasCharacterAssets() && target.AttachmentUpdates == 1 && !target.WeaponAttachment
                && target.AttachmentSpawns == 0 && !target.Hidden && target.Collision,
                "dedicated target required a local cosmetic attachment");
    } else if (name == "scale_stable") {
        target.PostInitializeComponents();
        Require(target.Body.Relative.Scale == .8f && target.Body.Relative.Z == -110.f,
                "reapplication compounded skin scale or lost authored offset");
        Require(target.Body.AnimClass == &ut3Animation, "reapplication lost pawn animation");
    } else if (name == "dedicated_pose") {
        Require(target.Body.MeshComponentUpdateFlag == EMeshComponentUpdateFlag::AlwaysTickPoseAndRefreshBones,
                "unrendered head bones stop updating on the dedicated server");
        Require(!target.Body.bEnableUpdateRateOptimizations && target.Move.bRunPhysicsWithNoController,
                "target cannot animate/move independently of a bot controller");
        Require(target.Move.Nav.bCanCrouch && target.Move.MaxWalkSpeedCrouched == 315.f,
                "controllerless target lacks authored crouch capability or speed");
    } else if (name == "missing_template" || name == "missing_mesh" || name == "missing_ut3") {
        Require(!target.HasCharacterAssets(), "unusable target incorrectly accepted");
    } else if (name == "instagib_defaults") {
        ANCAimTrainerInstagibCharacter trainee(initializer);
        ANCAimTrainerInstagibTarget igTarget(initializer);
        Require(trainee.MovementType == UNCAimTrainerMovement::Type && trainee.Hands.AnimClass == &handsAnimation,
                "instagib subclass lost native movement or playable hand animation");
        Require(trainee.Shape.Radius == 38.f && trainee.Shape.HalfHeight == 103.f
                && igTarget.Shape.Radius == 38.f && igTarget.Shape.HalfHeight == 103.f,
                "instagib pawn and target class defaults disagree on capsule");
        Require(trainee.Body.Relative.Scale == .95f && igTarget.Body.Relative.Scale == .95f
                && trainee.Body.Relative.Z == -110.f && igTarget.Body.Relative.Z == -110.f,
                "instagib constructors did not apply their own mesh defaults");
        igTarget.ClassDefaultMeshScale = igTarget.Body.Relative.Scale;
        igTarget.CharacterData = &skin;
        igTarget.PostInitializeComponents(); igTarget.PostInitializeComponents();
        Require(igTarget.Body.Relative.Scale == .95f*.8f && igTarget.Body.AnimClass == &ut3Animation,
                "instagib skin reapplication lost class-default scale or animation");
        Require(trainee.Move.MaxWalkSpeed == 940.f && igTarget.Move.MaxWalkSpeed == 940.f
                && trainee.Move.DodgeAirControl == .6f && igTarget.Move.DodgeAirControl == .6f,
                "instagib pawn and target did not receive the same movement profile");
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
        trainee = (PLUGIN / "Source/Private/NCAimTrainerCharacter.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "ANCAimTrainerTarget::ANCAimTrainerTarget",
            "ANCAimTrainerInstagibTarget::ANCAimTrainerInstagibTarget",
            "ANCAimTrainerSACTFTarget::ANCAimTrainerSACTFTarget",
            "void ANCAimTrainerTarget::PostInitializeComponents",
            "void ANCAimTrainerTarget::ApplyCharacterData",
            "void ANCAimTrainerTarget::UpdateTrainerTint",
            "void ANCAimTrainerTarget::BeginPlay",
            "void ANCAimTrainerTarget::UpdateWeaponAttachment",
            "void ANCAimTrainerTarget::OnRep_TrainerVisible",
            "bool ANCAimTrainerTarget::HasCharacterAssets",
        )
        source = directory / "trainer_assets.cpp"
        layout = (PLUGIN / "Source/Private/NCAimTrainerLayout.h").as_posix()
        profile = (PLUGIN / "Source/Private/NCAimTrainerCharacterProfile.h").as_posix()
        source.write_text("\n".join([ADAPTER, f'#include "{layout}"', f'#include "{profile}"']
                                   + [native_function(native, s) for s in signatures]
                                   + [native_function(trainee, "ANCAimTrainerCharacter::ANCAimTrainerCharacter"),
                                      native_function(trainee, "ANCAimTrainerInstagibCharacter::ANCAimTrainerInstagibCharacter"), CASES]), encoding="utf-8")
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

    def test_target_mesh_stays_locked_and_enemy_tint_restores_without_pose_restart(self): self.run_case("locked_model_tint")
    def test_explicit_ut3_animation_survives_skin_and_preserves_trainee_arms(self): self.run_case("explicit_ut3")
    def test_valid_skin_without_anim_class_is_ready_before_begin_play(self): self.run_case("spawn_ready")
    def test_skin_scale_does_not_compound(self): self.run_case("scale_stable")
    def test_stock_rifle_spawns_locally_and_follows_target_reuse(self): self.run_case("rifle_lifecycle")
    def test_late_or_replaced_rifle_uses_current_target_visibility(self): self.run_case("rifle_late_attachment")
    def test_missing_optional_rifle_does_not_disable_valid_target(self): self.run_case("missing_rifle")
    def test_dedicated_target_does_not_require_cosmetic_rifle_actor(self): self.run_case("rifle_dedicated")
    def test_dedicated_server_pose_refresh_is_preserved(self): self.run_case("dedicated_pose")
    def test_missing_pawn_template_fails_closed(self): self.run_case("missing_template")
    def test_missing_ut3_animation_fails_instead_of_using_stock_pose(self): self.run_case("missing_ut3")
    def test_missing_skin_mesh_fails_closed(self): self.run_case("missing_mesh")
    def test_trainee_inherits_body_and_first_person_defaults_with_native_movement(self): self.run_case("trainee_defaults")
    def test_trainee_missing_template_does_not_crash(self): self.run_case("trainee_missing_template")
    def test_trainee_missing_first_person_template_does_not_crash(self): self.run_case("trainee_missing_arms")
    def test_instagib_native_class_defaults_survive_skin_reapplication(self): self.run_case("instagib_defaults")


if __name__ == "__main__":
    unittest.main()
