"""Tests of a bounded source-semantics model, not of compiled Unreal gameplay."""

import importlib.util
from pathlib import Path
import sys
import unittest


MODEL_PATH = Path(__file__).resolve().parents[1] / "experiments" / "stop_identity_model.py"
SPEC = importlib.util.spec_from_file_location("stop_identity_model", MODEL_PATH)
model = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = model
SPEC.loader.exec_module(model)

RPC = model.RPC
Policy = model.Policy


class ReliableTransportTests(unittest.TestCase):
    def test_unreliable_stop_overtakes_buffered_reliable_original(self):
        channel = model.ReliableChannels()
        start = RPC("start", "start", 1, 1)
        stop = RPC("stop", "stop", 1, 2)
        copy = RPC("stop_copy", "stop", 1)
        self.assertEqual(channel.receive(stop), ())
        self.assertEqual(channel.receive(copy), (copy,))
        self.assertEqual(channel.receive(start), (start, stop))
        self.assertEqual(channel.receive(start), ())
        self.assertEqual(channel.receive(stop), ())

    def test_reliable_order_is_per_channel_not_global(self):
        channel = model.ReliableChannels()
        old_late = RPC("old_stop", "stop", 8, 2, "old_weapon")
        new_start = RPC("new_start", "start", 1, 1, "new_weapon")
        self.assertEqual(channel.receive(old_late), ())
        self.assertEqual(channel.receive(new_start), (new_start,))

    def test_engine_retransmission_duplicate_is_not_a_second_release(self):
        release = RPC("release", "stop", 0, 1)
        server = model.run((release, release, release), Policy.ORIGINAL_ONLY)
        self.assertEqual(server.state().stop_mutations, ["release"])


class StopPolicyTests(unittest.TestCase):
    def test_equal_id_distinct_reliable_releases_are_both_observed(self):
        first = RPC("release_a", "stop", 0, 1)
        second = RPC("release_b", "stop", 0, 2)
        for policy, expected in ((Policy.CURRENT, False), (Policy.ORIGINAL_ONLY, True)):
            server = model.FireServer(policy)
            server.dispatch(first)
            # Independent upstream pending mutation between two no-shot releases.
            # This tests identity only, not whether that upstream mutation is legal.
            server.state().pending = True
            server.dispatch(second)
            self.assertEqual(server.state().pending, not expected)
            self.assertEqual(len(server.state().stop_mutations), 2 if expected else 1)

    def test_retry_first_then_original_mutates_once_at_original(self):
        events = (
            RPC("retry_a", "stop", 0),
            RPC("retry_b", "stop", 0),
            RPC("original", "stop", 0, 1),
        )
        candidate = model.run(events, Policy.ORIGINAL_ONLY)
        current = model.run(events, Policy.CURRENT)
        self.assertEqual(candidate.state().stop_mutations, ["original"])
        self.assertEqual(current.state().stop_mutations, ["retry_a"])

    def test_old_stop_cannot_clear_newer_accepted_start(self):
        server = model.FireServer(Policy.ORIGINAL_ONLY)
        server.dispatch(RPC("new_start_copy", "start", 2))
        old = server.dispatch(RPC("old_original", "stop", 1, 1))
        self.assertEqual(old.result, "stop_older_than_authoritative")
        self.assertTrue(server.state().pending)
        self.assertEqual(server.state().authoritative, 2)

    def test_current_retry_stop_retires_never_processed_start(self):
        order = (
            RPC("stop_copy", "stop", 1),
            RPC("start_original", "start", 1, 1),
            RPC("stop_original", "stop", 1, 2),
        )
        current = model.run(order, Policy.CURRENT)
        candidate = model.run(order, Policy.ORIGINAL_ONLY)
        self.assertEqual(current.state().accepted_starts, [])
        self.assertEqual(current.state().retired_unprocessed_starts, [1])
        self.assertEqual(candidate.state().accepted_starts, [1])
        self.assertEqual(candidate.state().retired_unprocessed_starts, [])
        self.assertEqual(candidate.state().stop_mutations, ["stop_original"])

    def test_every_bounded_one_shot_order_including_copy_loss(self):
        count = 0
        current_failures = 0
        for order in model.delivery_orders(model.one_shot_events()):
            count += 1
            candidate = model.run(order, Policy.ORIGINAL_ONLY).state()
            self.assertEqual(candidate.accepted_starts, [1], order)
            self.assertEqual(candidate.stop_mutations, ["stop_original"], order)
            self.assertFalse(candidate.pending, order)
            self.assertEqual(candidate.retired_unprocessed_starts, [], order)
            if not model.run(order, Policy.CURRENT).state().accepted_starts:
                current_failures += 1
        # Two mandatory ordered originals and zero to four distinguishable copies.
        self.assertEqual(count, 685)
        self.assertGreater(current_failures, 0)

    def test_every_bounded_two_shot_order_does_not_claim_start_reordering_fixed(self):
        count = 0
        candidate_missing = 0
        for order in model.delivery_orders(model.two_shot_events()):
            count += 1
            state = model.run(order, Policy.ORIGINAL_ONLY).state()
            self.assertEqual(state.retired_unprocessed_starts, [], order)
            self.assertFalse(state.pending, order)
            self.assertEqual(len(state.accepted_starts), len(set(state.accepted_starts)))
            if 1 not in state.accepted_starts:
                candidate_missing += 1
        self.assertEqual(count, 2721)
        self.assertGreater(candidate_missing, 0)

    def test_newer_start_retry_can_still_overtake_older_start(self):
        order = (
            RPC("new_start_copy", "start", 2),
            RPC("start_1", "start", 1, 1),
            RPC("stop_1", "stop", 1, 2),
            RPC("start_2", "start", 2, 3),
            RPC("stop_2", "stop", 2, 4),
        )
        state = model.run(order, Policy.ORIGINAL_ONLY).state()
        self.assertEqual(state.accepted_starts, [2])
        self.assertEqual(state.retired_unprocessed_starts, [])

    def test_stock_managed_stop_semantics_remain_unchanged(self):
        events = (
            RPC("charged_copy", "stop", 1, mode=1),
            RPC("charged_original", "stop", 1, 1, mode=1),
        )
        current = model.run(events, Policy.CURRENT)
        candidate = model.run(events, Policy.ORIGINAL_ONLY)
        self.assertEqual(current.rows, candidate.rows)
        self.assertEqual(candidate.state(("weapon", 1)).stop_mutations, ["charged_copy"])

    def test_no_new_client_counter_semantics_are_required(self):
        old_client = (
            RPC("start_1", "start", 1, 1),
            RPC("release_a", "stop", 1, 2),
            RPC("release_b", "stop", 1, 3),
            RPC("start_2", "start", 2, 4),
        )
        candidate = model.run(old_client, Policy.ORIGINAL_ONLY).state()
        self.assertEqual(candidate.accepted_starts, [1, 2])
        self.assertEqual(candidate.stop_mutations, ["release_a", "release_b"])


