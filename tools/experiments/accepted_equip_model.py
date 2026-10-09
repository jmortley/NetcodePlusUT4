"""Bounded source-semantics model of an accepted request at equip completion.

This is not an Unreal runtime proof. It isolates the ordering in
UTWeaponStateEquipping::BringUpFinished: GotoActiveState scans pawn pending bits
before stock replays PendingFireSequence. A competing charged state only queues
the primary replay; a zoom state immediately changes fire mode. The first-turn
NetcodePlus replay wrapper clears the retained request after either attempt.

ACTIVE_FIRST models that first-turn patch, not unmodified stock UT or baseline
328. ACCEPTED_EQUIP_FIRST models the candidate priority for a valid accepted
transactional equip request before the ActiveState scan. It clears the matching
equip slot after direct dispatch so BringUpFinished cannot replay the request.

Every scenario begins after primary acceptance, with an exact event, aim,
owner/generation, and matching stock equip slot. Timing, packets, ACKs, ammo
consumption, charged rockets, zoom side effects, Blueprint callbacks, projectile
spawning, and the next-tick release callback are not simulated. A recorded shot
means one authorized FireShot dispatch, not a proven projectile spawn. The
delayed_origin field records the bNetDelayedShot selector, not a calculated
origin vector. Synchronous owner/generation changes during dispatch are outside
the model; source restoration must independently guard those lifetimes. The
outgoing Stop operation models only its existing pawn-global pending-bit side
effect; it deliberately does not invent release ownership for this weapon.

Source anchors: Source/Private/UTWeaponFix.cpp methods GotoState,
CompleteAcceptedDeferredFire, ClearDeferredEquipFireContext,
BeginFiringSequence, HasAcceptedTransactionalRequest, and FireShot. Stock
anchors are UTWeaponStateEquipping.cpp, UTWeaponStateActive.cpp,
UTWeaponStateFiring.cpp, and UTWeaponStateZooming.cpp.
"""

from dataclasses import dataclass, field
from enum import Enum
from itertools import permutations
from typing import Iterator, List, Optional, Tuple


Aim = Tuple[float, float, float]


class Policy(Enum):
    ACTIVE_FIRST = "active_first"
    ACCEPTED_EQUIP_FIRST = "accepted_equip_first"


@dataclass
class AcceptedRequest:
    event: int = 17
    aim: Aim = (12.0, 83.0, -9.0)
    mode: int = 0
    owner: str = "pawn"
    generation: int = 9
    waiting_state: str = "equipping"
    expected_state: str = "primary"
    deferred_equip: bool = True
    target_transactional: bool = True
    release_seen: bool = False
    dispatched: bool = False


@dataclass(frozen=True)
class Shot:
    event: int
    aim: Aim
    owner: str
    generation: int
    mode: int
    delayed_origin: bool


