"""Synthetic acceptance tests; no engine build or live gameplay required."""

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "check-fire-provenance.py"
SPEC = importlib.util.spec_from_file_location("check_fire_provenance", SCRIPT)
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)


def row(kind, source="FixedInitial", event=7, auth=7, generation=10, **values):
    fields = dict(source=source, weapon="BP_RocketLauncher_C_3", mode=0,
                  event=event, auth=auth, generation=generation)
    fields.update(values)
    return "[2026.09.04-12.00.00:000]LogUTWeaponFix: [NCFireAuth] " + kind + " " + " ".join(
        f"{key}={value}" for key, value in fields.items())


def report(*lines, **options):
    return checker.analyze(checker.parse_records(lines), **options)


def codes(result):
    return {finding["code"] for finding in result.findings}


class FireProvenanceTests(unittest.TestCase):
    def test_duplicate_numbered_dispatch_is_violation(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SHOT_END", attempts=1, spawns=1),
                        row("SHOT"), row("SHOT_END", attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("DUPLICATE_DISPATCH", codes(result))

    def test_deferred_uses_original_event_despite_newer_stop_auth(self):
        result = report(row("ACCEPT", source="DeferredChargedTail", deferred=1),
                        row("SHOT", source="DeferredChargedTail", auth=8),
                        row("SPAWN", source="DeferredChargedTail", auth=8, result="ok"),
                        row("SHOT_END", source="DeferredChargedTail", auth=8, attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["accepted_events_dispatched"], 1)

    def test_minigun_shard_after_stock_spin_retains_numbered_context(self):
        identity = dict(source="DeferredStateTail", weapon="BP_Minigun_C_4", mode=1, event=19, generation=44)
        result = report(row("ACCEPT", **identity, auth=19, deferred=1, state="SpinUp"),
                        row("SHOT", **identity, auth=20, state="Firing_Transactional"),
                        row("SPAWN", **identity, auth=20, result="ok"),
                        row("SHOT_END", **identity, auth=20, attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["accepted_events_dispatched"], 1)
        self.assertEqual(result.counts["numbered_dispatches"], 1)
        self.assertEqual(result.counts["verified_spawn_observations"], 1)

    def test_suppression_is_dispatch_but_not_successful_spawn(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="suppressed"),
                        row("SHOT_END", attempts=1, spawns=0))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["numbered_dispatches"], 1)
        self.assertEqual(result.counts["spawn_suppressed"], 1)
        self.assertEqual(result.counts["spawn_ok"], 0)
        self.assertEqual(result.counts["reported_spawn_successes"], 0)

    def test_absent_telemetry_cannot_pass(self):
        result = report("[RocketM1Diag] FIRE_SHOT_ENTER lft0=17.5", "Server rejected rapid fire")
        self.assertEqual(result.exit_code, 2)
        self.assertIn("NO_TELEMETRY", codes(result))

    def test_truncated_capture_reports_unresolved_accept(self):
        result = report(row("ACCEPT", source="DeferredEquip", deferred=1))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("ACCEPT_WITHOUT_DISPATCH", codes(result))

    def test_explicit_pending_remains_unresolved(self):
        result = report(row("ACCEPT"), row("PENDING"))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("ACCEPT_STILL_PENDING", codes(result))

    def test_lifecycle_cancellation_closes_reservation(self):
        result = report(row("ACCEPT", source="DeferredEquip"), row("CANCEL", reason="death"))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["accepted_events_lifecycle_canceled"], 1)

    def test_multiple_flak_spawns_are_one_dispatch(self):
        result = report(row("ACCEPT", weapon="BP_Flak_C_2"), row("SHOT", weapon="BP_Flak_C_2"),
                        *(row("SPAWN", weapon="BP_Flak_C_2", result="ok") for _ in range(9)),
                        row("SHOT_END", weapon="BP_Flak_C_2", attempts=9, spawns=9))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["numbered_dispatches"], 1)
        self.assertEqual(result.counts["spawn_ok"], 9)

    def test_unpermitted_shot_source_is_violation(self):
        result = report(row("SHOT", source="StateAutoNoContext", generation=0),
                        row("SHOT_END", source="StateAutoNoContext", generation=0, attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("UNPERMITTED_SHOT_SOURCE", codes(result))

    def test_generation_mismatch_is_violation(self):
        result = report(row("ACCEPT"), row("SHOT", generation=11),
                        row("SHOT_END", generation=11, attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("REQUEST_GENERATION_MISMATCH", codes(result))

    def test_actor_name_reuse_is_ambiguous_not_proven_duplicate(self):
        result = report(*(line for generation in (10, 20) for line in (
            row("ACCEPT", generation=generation), row("SHOT", generation=generation),
            row("SPAWN", generation=generation, result="ok"),
            row("SHOT_END", generation=generation, attempts=1, spawns=1))))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("EVENT_ID_REUSED", codes(result))
        self.assertNotIn("DUPLICATE_DISPATCH", codes(result))
        self.assertEqual(result.counts["accepted_events_dispatched"], 2)

    def test_repeated_stock_managed_dispatch_is_allowed(self):
        result = report(*(line for _ in range(2) for line in (
            row("SHOT", source="StockManaged", generation=0),
            row("SPAWN", source="StockManaged", generation=0, result="ok"),
            row("SHOT_END", source="StockManaged", generation=0, attempts=1, spawns=1))))
        self.assertEqual(result.exit_code, 0)
        self.assertNotIn("DUPLICATE_DISPATCH", codes(result))

    def test_files_do_not_share_event_identity(self):
        lines = [row("ACCEPT"), row("SHOT"), row("SPAWN", result="ok"), row("SHOT_END", attempts=1, spawns=1)]
        result = checker.analyze(checker.parse_records(lines, "one.log") + checker.parse_records(lines, "two.log"))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["accepted_events"], 2)

    def test_rocket_primary_scope_excludes_other_modes_and_weapons(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="ok"), row("SHOT_END", attempts=1, spawns=1),
                        row("SHOT", mode=1, source="Unknown"),
                        row("SHOT", weapon="BP_Shock_C_1", source="Unknown"), rocket_primary=True)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["numbered_dispatches"], 1)

    def test_unknown_rocket_scope_is_insufficient(self):
        result = report(row("SHOT", weapon="Weapon_C_1", source="Bot"), rocket_primary=True)
        self.assertEqual(result.exit_code, 2)
        self.assertIn("NO_ROCKET_PRIMARY_TELEMETRY", codes(result))

    def test_complete_spawn_results_contradict_success_counter(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="suppressed"),
                        row("SHOT_END", attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("SPAWN_COUNTER_MISMATCH", codes(result))

    def test_observed_success_exceeds_success_counter(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="ok"),
                        row("SHOT_END", attempts=1, spawns=0))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("SPAWN_COUNTER_MISMATCH", codes(result))

    def test_present_spawn_records_exceed_attempt_counter(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="null"),
                        row("SPAWN", result="null"), row("SHOT_END", attempts=1, spawns=0))
        self.assertEqual(result.exit_code, 1)
        self.assertIn("SPAWN_COUNTER_MISMATCH", codes(result))

    def test_absent_spawn_rows_leave_counters_unresolved(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SHOT_END", attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("MISSING_SPAWN_TELEMETRY", codes(result))

    def test_partial_spawn_rows_leave_counters_unresolved(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SPAWN", result="ok"),
                        row("SHOT_END", attempts=2, spawns=2))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("MISSING_SPAWN_TELEMETRY", codes(result))

    def test_no_instrumented_projectile_path_is_zero_attempts(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SHOT_END", attempts=0, spawns=0))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["verified_spawn_observations"], 1)

    def test_stock_managed_scopes_compare_separately(self):
        result = report(row("SHOT", source="StockManaged", generation=0),
                        row("SPAWN", source="StockManaged", generation=0, result="ok"),
                        row("SHOT_END", source="StockManaged", generation=0, attempts=1, spawns=0),
                        row("SHOT", source="StockManaged", generation=0),
                        row("SPAWN", source="StockManaged", generation=0, result="null"),
                        row("SHOT_END", source="StockManaged", generation=0, attempts=1, spawns=1))
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(sum(item["code"] == "SPAWN_COUNTER_MISMATCH" for item in result.findings), 2)

    def test_charged_direct_outcome_does_not_require_or_count_dispatch(self):
        result = report(row("SPAWN", source="ChargedDirect", mode=1, event=-1, generation=0, result="ok"))
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.counts["permitted_direct_outcomes"], 1)
        self.assertEqual(result.counts["numbered_dispatches"], 0)
        self.assertEqual(result.counts["dispatches"], 0)
        self.assertNotIn("SPAWN_WITHOUT_SHOT", codes(result))

    def test_unscoped_projectile_outcome_remains_unresolved(self):
        result = report(row("SPAWN", source="UnscopedProjectile", event=-1, generation=0, result="ok"))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("SPAWN_WITHOUT_SHOT", codes(result))

    def test_spawn_after_completed_scope_is_unresolved(self):
        result = report(row("ACCEPT"), row("SHOT"), row("SHOT_END", attempts=0, spawns=0),
                        row("SPAWN", result="ok"))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("SPAWN_OUTSIDE_SHOT", codes(result))

    def test_charged_direct_label_does_not_exempt_primary(self):
        result = report(row("SPAWN", source="ChargedDirect", mode=0, event=-1, generation=0, result="ok"))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("SPAWN_WITHOUT_SHOT", codes(result))


if __name__ == "__main__":
    unittest.main()
