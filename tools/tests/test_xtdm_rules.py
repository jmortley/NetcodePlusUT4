"""Run the real xTDM native automation cases with a minimal Unreal type adapter.

This compiles only the pure rules and their existing automation source. Actor
creation, bot destruction, replication, and editor defaults still need a normal
engine playtest; this does not launch UBT or build the plugin.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler
except ImportError:
    from test_wipeout_healing import find_compiler


PLUGIN = Path(__file__).resolve().parents[2]
NATIVE_TESTS = PLUGIN / "Source/Private/Tests/NCPlusXTDMRulesTests.cpp"

CORE = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
using int32 = int32_t;
using uint8 = uint8_t;
using FString = std::string;
#define TEXT(value) value
struct FMath {
    template<class T> static T Clamp(T value, T low, T high) {
        return std::max(low, std::min(value, high));
    }
};
'''

AUTOMATION = r'''
#pragma once
#include "CoreMinimal.h"
namespace EAutomationTestFlags { enum { EditorContext = 1, ClientContext = 2, EngineFilter = 4 }; }
struct FAutomationTestBase {
    int32 Failures = 0, Checks = 0;
    void TestTrue(const char* message, bool value) {
        ++Checks;
        if (!value) { ++Failures; std::cerr << message << '\n'; }
    }
    void TestFalse(const char* message, bool value) { TestTrue(message, !value); }
    template<class T, class U> void TestEqual(const char* message, const T& value, const U& expected) {
        TestTrue(message, value == expected);
    }
};
#define IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, Name, Flags) \
    struct Class : FAutomationTestBase { bool RunTest(const FString& Parameters); };
template<class T> int Run() {
    T test;
    const bool completed = test.RunTest("");
    std::cout << test.Checks << " native assertions\n";
    return completed && test.Failures == 0 ? 0 : 1;
}
'''


class XTDMRulesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-xtdm-rules-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        (directory / "CoreMinimal.h").write_text(CORE, encoding="utf-8")
        (directory / "NetcodePlus.h").write_text('#include "CoreMinimal.h"\n', encoding="utf-8")
        (directory / "Misc").mkdir()
        (directory / "Misc/AutomationTest.h").write_text(AUTOMATION, encoding="utf-8")
        native = NATIVE_TESTS.read_text(encoding="utf-8-sig")
        classes = re.findall(r"IMPLEMENT_SIMPLE_AUTOMATION_TEST\((\w+),", native)
        if not classes:
            raise AssertionError("No actual native xTDM automation cases found")
        main = '\nint main(int argc, char** argv) {\n if (argc != 2) return 2;\n'
        for name in classes:
            main += f' if (std::string(argv[1]) == "{name}") return Run<{name}>();\n'
        main += ' return 2;\n}\n'
        source = directory / "xtdm_rules.cpp"
        source.write_text(
            '#define WITH_DEV_AUTOMATION_TESTS 1\n'
            f'#include "{NATIVE_TESTS.as_posix()}"\n' + main, encoding="utf-8")
        cls.executable = directory / ("xtdm_rules.exe" if os.name == "nt" else "xtdm_rules")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/wd4100", "/std:c++14",
                       f"/I{directory}", str(source), f"/Fe{cls.executable}",
                       f"/Fo{directory / 'xtdm_rules.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", "-I", str(directory), str(source),
                       "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"xTDM rules adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_native_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"[1-9]\d* native assertions")

    def test_original_four_team_seat_policy(self):
        self.run_native_case("FNCPlusXTDMSeatsTest")

    def test_human_seats_and_replaceable_bots_preserve_capacity(self):
        self.run_native_case("FNCPlusXTDMHumanSeatsTest")

    def test_human_ready_gate_with_incomplete_or_bot_filled_roster(self):
        self.run_native_case("FNCPlusXTDMReadyRosterTest")

    def test_bot_fill_capacity_lock_and_draft_gates(self):
        self.run_native_case("FNCPlusXTDMBotPolicyTest")

    def test_existing_four_team_winner_policy(self):
        self.run_native_case("FNCPlusXTDMWinnerTest")


if __name__ == "__main__":
    unittest.main()