class BoundsAndStockCounterTests(unittest.TestCase):
    def test_start_and_stop_lookahead_and_equality(self):
        for kind in ("start", "stop"):
            for event_id, accepted in ((99, False), (100, kind == "stop"), (110, True), (111, False)):
                server = model.FireServer(Policy.ORIGINAL_ONLY)
                server.state().authoritative = 100
                row = server.dispatch(RPC("attempt", kind, event_id, 1))
                self.assertEqual(row.result in ("start_accept", "stop_process"), accepted)

    def test_source_int32_overflow_is_explicitly_not_proven(self):
        server = model.FireServer(Policy.ORIGINAL_ONLY)
        server.state().authoritative = model.INT32_MAX - 5
        with self.assertRaisesRegex(ValueError, "unmodeled"):
            server.dispatch(RPC("near_overflow", "start", model.INT32_MAX, 1))
        self.assertEqual(
            server.dispatch(RPC("negative_wrap", "stop", -(1 << 31), 2)).result,
            "payload_reject",
        )

    def test_counter_increment_breaks_next_charged_start_on_old_server(self):
        old = model.counter_increment_stock_trace(False)
        incremented = model.counter_increment_stock_trace(True)
        self.assertEqual(old["fixed_shots"], [1, 2, 3])
        self.assertEqual(incremented["fixed_shots"], [1, 3, 5])
        self.assertTrue(old["stock_start_accepted"])
        self.assertFalse(incremented["stock_start_accepted"])
        self.assertEqual(incremented["server_stock"], incremented["next_client_stock_start"])

    def test_stock_byte_wrap_and_sentinel_are_a_separate_policy(self):
        self.assertTrue(model.stock_accepts(254, 0))
        self.assertTrue(model.stock_accepts(19, 255))
        self.assertFalse(model.stock_accepts(19, 19))
        self.assertFalse(model.stock_accepts(19, 18))


if __name__ == "__main__":
    unittest.main()
