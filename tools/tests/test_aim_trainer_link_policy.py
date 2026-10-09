"""Compile the real trainer pull policy and its existing native call sites.

The adapter observes local pull effects, RPC admission, readiness, and release.
Server geometry/damage after admission and the rest of the per-frame beam trace
are intentionally outside this test; their exact production policy blocks are
extracted at explicit boundaries rather than emulating the whole engine.
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
#define TEXT(value) value
using uint8 = unsigned char;
using FName = const char*;
struct FVector { float X=0.f, Y=0.f, Z=0.f; };
struct FVector2D { float X,Y; FVector2D(float x,float y):X(x),Y(y){} };
struct AActor { virtual ~AActor()=default; FVector Position; FVector GetActorLocation() const { return Position; } };
struct AController { virtual ~AController()=default; };
struct AUTPlayerController : AController { int Impulses=0; void AddHUDImpulse(FVector2D) { ++Impulses; } };
struct AUTCharacter : AActor {
    AController* Controller=nullptr;
    AActor* PulseTarget=nullptr;
    FVector TargetEyeOffset;
    bool Local=true;
    bool IsLocallyControlled() const { return Local; }
};
struct ATeamArenaCharacter : AUTCharacter {};
struct ANCAimTrainerCharacter : ATeamArenaCharacter {};
struct ANCAimTrainerInstagibCharacter : ANCAimTrainerCharacter {};
template<class T,class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct World { float TimeSeconds=10.f; float GetTimeSeconds() const { return TimeSeconds; } };
struct Effect {
    int Templates=0, Targets=0;
    void SetTemplate(int) { ++Templates; }
    void SetActorParameter(FName,AActor*) { ++Targets; }
};
template<class T> struct Array : std::vector<T> {
    int Num() const { return int(this->size()); }
    bool IsValidIndex(int index) const { return index>=0&&index<Num(); }
};
struct AUTWeapon {
    virtual ~AUTWeapon()=default;
    uint8 CurrentFireMode=1;
    int ActiveTransitions=0;
    uint8 GetCurrentFireMode() const { return CurrentFireMode; }
    void GotoActiveState() { ++ActiveTransitions; }
};
struct AUTWeap_LinkGun_NCP : AUTWeapon {
    AUTCharacter* UTOwner=nullptr;
    World TheWorld;
    int Role=3;
    bool bReadyToPull=false, Pulsing=false;
    AActor* PulseTarget=nullptr;
    AActor* CurrentLinkedTarget=nullptr;
    AActor* LastRpcTarget=nullptr;
    float LinkStartTime=-100.f, LastBeamPulseTime=-100.f, LinkPullKickbackY=4.f, PullWarmupTime=.15f;
    FVector PulseLoc;
    Array<int> FiringState;
    Array<Effect*> MuzzleFlash;
    int PulseSuccessEffect=1, PulseAnim=2, PulseAnimHands=3;
    int RpcCalls=0, ServerPullAdmissions=0, Animations=0;
    AUTCharacter* GetUTOwner() const { return UTOwner; }
    World* GetWorld() { return &TheWorld; }
    bool IsValidLinkTarget(AActor* target) const { return target&&target!=UTOwner; }
    bool IsLinkPulsing() const { return Pulsing; }
    void PlayWeaponAnim(int,int) { ++Animations; }
    void ServerSetPulseTarget(AActor* target) {
        ++RpcCalls; LastRpcTarget=target; ServerSetPulseTarget_Implementation(target);
    }
    bool SupportsLinkPull() const;
    void StartLinkPull();
    void ServerSetPulseTarget_Implementation(AActor*);
};
struct UUTWeaponStateFiringBeam {
    int EndCalls=0;
    void EndFiringSequence(uint8) { ++EndCalls; }
};
struct UUTWeaponStateFiringLinkBeam_NCP : UUTWeaponStateFiringBeam {
    using Super=UUTWeaponStateFiringBeam;
    AUTWeap_LinkGun_NCP* Weapon=nullptr;
    bool bPendingEndFire=false, bPendingStartFire=false;
    int EndAnimations=0;
    uint8 GetFireMode() const { return 1; }
    AUTWeapon* GetOuterAUTWeapon() const { return Weapon; }
    World* GetWorld() { return Weapon->GetWorld(); }
    void PlayEndFireAnims() { ++EndAnimations; }
    void EndFiringSequence(uint8);
    void UpdateReadiness(AActor* OldLinkedTarget);
};
void Require(bool value,const char* why) {
    if(!value) { std::cerr<<why<<'\n'; std::exit(1); }
}
'''


CASES = r'''
int main(int argc,char** argv) {
    Require(argc==2,"case required"); const std::string name=argv[1];
    AUTPlayerController pc;
    AUTCharacter ordinary; ATeamArenaCharacter team;
    ANCAimTrainerCharacter trainer; ANCAimTrainerInstagibCharacter instagib;
    AUTCharacter* owners[]={nullptr,&ordinary,&team,&trainer,&instagib};
    AActor target;
    if(name=="policy") {
        for(int role:{1,3}) for(int i=0;i<5;++i) {
            AUTWeap_LinkGun_NCP gun; gun.Role=role; gun.UTOwner=owners[i];
            Require(gun.SupportsLinkPull()==(i<3),"pull policy changed normal owner or admitted trainer subclass");
        }
    } else if(name=="trainer_local") {
        for(int role:{1,3}) for(AUTCharacter* owner:{static_cast<AUTCharacter*>(&trainer),static_cast<AUTCharacter*>(&instagib)}) {
            owner->Controller=&pc;
            AUTWeap_LinkGun_NCP gun; gun.Role=role; gun.UTOwner=owner;
            gun.bReadyToPull=true; gun.PulseTarget=gun.CurrentLinkedTarget=&target; gun.LinkStartTime=1.f;
            gun.StartLinkPull();
            Require(!gun.bReadyToPull&&!gun.PulseTarget&&!gun.CurrentLinkedTarget&&gun.LinkStartTime==-100.f,
                    "disabled trainer pull retained armed state");
            Require(gun.RpcCalls==0&&gun.ServerPullAdmissions==0&&gun.Animations==0
                    &&gun.LastBeamPulseTime==-100.f&&pc.Impulses==0&&!owner->PulseTarget,
                    "trainer local pull emitted RPC, pulse, animation or HUD kick");
        }
    } else if(name=="normal_local") {
        ordinary.Controller=&pc;
        AUTWeap_LinkGun_NCP gun; gun.UTOwner=&ordinary; gun.CurrentLinkedTarget=&target; gun.bReadyToPull=true;
        Effect effect; gun.FiringState.resize(2); gun.MuzzleFlash.resize(3); gun.MuzzleFlash[2]=&effect;
        gun.StartLinkPull();
        Require(gun.RpcCalls==1&&gun.ServerPullAdmissions==1&&gun.LastRpcTarget==&target
                &&ordinary.PulseTarget==&target&&gun.PulseTarget==&target,
                "ordinary local pull lost its existing request/target path");
        Require(gun.Animations==1&&pc.Impulses==1&&effect.Templates==1&&effect.Targets==1
                &&gun.LastBeamPulseTime==10.f&&!gun.CurrentLinkedTarget&&!gun.bReadyToPull,
                "ordinary pull lost effects or cleanup");
    } else if(name=="server_admission") {
        for(int i=0;i<5;++i) {
            AUTWeap_LinkGun_NCP gun; gun.UTOwner=owners[i];
            if(owners[i]) { owners[i]->Controller=&pc; owners[i]->Local=false; }
            gun.ServerSetPulseTarget_Implementation(&target);
            Require(gun.ServerPullAdmissions==((i==1||i==2)?1:0),
                    "server admitted trainer pull or rejected ordinary remote owner");
        }
        AUTWeap_LinkGun_NCP gun; gun.UTOwner=&ordinary;
        gun.ServerSetPulseTarget_Implementation(nullptr);
        ordinary.Controller=nullptr; gun.ServerSetPulseTarget_Implementation(&target);
        Require(gun.ServerPullAdmissions==0,"server lost target/controller admission guards");
    } else if(name=="readiness_release") {
        for(int role:{1,3}) for(int i=1;i<5;++i) {
            AUTWeap_LinkGun_NCP gun; gun.UTOwner=owners[i]; gun.Role=role; gun.CurrentLinkedTarget=&target;
            UUTWeaponStateFiringLinkBeam_NCP state; state.Weapon=&gun;
            state.UpdateReadiness(nullptr);
            Require(!gun.bReadyToPull&&gun.LinkStartTime==10.f,"new contact did not reset native pull warmup");
            gun.TheWorld.TimeSeconds=10.3f; state.UpdateReadiness(&target);
            Require(gun.bReadyToPull==(i<3),"sustained trainer beam became pull-ready or normal warmup broke");
            state.EndFiringSequence(0);
            Require(!state.bPendingEndFire&&gun.ActiveTransitions==0,"unrelated fire mode ended beam");
            state.EndFiringSequence(1);
            if(i>=3) Require(!state.bPendingEndFire&&state.EndCalls==1&&state.EndAnimations==1&&gun.ActiveTransitions==1,
                           "trainer release waited for a disabled pull instead of ending beam");
            else Require(state.bPendingEndFire&&state.EndCalls==0&&state.EndAnimations==0&&gun.ActiveTransitions==0,
                         "ordinary ready pull lost its deferred release sequence");
        }
    } else Require(false,"unknown case");
}
'''


class AimTrainerLinkPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-link-policy-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        weapon = (PLUGIN / "Source/Private/UTWeap_LinkGun_NCP.cpp").read_text(encoding="utf-8-sig")
        state = (PLUGIN / "Source/Private/UTWeaponStateFiringLinkBeam_NCP.cpp").read_text(encoding="utf-8-sig")
        server = native_function(weapon, "void AUTWeap_LinkGun_NCP::ServerSetPulseTarget_Implementation")
        # Test the production admission guard, instrumenting entry into the
        # separate geometry/damage portion instead of mocking that entire RPC.
        server_prefix, separator, _ = server.partition("\tAActor* ClientPulseTarget = InTarget;")
        if not separator:
            raise AssertionError("Server pull admission boundary changed; re-audit extraction")
        server_admission = server_prefix + "\n++ServerPullAdmissions;\n}\n"
        tick = native_function(state, "void UUTWeaponStateFiringLinkBeam_NCP::Tick")
        start = "\t\tif (OldLinkedTarget != LinkGun->CurrentLinkedTarget)"
        _, separator, readiness = tick.partition(start)
        if not separator:
            raise AssertionError("Link readiness boundary changed; re-audit extraction")
        readiness, separator, _ = readiness.partition("\n\t\t// The owning client traces only its beam endpoint.")
        if not separator:
            raise AssertionError("Link readiness end boundary changed; re-audit extraction")
        readiness = ("void UUTWeaponStateFiringLinkBeam_NCP::UpdateReadiness(AActor* OldLinkedTarget) {\n"
                     "AUTWeap_LinkGun_NCP* LinkGun=Weapon;\n" + start + readiness + "\n}\n")
        source = directory / "trainer_link_policy.cpp"
        source.write_text("\n".join([
            ADAPTER,
            native_function(weapon, "bool AUTWeap_LinkGun_NCP::SupportsLinkPull"),
            native_function(weapon, "void AUTWeap_LinkGun_NCP::StartLinkPull"),
            server_admission, readiness,
            native_function(state, "void UUTWeaponStateFiringLinkBeam_NCP::EndFiringSequence"),
            CASES,
        ]), encoding="utf-8")
        cls.executable = directory / ("trainer_link_policy.exe" if os.name == "nt" else "trainer_link_policy")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_link_policy.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Trainer Link policy compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_trainer_and_instagib_trainer_owners_disable_pull(self): self.run_case("policy")
    def test_trainer_local_pull_cannot_emit_rpc_or_cosmetics(self): self.run_case("trainer_local")
    def test_ordinary_local_pull_keeps_request_effects_and_cleanup(self): self.run_case("normal_local")
    def test_server_pull_rejects_trainer_owner_and_preserves_existing_guards(self): self.run_case("server_admission")
    def test_trainer_never_becomes_pull_ready_and_release_finishes_normally(self): self.run_case("readiness_release")


if __name__ == "__main__":
    unittest.main()
