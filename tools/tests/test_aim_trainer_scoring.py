"""Native tests of the production score functions and authoritative hit gate.

Uses real C++ implementations with a minimal actor adapter. Cooked character
animation, the full firing/replication path and rendered room need a playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function

ADAPTER = r'''
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using int32 = int;
constexpr int INDEX_NONE = -1;
struct FMath { static bool IsFinite(float n) { return std::isfinite(n); } };
struct AActor { virtual ~AActor() = default; };
struct AController : AActor {};
struct ANCAimTrainerPlayerController : AController {
    int Confirmations = 0;
    void NotifyTrainerHit(float) { ++Confirmations; }
};
struct AUTWeapon : AActor {};
struct AUTWeaponFix : AUTWeapon { float Rewind = 0; float GetHitValidationPredictionTime() const { return Rewind; } };
struct AUTPlusSniper : AUTWeaponFix { int HeadshotDamageType = 5; };
template<class T, class U> T* Cast(U* p) { return dynamic_cast<T*>(p); }
struct FDamageEvent { int DamageTypeClass = 5; };
struct ANCAimTrainerTarget : AActor {
    bool Visible = true;
    int Hidden = 0;
    float AppearanceTime = 0;
    bool IsAvailable() const { return Visible; }
    float GetAppearanceTime() const { return AppearanceTime; }
    void HideTarget() { Visible = false; ++Hidden; }
};
struct TargetsAdapter : std::vector<ANCAimTrainerTarget*> {
    int IndexOfByKey(ANCAimTrainerTarget* Target) const {
        for (unsigned i = 0; i < size(); ++i) if ((*this)[i] == Target) return int(i);
        return INDEX_NONE;
    }
};
struct ANCAimTrainerGame {
    struct { int Phase = 2, Scenario = 1, Hits = 0, Headshots = 0; } Progress;
    struct World { float Now = 1.f; float GetTimeSeconds() { return Now; } } TheWorld;
    struct { float FRandRange(float a, float b) { return (a + b) * .5f; } } Schedule;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TargetsAdapter Targets;
    float PhaseStartedAt = 0.f;
    float TargetExpiry[3] = { 4.f, 4.f, 4.f };
    float NextTargetTime[3] = {};
    bool ValidTrainee = true;
    bool IsTrainee(AController* PC) { return PC && ValidTrainee; }
    World* GetWorld() { return &TheWorld; }
    float RecordTargetHit(ANCAimTrainerTarget*, float, const FDamageEvent&, AController*, AActor*);
};
void Require(bool okay, const char* why) { if (!okay) { std::cerr << why; std::exit(1); } }
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    AUTPlusSniper Gun;
    ANCAimTrainerTarget Target;
    FDamageEvent Event;
    Fixture() { Game.Trainee = &Player; Game.RunWeapon = &Gun; Game.Targets.push_back(&Target); }
    float Hit() { return Game.RecordTargetHit(&Target, 100.f, Event, &Player, &Gun); }
};
'''

CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case missing");
    const std::string name(argv[1]);
    using namespace NCAimTrainerScoring;
    if (name == "precision") {
        Require(PrecisionScore(6, 10, 2) == 450, "hit/miss/expiry score drift");
        Require(PrecisionScore(0, 8, 3) == 0, "negative score must clamp");
        Require(PrecisionScore(200, 200, 0) == 20000, "valid maximum lost");
        Require(PrecisionScore(2, 1, 0) == 0, "hits exceed shots");
        Require(PrecisionScore(2147483647, 2147483647, 0) == 0, "overflow input accepted");
        Require(PrecisionScore(1, 1, -1) == 0, "negative expiry accepted");
    } else if (name == "tracking") {
        Require(TrackingCredit(.04, true, true) == .04, "sustained contact lost");
        Require(TrackingCredit(.04, false, true) == 0, "approach interval overcredited");
        Require(TrackingCredit(.04, true, false) == 0, "exit interval overcredited");
        Require(TrackingCredit(1.0, true, true) == 0, "server stall became tracking time");
        Require(TrackingCredit(-.1, true, true) == 0, "negative time accepted");
        Require(TrackingCredit(std::numeric_limits<double>::quiet_NaN(), true, true) == 0, "NaN accepted");
        Require(TrackingMilliseconds(1.251) == 1251, "duration conversion drift");
        Require(TrackingMilliseconds(600) == 60000, "tracking cap lost");
        Require(TrackingMilliseconds(-1) == 0, "negative tracking score");
    } else if (name == "one_hit") {
        Fixture f;
        Require(f.Hit() == 100.f && f.Game.Progress.Hits == 1 && f.Game.Progress.Headshots == 1, "head not awarded");
        Require(!f.Target.Visible && f.Target.Hidden == 1, "scored target still shootable");
        Require(f.Hit() == 0.f && f.Game.Progress.Hits == 1, "duplicate appearance awarded");
        Require(f.Player.Confirmations == 1, "confirmation must occur once per accepted appearance");
        Require(f.Game.NextTargetTime[0] > f.Game.TheWorld.Now, "no hide interval");
    } else if (name == "body") {
        Fixture f; f.Event.DamageTypeClass = 1;
        Require(f.Hit() == 0 && f.Game.Progress.Hits == 0 && f.Target.Visible, "body hit counted as headshot");
        Require(f.Player.Confirmations == 0, "rejected body hit played a success sound");
        f.Event.DamageTypeClass = f.Gun.HeadshotDamageType;
        Require(f.Hit() > 0 && f.Game.Progress.Headshots == 1, "legitimate head rejected");
    } else if (name == "identity") {
        Fixture f; AController OtherPlayer; AUTWeapon OtherGun; ANCAimTrainerTarget OtherTarget;
        Require(f.Game.RecordTargetHit(&f.Target, 100, f.Event, &OtherPlayer, &f.Gun) == 0, "other controller scored");
        Require(f.Game.RecordTargetHit(&f.Target, 100, f.Event, &f.Player, &OtherGun) == 0, "other weapon scored");
        Require(f.Game.RecordTargetHit(&OtherTarget, 100, f.Event, &f.Player, &f.Gun) == 0, "outside target scored");
        f.Game.ValidTrainee = false;
        Require(f.Hit() == 0, "invalid trainee scored");
        Require(f.Player.Confirmations == 0, "unauthorized damage played a success sound");
    } else if (name == "deadline") {
        Fixture expired; expired.Game.TheWorld.Now = 4.f;
        Require(expired.Hit() == 0, "expired target scored before tick retired it");
        Fixture ended; ended.Game.TheWorld.Now = 60.f; ended.Game.TargetExpiry[0] = 65.f;
        Require(ended.Hit() == 0, "shot after run deadline scored");
        Fixture inactive; inactive.Game.Progress.Phase = 3;
        Require(inactive.Hit() == 0, "results-phase shot scored");
        Fixture earlierAppearance; earlierAppearance.Target.AppearanceTime = .5f; earlierAppearance.Gun.Rewind = .7f;
        Require(earlierAppearance.Hit() == 0, "late shot from previous appearance scored reused actor");
        earlierAppearance.Gun.Rewind = .4f;
        Require(earlierAppearance.Hit() > 0, "current appearance headshot lost");
    } else if (name == "instagib") {
        Fixture f; f.Game.Progress.Scenario = 2; f.Event.DamageTypeClass = 1;
        Require(f.Hit() > 0 && f.Game.Progress.Hits == 1 && f.Game.Progress.Headshots == 0, "IG body not scored");
        Fixture track; track.Game.Progress.Scenario = 0;
        Require(track.Hit() == 0 && track.Game.Progress.Hits == 0, "weapon damage inflated tracking");
    } else Require(false, "unknown case");
}
'''


class AimTrainerScoringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        game = (PLUGIN / "Source/Private/NCAimTrainerGame.cpp").read_text(encoding="utf-8-sig")
        scoring = (PLUGIN / "Source/Private/NCAimTrainerScoring.h").read_text(encoding="utf-8-sig").replace("#pragma once", "")
        source = directory / "trainer.cpp"
        source.write_text("\n".join((ADAPTER, scoring, native_function(game, "float ANCAimTrainerGame::RecordTargetHit"), CASES)), encoding="utf-8")
        cls.executable = directory / ("trainer.exe" if os.name == "nt" else "trainer")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Trainer adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_precision_formula_and_untrusted_bounds(self): self.run_case("precision")
    def test_tracking_requires_continuity_and_rejects_long_stalls(self): self.run_case("tracking")
    def test_each_appearance_scores_once(self): self.run_case("one_hit")
    def test_headshots_use_actual_sniper_damage_type(self): self.run_case("body")
    def test_authorized_trainee_weapon_and_target_only(self): self.run_case("identity")
    def test_expiry_and_finish_deadlines_apply_before_tick(self): self.run_case("deadline")
    def test_instigib_accepts_body_while_tracking_ignores_damage(self): self.run_case("instagib")


if __name__ == "__main__":
    unittest.main()
