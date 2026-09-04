"""Executable counterexamples for pawn-global fire ownership.

This is a small source-semantics model, not an Unreal network/runtime test.
Switch means completed WeaponChanged/BringUp, not merely receipt of the switch
RPC. Each action is assumed otherwise valid. Arbitrary cross-channel delivery
is allowed; reliable delivery within a channel remains ordered. It deliberately
omits state callbacks, timers, packet encoding, and projectile creation.
"""

from dataclasses import dataclass, field
from itertools import permutations
import json


@dataclass(frozen=True)
class Message:
    name: str
    channel: str
    sequence: int
    kind: str
    weapon: str
    event: int = 0


SWITCH_B = Message("switch_B", "pawn", 1, "switch", "B")
START_B = Message("start_B_20", "weapon_B", 1, "fixed_start", "B", 20)
STOP_A = Message("stop_A_10", "weapon_A", 1, "fixed_stop", "A", 10)
SWITCH_A = Message("switch_A", "pawn", 2, "switch", "A")


def legal_deliveries(messages):
    """Enumerate ordered reliable channel merges, excluding duplicated inputs."""
    for order in permutations(messages):
        last = {}
        for message in order:
            if message.sequence <= last.get(message.channel, 0):
                break
            last[message.channel] = message.sequence
        else:
            yield order


def legal_prefix(prefix, sent):
    """True when prefix can precede some complete legal channel merge."""
    return any(order[: len(prefix)] == tuple(prefix) for order in legal_deliveries(sent))


@dataclass
class Server:
    """One pawn/mode, persistent weapon instances, low nonwrapping IDs.

    legacy: fixed and stock releases clear the shared pawn bit.
    fixed_guard: ignore off-current fixed release into a transactional mode.
    scoped_candidate: also guard stock Begin/End writes, and start a fresh
      transactional pending lifetime on completed equip. This policy models a
      possible semantic change; it is NOT implemented in the plugin. It retains
      legacy fixed Stop deduplication; original-only identity is a separate model.
    """

    policy: str = "legacy"
    current: str = "A"
    pending: bool = False
    transactional: dict = field(default_factory=lambda: {"A": True, "B": True})
    auth: dict = field(default_factory=lambda: {"A": 10, "B": 19})
    last_stop: dict = field(default_factory=lambda: {"A": 9, "B": 18})
    applied: list = field(default_factory=list)

    def off_current_into_transactional(self, message):
        return message.weapon != self.current and self.transactional[self.current]

    def receive(self, message):
        self.applied.append(message.name)
        if message.kind == "switch":
            self.current = message.weapon
            if self.policy == "scoped_candidate" and self.transactional[self.current]:
                self.pending = False
            return
        if message.kind == "fixed_start":
            # Only accept a request for the current weapon in this bounded model.
            if message.weapon != self.current:
                return
            if not self.auth[message.weapon] < message.event <= self.auth[message.weapon] + 10:
                return
            self.auth[message.weapon] = message.event
            self.pending = True
            return
        if message.kind == "fixed_stop":
            if message.event <= self.last_stop[message.weapon]:
                return
            if not self.auth[message.weapon] <= message.event <= self.auth[message.weapon] + 10:
                return
            self.auth[message.weapon] = message.event
            self.last_stop[message.weapon] = message.event
            if self.policy in {"fixed_guard", "scoped_candidate"} and self.off_current_into_transactional(message):
                return
            self.pending = False
            return
        if message.kind in {"stock_start", "stock_stop"}:
            # Assume stock's separate byte-sequence check accepted this RPC.
            if self.policy == "scoped_candidate" and self.off_current_into_transactional(message):
                return
            self.pending = message.kind == "stock_start"
            return
        raise ValueError(message.kind)

    def snapshot(self):
        return self.current, self.pending, tuple(sorted(self.auth.items())), tuple(sorted(self.last_stop.items()))


def run(messages, policy="legacy", **kwargs):
    server = Server(policy=policy, **kwargs)
    for message in messages:
        server.receive(message)
    return server


def indistinguishable_prefix():
    """Two physical histories with the same information available to the server.

    stale: release A; switch to B; press/fire B and keep holding.
    recent: switch to B while held; predict/fire B; switch back to A while held.
      A becomes current locally but is still equipping; release before its first
      new shot. The second pawn-channel switch is still in flight. Releasing
      before A became current would instead address B and is not this history.

    Other messages/callbacks may exist and remain in flight; there is no claimed
    equality after all reliable traffic has drained. A's fixed watermark can
    stay 10 across the no-shot switch back. Both prefixes respect channel order.
    """
    stale_sent = (STOP_A, SWITCH_B, START_B)
    recent_sent = (SWITCH_B, START_B, SWITCH_A, STOP_A)
    prefix = (SWITCH_B, START_B, STOP_A)
    assert legal_prefix(prefix, stale_sent)
    assert legal_prefix(prefix, recent_sent)
    return {
        "prefix": [message.name for message in prefix],
        "stale_release_desired_physical_pending": True,
        "recent_release_desired_physical_pending": False,
        "legacy_pending": run(prefix, pending=True).pending,
        "scoped_candidate_pending": run(prefix, "scoped_candidate", pending=True).pending,
        "recent_after_delayed_switch_pending": run(prefix + (SWITCH_A,), "scoped_candidate", pending=True).pending,
    }


def report():
    stock_stop = Message("stock_stop_A", "weapon_A", 1, "stock_stop", "A", 12)
    counterexample = (SWITCH_B, START_B, stock_stop)
    orders = list(legal_deliveries((SWITCH_B, START_B, STOP_A)))
    eligible = [order for order in orders if order.index(SWITCH_B) < order.index(START_B)]
    switch_c = Message("switch_C", "pawn", 2, "switch", "C")
    held_switches = (SWITCH_B, switch_c)
    held_config = {"pending": True, "transactional": {"A": True, "B": True, "C": False}}
    return {
        "scope": "source-semantics counterexamples; no engine or C++ execution",
        "cross_channel_orders": len(orders),
        "orders_with_B_equipped_before_B_start": len(eligible),
        "legacy_final_B_latch_losses": sum(not run(order).pending for order in eligible),
        "scoped_candidate_final_B_latch_losses": sum(not run(order, "scoped_candidate").pending for order in eligible),
        "stock_stop_bypasses_fixed_only_guard": not run(counterexample, "fixed_guard").pending,
        "held_through_transactional_B_to_stock_C": {
            "legacy_pending": run(held_switches, **held_config).pending,
            "scoped_candidate_pending": run(held_switches, "scoped_candidate", **held_config).pending,
            "caveat": "No B request accepted before switching to C; candidate loses inherited hold until stock sync.",
        },
        "physical_ownership_ambiguity": indistinguishable_prefix(),
    }


if __name__ == "__main__":
    print(json.dumps(report(), indent=2))
