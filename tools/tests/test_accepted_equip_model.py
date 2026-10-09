"""Tests of the equip ordering model, not compiled Unreal gameplay."""

import importlib.util
from pathlib import Path
import sys
import unittest


MODEL_PATH = Path(__file__).resolve().parents[1] / "experiments" / "accepted_equip_model.py"
SPEC = importlib.util.spec_from_file_location("accepted_equip_model", MODEL_PATH)
model = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = model
SPEC.loader.exec_module(model)
Policy = model.Policy


class AcceptedEquipBoundaryTests(unittest.TestCase):
    def test_own_release_and_competing_charged_reproduces_lost_request(self):
        case = model.Scenario("charged", (0.0, 0.0, 0.0), ("stock_hold", "own_release", "turn_view"))
        old = model.run(case, Policy.ACTIVE_FIRST)
        candidate = model.run(case, Policy.ACCEPTED_EQUIP_FIRST)
        self.assertEqual(old.shots, [])
        self.assertIsNone(old.request)
        self.assertEqual(old.firing_slot, 0)
        self.assertIn("queue_primary_in_charged", old.trace)
        self.assertEqual(len(candidate.shots), 1)
        self.assertEqual(candidate.shots[0].event, 17)
        self.assertEqual(candidate.shots[0].aim, case.aim)
        self.assertNotEqual(candidate.shots[0].aim, candidate.server_aim)
        self.assertEqual(candidate.release_cleanup_count, 1)
        self.assertFalse(candidate.pending[0])
        self.assertTrue(candidate.pending[1])

    def test_outgoing_stop_does_not_acquire_release_ownership(self):
        case = model.Scenario("charged", (12.0, 83.0, -9.0), ("outgoing_stop", "turn_view", "stock_hold"))
        old = model.run(case, Policy.ACTIVE_FIRST)
        candidate = model.run(case, Policy.ACCEPTED_EQUIP_FIRST)
        self.assertEqual(old.shots, [])
        # Old stock replay manufactures a held primary even though outgoing Stop
        # did not mark this retained request as physically released.
        self.assertTrue(old.pending[0])
        self.assertEqual(len(candidate.shots), 1)
        self.assertFalse(candidate.pending[0])
        self.assertTrue(candidate.pending[1])
        self.assertEqual(candidate.release_cleanup_count, 0)

    def test_zoom_replay_already_dispatches_in_old_patch(self):
        case = model.Scenario("zoom", (0.0, 0.0, 0.0), ("stock_hold", "own_release"))
        old = model.run(case, Policy.ACTIVE_FIRST)
        candidate = model.run(case, Policy.ACCEPTED_EQUIP_FIRST)
        self.assertEqual(len(old.shots), 1)
        self.assertIn("zoom_immediate_mode_change", old.trace)
        self.assertEqual(old.shots, candidate.shots)
        self.assertTrue(candidate.pending[1])
        self.assertEqual(candidate.replay_count, 0)

    def test_no_competing_stock_mode_old_patch_dispatches_released_primary(self):
        case = model.Scenario("none", (8.0, 7.0, 6.0), ("own_release",))
        self.assertEqual(len(model.run(case, Policy.ACTIVE_FIRST).shots), 1)
        self.assertEqual(len(model.run(case, Policy.ACCEPTED_EQUIP_FIRST).shots), 1)

    def test_cleared_equip_slot_prevents_replay_and_late_reuse(self):
        weapon = model.Weapon(Policy.ACCEPTED_EQUIP_FIRST)
        weapon.bring_up_finished()
        self.assertEqual(weapon.priority_count, 1)
        self.assertEqual(weapon.replay_count, 0)
        self.assertIsNone(weapon.request)
        self.assertIsNone(weapon.equip_slot)
        self.assertIsNone(weapon.firing_slot)
        weapon.bring_up_finished()
        weapon.attempt_late_primary()
        self.assertEqual(len(weapon.shots), 1)

    def test_nontransactional_target_uses_existing_fallback(self):
        results = []
        for policy in Policy:
            weapon = model.Weapon(policy)
            weapon.request.target_transactional = False
            weapon.apply("stock_hold")
            weapon.apply("own_release")
            weapon.bring_up_finished()
            self.assertEqual(weapon.priority_count, 0)
            results.append((weapon.trace, weapon.shots, weapon.pending))
        self.assertEqual(*results)

    def test_dedicated_released_primary_preserves_stock_replay_origin(self):
        case = model.Scenario("none", (0.0, 0.0, 0.0), ("own_release",))
        old = model.run(case, Policy.ACTIVE_FIRST)
        candidate = model.run(case, Policy.ACCEPTED_EQUIP_FIRST)
        self.assertTrue(old.shots[0].delayed_origin)
        self.assertTrue(candidate.shots[0].delayed_origin)
        self.assertFalse(candidate.delayed_origin)

    def test_held_primary_preserves_prior_origin_for_either_net_mode(self):
        for net_mode in ("dedicated", "listen"):
            for previous in (False, True):
                for policy in Policy:
                    weapon = model.Weapon(policy, net_mode=net_mode, delayed_origin=previous)
                    weapon.bring_up_finished()
                    self.assertEqual(weapon.shots[0].delayed_origin, previous)

    def test_listen_remote_released_primary_uses_stock_false_origin(self):
        for policy in Policy:
            weapon = model.Weapon(policy, net_mode="listen", delayed_origin=True)
            weapon.apply("own_release")
            weapon.bring_up_finished()
            self.assertFalse(weapon.shots[0].delayed_origin)
            if policy is Policy.ACCEPTED_EQUIP_FIRST:
                self.assertTrue(weapon.delayed_origin)

    def test_every_bounded_before_completion_order_retains_exact_payload(self):
        count = old_lost = 0
        for case in model.bounded_scenarios():
            count += 1
            old = model.run(case, Policy.ACTIVE_FIRST)
            candidate = model.run(case, Policy.ACCEPTED_EQUIP_FIRST)
            old_lost += int(not old.shots)
            self.assertEqual(len(candidate.shots), 1, case)
            shot = candidate.shots[0]
            self.assertEqual((shot.event, shot.aim, shot.owner, shot.generation, shot.mode),
                             (17, case.aim, "pawn", 9, 0), case)
            self.assertEqual(candidate.replay_count, 0, case)
            self.assertIsNone(candidate.equip_slot, case)
            self.assertEqual(candidate.pending[1], case.competing != "none", case)
            self.assertEqual(candidate.release_cleanup_count,
                             int("own_release" in case.operations), case)
        self.assertEqual(count, 66)
        self.assertEqual(old_lost, 24)


