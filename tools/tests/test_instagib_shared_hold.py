"""Exercise the actual firing input methods with a small native engine adapter.

Compiles StartFire, StopFire, Instagib equip retention and action provenance,
and the existing deferred/active/equip/refire methods. The adapter supplies
deterministic timers, explicit frame boundaries, and a shot recorder. It does
not simulate replication, hit traces, or the engine input binding dispatcher.
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


PLUGIN = Path(os.environ.get("NCP_TEST_PLUGIN", Path(__file__).resolve().parents[2]))
SOURCE_OVERRIDE = Path(os.environ["NCP_TEST_SOURCE"]) if "NCP_TEST_SOURCE" in os.environ else None
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
                "bool AUTPlusShockRifle::CanRetainInstagibEquipTap",
                "void AUTPlusShockRifle::ClearInstagibEquipTap",
                "void AUTPlusShockRifle::PumpInstagibEquipTap",
                "void AUTPlusShockRifle::StartFire",
                "void AUTPlusShockRifle::StopFire",
                "void AUTPlusShockRifle::GotoState",
                "void AUTPlusShockRifle::BringUp",
            )),
            (PLUGIN / "Source/Private/NCInstagibEquipInput.cpp", (
                "void AUTPlusShockRifle::NoteInstagibEquipPress",
                "void AUTPlusShockRifle::NoteInstagibEquipRelease",
                "bool AUTPlusShockRifle::ConsumeInstagibEquipPress",
            )),
            (PLUGIN / "Source/Private/UTWeaponFix.cpp", (
                "bool AUTWeaponFix::TryPreserveInstagibHeldFire",
                "void AUTWeaponFix::StartFire", "void AUTWeaponFix::StopFire(uint8",
                "void AUTWeaponFix::StopFireInternal", "void AUTWeaponFix::OnRetryTimer",
                "void AUTWeaponFix::DeferredGotoActiveState",
                "void AUTWeaponFix::ClearDeferredActiveState",
                "void AUTWeaponFix::ScheduleDeferredActiveState",
                "bool AUTWeaponFix::IsFireModeOnCooldown",
            )),
            (STOCK / "UTWeapon.cpp", (
                "bool AUTWeapon::BeginFiringSequence", "void AUTWeapon::EndFiringSequence",
                "float AUTWeapon::GetRefireTime", "bool AUTWeapon::CanFireAgain",
                "bool AUTWeapon::HandleContinuedFiring",
            )),
            (STOCK / "UTPlayerController.cpp", (
                "void AUTPlayerController::ApplyDeferredFireInputs",
                "bool AUTPlayerController::HasDeferredFireInputs",
            )),
            (STOCK / "UTWeaponStateActive.cpp", (
                "void UUTWeaponStateActive::BeginState",
                "bool UUTWeaponStateActive::BeginFiringSequence",
            )),
            (STOCK / "UTWeaponStateEquipping.cpp", (
                "void UUTWeaponStateEquipping::BringUpFinished",
                "bool UUTWeaponStateEquipping::BeginFiringSequence",
            )),
            (PLUGIN / "Source/Private/UTWeaponStateFiring_Transactional.cpp", (
                "void UUTWeaponStateFiring_Transactional::BeginState",
                "void UUTWeaponStateFiring_Transactional::RefireCheckTimer",
            )),
        ):
            if SOURCE_OVERRIDE and path.is_relative_to(PLUGIN):
                candidate = SOURCE_OVERRIDE / path.relative_to(PLUGIN)
                if candidate.exists():
                    path = candidate
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

    def test_ready_shared_mode_repress_preserves_hold_without_debounce(self):
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

    def test_old_release_cannot_cancel_new_cycle_with_input_before_timers(self):
        self.run_case("stale_release")

    def test_ready_short_press_fires_and_release_does_not_create_a_hold(self):
        self.run_case("ready_debounce")

    def test_release_callback_cannot_affect_replacement_owner(self):
        self.run_case("release_ownership")

    def test_physical_equip_tap_fires_once_when_ready_in_each_local_net_mode(self):
        self.run_case("equip_tap")

    def test_equip_hold_keeps_normal_cadence_without_extra_shot(self):
        self.run_case("equip_hold")

    def test_multiple_equip_actions_coalesce_and_unprovenanced_calls_do_not_queue(self):
        self.run_case("equip_provenance")

    def test_ineligible_pawn_weapon_or_gameplay_state_cannot_retain_tap(self):
        self.run_case("equip_guards")

    def test_equip_queue_does_not_survive_lifecycle_or_internal_stop(self):
        self.run_case("equip_lifecycle")

    def test_retained_shot_uses_dispatch_aim_and_time_and_waits_for_legal_cadence(self):
        self.run_case("equip_dispatch")

    def test_fresh_active_input_supersedes_waiting_equip_intent(self):
        self.run_case("equip_fresh_input")

    def test_retained_dispatch_cleanup_cannot_cancel_reentrant_action_or_owner(self):
        self.run_case("equip_reentrant")

    def test_internal_stop_cancels_observed_equip_action_before_dispatch(self):
        self.run_case("equip_token_stop")

    def test_stock_dropped_start_forwarded_release_recovers_once_after_possession_and_equip(self):
        self.run_case("possession_tap")

    def test_possession_capture_excludes_blocked_spectator_acknowledged_and_synthetic_inputs(self):
        self.run_case("possession_guards")

    def test_possession_tap_cancels_on_focus_menu_input_and_weapon_lifecycle_changes(self):
        self.run_case("possession_invalidation")

    def test_possession_tap_requires_acknowledgment_and_expires_on_real_time(self):
        self.run_case("possession_deadline")

    def test_possession_taps_coalesce_and_yield_to_fresh_playing_actions(self):
        self.run_case("possession_coalescing")

    def test_possession_tap_cannot_duplicate_restart_recovered_held_input(self):
        self.run_case("possession_held")


if __name__ == "__main__":
    unittest.main()
