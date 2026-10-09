"""Counterexamples for ownership policy; not production behavior tests."""

import importlib.util
from pathlib import Path
import sys
import unittest


MODEL_PATH = Path(__file__).resolve().parents[1] / "experiments" / "stop_ownership_model.py"
spec = importlib.util.spec_from_file_location("stop_ownership_model", MODEL_PATH)
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)


class StopOwnershipModelTests(unittest.TestCase):
    def test_cross_actor_channels_allow_old_release_after_new_start(self):
        order = (m.SWITCH_B, m.START_B, m.STOP_A)
        self.assertTrue(m.legal_prefix(order, (m.STOP_A, m.SWITCH_B, m.START_B)))
        self.assertFalse(m.run(order).pending)
        self.assertTrue(m.run(order, "fixed_guard").pending)

    def test_same_actor_reliable_order_is_preserved(self):
        for order in m.legal_deliveries((m.SWITCH_B, m.SWITCH_A, m.STOP_A)):
            self.assertLess(order.index(m.SWITCH_B), order.index(m.SWITCH_A))

    def test_all_bounded_orders_preserve_new_transactional_latch_in_candidate(self):
        result = m.report()
        self.assertEqual(result["cross_channel_orders"], 6)
        self.assertEqual(result["orders_with_B_equipped_before_B_start"], 3)
        self.assertEqual(result["legacy_final_B_latch_losses"], 1)
        self.assertEqual(result["scoped_candidate_final_B_latch_losses"], 0)

    def test_stock_stop_bypasses_fixed_only_guard(self):
        stop = m.Message("stock_A", "weapon_A", 1, "stock_stop", "A", 12)
        order = (m.SWITCH_B, m.START_B, stop)
        self.assertFalse(m.run(order, "fixed_guard").pending)
        self.assertTrue(m.run(order, "scoped_candidate").pending)

    def test_stock_start_can_reintroduce_an_unowned_latch(self):
        start = m.Message("stock_A", "weapon_A", 1, "stock_start", "A", 12)
        self.assertTrue(m.run((m.SWITCH_B, start), "fixed_guard").pending)
        self.assertFalse(m.run((m.SWITCH_B, start), "scoped_candidate").pending)

    def test_current_weapon_guard_alone_leaves_inherited_latch(self):
        self.assertTrue(m.run((m.SWITCH_B, m.STOP_A), "fixed_guard", pending=True).pending)
        self.assertFalse(m.run((m.SWITCH_B, m.STOP_A), "scoped_candidate", pending=True).pending)

    def test_no_blanket_fresh_lifetime_for_stock_managed_incoming_mode(self):
        server = m.run((m.SWITCH_B,), "scoped_candidate", pending=True,
                       transactional={"A": True, "B": False})
        self.assertTrue(server.pending)
        server.receive(m.STOP_A)
        self.assertFalse(server.pending)

    def test_same_received_prefix_can_require_opposite_physical_answers(self):
        result = m.indistinguishable_prefix()
        self.assertNotEqual(result["stale_release_desired_physical_pending"],
                            result["recent_release_desired_physical_pending"])
        self.assertFalse(result["legacy_pending"])
        self.assertTrue(result["scoped_candidate_pending"])

    def test_fresh_transactional_lifetime_loses_hold_through_next_stock_weapon(self):
        switch_c = m.Message("switch_C", "pawn", 2, "switch", "C")
        config = {"pending": True, "transactional": {"A": True, "B": True, "C": False}}
        order = (m.SWITCH_B, switch_c)
        self.assertTrue(m.run(order, **config).pending)
        server = m.run(order, "scoped_candidate", **config)
        self.assertFalse(server.pending)
        # No accepted B request restored the bit. C's later stock reconciliation
        # can repair it, but that is a behavioral latency change, not equivalence.
        sync = m.Message("stock_sync_C_held", "weapon_C", 1, "stock_start", "C", 255)
        server.receive(sync)
        self.assertTrue(server.pending)

    def test_physical_ambiguity_is_not_claimed_after_switch_traffic_drains(self):
        self.assertFalse(m.indistinguishable_prefix()["recent_after_delayed_switch_pending"])

    def test_switch_back_then_new_numbered_hold_rejects_old_stop(self):
        start_a = m.Message("start_A_11", "weapon_A", 2, "fixed_start", "A", 11)
        # Inject an already-accepted unreliable Start retry as a state event.
        # This direct run intentionally does not use legal_deliveries: sequence 2
        # before 1 would be illegal for two reliable originals. The guard tested
        # here is the higher authoritative watermark, not arrival age.
        server = m.run((m.SWITCH_B, m.START_B, m.SWITCH_A, start_a, m.STOP_A), "scoped_candidate")
        self.assertTrue(server.pending)
        self.assertEqual(server.auth["A"], 11)


if __name__ == "__main__":
    unittest.main()
