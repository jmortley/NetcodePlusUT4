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
#define TEXT(value) value
using int32 = int;
constexpr int INDEX_NONE = -1;
constexpr int NAME_None = 0;
struct FMath {
    static bool IsFinite(float n) { return std::isfinite(n); }
    static int RoundToInt(float n) { return int(std::lround(n)); }
};
struct AActor { virtual ~AActor() = default; };
struct AUTPlayerState : AActor {
    float StoredShots = 0.f;
    float LightningShots = 0.f;
    float GetStatsValue(int name) const { return name == 2 ? LightningShots : StoredShots; }
};
struct AController : AActor { AActor* PlayerState = nullptr; };
struct ANCAimTrainerPlayerController : AController {
    int Confirmations = 0;
    std::vector<float> ConfirmedDamage;
    void NotifyTrainerHit(float damage) { ++Confirmations; ConfirmedDamage.push_back(damage); }
};
struct AUTWeapon : AActor {
    int ShotsStatsName = 1;
    float GetWeaponShotsStats(AUTPlayerState*) const;
};
struct AUTWeaponFix : AUTWeapon { float Rewind = 0; float GetHitValidationPredictionTime() const { return Rewind; } };
struct AUTPlusSniper : AUTWeaponFix { int HeadshotDamageType = 5; };
struct AUTWeap_LinkGun_NCP : AUTWeaponFix {
    bool Firing = true, Pulsing = false;
    int Mode = 1;
    AActor* CurrentLinkedTarget = nullptr;
    struct BeamInfo { int DamageType = 7; };
    struct : std::vector<BeamInfo> {
        bool IsValidIndex(int index) const { return index >= 0 && index < int(size()); }
    } InstantHitInfo;
    AUTWeap_LinkGun_NCP() { InstantHitInfo.resize(2); }
    bool IsFiring() const { return Firing; }
    int GetCurrentFireMode() const { return Mode; }
    bool IsLinkPulsing() const { return Pulsing; }
};
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
    int Num() const { return int(size()); }
    bool IsValidIndex(int index) const { return index >= 0 && index < Num(); }
    int IndexOfByKey(ANCAimTrainerTarget* Target) const {
        for (unsigned i = 0; i < size(); ++i) if ((*this)[i] == Target) return int(i);
        return INDEX_NONE;
    }
};
struct ANCAimTrainerGame {
    struct {
        int Phase = 2, Scenario = 1, Hits = 0, Headshots = 0, Score = 0, Shots = 0, TargetsExpired = 0;
        float TrackingSeconds = 0.f, FiringSeconds = 0.f, Accuracy = 0.f;
    } Progress;
    struct World { float Now = 1.f; float GetTimeSeconds() { return Now; } } TheWorld;
    struct { float FRandRange(float a, float b) { return (a + b) * .5f; } } Schedule;
    ANCAimTrainerPlayerController* Trainee = nullptr;
    AUTWeapon* RunWeapon = nullptr;
    TargetsAdapter Targets;
    float PhaseStartedAt = 0.f;
    float LastTraceTime = 0.f;
    double TrackedSeconds = 0.0, FiredSeconds = 0.0;
    bool bPreviousContact = false, bPreviousFiring = false;
    float TargetExpiry[5] = { 4.f, 4.f, 4.f, 4.f, 4.f };
    float NextTargetTime[5] = {};
    float ShotStatBaseline = 0.f;
    bool bRankedRun = true;
    std::string UnrankedReason;
    bool ValidTrainee = true;
    bool IsTrainee(AController* PC) { return PC && ValidTrainee; }
    World* GetWorld() { return &TheWorld; }
    bool HasTrackingContact() const;
    bool IsTrackingBeamFiring() const;
    void UpdateTrackingSample(float);
    void UpdateShotCount();
    float RecordTargetHit(ANCAimTrainerTarget*, float, const FDamageEvent&, AController*, AActor*);
};
void Require(bool okay, const char* why) { if (!okay) { std::cerr << why; std::exit(1); } }
struct Fixture {
    ANCAimTrainerGame Game;
    ANCAimTrainerPlayerController Player;
    AUTPlayerState PlayerState;
    AUTPlusSniper Gun;
    ANCAimTrainerTarget Target;
    FDamageEvent Event;
    Fixture() {
        Player.PlayerState = &PlayerState;
        Game.Trainee = &Player; Game.RunWeapon = &Gun; Game.Targets.push_back(&Target);
    }
    float Hit() { return Game.RecordTargetHit(&Target, 100.f, Event, &Player, &Gun); }
};
'''

CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case missing");
    const std::string name(argv[1]);
    using namespace NCAimTrainerScoring;
    if (name == "precision") {
        Require(HeadshotScore(6, 10) == 600, "headshot points still subtract misses");
        Require(HeadshotScore(0, 8) == 0 && HeadshotScore(200, 200) == 20000,
                "headshot score zero or maximum drifted");
        Require(HeadshotScore(2, 1) == 0 && HeadshotScore(-1, 1) == 0
                && HeadshotScore(1, -1) == 0 && HeadshotScore(201, 201) == 0
                && HeadshotScore(2147483647, 2147483647) == 0,
                "headshot score accepted impossible counts or overflow");
        Require(PrecisionScore(6, 10, 2) == 450, "hit/miss/expiry score drift");
        Require(PrecisionScore(0, 8, 3) == 0, "negative score must clamp");
        Require(PrecisionScore(200, 200, 0) == 20000, "valid maximum lost");
        Require(PrecisionScore(2, 1, 0) == 0, "hits exceed shots");
        Require(PrecisionScore(2147483647, 2147483647, 0) == 0, "overflow input accepted");
        Require(PrecisionScore(1, 1, -1) == 0, "negative expiry accepted");
    } else if (name == "shot_baseline") {
        Fixture f;
        f.PlayerState.StoredShots=123.f;
        f.Game.ShotStatBaseline=f.Gun.GetWeaponShotsStats(&f.PlayerState);
        f.Game.UpdateShotCount();
        Require(f.Game.Progress.Shots==0 && f.Game.Progress.Score==0 && f.Game.Progress.Accuracy==0.f,
                "new run counted shots from previous sessions");
        f.PlayerState.StoredShots=128.f; f.Game.UpdateShotCount();
        Require(f.Game.Progress.Shots==5 && f.Game.bRankedRun,
                "authoritative stats delta did not reach trainer shot count");
        // A server stats reset mid-run is invalid, not five new phantom shots.
        f.PlayerState.StoredShots=0.f; f.Game.UpdateShotCount();
        Require(!f.Game.bRankedRun && !f.Game.UnrankedReason.empty() && f.Game.Progress.Shots==5,
                "counter reset was accepted as a new valid shot stream");
        // A fresh run may intentionally establish that new zero baseline.
        Fixture next; next.PlayerState.StoredShots=0.f;
        next.Game.ShotStatBaseline=next.Gun.GetWeaponShotsStats(&next.PlayerState);
        next.PlayerState.StoredShots=1.f;
        Require(next.Hit()>0.f,"fresh run headshot not accepted");
        next.Game.UpdateShotCount();
        Require(next.Game.Progress.Shots==1 && next.Game.Progress.Hits==1
                && next.Game.Progress.Score==100 && next.Game.Progress.Accuracy==100.f && next.Game.bRankedRun,
                "fresh baseline retained old misses or rejected valid first headshot");
    } else if (name == "lightning_scoring") {
        Fixture f;
        f.Gun.ShotsStatsName = 2; f.Gun.HeadshotDamageType = 9;
        f.PlayerState.StoredShots = 150.f; f.PlayerState.LightningShots = 20.f;
        f.Game.ShotStatBaseline = f.Gun.GetWeaponShotsStats(&f.PlayerState);
        f.PlayerState.LightningShots = 23.f; f.Game.Progress.TargetsExpired = 10;
        Require(f.Hit() == 0.f && f.Target.Visible,
                "Lightning practice accepted the sniper's damage type");
        f.Event.DamageTypeClass = 9;
        Require(f.Hit() > 0.f && !f.Target.Visible, "Lightning headshot damage type did not score");
        f.Game.UpdateShotCount();
        Require(f.Game.Progress.Shots == 3 && f.Game.Progress.Hits == 1 && f.Game.Progress.Headshots == 1
                && f.Game.Progress.Score == 100 && std::fabs(f.Game.Progress.Accuracy - 100.f/3.f) < .01f,
                "Lightning shot stat or headshot score was mixed with sniper history");
    } else if (name == "headshot_points") {
        Fixture f; f.Game.ShotStatBaseline=100.f; f.PlayerState.StoredShots=105.f;
        f.Game.Progress.TargetsExpired=20; f.Game.UpdateShotCount();
        Require(f.Game.Progress.Shots==5 && f.Game.Progress.Score==0, "initial miss/expiry floor drifted");
        for(int head=1;head<=7;++head) {
            f.Target.Visible=true; ++f.PlayerState.StoredShots;
            Require(f.Hit()>0.f && !f.Target.Visible && f.Player.Confirmations==head,
                    "real accepted headshot failed before score calculation");
            f.Game.UpdateShotCount();
            Require(f.Game.Progress.Headshots==head && f.Game.Progress.Hits==head
                    && f.Game.Progress.Shots==5+head && f.Game.Progress.Accuracy>0.f,
                    "accepted headshot disappeared from counters/accuracy");
            Require(f.Game.Progress.Score==head*100,
                    "accepted headshot was hidden by earlier miss or expiry penalties");
            Require(std::fabs(f.Game.Progress.Accuracy-100.f*head/(5+head))<.001f,
                    "headshot points erased misses from accuracy");
        }
        Require(f.Game.bRankedRun, "legitimate low-scoring headshots were treated as corrupt stats");
        Fixture instagib; instagib.Game.Progress.Scenario=2; instagib.Event.DamageTypeClass=1;
        instagib.Game.ShotStatBaseline=100.f; instagib.PlayerState.StoredShots=105.f;
        instagib.Game.Progress.TargetsExpired=20;
        for(int hit=1;hit<=7;++hit) {
            instagib.Target.Visible=true; ++instagib.PlayerState.StoredShots;
            Require(instagib.Hit()>0.f,"instagib body shot was rejected");
            instagib.Game.UpdateShotCount();
            Require(instagib.Game.Progress.Score==(hit<7?0:75) && instagib.Game.Progress.Headshots==0,
                    "headshot-only score change removed instagib miss/expiry penalties");
        }
    } else if (name == "shot_invalid") {
        Fixture inconsistent; inconsistent.PlayerState.StoredShots=1.f;
        inconsistent.Game.Progress.Hits=2; inconsistent.Game.Progress.Headshots=2;
        inconsistent.Game.UpdateShotCount();
        Require(inconsistent.Game.Progress.Shots==1 && inconsistent.Game.Progress.Score==0,
                "more hits than shots awarded an impossible precision score");
        for(float invalid:{-1.f,201.f,std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
            Fixture f; f.Game.Progress.Shots=3; f.Game.Progress.Score=100; f.Game.Progress.Accuracy=25.f;
            f.PlayerState.StoredShots=invalid; f.Game.UpdateShotCount();
            Require(!f.Game.bRankedRun && !f.Game.UnrankedReason.empty() && f.Game.Progress.Shots==3
                    && f.Game.Progress.Score==100 && f.Game.Progress.Accuracy==25.f,
                    "invalid server counter overwrote last valid display or remained eligible");
        }
        Fixture track; track.Game.Progress.Scenario=0;
        track.Game.Progress.Score=900; track.Game.Progress.Accuracy=60.f;
        track.PlayerState.StoredShots=std::numeric_limits<float>::quiet_NaN();
        track.Game.UpdateShotCount();
        Require(track.Game.bRankedRun && track.Game.Progress.Score==900 && track.Game.Progress.Accuracy==60.f,
                "precision counter validation changed beam tracking results");
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
    } else if (name == "beam_damage") {
        Fixture f; AUTWeap_LinkGun_NCP link;
        f.Game.Progress.Scenario = 0; f.Game.RunWeapon = &link; f.Event.DamageTypeClass = 7;
        auto beamHit = [&]() { return f.Game.RecordTargetHit(&f.Target, 3.f, f.Event, &f.Player, &link); };
        Require(beamHit() == 3.f && f.Player.Confirmations == 1, "real beam hit not accepted or confirmed");
        Require(f.Target.Visible && f.Target.Hidden == 0 && f.Game.Progress.Hits == 0
                && f.Game.Progress.Headshots == 0 && f.Game.NextTargetTime[0] == 0.f,
                "beam damage retired target or changed precision counts");
        // Damage arrives in the weapon's own batches. The trainer must forward
        // every accepted batch, including two in one frame and gaps < 120ms.
        int accepted = 1;
        for (float offset : {0.f, .001f, .01f, .03f, .06f, .119f}) {
            f.Game.TheWorld.Now = 1.f + offset;
            Require(beamHit() == 3.f && f.Player.Confirmations == ++accepted,
                    "accepted beam batch lost its consecutive hit confirmation");
            Require(f.Player.ConfirmedDamage.back() == 3.f, "beam confirmation changed accepted damage");
        }
        for (float invalid : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity()}) {
            Require(f.Game.RecordTargetHit(&f.Target, invalid, f.Event, &f.Player, &link) == 0.f,
                    "invalid beam damage batch accepted");
        }
        link.Firing = false; Require(beamHit() == 0.f, "idle Link accepted beam damage");
        link.Firing = true; link.Mode = 0; Require(beamHit() == 0.f, "wrong fire mode accepted beam damage");
        link.Mode = 1; f.Event.DamageTypeClass = 5; Require(beamHit() == 0.f, "non-beam damage type accepted");
        f.Event.DamageTypeClass = 7;
        ANCAimTrainerTarget other; f.Game.Targets.push_back(&other);
        Require(f.Game.RecordTargetHit(&other, 3.f, f.Event, &f.Player, &link) == 0.f,
                "tracking accepted a precision target slot");
        f.Game.TheWorld.Now = 60.f; f.Game.TargetExpiry[0] = 65.f;
        Require(beamHit() == 0.f, "beam damage accepted after run end");
        Require(f.Player.Confirmations == accepted, "rejected beam damage emitted success sound");
    } else if (name == "tracking_accuracy") {
        Require(TrackingAccuracy(0, 0) == 0.f, "no-fire run has nonzero accuracy");
        Require(TrackingAccuracy(1000, 2000) == 50.f, "accuracy denominator is not beam firing time");
        Require(TrackingAccuracy(60000, 60000) == 100.f, "perfect minute failed");
        Require(TrackingAccuracy(1, 0) == 0.f && TrackingAccuracy(2001, 2000) == 0.f,
                "more contact than firing was accepted");
        Require(TrackingAccuracy(-1, 100) == 0.f && TrackingAccuracy(0, -1) == 0.f
                && TrackingAccuracy(60001, 60001) == 0.f, "invalid duration bounds accepted");
    } else if (name == "tracking_sample") {
        Fixture f; AUTWeap_LinkGun_NCP link;
        f.Game.Progress.Scenario = 0; f.Game.RunWeapon = &link; link.CurrentLinkedTarget = &f.Target;
        f.Game.UpdateTrackingSample(.03125f);
        Require(f.Game.FiredSeconds == 0 && f.Game.TrackedSeconds == 0, "press onset received unsampled time");
        f.Game.UpdateTrackingSample(.0625f);
        Require(f.Game.FiredSeconds == .03125 && f.Game.TrackedSeconds == .03125
                && f.Game.Progress.Score == 31 && f.Game.Progress.Accuracy == 100.f,
                "continuous real beam contact did not accrue score/accuracy");
        link.Firing = false;
        f.Game.UpdateTrackingSample(.09375f); f.Game.UpdateTrackingSample(.125f);
        Require(f.Game.FiredSeconds == .03125 && f.Game.TrackedSeconds == .03125
                && f.Game.Progress.Accuracy == 100.f, "idle time changed tracking accuracy");
        link.Firing = true; link.CurrentLinkedTarget = nullptr;
        f.Game.UpdateTrackingSample(.15625f); f.Game.UpdateTrackingSample(.1875f);
        Require(f.Game.FiredSeconds == .0625 && f.Game.TrackedSeconds == .03125
                && f.Game.Progress.Accuracy == 50.f, "off-target firing did not lower accuracy");
        link.CurrentLinkedTarget = &f.Target;
        f.Game.UpdateTrackingSample(.21875f);
        Require(f.Game.TrackedSeconds == .03125, "approach interval credited target contact");
        f.Game.UpdateTrackingSample(.25f);
        Require(f.Game.FiredSeconds == .125 && f.Game.TrackedSeconds == .0625
                && f.Game.Progress.Score == 62 && std::fabs(f.Game.Progress.Accuracy - 49.6f) < .001f,
                "new continuous contact failed to accrue matching fire/contact clocks");
        Require(f.Game.Progress.FiringSeconds == .125f && f.Game.Progress.TrackingSeconds == .0625f,
                "owner snapshot omitted firing or tracking duration");
    } else if (name == "tracking_clock") {
        Fixture f; AUTWeap_LinkGun_NCP link;
        f.Game.Progress.Scenario = 0; f.Game.RunWeapon = &link; link.CurrentLinkedTarget = &f.Target;
        f.Game.UpdateTrackingSample(.03125f); f.Game.UpdateTrackingSample(.0625f);
        f.Game.UpdateTrackingSample(.3125f);
        Require(f.Game.FiredSeconds == .03125 && f.Game.TrackedSeconds == .03125,
                "server stall inflated firing or target-contact duration");
        for(float time : {.3f, .3125f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
            f.Game.UpdateTrackingSample(time);
        Require(f.Game.LastTraceTime == .3125f && f.Game.FiredSeconds == .03125
                && f.Game.TrackedSeconds == .03125, "invalid sample corrupted time/accounting");
        f.Game.Progress.Phase = 3; f.Game.UpdateTrackingSample(.34375f);
        Require(f.Game.FiredSeconds == .03125 && f.Game.TrackedSeconds == .03125,
                "ended run retained live beam accounting");
    } else if (name == "beam_contact") {
        Fixture f; AUTWeap_LinkGun_NCP link; AActor obstruction;
        f.Game.Progress.Scenario = 0; f.Game.RunWeapon = &link;
        Require(!f.Game.HasTrackingContact(), "pointing without actual beam target earned contact");
        link.CurrentLinkedTarget = &f.Target;
        Require(f.Game.HasTrackingContact(), "actual firing beam contact rejected");
        Require(TrackingCredit(.04, true, f.Game.HasTrackingContact()) == .04, "valid beam interval lost");
        link.Firing = false; Require(!f.Game.HasTrackingContact(), "idle gun retained stale target credit");
        link.Firing = true; link.Mode = 0; Require(!f.Game.HasTrackingContact(), "wrong fire mode earned tracking credit");
        link.Mode = 1; link.Pulsing = true; Require(!f.Game.HasTrackingContact(), "link pull earned beam credit");
        link.Pulsing = false; link.CurrentLinkedTarget = &obstruction;
        Require(!f.Game.HasTrackingContact(), "world obstruction or other actor earned target contact");
        link.CurrentLinkedTarget = &f.Target; f.Target.Visible = false;
        Require(!f.Game.HasTrackingContact(), "hidden target earned contact");
        f.Target.Visible = true; f.Game.Progress.Phase = 3;
        Require(!f.Game.HasTrackingContact(), "results phase earned contact");
        f.Game.Progress.Phase = 2; f.Game.Progress.Scenario = 1;
        Require(!f.Game.HasTrackingContact(), "precision scenario earned tracking contact");
        f.Game.Progress.Scenario = 0; f.Game.RunWeapon = &f.Gun;
        Require(!f.Game.HasTrackingContact(), "precision weapon earned tracking contact");
        f.Game.RunWeapon = &link; f.Game.Targets[0] = nullptr;
        Require(!f.Game.HasTrackingContact(), "missing target accepted");
        f.Game.Targets.clear(); Require(!f.Game.HasTrackingContact(), "empty target list accepted");
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
        weapon = (PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTWeapon.cpp").read_text(encoding="utf-8-sig")
        scoring = (PLUGIN / "Source/Private/NCAimTrainerScoring.h").read_text(encoding="utf-8-sig").replace("#pragma once", "")
        source = directory / "trainer.cpp"
        source.write_text("\n".join((ADAPTER, scoring,
                                    native_function(weapon, "float AUTWeapon::GetWeaponShotsStats"),
                                    native_function(game, "float ANCAimTrainerGame::RecordTargetHit"),
                                    native_function(game, "void ANCAimTrainerGame::UpdateShotCount"),
                                    native_function(game, "bool ANCAimTrainerGame::HasTrackingContact"),
                                    native_function(game, "bool ANCAimTrainerGame::IsTrackingBeamFiring"),
                                    native_function(game, "void ANCAimTrainerGame::UpdateTrackingSample"), CASES)), encoding="utf-8")
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
    def test_shot_counter_subtracts_run_baseline_and_rejects_midrun_counter_reset(self): self.run_case("shot_baseline")
    def test_accepted_headshots_award_immediate_points_despite_misses_and_expiry_while_instagib_keeps_penalties(self): self.run_case("headshot_points")
    def test_shot_counter_rejects_impossible_and_nonfinite_stats_without_touching_tracking(self): self.run_case("shot_invalid")
    def test_tracking_requires_continuity_and_rejects_long_stalls(self): self.run_case("tracking")
    def test_each_appearance_scores_once(self): self.run_case("one_hit")
    def test_headshots_use_actual_sniper_damage_type(self): self.run_case("body")
    def test_lightning_uses_its_own_headshot_damage_type_and_shot_counter(self): self.run_case("lightning_scoring")
    def test_authorized_trainee_weapon_and_target_only(self): self.run_case("identity")
    def test_expiry_and_finish_deadlines_apply_before_tick(self): self.run_case("deadline")
    def test_instagib_accepts_body_while_tracking_rejects_nonbeam_damage(self): self.run_case("instagib")
    def test_each_accepted_beam_batch_confirms_without_retiring_target_or_scoring_precision(self): self.run_case("beam_damage")
    def test_tracking_contact_requires_actual_active_unobstructed_beam_target(self): self.run_case("beam_contact")
    def test_tracking_accuracy_uses_firing_duration_and_rejects_impossible_counts(self): self.run_case("tracking_accuracy")
    def test_tracking_sampler_distinguishes_idle_off_target_and_real_beam_contact(self): self.run_case("tracking_sample")
    def test_tracking_sampler_rejects_stalls_invalid_clocks_and_ended_runs(self): self.run_case("tracking_clock")


if __name__ == "__main__":
    unittest.main()
