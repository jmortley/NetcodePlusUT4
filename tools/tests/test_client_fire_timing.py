"""Compile the actual client cadence helper against a native UE adapter.

The adapter implements the relevant TimerManager pending/active/executing phases
and double clock. It does not replace a UE build or multiplayer playtest. Weak
pointer serial tracking is provided by UE in production, not emulated here.
NCP_TEST_SOURCE may point at an alternate plugin root for staged patch validation.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler
except ImportError:
    from test_wipeout_healing import find_compiler

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))


class ClientFireTimingTests(unittest.TestCase):
    def test_actual_helper_cadence_and_lifecycle(self):
        compiler, environment, msvc = find_compiler()
        with tempfile.TemporaryDirectory(prefix="ncp-client-cadence-") as temporary:
            directory = Path(temporary)
            tests = Path(__file__).parent
            (directory / "timing_adapter.h").write_bytes(
                (tests / "client_fire_timing_adapter.h").read_bytes())
            (directory / "timing_tests.cpp").write_bytes(
                (tests / "client_fire_timing_cases.cpp").read_bytes())
            for name in ("NCClientFireTiming.h", "NCClientFireTiming.cpp"):
                (directory / name).write_bytes((PLUGIN / "Source/Private" / name).read_bytes())
            for name in ("CoreMinimal.h", "NetcodePlus.h", "UTWeaponFix.h", "UTCharacter.h",
                         "Engine/DemoNetDriver.h", "Engine/World.h", "TimerManager.h",
                         "UObject/UObjectBase.h"):
                target = directory / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#include "timing_adapter.h"\n', encoding="utf-8")
            executable = directory / ("timing.exe" if os.name == "nt" else "timing")
            sources = [str(directory / "NCClientFireTiming.cpp"), str(directory / "timing_tests.cpp")]
            if msvc:
                command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/wd4100", "/wd4189",
                           "/std:c++14", f"/I{directory}", *sources, f"/Fe{executable}"]
            else:
                command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           f"-I{directory}", *sources, "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, env=environment,
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], cwd=directory, env=environment,
                                 capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("TOTAL 33 scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
