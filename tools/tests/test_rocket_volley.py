"""Compile the production 329 volley identity core; no UE build or simulated RPC claims."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import find_compiler, native_function
except ImportError:
    from test_wipeout_healing import find_compiler, native_function

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))


class RocketVolleyTests(unittest.TestCase):
    def test_actual_rpc_lifecycle_methods(self):
        compiler, environment, msvc = find_compiler()
        source = "\n".join((PLUGIN / path).read_text(encoding="utf-8") for path in (
            "Source/Private/NCRocketVolley.cpp", "Source/Private/UTPlusWeap_RocketLauncher.cpp"))
        methods = [
            "void AUTPlusWeap_RocketLauncher::ResetLoadedOwnershipState",
            "void AUTPlusWeap_RocketLauncher::GivenTo",
            "void AUTPlusWeap_RocketLauncher::Removed",
            "void AUTPlusWeap_RocketLauncher::OnRep_LoadedOwnershipEpoch",
            "void AUTPlusWeap_RocketLauncher::StartFire",
            "void AUTPlusWeap_RocketLauncher::StopFire",
            "void AUTPlusWeap_RocketLauncher::NotifyLoadedVolleyRelease",
            "bool AUTPlusWeap_RocketLauncher::CanBeginLoadedVolleyInput",
            "void AUTPlusWeap_RocketLauncher::BufferLoadedVolleyInput",
            "void AUTPlusWeap_RocketLauncher::TryDrainLoadedVolleyInput",
            "void AUTPlusWeap_RocketLauncher::ClearLoadedVolleyInput",
            "bool AUTPlusWeap_RocketLauncher::PutDown",
            "void AUTPlusWeap_RocketLauncher::DetachFromOwner_Implementation",
            "void AUTPlusWeap_RocketLauncher::ClientGivenTo_Internal",
            "void AUTPlusWeap_RocketLauncher::Destroyed",
            "void AUTPlusWeap_RocketLauncher::EndPlay",
            "void AUTPlusWeap_RocketLauncher::StateChanged",
            "bool AUTPlusWeap_RocketLauncher::ObserveLoadedRocketActor",
            "bool AUTPlusWeap_RocketLauncher::CanBeginLoadedVolley(",
            "bool AUTPlusWeap_RocketLauncher::IsLoadedVolleyModeValid",
            "void AUTPlusWeap_RocketLauncher::ResetLoadedVolley",
            "void AUTPlusWeap_RocketLauncher::TryBeginLoadedVolley",
            "void AUTPlusWeap_RocketLauncher::ServerBeginLoadedVolley_Implementation",
            "void AUTPlusWeap_RocketLauncher::ServerReleaseLoadedVolley_Implementation",
            "void AUTPlusWeap_RocketLauncher::ServerSetLoadedRocketMode_Implementation",
            "bool AUTPlusWeap_RocketLauncher::CommitLoadedVolley",
            "void AUTPlusWeap_RocketLauncher::SendLoadedVolleyReceipt",
            "void AUTPlusWeap_RocketLauncher::CompleteLoadedVolley",
            "FNCLoadedRocketPrediction& AUTPlusWeap_RocketLauncher::FindOrAddLoadedRocket",
            "void AUTPlusWeap_RocketLauncher::ClientLoadedRocketResult_Implementation",
            "void AUTPlusWeap_RocketLauncher::ClientLoadedVolleyResult_Implementation",
            "void AUTPlusWeap_RocketLauncher::ReconcileLoadedRockets",
            "void AUTPlusWeap_RocketLauncher::CaptureLoadedRocketSpawn",
            "AUTProjectile* AUTPlusWeap_RocketLauncher::SpawnNetPredictedProjectile",
        ]
        with tempfile.TemporaryDirectory(prefix="ncp-volley-lifecycle-") as temporary:
            directory = Path(temporary)
            tests = Path(__file__).parent
            unit = directory / "lifecycle.cpp"
            unit.write_text((tests / "rocket_volley_adapter.h").read_text()
                            + "\n".join(native_function(source, signature) for signature in methods)
                            + (tests / "rocket_volley_lifecycle_cases.cpp").read_text(), encoding="utf-8")
            executable = directory / ("lifecycle.exe" if os.name == "nt" else "lifecycle")
            include = PLUGIN / "Source/Public"
            if msvc:
                command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                           f"/I{include}", str(unit), f"/Fe{executable}"]
            else:
                command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror",
                           f"-I{include}", str(unit), "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, env=environment,
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], cwd=directory, env=environment,
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_protocol_core(self):
        compiler, environment, msvc = find_compiler()
        with tempfile.TemporaryDirectory(prefix="ncp-rocket-volley-") as temporary:
            directory = Path(temporary)
            source = Path(__file__).with_name("rocket_volley_cases.cpp")
            executable = directory / ("volley.exe" if os.name == "nt" else "volley")
            include = PLUGIN / "Source/Public"
            if msvc:
                command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                           f"/I{include}", str(source), f"/Fe{executable}"]
            else:
                command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror",
                           f"-I{include}", str(source), "-o", str(executable)]
            build = subprocess.run(command, cwd=directory, env=environment,
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], cwd=directory, env=environment,
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("cases passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