class InvalidatedEquipTests(unittest.TestCase):
    def assert_cannot_commit(self, mutate):
        weapon = model.Weapon(Policy.ACCEPTED_EQUIP_FIRST)
        weapon.apply("stock_hold")
        mutate(weapon)
        weapon.bring_up_finished()
        weapon.attempt_late_primary()
        self.assertEqual(weapon.shots, [])
        self.assertEqual(weapon.priority_count, 0)

    def test_stale_generation_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "generation", 10))

    def test_changed_owner_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "owner", "other_pawn"))

    def test_switched_away_or_pending_other_weapon_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "current_weapon", "C"))
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "pending_weapon", "C"))

    def test_dead_owner_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "alive", False))

    def test_wrong_waiting_state_or_target_or_queue_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon.request, "waiting_state", "charged"))
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "primary_state", "replacement"))
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "equip_slot", 1))

    def test_stale_completion_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: weapon.leave_equip())

    def test_no_retained_request_cannot_commit(self):
        self.assert_cannot_commit(lambda weapon: setattr(weapon, "request", None))

    def test_commit_policy_failure_cancels_without_dispatch(self):
        for name, value in (("ammo", False), ("fire_disabled", True),
                            ("mode_allowed", False), ("game_prevents_fire", True)):
            weapon = model.Weapon(Policy.ACCEPTED_EQUIP_FIRST)
            setattr(weapon, name, value)
            weapon.bring_up_finished()
            self.assertEqual(weapon.shots, [], name)
            self.assertIsNone(weapon.request, name)
            self.assertIsNone(weapon.equip_slot, name)
            self.assertEqual(weapon.replay_count, 0, name)


if __name__ == "__main__":
    unittest.main()
