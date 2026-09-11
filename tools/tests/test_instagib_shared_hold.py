"""Exercise the actual firing input methods with a small native engine adapter.

Compiles StartFire, StopFire, the new Instagib guard/classifier, and the existing
deferred/active/refire methods. The adapter supplies deterministic timers and a
shot recorder; it does not simulate replication, hit traces, or engine frames.
Run the documented client/server playtest after the normal Unreal build too.
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


PLUGIN = Path(__file__).resolve().parents[2]
STOCK = PLUGIN.parents[1] / "Source/UnrealTournament/Private"


class InstagibSharedHoldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-instagib-hold-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        definitions = []
        for path, signatures in (
            (PLUGIN / "Source/Private/UTPlusShockRifle.cpp", (
                "bool AUTPlusShockRifle::HasSharedInstagibFireModes",
            )),
            (PLUGIN / "Source/Private/UTWeaponFix.cpp", (
                "bool AUTWeaponFix::TryPreserveInstagibHeldFire",
                "void AUTWeaponFix::StartFire", "void AUTWeaponFix::StopFire(uint8",
                "void AUTWeaponFix::StopFireInternal", "void AUTWeaponFix::OnRetryTimer",
                "void AUTWeaponFix::DeferredGotoActiveState",
                "bool AUTWeaponFix::IsFireModeOnCooldown",
            )),
            (STOCK / "UTWeapon.cpp", (
                "bool AUTWeapon::BeginFiringSequence", "void AUTWeapon::EndFiringSequence",
                "float AUTWeapon::GetRefireTime", "bool AUTWeapon::CanFireAgain",
                "bool AUTWeapon::HandleContinuedFiring",
            )),
            (STOCK / "UTWeaponStateActive.cpp", (
                "void UUTWeaponStateActive::BeginState",
                "bool UUTWeaponStateActive::BeginFiringSequence",
            )),
            (PLUGIN / "Source/Private/UTWeaponStateFiring_Transactional.cpp", (
                "void UUTWeaponStateFiring_Transactional::BeginState",
                "void UUTWeaponStateFiring_Transactional::RefireCheckTimer",
            )),
        ):
            source = path.read_text(encoding="utf-8-sig")
            definitions.extend(native_function(source, signature) for signature in signatures)
        adapter = (Path(__file__).with_name("instagib_shared_hold_adapter.cpp")).read_text(
            encoding="utf-8")
        source = directory / "instagib.cpp"
        source.write_text(adapter.replace("// NATIVE_METHODS", "\n".join(definitions)),
                          encoding="utf-8")
        cls.executable = directory / ("instagib.exe" if os.name == "nt" else "instagib")
        # UE_LOG is elided; diagnostic-only locals/parameters are intentionally unused.
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/wd4100", "/wd4189",
                       "/std:c++14", str(source), f"/Fe{cls.executable}",
                       f"/Fo{directory / 'instagib.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", "-Wno-unused-variable",
                       "-Wno-unused-but-set-variable", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Instagib adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_identical_beam_classification_and_custom_mode_exclusions(self):
        self.run_case("classifier")

    def test_guard_ownership_lifecycle_and_rollback_are_side_effect_free(self):
        self.run_case("guards")

    def test_other_button_tap_does_not_cancel_continued_hold_in_either_direction(self):
        self.run_case("overlap")

    def test_both_buttons_held_and_original_released_handoff_at_cooldown(self):
        self.run_case("handoff")

    def test_three_recorded_short_overlap_sequences_do_not_gain_extra_shots(self):
        self.run_case("recorded")

    def test_debounce_retry_and_refire_at_same_time_in_both_callback_orders(self):
        self.run_case("debounce")

    def test_repress_during_deferred_stop_does_not_duplicate_or_lose_hold(self):
        self.run_case("repress")

    def test_released_equip_and_cooldown_taps_still_cancel(self):
        self.run_case("early_taps")

    def test_normal_shock_core_then_primary_matches_legacy_path(self):
        self.run_case("combo")

    def test_local_listen_host_and_fire_rate_multiplier(self):
        self.run_case("local")

    def test_release_before_or_after_refire_boundary(self):
        self.run_case("boundary")


if __name__ == "__main__":
    unittest.main()