@dataclass
class Weapon:
    policy: Policy
    request: Optional[AcceptedRequest] = field(default_factory=AcceptedRequest)
    owner: str = "pawn"
    generation: int = 9
    weapon: str = "B"
    current_weapon: str = "B"
    pending_weapon: Optional[str] = None
    alive: bool = True
    remote_authority: bool = True
    ammo: bool = True
    fire_disabled: bool = False
    mode_allowed: bool = True
    game_prevents_fire: bool = False
    net_mode: str = "dedicated"
    delayed_origin: bool = False
    state: str = "equipping"
    primary_state: str = "primary"
    competing_state: str = "charged"
    pending: List[bool] = field(default_factory=lambda: [True, False])
    equip_slot: Optional[int] = 0
    firing_slot: Optional[int] = None
    server_aim: Aim = (12.0, 83.0, -9.0)
    shots: List[Shot] = field(default_factory=list)
    trace: List[str] = field(default_factory=list)
    replay_count: int = 0
    release_cleanup_count: int = 0
    priority_count: int = 0

    def apply(self, operation: str) -> None:
        if operation == "own_release":
            self.pending[0] = False
            if self.request is not None and self.request.owner == self.owner:
                self.request.release_seen = True
        elif operation == "outgoing_stop":
            self.pending[0] = False
        elif operation == "stock_hold":
            self.pending[1] = True
        elif operation == "turn_view":
            self.server_aim = (-32.0, -140.0, 5.0)
        else:
            raise ValueError("unknown operation: " + operation)
        self.trace.append(operation)

    def leave_equip(self, next_state: str = "unequipping") -> None:
        """Normal lifecycle transition invalidates the request and stock slot."""
        self._clear_request()
        self.generation += 1
        self.state = next_state

    def _identity_valid(self) -> bool:
        request = self.request
        return bool(
            request is not None
            and request.generation == self.generation
            and request.owner == self.owner
            and self.alive
            and self.current_weapon == self.weapon
            and self.pending_weapon in (None, self.weapon)
            and request.expected_state == self.primary_state
            and request.mode == 0
            and request.deferred_equip
            and request.waiting_state == "equipping"
            and self.equip_slot == request.mode
        )

    def _fire_policy_valid(self) -> bool:
        return (
            self.ammo and not self.fire_disabled and self.mode_allowed
            and not self.game_prevents_fire
        )

    def _dispatch_primary(self) -> bool:
        request = self.request
        if not self._identity_valid() or not self._fire_policy_valid():
            self.trace.append("primary_blocked")
            return False
        assert request is not None
        if request.dispatched:
            self.trace.append("primary_already_consumed")
            return False
        self.state = self.primary_state
        request.dispatched = True
        self.shots.append(Shot(
            request.event, request.aim, request.owner, request.generation,
            request.mode, self.delayed_origin,
        ))
        self.trace.append("dispatch_exact_primary")
        return True

    def _clear_request(self) -> None:
        request = self.request
        if request is not None and request.deferred_equip and self.equip_slot == request.mode:
            self.equip_slot = None
            self.trace.append("clear_matching_equip_slot")
        self.request = None

    def _finish_attempt(self) -> None:
        request = self.request
        if request is None:
            return
        released, dispatched = request.release_seen, request.dispatched
        self._clear_request()
        if released:
            self.pending[0] = False
            if dispatched:
                self.release_cleanup_count += 1
                self.trace.append("schedule_owned_release_cleanup")

    def _complete_accepted_equip(self) -> bool:
        request = self.request
        if not (
            self.remote_authority and self.state == "equipping"
            and request is not None and request.target_transactional
            and not request.dispatched and self._identity_valid()
            and self.pending_weapon is None
        ):
            return False
        if not self._fire_policy_valid():
            self._clear_request()
            self.trace.append("cancel_commit_policy")
            return False
        self.priority_count += 1
        self.trace.append("prioritize_accepted_equip")
        previous_delayed = self.delayed_origin
        if not self.pending[0]:
            # Without a held primary bit, stock would dispatch during its
            # explicit replay, which sets bNetDelayedShot from net mode.
            self.delayed_origin = self.net_mode == "dedicated"
        self._dispatch_primary()
        self._finish_attempt()
        # No reentrant owner/generation mutation is modeled in this synchronous
        # dispatch. The source restoration additionally guards those lifetimes.
        self.delayed_origin = previous_delayed
        return True

    def _active_scan(self) -> None:
        self.state = "active"
        self.trace.append("active_scan")
        if self.pending[0] and self._dispatch_primary():
            return
        if self.pending[1]:
            self.state = self.competing_state
            self.trace.append("enter_" + self.competing_state)

    def _stock_equip_replay(self) -> None:
        self.replay_count += 1
        self.trace.append("stock_equip_replay")
        self.delayed_origin = self.net_mode == "dedicated"
        # AUTWeapon::BeginFiringSequence re-latches the pawn bit before delegating.
        self.pending[0] = True
        if self.state in ("active", "zoom"):
            if self.state == "zoom":
                self.trace.append("zoom_immediate_mode_change")
            self._dispatch_primary()
        else:
            # Charged inherited BeginFiringSequence records a later state replay.
            self.firing_slot = 0
            self.trace.append("queue_primary_in_" + self.state)
        # The first-turn wrapper's one synchronous replay attempt is now over.
        if self._identity_valid():
            self._finish_attempt()
        self.delayed_origin = False

    def bring_up_finished(self) -> None:
        if self.state != "equipping":
            self.trace.append("stale_equip_completion_ignored")
            return
        completed = (
            self.policy is Policy.ACCEPTED_EQUIP_FIRST
            and self._complete_accepted_equip()
        )
        if not completed:
            self._active_scan()
        if self.equip_slot is not None:
            self._stock_equip_replay()
            self.equip_slot = None

    def attempt_late_primary(self) -> None:
        """A later timer/held-bit entry cannot reuse a removed accepted payload."""
        self._dispatch_primary()


@dataclass(frozen=True)
class Scenario:
    competing: str
    aim: Aim
    operations: Tuple[str, ...]


def bounded_scenarios() -> Iterator[Scenario]:
    """All orders of up to three pre-completion mutations, two captured aims.

    The 66 cases are an ordering cross-product, not runtime probability or a
    model of every weapon class. A competing mode is charged or zoom, explicitly
    preserving their different stock BeginFiringSequence behavior.
    """
    for aim in ((0.0, 0.0, 0.0), (12.0, 83.0, -9.0)):
        for competing in ("none", "charged", "zoom"):
            for release in (None, "own_release", "outgoing_stop"):
                operations = ["turn_view"]
                if competing != "none":
                    operations.append("stock_hold")
                if release:
                    operations.append(release)
                for order in permutations(operations):
                    yield Scenario(competing, aim, order)


def run(scenario: Scenario, policy: Policy) -> Weapon:
    weapon = Weapon(policy, AcceptedRequest(aim=scenario.aim))
    weapon.competing_state = scenario.competing
    for operation in scenario.operations:
        weapon.apply(operation)
    weapon.bring_up_finished()
    return weapon


def main() -> None:
    import json

    counts = {policy.value: {"dispatches": 0, "lost_requests": 0} for policy in Policy}
    cases = list(bounded_scenarios())
    for case in cases:
        for policy in Policy:
            weapon = run(case, policy)
            counts[policy.value]["dispatches"] += len(weapon.shots)
            counts[policy.value]["lost_requests"] += int(not weapon.shots)
    example = Scenario("charged", (0.0, 0.0, 0.0), ("stock_hold", "own_release", "turn_view"))
    print(json.dumps({
        "scope": "Source-semantics ordering model; not an Unreal runtime proof.",
        "cases": len(cases),
        "counts": counts,
        "charged_own_release_trace": {
            policy.value: run(example, policy).trace for policy in Policy
        },
    }, indent=2))


if __name__ == "__main__":
    main()
