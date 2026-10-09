"""Exercise trainer feedback using the real UT hit-info and damage-type helpers.

The adapter supplies world/material/event types and invokes the real replicated
notification. GPU rendering and actual network transport still need a playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, STOCK, find_compiler, native_function


ADAPTER = r'''
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#define TEXT(value) value
using int32 = int;
using uint8 = unsigned char;
enum { NM_Standalone, NM_ListenServer, NM_DedicatedServer, NM_Client };
struct FMath {
    static int RoundToInt(float value) { return int(std::lround(value)); }
    static float Clamp(float value,float low,float high) { return value<low?low:value>high?high:value; }
};
struct FRotator {
    float Pitch=0.f,Yaw=0.f;
    static uint8 CompressAxisToByte(float value) { return uint8(value); }
};
struct FVector {
    float X,Y,Z;
    FVector(float x=0.f,float y=0.f,float z=0.f):X(x),Y(y),Z(z){}
    FVector operator-(const FVector& other) const { return FVector(X-other.X,Y-other.Y,Z-other.Z); }
    bool IsNearlyZero(float tolerance) const { return std::abs(X)<tolerance&&std::abs(Y)<tolerance&&std::abs(Z)<tolerance; }
    FVector GetSafeNormal() const { return *this; }
    FRotator Rotation() const { FRotator value;value.Pitch=Z;value.Yaw=Y;return value; }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
struct FLinearColor {
    float Value;
    explicit FLinearColor(float value=0.f):Value(value){}
    static const FLinearColor Transparent;
};
const FLinearColor FLinearColor::Transparent;
using FName = std::string;
struct UMaterialInstanceDynamic {
    FLinearColor Tint;
    float FullBody=0.f;
    void SetVectorParameterValue(const char*,FLinearColor color) { Tint=color; }
    void SetScalarParameterValue(FName,float value) { FullBody=value; }
};
struct UCurveLinearColor { int Id; explicit UCurveLinearColor(int id):Id(id){} };
struct AUTCharacter;
struct UUTDamageType {
    const UCurveLinearColor* BodyDamageColor=nullptr;
    const UCurveLinearColor* SuperHealthDamageColor=nullptr;
    const UCurveLinearColor* ArmorDamageColor=nullptr;
    bool bBlockedByArmor=true,bBodyDamageColorRimOnly=false;
    void PlayHitEffects_Implementation(AUTCharacter*,bool) const;
};
struct AUTInventory {
    bool ShouldDisplayHitEffect(int,int,int,int) const { return true; }
    const AUTInventory* GetClass() const { return this; }
};
struct AUTTimedPowerup {
    static const AUTInventory* StaticClass() { static AUTInventory value;return &value; }
};
struct FDamageEvent {
    UUTDamageType* DamageTypeClass=nullptr;
    int Type=0;
    virtual ~FDamageEvent()=default;
    bool IsOfType(int id) const { return Type==id; }
};
struct FPointDamageEvent : FDamageEvent {
    static constexpr int ClassID=1;
    struct { FVector Location; } HitInfo;
    FVector ShotDirection;
    FPointDamageEvent() { Type=ClassID; }
};
struct FRadialDamageEvent : FDamageEvent {
    static constexpr int ClassID=2;
    struct ComponentHit { FVector Location,ImpactPoint; };
    struct : std::vector<ComponentHit> { int Num() const { return int(size()); } } ComponentHits;
    FVector Origin;
};
struct AActor { virtual ~AActor()=default; };
struct AController : AActor {};
struct ANCAimTrainerTarget;
struct ANCAimTrainerGame : AActor {
    float Accepted=4.f;
    bool HideOnAccepted=false;
    int HitCalls=0;
    float RecordTargetHit(ANCAimTrainerTarget*,float,const FDamageEvent&,AController*,AActor*);
};
struct World {
    float TimeSeconds=42.f;
    ANCAimTrainerGame* Game=nullptr;
    AActor* GetAuthGameMode() { return Game; }
};
template<class T,class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct AUTCharacter : AActor {
    struct HitInfo {
        int Damage=0,Count=0;
        UUTDamageType* DamageType=nullptr;
        const AUTInventory* HitArmor=nullptr;
        FVector Momentum,RelHitLocation;
        uint8 ShotDirPitch=0,ShotDirYaw=0;
    } LastTakeHitInfo;
    int Health=100,HealthMax=100,ArmorAmount=0,Mode=NM_Standalone,EffectCalls=0;
    bool bTearOff=false;
    float LastTakeHitTime=-1.f,LastTakeHitReplicatedTime=0.f,BodyColorFlashElapsedTime=0.f;
    const UCurveLinearColor* BodyColorFlashCurve=nullptr;
    std::vector<UMaterialInstanceDynamic*> BodyMIs;
    World TheWorld;
    FVector Position=FVector(10.f,20.f,30.f);
    int GetNetMode() const { return Mode; }
    World* GetWorld() { return &TheWorld; }
    FVector GetActorLocation() const { return Position; }
    void SetLastTakeHitInfo(int32,int32,const FVector&,AUTInventory*,const FDamageEvent&);
    void SetBodyColorFlash(const UCurveLinearColor*,bool);
    // The real BlueprintCosmetic dispatch absorbs the event on dedicated servers.
    void PlayTakeHitEffects() { if(Mode!=NM_DedicatedServer) PlayTakeHitEffects_Implementation(); }
    virtual void PlayTakeHitEffects_Implementation() {
        if(Mode!=NM_DedicatedServer) {
            ++EffectCalls;
            if(LastTakeHitInfo.DamageType) LastTakeHitInfo.DamageType->PlayHitEffects_Implementation(this,false);
        }
    }
};
struct ANCAimTrainerTarget : AUTCharacter {
    using Super=AUTCharacter;
    bool bTrainerVisible=true,Hidden=false,Collision=true;
    int NetUpdates=0;
    void SetActorHiddenInGame(bool hidden) { Hidden=hidden; }
    void SetActorEnableCollision(bool enabled) { Collision=enabled; }
    void ForceNetUpdate() { ++NetUpdates; }
    void OnRep_TrainerVisible();
    float TakeDamage(float,const FDamageEvent&,AController*,AActor*);
    void PlayTakeHitEffects_Implementation() override;
};
float ANCAimTrainerGame::RecordTargetHit(ANCAimTrainerTarget* target,float,const FDamageEvent&,AController*,AActor*) {
    ++HitCalls;
    if(Accepted>0.f&&HideOnAccepted) { target->bTrainerVisible=false;target->OnRep_TrainerVisible(); }
    return Accepted;
}
void Require(bool condition,const char* message) {
    if(!condition) { std::cerr<<message<<'\n';std::exit(1); }
}
struct Fixture {
    ANCAimTrainerTarget Target;
    ANCAimTrainerGame Game;
    UUTDamageType DamageType;
    UCurveLinearColor Body{1},Overhealth{2},Armor{3};
    UMaterialInstanceDynamic Material;
    FPointDamageEvent Event;
    Fixture() {
        Target.TheWorld.Game=&Game;
        Target.BodyMIs.push_back(&Material);
        DamageType.BodyDamageColor=&Body;DamageType.SuperHealthDamageColor=&Overhealth;DamageType.ArmorDamageColor=&Armor;
        Event.DamageTypeClass=&DamageType;
        Event.HitInfo.Location=FVector(15.f,26.f,37.f);Event.ShotDirection=FVector(1.f,2.f,3.f);
    }
    float Hit() { return Target.TakeDamage(4.f,Event,nullptr,nullptr); }
};
'''

CASES = r'''
int main(int argc,char** argv) {
    Require(argc==2,"choose a case");
    const std::string name=argv[1];
    Fixture f;
    if(name=="standalone"||name=="listen"||name=="repeat"||name=="damage_type") {
        if(name=="listen") f.Target.Mode=NM_ListenServer;
        if(name=="damage_type") f.DamageType.BodyDamageColor=&f.Armor;
        Require(f.Hit()==4.f&&f.Game.HitCalls==1&&f.Target.EffectCalls==1,"accepted beam hit did not reach native feedback");
        Require(f.Target.BodyColorFlashCurve==f.DamageType.BodyDamageColor,"wrong damage-type body flash or false overhealth effect");
        Require(f.Target.LastTakeHitInfo.HitArmor==nullptr&&f.Target.Health==100&&f.Target.ArmorAmount==0
                &&f.Target.LastTakeHitInfo.Momentum.IsNearlyZero(.01f),"feedback changed target health/armor or applied knockback");
        Require(f.Target.LastTakeHitInfo.RelHitLocation.X==5.f&&f.Target.LastTakeHitInfo.RelHitLocation.Y==6.f
                &&f.Target.LastTakeHitInfo.ShotDirPitch==3&&f.Target.LastTakeHitInfo.ShotDirYaw==2,
                "native hit location or shot direction was lost");
        Require(f.Target.LastTakeHitTime==42.f&&f.Target.NetUpdates==1,"native recent-hit replication was not scheduled");
        if(name=="repeat") {
            const float old=f.Target.LastTakeHitInfo.RelHitLocation.Z;
            Require(f.Hit()==4.f&&f.Target.LastTakeHitInfo.RelHitLocation.Z!=old&&f.Target.EffectCalls==2,
                    "repeated contact at the same location would not replicate or refresh native feedback");
        }
    } else if(name=="dedicated_client") {
        f.Target.Mode=NM_DedicatedServer;
        Require(f.Hit()==4.f&&f.Target.EffectCalls==0&&f.Target.BodyColorFlashCurve==nullptr,
                "dedicated server executed cosmetic effects");
        Require(f.Target.LastTakeHitInfo.Damage==4&&f.Target.LastTakeHitInfo.HitArmor==nullptr,
                "authority replicated a fake overhealth marker or lost hit damage");
        ANCAimTrainerTarget client;client.Mode=NM_Client;
        client.LastTakeHitInfo=f.Target.LastTakeHitInfo;
        client.PlayTakeHitEffects();
        Require(client.EffectCalls==1&&client.BodyColorFlashCurve==&f.Body&&client.Health==100,
                "remote native hit notification did not play the weapon's body flash");
        Require(client.TakeDamage(4.f,f.Event,nullptr,nullptr)==0.f&&client.EffectCalls==1,
                "non-authoritative client fabricated accepted feedback");
    } else if(name=="reject") {
        f.Game.Accepted=0.f;
        Require(f.Hit()==0.f&&f.Game.HitCalls==1&&f.Target.EffectCalls==0&&f.Target.NetUpdates==0,
                "rejected hit flashed the target");
        f.Target.bTrainerVisible=false;f.Game.Accepted=4.f;
        Require(f.Hit()==0.f&&f.Game.HitCalls==1&&f.Target.EffectCalls==0,"hidden target admitted damage");
    } else if(name=="precision") {
        f.Game.HideOnAccepted=true;
        Require(f.Hit()==4.f&&!f.Target.bTrainerVisible&&f.Target.EffectCalls==0&&f.Target.NetUpdates==0,
                "already-scored precision target started a lingering beam effect");
    } else if(name=="hide_reset") {
        f.Hit();f.Material.Tint=FLinearColor(1.f);f.Target.BodyColorFlashElapsedTime=.2f;
        f.Target.bTrainerVisible=false;f.Target.OnRep_TrainerVisible();
        Require(f.Target.Hidden&&!f.Target.Collision&&f.Target.BodyColorFlashCurve==nullptr
                &&f.Target.BodyColorFlashElapsedTime==0.f&&f.Material.Tint.Value==0.f,
                "hidden reused target retained native flash curve or shader tint");
        f.Target.PlayTakeHitEffects();
        Require(f.Target.EffectCalls==1&&f.Target.BodyColorFlashCurve==nullptr,"late hidden hit notification restored the flash");
        f.Target.bTrainerVisible=true;f.Target.OnRep_TrainerVisible();
        Require(!f.Target.Hidden&&f.Target.Collision&&f.Target.BodyColorFlashCurve==nullptr&&f.Material.Tint.Value==0.f,
                "next appearance inherited the previous contact effect");
    } else if(name=="ordinary_native") {
        AUTCharacter ordinary;
        ordinary.SetLastTakeHitInfo(4,4,FVector::ZeroVector,nullptr,f.Event);
        Require(ordinary.BodyColorFlashCurve==&f.Overhealth,
                "adapter did not exercise native post-damage overhealth inference");
        ordinary.Health=96;
        ordinary.SetLastTakeHitInfo(4,4,FVector::ZeroVector,nullptr,f.Event);
        Require(ordinary.BodyColorFlashCurve==&f.Body,"normal reduced-health native body feedback changed");
    } else { Require(false,"unknown case"); }
}
'''


class TrainerFeedbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-feedback-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        trainer = (PLUGIN / "Source/Private/NCAimTrainerTarget.cpp").read_text(encoding="utf-8-sig")
        stock = STOCK.read_text(encoding="utf-8-sig")
        damage = STOCK.with_name("UTDamageType.cpp").read_text(encoding="utf-8-sig")
        functions = [
            native_function(stock, "void AUTCharacter::SetLastTakeHitInfo"),
            native_function(stock, "void AUTCharacter::SetBodyColorFlash"),
            native_function(damage, "void UUTDamageType::PlayHitEffects_Implementation"),
            native_function(trainer, "void ANCAimTrainerTarget::OnRep_TrainerVisible"),
            native_function(trainer, "float ANCAimTrainerTarget::TakeDamage"),
            native_function(trainer, "void ANCAimTrainerTarget::PlayTakeHitEffects_Implementation"),
        ]
        source = directory / "trainer_feedback.cpp"
        source.write_text("\n".join([ADAPTER] + functions + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_feedback.exe" if os.name == "nt" else "trainer_feedback")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_feedback.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Feedback adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_accepted_standalone_contact_uses_native_weapon_body_flash(self): self.run_case("standalone")
    def test_listen_host_contact_plays_immediately(self): self.run_case("listen")
    def test_repeated_contact_changes_native_replicated_hit_info(self): self.run_case("repeat")
    def test_weapon_damage_type_selects_effect(self): self.run_case("damage_type")
    def test_dedicated_hit_info_drives_remote_native_notification(self): self.run_case("dedicated_client")
    def test_rejected_or_hidden_target_does_not_flash(self): self.run_case("reject")
    def test_precision_hit_disappearance_does_not_start_lingering_effect(self): self.run_case("precision")
    def test_hidden_and_reused_target_resets_curve_and_material(self): self.run_case("hide_reset")
    def test_native_post_damage_overhealth_inference_remains_unchanged(self): self.run_case("ordinary_native")


if __name__ == "__main__":
    unittest.main()
