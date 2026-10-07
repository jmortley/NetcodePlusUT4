"""Compile the real projectile claim/RPC/grace methods with observable UE shims.

This exercises identity selection and the existing geometry gates together.
Projectile ProcessHit and engine world traces are observed, not reimplemented;
full replicated rocket physics still requires a packaged multiplayer test.
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

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))


class ProjectileHitClaimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler, cls.environment, cls.msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-projectile-claims-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        tests = Path(__file__).parent
        source = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        header = (PLUGIN / "Source/Public/UTWeaponFix.h").read_text(encoding="utf-8-sig")
        start = header.index("struct FActiveServerProjectile")
        end = header.index("\n};", start) + len("\n};")
        entry = header[start:end]
        adapter = (tests / "projectile_hit_claims_adapter.h").read_text(encoding="utf-8")
        methods = (
            "bool AUTWeaponFix::Is329FireProtocolReady",
            "void AUTWeaponFix::OnTrackedProjectileResolved",
            "void AUTWeaponFix::OnTrackedRocketExploding",
            "void AUTWeaponFix::OnTrackedFlakExploding",
            "void AUTWeaponFix::CaptureFlakShellSpawn",
            "void AUTWeaponFix::ClearFlakShellClaims",
            "void AUTWeaponFix::NotifyFakeProjectileHit",
            "AUTWeaponFix* AUTWeaponFix::FindFiringWeaponForProjectile",
            "void AUTWeaponFix::PruneTrackedProjectiles",
            "void AUTWeaponFix::ServerProjectileHitClaim_Implementation",
            "void AUTWeaponFix::ServerLoadedRocketHitClaim_Implementation",
            "void AUTWeaponFix::ServerFlakShellHitClaim_Implementation",
            "void AUTWeaponFix::ProcessProjectileHitClaim",
        )
        native_methods = []
        for signature in methods:
            body = native_function(source, signature)
            if signature == "void AUTWeaponFix::NotifyFakeProjectileHit":
                # Existing hitsound estimation assigns float radial damage to
                # an integer. UE's compiler settings permit that narrowing;
                # scope the matching diagnostic policy to this actual method.
                body = ("#ifdef _MSC_VER\n#pragma warning(push)\n#pragma warning(disable:4244)\n#endif\n"
                        + body + "\n#ifdef _MSC_VER\n#pragma warning(pop)\n#endif\n")
            native_methods.append(body)
        cls.unit_source = (adapter.replace("// TRACKED_ENTRY", entry)
                           + "\n".join(native_methods)
                           + (tests / "projectile_hit_claims_cases.cpp").read_text(encoding="utf-8"))
        cls.executable = cls.compile_unit(cls.unit_source, "claims")

    @classmethod
    def compile_unit(cls, source, name):
        unit = cls.directory / f"{name}.cpp"
        unit.write_text(source, encoding="utf-8")
        executable = cls.directory / (f"{name}.exe" if os.name == "nt" else name)
        if cls.msvc:
            command = [cls.compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", "/utf-8", str(unit),
                       f"/I{PLUGIN / 'Source/Public'}",
                       f"/Fe{executable}", f"/Fo{cls.directory / (name + '.obj')}"]
        else:
            command = [cls.compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", f"-I{PLUGIN / 'Source/Public'}",
                       str(unit), "-o", str(executable)]
        build = subprocess.run(command, cwd=cls.directory, env=cls.environment,
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError(f"Projectile claim compilation failed:\n{build.stdout}\n{build.stderr}")
        return executable

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_exact_third_sibling_selected_instead_of_fifo(self): self.run_case("exact_sibling")
    def test_out_of_order_and_duplicate_claims_consume_each_rocket_once(self): self.run_case("out_of_order")
    def test_unknown_identity_epoch_ordinal_owner_and_weapon_fail_closed(self): self.run_case("identity_guards")
    def test_loaded_rpc_requires_authority_handshake_and_current_ownership(self): self.run_case("rpc_guards")
    def test_legacy_claim_cannot_consume_identified_loaded_rockets(self): self.run_case("legacy")
    def test_grace_survives_new_spawn_pruning_and_selects_exact_resolved_sibling(self): self.run_case("grace_spawn")
    def test_authoritative_direct_hit_cannot_be_applied_twice(self): self.run_case("direct_hit")
    def test_expired_unresolved_and_bounded_tracking_cleanup(self): self.run_case("prune")
    def test_contact_history_wall_and_ping_validation_still_reject(self): self.run_case("validation")
    def test_claim_anchor_does_not_replace_authoritative_damage_origin(self): self.run_case("damage_origin")
    def test_reentrant_damage_callbacks_cannot_reuse_identity_or_remove_another_entry(self): self.run_case("reentrant")
    def test_explosion_hook_captures_stock_query_and_nonblocking_overlap_candidates(self): self.run_case("explosion_snapshot")
    def test_explosion_hook_requires_authoritative_first_loaded_terminal_transition(self): self.run_case("explosion_guards")
    def test_invalid_explosion_geometry_denies_grace_and_zero_radius_is_valid(self): self.run_case("explosion_geometry")
    def test_possible_splash_victim_cannot_receive_grace_damage_even_behind_wall(self): self.run_case("splash_guard")
    def test_unknown_explosion_snapshot_fails_closed_without_changing_legacy_grace(self): self.run_case("unknown_snapshot")
    def test_low_ping_exact_live_and_grace_claims_keep_bounded_rewind(self): self.run_case("low_ping")
    def test_flak_exact_overlapping_shells_and_duplicates_keep_native_hit_dispatch(self): self.run_case("flak_exact")
    def test_flak_unknown_stale_owner_fire_mode_and_protocol_fail_closed(self): self.run_case("flak_guards")
    def test_flak_loaded_rocket_and_legacy_claims_cannot_consume_each_other(self): self.run_case("flak_isolation")
    def test_flak_exact_grace_does_not_consume_unrelated_live_shell(self): self.run_case("flak_grace")
    def test_flak_grace_denies_shards_and_any_pawn_impact(self): self.run_case("flak_terminal_guards")
    def test_flak_splash_query_and_untrusted_snapshot_guard(self): self.run_case("flak_splash")
    def test_flak_low_ping_live_and_grace_rewind_gravity_and_physics_gates(self): self.run_case("flak_low_ping")
    def test_flak_spawn_capture_requires_ownership_and_never_reuses_ids(self): self.run_case("flak_capture")
    def test_flak_explosion_snapshot_requires_first_authoritative_terminal(self): self.run_case("flak_explosion_guards")
    def test_flak_drop_repick_clears_claims_without_resetting_identity_counter(self): self.run_case("flak_drop_repick")
    def test_flak_client_exact_route_has_no_wrong_weapon_or_legacy_fallback(self): self.run_case("flak_client_route")

    def test_identity_and_splash_regressions_detect_broken_guards(self):
        # Mutate only the assembled temporary translation unit. Repository
        # production files remain untouched, and each broken unit must compile.
        mutants = (
            ("ordinal", "&& Entry.LoadedRocketOrdinal == ClaimedOrdinal", "&& true",
             "exact_sibling", "third loaded rocket claim consumed oldest sibling"),
            ("splash", "!E.bLoadedExplosionObserved\n\t\t\t|| E.PossibleSplashTargets.Contains(ClaimedTarget)",
             "!E.bLoadedExplosionObserved || false",
             "splash_guard", "possible splash victim received full-damage top-up"),
            ("flak_id", "Entry.FlakShotId == ClaimedFlakShotId", "true",
             "flak_exact", "exact Flak ID selected the older overlapping shell"),
            ("flak_shards", "!E.bFlakExplosionObserved || !E.bFlakGraceEligible", "!E.bFlakExplosionObserved",
             "flak_terminal_guards", "Flak terminal with authored shards or pawn impact accepted a top-up"),
        )
        for name, original, replacement, case, expected in mutants:
            with self.subTest(guard=name):
                self.assertEqual(self.unit_source.count(original), 1)
                executable = self.compile_unit(self.unit_source.replace(original, replacement), f"mutant_{name}")
                result = subprocess.run([str(executable), case], env=self.environment,
                                        capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0, f"{name} regression failed to detect the disabled guard")
                self.assertIn(expected, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
