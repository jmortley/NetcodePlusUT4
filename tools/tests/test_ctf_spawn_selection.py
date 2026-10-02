"""Run production CTF spawn methods against a small native Unreal adapter.

The selector and successful-spawn commit are extracted verbatim, including the
production history structure. This is not a UBT, collision-world, replication,
or multiplayer test; the adapter controls LOS and stock start ratings.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import find_compiler, native_function


PLUGIN = Path(__file__).resolve().parents[2]


class CTFSpawnSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-ctf-spawn-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        source = (PLUGIN / "Source/Private/NCPlusCTFGameMode.cpp").read_text(encoding="utf-8-sig")
        header = (PLUGIN / "Source/Public/NCPlusCTFGameMode.h").read_text(encoding="utf-8-sig")
        definitions = "\n".join(native_function(source, signature) for signature in (
            "void ANCPlusCTFGameMode::RestartPlayerAtPlayerStart(",
            "void ANCPlusCTFGameMode::CommitUsedSpawn(",
            "AActor* ANCPlusCTFGameMode::ChoosePlayerStart_Implementation(",
            "APlayerStart* ANCPlusCTFGameMode::ChooseNewCTFPlayerStart(",
        ))
        history = native_function(header, "struct FRecentSpawns") + ";"
        adapter = Path(__file__).with_name("ctf_spawn_selection_adapter.cpp").read_text(encoding="utf-8")
        generated = directory / "ctf_spawn.cpp"
        generated.write_text(adapter.replace("// PRODUCTION_HISTORY", history).replace(
            "// PRODUCTION_FUNCTIONS", definitions), encoding="utf-8")
        cls.executable = directory / ("ctf_spawn.exe" if os.name == "nt" else "ctf_spawn")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                       str(generated), f"/Fe{cls.executable}", f"/Fo{directory / 'ctf_spawn.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(generated), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Spawn adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_acrony_teammate_interleaving_does_not_reenable_previous_start(self):
        self.run_case("acrony")

    def test_secondary_rejects_previous_start_even_when_it_has_best_distance_score(self):
        self.run_case("secondary")

    def test_nearby_duplicate_is_excluded_but_other_floor_is_allowed(self):
        self.run_case("positions")

    def test_saved_position_survives_old_start_destruction(self):
        self.run_case("invalid-history")

    def test_same_actor_remains_excluded_after_it_moves(self):
        self.run_case("moved-start")

    def test_first_spawn_has_no_phantom_previous_origin(self):
        self.run_case("first")

    def test_cycle_tail_relaxes_before_per_player_exclusion(self):
        self.run_case("cycle-tail")

    def test_single_start_and_colocated_pool_have_explicit_emergency_logs(self):
        self.run_case("emergency")

    def test_secondary_disabled_keeps_exclusion_in_stock_rating_fallback(self):
        self.run_case("secondary-off")

    def test_preview_failed_spawn_existing_pawn_and_nonlive_spawn_do_not_commit(self):
        self.run_case("commit")

    def test_threshold_legacy_and_null_controller_keep_original_routes(self):
        self.run_case("wrapper-routes")

    def test_above_threshold_missing_pool_fallback_is_unconditionally_logged(self):
        self.run_case("wrapper-emergency")

    def test_normal_new_selector_does_not_call_stock_or_legacy(self):
        self.run_case("wrapper-new")


if __name__ == "__main__":
    unittest.main()
