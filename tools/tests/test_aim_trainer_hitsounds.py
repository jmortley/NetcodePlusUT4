"""Verify trainer confirmation delivery uses NCP playback after authority approval.

The real controller methods are compiled. Audio/config and network delivery are
small adapters; this does not claim an audible packaged-client playtest.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#define UE_SERVER 0
using int32 = int;
constexpr int ROLE_Authority = 3;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static float Clamp(float value, float low, float high) { return std::max(low, std::min(high, value)); }
    static int RoundToInt(float value) { return int(std::round(value)); }
};
struct AClientHitsounds;
struct World { std::vector<AClientHitsounds*> Mutators; };
struct FHitsoundsConfig { int SelectedPreset = 0; float Volume = 1.f; };
struct AClientHitsounds {
    bool Suppress = false;
    int Plays = 0, Checks = 0, LastDamage = 0;
    bool LastFriendly = true;
    static int PreviewPlays, ConfigReads, LastPreset, PreviewDamage;
    static float PreviewVolume;
    static FHitsoundsConfig Ini;
    bool ShouldSuppressServerHitsound(int damage, bool friendly) {
        ++Checks; LastDamage = damage; LastFriendly = friendly; return Suppress;
    }
    void PlayHitsound(int damage, bool friendly) {
        ++Plays; LastDamage = damage; LastFriendly = friendly;
    }
    static FHitsoundsConfig LoadConfigFromIni() { ++ConfigReads; return Ini; }
    static void PlayPreview(void*, const FHitsoundsConfig& config, bool friendly, int damage) {
        if (friendly) std::exit(20);
        ++PreviewPlays; LastPreset = config.SelectedPreset; PreviewVolume = config.Volume; PreviewDamage = damage;
    }
};
int AClientHitsounds::PreviewPlays = 0, AClientHitsounds::ConfigReads = 0;
int AClientHitsounds::LastPreset = 0, AClientHitsounds::PreviewDamage = 0;
float AClientHitsounds::PreviewVolume = 0;
FHitsoundsConfig AClientHitsounds::Ini;
template<class T> struct TActorIterator {
    World* Value;
    std::size_t Index = 0;
    explicit TActorIterator(World* world) : Value(world) {}
    explicit operator bool() const { return Index < Value->Mutators.size(); }
    T* operator->() const { return Value->Mutators[Index]; }
    TActorIterator& operator++() { ++Index; return *this; }
};
struct ANCAimTrainerPlayerController {
    int Role = ROLE_Authority;
    bool Local = true, HasWorld = true;
    int Sent = 0, LastSent = 0;
    World TheWorld;
    bool IsLocalController() const { return Local; }
    World* GetWorld() { return HasWorld ? &TheWorld : nullptr; }
    void ClientTrainerConfirmedHit(int damage) {
        ++Sent; LastSent = damage; ClientTrainerConfirmedHit_Implementation(damage);
    }
    void NotifyTrainerHit(float);
    void ClientTrainerConfirmedHit_Implementation(int);
};
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
'''

CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    ANCAimTrainerPlayerController pc;
    if (name == "authority") {
        pc.Role = 1;
        pc.NotifyTrainerHit(125.f);
        Require(pc.Sent == 0, "client generated an authoritative confirmation");
        pc.Role = ROLE_Authority;
        for (float damage : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            pc.NotifyTrainerHit(damage);
        Require(pc.Sent == 0, "invalid damage sent a confirmation");
        pc.NotifyTrainerHit(125.f);
        Require(pc.Sent == 1 && pc.LastSent == 125 && AClientHitsounds::PreviewDamage == 125,
                "accepted sniper damage did not reach configured playback");
        pc.NotifyTrainerHit(std::numeric_limits<float>::max());
        Require(pc.LastSent == 10000, "large damage overflowed before integer conversion");
    } else if (name == "preferences") {
        AClientHitsounds::Ini.SelectedPreset = 3;
        AClientHitsounds::Ini.Volume = .25f;
        pc.ClientTrainerConfirmedHit_Implementation(125);
        Require(AClientHitsounds::PreviewPlays == 1 && AClientHitsounds::LastPreset == 3
                && AClientHitsounds::PreviewVolume == .25f, "NCP preset/volume ignored");
        AClientHitsounds::Ini.SelectedPreset = 8;
        AClientHitsounds::Ini.Volume = 0.f;
        pc.ClientTrainerConfirmedHit_Implementation(1000);
        Require(AClientHitsounds::ConfigReads == 2 && AClientHitsounds::LastPreset == 8
                && AClientHitsounds::PreviewVolume == 0.f, "current preset or mute preference ignored");
    } else if (name == "dedup") {
        AClientHitsounds mutator;
        pc.TheWorld.Mutators.push_back(&mutator);
        pc.ClientTrainerConfirmedHit_Implementation(125);
        Require(mutator.Plays == 1 && !mutator.LastFriendly && AClientHitsounds::PreviewPlays == 0,
                "live NCP hitsound path bypassed or duplicated");
        mutator.Suppress = true;
        pc.ClientTrainerConfirmedHit_Implementation(125);
        Require(mutator.Checks == 2 && mutator.Plays == 1 && AClientHitsounds::PreviewPlays == 0,
                "predicted hit played a duplicate authoritative sound");
    } else if (name == "receiver") {
        pc.Local = false;
        pc.ClientTrainerConfirmedHit_Implementation(125);
        pc.Local = true; pc.HasWorld = false;
        pc.ClientTrainerConfirmedHit_Implementation(125);
        pc.HasWorld = true;
        for (int damage : {0, -1, 10001}) pc.ClientTrainerConfirmedHit_Implementation(damage);
        Require(AClientHitsounds::PreviewPlays == 0 && AClientHitsounds::ConfigReads == 0,
                "invalid receiver or payload reached audio/config work");
    } else Require(false, "unknown case");
}
'''


class AimTrainerHitsoundTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-hitsounds-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerPlayerController.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "void ANCAimTrainerPlayerController::NotifyTrainerHit",
            "void ANCAimTrainerPlayerController::ClientTrainerConfirmedHit_Implementation",
        )
        source = directory / "trainer_hitsounds.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_hitsounds.exe" if os.name == "nt" else "trainer_hitsounds")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_hitsounds.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Hitsound adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_valid_authority_confirmation_reaches_playback(self): self.run_case("authority")
    def test_selected_preset_volume_and_mute_are_preserved(self): self.run_case("preferences")
    def test_existing_ncp_mutator_prediction_deduplicates_confirmation(self): self.run_case("dedup")
    def test_nonlocal_missing_world_and_invalid_payload_do_not_play(self): self.run_case("receiver")


if __name__ == "__main__":
    unittest.main()
