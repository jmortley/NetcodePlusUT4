"""Compile the trainer countdown message's real declaration and native methods.

UT object ownership, the stock message base, and world time are small adapters.
These checks cover admission, interruption, and stale queued speech; they do not
claim an audible packaged-client test or an Unreal reflection build.
"""
import os
from pathlib import Path
import re
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
#define UCLASS(...)
#define GENERATED_BODY() using Super = UUTCountDownMessage;
#define NETCODEPLUS_API
using int32 = int;
struct FObjectInitializer {};
struct FText {
    bool Empty = false;
    static FText GetEmpty() { FText result; result.Empty = true; return result; }
};
struct UObject { virtual ~UObject() {} bool Valid = true; };
template<class T> const T* Cast(const UObject* object) { return dynamic_cast<const T*>(object); }
bool IsValid(const UObject* object) { return object && object->Valid; }
struct APlayerState : UObject {};
struct AUTGameState { bool AllowSpeech = true; };
struct FAnnouncementInfo {
    int32 Switch = 0;
    const UObject* OptionalObject = nullptr;
    float QueueTime = 0.f;
};
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static int32 CeilToInt(float value) { return int32(std::ceil(value)); }
};
struct World {
    float Now = 20.f;
    float GetTimeSeconds() const { return Now; }
};
struct FNCAimTrainerProgress { int Phase = 1; float RemainingSeconds = 2.95f; };
struct ANCAimTrainerPlayerController : UObject {
    bool Local = true, HasWorld = true;
    World TheWorld;
    FNCAimTrainerProgress Progress;
    bool IsLocalController() const { return Local; }
    const World* GetWorld() const { return HasWorld ? &TheWorld : nullptr; }
    const FNCAimTrainerProgress& GetTrainerProgress() const { return Progress; }
};
class UUTCountDownMessage {
public:
    explicit UUTCountDownMessage(const FObjectInitializer&) {}
    virtual ~UUTCountDownMessage() {}
    float MaxAnnouncementDelay = 99.f;
    virtual FText GetText(int32, bool, APlayerState*, APlayerState*, UObject*) const { return FText(); }
    virtual bool IsOptionalSpoken(int32) const { return true; }
    virtual float GetAnnouncementPriority(const FAnnouncementInfo) const { return .5f; }
    virtual bool InterruptAnnouncement(const FAnnouncementInfo, const FAnnouncementInfo) const { return false; }
    virtual bool ShouldStillPlay(AUTGameState* gs, const FAnnouncementInfo) const {
        return !gs || gs->AllowSpeech;
    }
};
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
'''


CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    FObjectInitializer initializer;
    UNCAimTrainerCountdownMessage message(initializer);
    ANCAimTrainerPlayerController pc;
    AUTGameState gs;
    FAnnouncementInfo incoming;
    incoming.Switch = 3;
    incoming.OptionalObject = &pc;
    incoming.QueueTime = pc.TheWorld.Now;
    if (name == "valid") {
        for (int count : {3, 2, 1}) {
            pc.Progress.RemainingSeconds = float(count) - .05f;
            incoming.Switch = count;
            Require(message.ShouldStillPlay(&gs, incoming), "current local countdown was rejected");
        }
        gs.AllowSpeech = false;
        Require(!message.ShouldStillPlay(&gs, incoming), "stock base playback veto was ignored");
    } else if (name == "owner") {
        incoming.OptionalObject = nullptr;
        Require(!message.ShouldStillPlay(&gs, incoming), "countdown without an owner played");
        UObject stranger;
        incoming.OptionalObject = &stranger;
        Require(!message.ShouldStillPlay(&gs, incoming), "non-trainer object admitted speech");
        incoming.OptionalObject = &pc;
        pc.Local = false;
        Require(!message.ShouldStillPlay(&gs, incoming), "remote controller admitted local speech");
        pc.Local = true; pc.Valid = false;
        Require(!message.ShouldStillPlay(&gs, incoming), "destroying controller admitted speech");
        pc.Valid = true; pc.HasWorld = false;
        Require(!message.ShouldStillPlay(&gs, incoming), "owner without a world admitted speech");
    } else if (name == "boundaries") {
        for (float remaining : {3.f, 3.01f, 0.f, -1.f,
                std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            pc.Progress.RemainingSeconds = remaining;
            Require(!message.ShouldStillPlay(&gs, incoming), "auth wait or invalid countdown admitted speech");
        }
        pc.Progress.RemainingSeconds = 2.95f;
        for (int phase : {0, 2, 3}) {
            pc.Progress.Phase = phase;
            Require(!message.ShouldStillPlay(&gs, incoming), "speech played outside the countdown phase");
        }
        pc.Progress.Phase = 1;
        for (int count : {-1, 0, 1, 2, 4, 1003}) {
            incoming.Switch = count;
            Require(!message.ShouldStillPlay(&gs, incoming), "wrong spoken count or stock round message admitted");
        }
    } else if (name == "stale") {
        pc.TheWorld.Now = incoming.QueueTime + message.MaxAnnouncementDelay;
        Require(message.ShouldStillPlay(&gs, incoming), "fresh queued count expired early");
        pc.TheWorld.Now += .01f;
        Require(!message.ShouldStillPlay(&gs, incoming), "stale count replayed while progress was unchanged");
        incoming.QueueTime = pc.TheWorld.Now;
        pc.Progress.RemainingSeconds = 1.95f;
        Require(!message.ShouldStillPlay(&gs, incoming), "queued 3 played after countdown reached 2");
        incoming.Switch = 2;
        Require(message.ShouldStillPlay(&gs, incoming), "fresh 2 was rejected after discarding 3");
        pc.Progress.Phase = 0;
        Require(!message.ShouldStillPlay(&gs, incoming), "aborted countdown replayed queued speech");
    } else if (name == "presentation") {
        Require(message.GetText(3, false, nullptr, nullptr, &pc).Empty,
                "trainer countdown created duplicate stock HUD text");
        Require(message.MaxAnnouncementDelay > 0.f && message.MaxAnnouncementDelay < 1.f,
                "speech queue can delay a countdown into the next second");
        FAnnouncementInfo previous;
        for (int count : {3, 2, 1}) {
            incoming.Switch = count;
            Require(!message.IsOptionalSpoken(count), "trainer speech remained optional and interruptible");
            Require(message.GetAnnouncementPriority(incoming) > .8f,
                    "trainer speech stayed behind ordinary reward announcements");
            Require(message.InterruptAnnouncement(incoming, previous),
                    "previous announcement can hold up the trainer countdown");
        }
        for (int count : {0, 4, 1003}) {
            incoming.Switch = count;
            Require(!message.InterruptAnnouncement(incoming, previous),
                    "non-trainer count interrupted an announcement");
        }
    } else Require(false, "unknown case");
}
'''


class AimTrainerCountdownMessageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-countdown-message-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerCountdownMessage.cpp").read_text(encoding="utf-8-sig")
        header = (PLUGIN / "Source/Public/NCAimTrainerCountdownMessage.h").read_text(encoding="utf-8-sig")
        declaration = re.sub(r"^#(?:include|pragma).*\n", "", header, flags=re.MULTILINE)
        signatures = (
            "UNCAimTrainerCountdownMessage::UNCAimTrainerCountdownMessage",
            "FText UNCAimTrainerCountdownMessage::GetText",
            "bool UNCAimTrainerCountdownMessage::IsOptionalSpoken",
            "float UNCAimTrainerCountdownMessage::GetAnnouncementPriority",
            "bool UNCAimTrainerCountdownMessage::InterruptAnnouncement",
            "bool UNCAimTrainerCountdownMessage::ShouldStillPlay",
        )
        source = directory / "trainer_countdown_message.cpp"
        source.write_text("\n".join([ADAPTER, declaration]
            + [native_function(native, signature) for signature in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_countdown_message.exe" if os.name == "nt" else "trainer_countdown_message")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_countdown_message.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Countdown message adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_current_local_count_and_base_policy_are_respected(self): self.run_case("valid")
    def test_only_a_valid_local_trainer_owner_can_play(self): self.run_case("owner")
    def test_auth_wait_phase_and_count_boundaries_are_rejected(self): self.run_case("boundaries")
    def test_delayed_superseded_and_aborted_speech_is_rejected(self): self.run_case("stale")
    def test_trainer_speech_is_prompt_and_has_no_stock_hud_text(self): self.run_case("presentation")


if __name__ == "__main__":
    unittest.main()
