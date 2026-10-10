"""Exercise production picker geometry and mouse routing without an engine renderer."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using int32 = int32_t;
using uint8 = uint8_t;
using TCHAR = char;
#define TEXT(x) x
struct FString : std::string {
    FString() = default;
    using std::string::string;
    FString(const std::string& s) : std::string(s) {}
    const char* operator*() const { return c_str(); }
    static FString FromInt(int value) { return std::to_string(value); }
    static FString Printf(const char* format, ...) {
        char buffer[1024]; va_list args; va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args); va_end(args); return buffer;
    }
};
struct FMath { template<class T> static T Clamp(T x, T lo, T hi) { return std::max(lo, std::min(x, hi)); } };
struct FLinearColor { FLinearColor(float=0, float=0, float=0, float=0) {} };
FLinearColor TrainerPanel, TrainerInk, TrainerMuted, TrainerAccent;
struct FVector2D { float X=0, Y=0; };
enum class FKey { LeftMouseButton, Other };
using EKeys = FKey;
enum EInputEvent { IE_Pressed, IE_Released };
struct FNCAimTrainerProgress {
    uint8 Scenario=0;
    bool bMovementPractice=false;
    int Score=0, Hits=0, Shots=0, TargetsExpired=0, Headshots=0;
    float TrackingSeconds=0, FiringSeconds=0, Accuracy=0;
};
struct ANCAimTrainerPlayerController {
    FNCAimTrainerProgress Progress;
    bool Menu=true, Focus=true, Local=false;
    int Selection=-1, Starts=0, Backs=0, Movement=0, Sources=0;
    const FNCAimTrainerProgress& GetTrainerProgress() const { return Progress; }
    bool IsTrainerMenuVisible() const { return Menu; }
    bool HasTrainerInputFocus() const { return Focus; }
    void SelectTrainerScenario(uint8 value) { Selection=value; }
    void StartTrainerRun() { ++Starts; }
    void ReturnToTrainerMenu() { ++Backs; }
    void ToggleTrainerMovementPractice() { ++Movement; }
    void SelectTrainerLeaderboardSource(bool local) { ++Sources; Local=local; }
};
template<class T> T* Cast(ANCAimTrainerPlayerController* pc) { return static_cast<T*>(pc); }
struct FTrainerButton { FVector2D Min, Max; int32 Action; FString Text; };
struct ANCAimTrainerHUD {
    ANCAimTrainerPlayerController* PlayerOwner=nullptr;
    std::vector<FTrainerButton> TrainerButtons;
    std::vector<FString> Labels;
    float MouseX=0, MouseY=0, BoardY=0;
    void Panel(float,float,float,float,const FLinearColor&) {}
    void Label(const FString& text,float,float,float,const FLinearColor&,float=0,bool=false) { Labels.push_back(text); }
    void Button(int32 action,const FString& text,float x,float y,float w,float h,bool) {
        FTrainerButton b; b.Min.X=x; b.Min.Y=y; b.Max.X=x+w; b.Max.Y=y+h; b.Action=action; b.Text=text; TrainerButtons.push_back(b);
    }
    void DrawLeaderboard(ANCAimTrainerPlayerController*,float y) { BoardY=y; }
    bool IsHovered(float x,float y,float w,float h) const { return MouseX>=x && MouseX<=x+w && MouseY>=y && MouseY<=y+h; }
    void DrawModePicker(ANCAimTrainerPlayerController*);
    void DrawResults(ANCAimTrainerPlayerController*);
    bool OverrideMouseClick(FKey,EInputEvent);
};
void Require(bool valid,const char* error) { if (!valid) { std::cerr<<error<<'\n'; std::exit(1); } }
'''

CASES = r'''
int main() {
    ANCAimTrainerPlayerController pc;
    ANCAimTrainerHUD hud; hud.PlayerOwner=&pc;
    hud.DrawModePicker(&pc);
    Require(hud.TrainerButtons.size()==15, "picker does not expose thirteen presets plus movement and start");
    for (int i=0;i<13;++i) {
        const auto& button=hud.TrainerButtons[i];
        Require(button.Action==i, "picker renumbered a preset");
        Require(button.Min.X>=90 && button.Max.X<=1190 && button.Min.Y>=200 && button.Max.Y<=356, "picker card escaped its panel");
        for (int j=0;j<i;++j) {
            const auto& other=hud.TrainerButtons[j];
            Require(button.Min.X>=other.Max.X || button.Max.X<=other.Min.X || button.Min.Y>=other.Max.Y || button.Max.Y<=other.Min.Y,
                "picker cards overlap");
        }
        hud.MouseX=(button.Min.X+button.Max.X)/2; hud.MouseY=(button.Min.Y+button.Max.Y)/2;
        Require(hud.OverrideMouseClick(EKeys::LeftMouseButton,IE_Pressed), "picker did not consume click");
        Require(pc.Selection==i && pc.Starts==0 && pc.Backs==0, "preset click collided with a control action");
    }
    Require(hud.BoardY>=400 && hud.BoardY+210<=hud.TrainerButtons.back().Min.Y, "leaderboard overlaps picker controls");
    const auto& start=hud.TrainerButtons.back();
    hud.MouseX=start.Min.X+1; hud.MouseY=start.Min.Y+1;
    hud.OverrideMouseClick(EKeys::LeftMouseButton,IE_Pressed);
    Require(pc.Starts==1, "start button did not start");
    pc.Focus=false;
    Require(!hud.OverrideMouseClick(EKeys::LeftMouseButton,IE_Pressed) && pc.Starts==1, "picker stole stock menu focus");
    pc.Focus=true; pc.Progress.Scenario=10; pc.Progress.Hits=5; pc.Progress.TargetsExpired=2;
    hud.TrainerButtons.clear(); hud.Labels.clear(); hud.DrawResults(&pc);
    bool found=false;
    for (const auto& text:hud.Labels) if (text.find("5 TARGETS HIT / 2 LANDED")!=std::string::npos) found=true;
    Require(found, "rockets display shot accuracy instead of hit versus landed targets");
    Require(hud.TrainerButtons.size()==3 && hud.TrainerButtons[0].Action==BackAction && hud.TrainerButtons[2].Action==StartAction,
        "result controls collide with scenario IDs");
    hud.MouseX=hud.TrainerButtons[0].Min.X+1; hud.MouseY=hud.TrainerButtons[0].Min.Y+1;
    hud.OverrideMouseClick(EKeys::LeftMouseButton,IE_Pressed);
    Require(pc.Backs==1 && pc.Selection==12, "back control selected rockets");
}
'''


class AimTrainerMenuTests(unittest.TestCase):
    def test_picker_layout_actions_and_rocket_result_labels(self):
        compiler, environment, msvc = find_compiler()
        native = (PLUGIN / "Source/Private/NCAimTrainerHUD.cpp").read_text(encoding="utf-8-sig")
        actions = re.search(r"enum \{ StartAction[^}]+\};", native).group()
        functions = [native_function(native, signature) for signature in (
            "const TCHAR* ScenarioName", "void ANCAimTrainerHUD::DrawModePicker",
            "void ANCAimTrainerHUD::DrawResults", "bool ANCAimTrainerHUD::OverrideMouseClick")]
        with tempfile.TemporaryDirectory(prefix="ncp-aim-menu-") as temporary:
            directory = Path(temporary)
            source = directory / "menu.cpp"
            policy = (PLUGIN / "Source/Private/NCAimTrainerScenarioPolicy.h").as_posix()
            source.write_text("\n".join([f'#include "{policy}"', ADAPTER, actions] + functions + [CASES]), encoding="utf-8")
            executable = directory / ("menu.exe" if os.name == "nt" else "menu")
            command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source), f"/Fe{executable}", f"/Fo{directory / 'menu.obj'}"] if msvc else
                       [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(executable)])
            result = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
