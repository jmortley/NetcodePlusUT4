"""Native regressions for the bounded server rate reservation.

Compiles real admission, callback, FIFO promotion and Stop-ownership source.
World/timers/state dispatch/damage are explicit adapters, not Unreal runtime.
The extracted Stop subset ends before stock visual/effect cleanup. Dedicated
server canaries must still validate projectile pairing and hit history.

NCP_TEST_PLUGIN chooses a sibling port. NCP_TEST_SOURCE overrides UTWeaponFix.cpp
directly (or its project root), allowing combined staged changes to be tested.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler, native_function
except ImportError:
    from test_wipeout_healing import find_compiler, native_function

PLUGIN = Path(os.environ.get("NCP_TEST_PLUGIN", Path(__file__).resolve().parents[2]))
SOURCE = Path(os.environ.get("NCP_TEST_SOURCE", PLUGIN / "Source/Private/UTWeaponFix.cpp"))
if SOURCE.is_dir():
    SOURCE /= "Source/Private/UTWeaponFix.cpp"


class ServerRateReservationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-server-rate-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = SOURCE.read_text(encoding="utf-8-sig")
        definitions = [native_function(source, name) for name in (
            "static float ServerRateReservationDelay",
            "bool AUTWeaponFix::CanReserveServerRateFire",
            "bool AUTWeaponFix::IsFireEventSequenceValid",
            "bool AUTWeaponFix::ValidateFireRequest",
            "void AUTWeaponFix::CompleteAcceptedRateFire",
            "void AUTWeaponFix::PromoteFollowingRateFire",
        )]
        stop = native_function(source, "void AUTWeaponFix::ServerStopFireFixed_Implementation")
        stop = stop[stop.index("    if (LastProcessedStopEventIndex.IsValidIndex"):
                    stop.index("\t// Log only the accepted fixed Stop.")]
        definitions.append("void AUTWeaponFix::ApplyStop(uint8 FireModeNum, int32 InFireEventIndex) {\n"
                           + stop + "\nif (UTOwner) UTOwner->SetPendingFire(FireModeNum, false);\n}\n")
        adapter = Path(__file__).with_name("server_rate_reservation_adapter.cpp").read_text(encoding="utf-8")
        cpp = directory / "server_rate.cpp"
        cpp.write_text(adapter.replace("// NATIVE_METHODS", "\n".join(definitions)), encoding="utf-8")
        cls.executable = directory / ("server_rate.exe" if os.name == "nt" else "server_rate")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/wd4100", "/wd4189",
                       "/wd4505", "/wd4458", "/std:c++14", str(cpp), f"/Fe{cls.executable}",
                       f"/Fo{directory / 'server_rate.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                       str(cpp), "-o", str(cls.executable)]
        build = subprocess.run(command, cwd=directory, env=cls.environment,
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError("Rate adapter compile failed:\n" + build.stdout + build.stderr)

    def test_admission_release_fifo_lifetime_duplicate_and_rate_boundaries(self):
        result = subprocess.run([str(self.executable)], env=self.environment,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("23 scenario groups", result.stdout)


if __name__ == "__main__":
    unittest.main()
