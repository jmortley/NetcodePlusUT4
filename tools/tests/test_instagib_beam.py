"""Compile the actual Instagib beam gates and prediction methods with UT adapters.

Stock prediction/timer and character flash methods are extracted too. Rendering
is recorded at the base impact/layer boundary; this is not an Unreal build or a
test of authored particle assets, Blueprint overrides, or transport.
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
SOURCE_OVERRIDE = Path(os.environ["NCP_TEST_SOURCE"]) if "NCP_TEST_SOURCE" in os.environ else None
STOCK = PLUGIN.parents[1] / "Source/UnrealTournament/Private"


class InstagibBeamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-instagib-beam-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        definitions = []
        for path, signatures in (
            (PLUGIN / "Source/Private/UTPlusShockRifle.cpp", (
                "bool AUTPlusShockRifle::HasSharedInstagibFireModes",
                "bool AUTPlusShockRifle::IsInstagibBeamFireMode",
                "bool AUTPlusShockRifle::NeedsLegacyInstagibBeamLayer",
                "void AUTPlusShockRifle::PlayPredictedImpactEffects",
                "void AUTPlusShockRifle::PlayImpactEffects_Implementation",
            )),
            (STOCK / "UTWeapon.cpp", (
                "void AUTWeapon::PlayPredictedImpactEffects",
                "void AUTWeapon::PlayDelayedImpactEffects",
            )),
            (STOCK / "UTCharacter.cpp", (
                "void AUTCharacter::SetFlashLocation",
                "void AUTCharacter::FiringInfoReplicated",
            )),
        ):
            if SOURCE_OVERRIDE and path.is_relative_to(PLUGIN):
                candidate = SOURCE_OVERRIDE / path.relative_to(PLUGIN)
                if candidate.exists():
                    path = candidate
            source = path.read_text(encoding="utf-8-sig")
            definitions.extend(native_function(source, signature) for signature in signatures)
        adapter = Path(__file__).with_name("instagib_beam_adapter.cpp").read_text(encoding="utf-8")
        source = directory / "beam.cpp"
        source.write_text(adapter.replace("// NATIVE_METHODS", "\n".join(definitions)), encoding="utf-8")
        cls.executable = directory / ("beam.exe" if os.name == "nt" else "beam")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                       str(source), f"/Fe{cls.executable}", f"/Fo{directory / 'beam.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Beam adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_modes_predict_immediately_above_sleep_budget_and_ignore_echo(self):
        self.run_case("immediate")

    def test_both_modes_hide_only_selected_beam_restore_asset_and_keep_endpoint(self):
        self.run_case("hidden")

    def test_both_modes_get_one_extra_layer_without_repeating_endpoint(self):
        self.run_case("layers")

    def test_layer_uses_effect_mode_and_obeys_stock_effect_interval(self):
        self.run_case("effect_mode")

    def test_normal_shock_and_custom_or_incomplete_alternate_keep_base_path(self):
        self.run_case("excluded")

    def test_remote_and_null_owner_prediction_remain_base_behavior(self):
        self.run_case("prediction_guards")

    def test_layer_requires_local_first_person_network_view_with_sleep_budget(self):
        self.run_case("layer_guards")


if __name__ == "__main__":
    unittest.main()
