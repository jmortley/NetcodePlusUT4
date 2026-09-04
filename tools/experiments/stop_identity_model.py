"""Bounded model of 328 fixed Stop identity and its two RPC transports.

This is a source-semantics model, not an Unreal simulation or runtime proof.
Original fixed Start/Stop calls use one reliable actor channel. Application
retry copies are unreliable and can overtake originals. The model assumes a
live owner/current weapon, legal shot timing, and a configured transactional
mode; cross-weapon pending ownership and retained aim are deliberately absent.

CURRENT models the existing <= last Stop deduplication and Stop advancement of
the shared authoritative event watermark. ORIGINAL_ONLY models the candidate:
transactional Stop copies do nothing, while each engine-delivered reliable
original may reuse the last Stop ID but still obeys the authoritative watermark
and +10 lookahead. Charged/stock-managed fixed Stops retain CURRENT semantics.

Reliable sequencing is modeled separately from gameplay. Engine retransmission
duplicates never become another original invocation. Loss in the enumerator
means optional unreliable copies are absent; reliable originals eventually
arrive. Delivery latency, disconnect, packet serialization, C++ signed overflow,
ammo/policy checks, and Unreal state transitions are not modeled.

The relative source anchors are Source/Private/UTWeaponFix.cpp functions
IsFireEventSequenceValid, ValidateFireRequest, ServerStopFireFixed_Implementation,
and ResendServerStopFireFixed_Implementation; their declarations in
Source/Public/UTWeaponFix.h distinguish original and retry reliability. Engine
NetConnection.cpp/UNetConnection::ReceivedPacket and
DataChannel.cpp/UChannel::ReceivedRawBunch enforce the separate channel-order
assumption. Counts are counts of distinguishable model
orders, not packet-loss probabilities or runtime failure rates. ACK/fake pairing
and whether an accepted request later dispatches a shot are outside this model.

Run directly for bounded counts and concrete counterexamples. No dependencies
outside Python's standard library and no files are written by this program.
"""

from dataclasses import dataclass, field
from enum import Enum
from itertools import combinations, permutations
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple


INT32_MAX = (1 << 31) - 1
Key = Tuple[str, int]


class Policy(Enum):
    CURRENT = "current"
    ORIGINAL_ONLY = "original_only"


@dataclass(frozen=True)
class RPC:
    label: str
    kind: str
    event_id: int
    reliable_sequence: Optional[int] = None
    channel: str = "weapon"
    mode: int = 0

    def __post_init__(self) -> None:
        if self.kind not in ("start", "stop"):
            raise ValueError("kind must be start or stop")
        if self.reliable_sequence is not None and self.reliable_sequence < 1:
            raise ValueError("reliable originals require a positive sequence")

    @property
    def reliable(self) -> bool:
        return self.reliable_sequence is not None

    @property
    def key(self) -> Key:
        return self.channel, self.mode


class ReliableChannels:
    """Minimal receive ordering, not a model of Unreal packet/bunch encoding.

    A sequence identifies an original invocation on one actor channel here.
    An actual Unreal reliable bunch can contain more than one RPC. The engine
    still preserves their order and suppresses retransmission duplicates.
    Unreliable invocations execute without waiting for missing reliable ones.
    """

    def __init__(self) -> None:
        self.next_sequence: Dict[str, int] = {}
        self.buffered: Dict[str, Dict[int, RPC]] = {}

    def receive(self, rpc: RPC) -> Tuple[RPC, ...]:
        if not rpc.reliable:
            return (rpc,)
        expected = self.next_sequence.get(rpc.channel, 1)
        sequence = rpc.reliable_sequence
        assert sequence is not None
        if sequence < expected:
            return ()  # Engine retransmission duplicate.
        queue = self.buffered.setdefault(rpc.channel, {})
        if sequence in queue and queue[sequence] != rpc:
            raise ValueError("two different originals share a channel sequence")
        queue[sequence] = rpc
        ready: List[RPC] = []
        while expected in queue:
            ready.append(queue.pop(expected))
            expected += 1
        self.next_sequence[rpc.channel] = expected
        return tuple(ready)


@dataclass
class ModeState:
    authoritative: int = 0
    last_stop: int = -1
    pending: bool = False
    accepted_starts: List[int] = field(default_factory=list)
    stop_mutations: List[str] = field(default_factory=list)
    retired_unprocessed_starts: List[int] = field(default_factory=list)


@dataclass(frozen=True)
class Row:
    label: str
    result: str
    authoritative: int
    last_stop: int
    pending: bool


class FireServer:
    def __init__(
        self,
        policy: Policy,
        originals: Sequence[RPC] = (),
        transactional_modes: Iterable[Key] = (("weapon", 0),),
    ) -> None:
        self.policy = policy
        self.transactional_modes = frozenset(transactional_modes)
        self.originals = tuple(originals)
        self.states: Dict[Key, ModeState] = {}
        self.rows: List[Row] = []

    def state(self, key: Key = ("weapon", 0)) -> ModeState:
        return self.states.setdefault(key, ModeState())

    def dispatch(self, rpc: RPC) -> Row:
        state = self.state(rpc.key)
        if rpc.event_id < 0 or rpc.event_id > INT32_MAX:
            result = "payload_reject"
        elif rpc.kind == "start":
            # Baseline C++ Start does signed last + 10. Python cannot prove what
            # C++ signed overflow does, so do not silently model it as wrapping.
            if state.authoritative > INT32_MAX - 10:
                raise ValueError("source signed-overflow boundary is unmodeled")
            if rpc.event_id <= state.authoritative:
                result = "start_old"
            elif rpc.event_id > state.authoritative + 10:
                result = "start_lookahead"
            else:
                state.authoritative = rpc.event_id
                state.pending = True
                state.accepted_starts.append(rpc.event_id)
                result = "start_accept"
        else:
            candidate_mode = (
                self.policy is Policy.ORIGINAL_ONLY
                and rpc.key in self.transactional_modes
            )
            if candidate_mode and not rpc.reliable:
                result = "stop_copy_ignored"
            elif not candidate_mode and rpc.event_id <= state.last_stop:
                result = "stop_duplicate"
            elif rpc.event_id < state.authoritative:
                result = "stop_older_than_authoritative"
            elif rpc.event_id > state.authoritative + 10:
                result = "stop_lookahead"
            else:
                # A Stop may move the watermark past a Start that never ran.
                for original in self.originals:
                    if (
                        original.kind == "start"
                        and original.key == rpc.key
                        and state.authoritative < original.event_id <= rpc.event_id
                        and original.event_id not in state.accepted_starts
                        and original.event_id not in state.retired_unprocessed_starts
                    ):
                        state.retired_unprocessed_starts.append(original.event_id)
                state.authoritative = rpc.event_id
                state.last_stop = rpc.event_id
                state.pending = False
                state.stop_mutations.append(rpc.label)
                result = "stop_process"
        row = Row(
            rpc.label, result, state.authoritative, state.last_stop, state.pending
        )
        self.rows.append(row)
        return row


def run(
    arrivals: Sequence[RPC],
    policy: Policy,
    transactional_modes: Iterable[Key] = (("weapon", 0),),
) -> FireServer:
    """Receive raw arrivals; buffer/deduplicate reliable originals first."""
    unique_originals = {rpc.label: rpc for rpc in arrivals if rpc.reliable}
    server = FireServer(policy, tuple(unique_originals.values()), transactional_modes)
    transport = ReliableChannels()
    for rpc in arrivals:
        for ready in transport.receive(rpc):
            server.dispatch(ready)
    return server


def delivery_orders(
    events: Sequence[RPC], allow_copy_loss: bool = True
) -> Iterator[Tuple[RPC, ...]]:
    """Enumerate gameplay deliveries respecting only per-channel reliable order.

    All event labels must be distinct. Original reliable sequences must be
    contiguous from one per channel. Originals all arrive eventually, while
    any subset of the unreliable copies can be lost. Packet arrival permutations
    that buffer into the same gameplay order are intentionally not recounted.
    """
    if len({rpc.label for rpc in events}) != len(events):
        raise ValueError("enumeration requires distinct invocation labels")
    originals = tuple(rpc for rpc in events if rpc.reliable)
    copies = tuple(rpc for rpc in events if not rpc.reliable)
    channels = {rpc.channel for rpc in originals}
    for channel in channels:
        numbers = sorted(
            rpc.reliable_sequence for rpc in originals if rpc.channel == channel
        )
        if numbers != list(range(1, len(numbers) + 1)):
            raise ValueError("original sequences must be contiguous per channel")
    sizes = range(len(copies) + 1) if allow_copy_loss else (len(copies),)
    for size in sizes:
        for selected in combinations(copies, size):
            for order in permutations(originals + selected):
                seen: Dict[str, int] = {}
                valid = True
                for rpc in order:
                    if rpc.reliable:
                        expected = seen.get(rpc.channel, 0) + 1
                        if rpc.reliable_sequence != expected:
                            valid = False
                            break
                        seen[rpc.channel] = expected
                if valid:
                    yield order


def one_shot_events(copies_per_call: int = 2) -> Tuple[RPC, ...]:
    return (
        RPC("start_original", "start", 1, 1),
        RPC("stop_original", "stop", 1, 2),
    ) + tuple(
        RPC("%s_copy_%d" % (kind, number), kind, 1)
        for kind in ("start", "stop")
        for number in range(1, copies_per_call + 1)
    )


def two_shot_events() -> Tuple[RPC, ...]:
    return (
        RPC("start_1", "start", 1, 1),
        RPC("stop_1", "stop", 1, 2),
        RPC("start_2", "start", 2, 3),
        RPC("stop_2", "stop", 2, 4),
        RPC("start_1_copy", "start", 1),
        RPC("stop_1_copy", "stop", 1),
        RPC("start_2_copy", "start", 2),
        RPC("stop_2_copy", "stop", 2),
    )


def summarize(events: Sequence[RPC]) -> Dict[str, object]:
    result: Dict[str, object] = {"orders": 0}
    for policy in Policy:
        result[policy.value] = {"missing_start_orders": 0, "stop_retirement_orders": 0}
    expected = {rpc.event_id for rpc in events if rpc.reliable and rpc.kind == "start"}
    for order in delivery_orders(events):
        result["orders"] += 1
        for policy in Policy:
            state = run(order, policy).state()
            bucket = result[policy.value]
            if expected.difference(state.accepted_starts):
                bucket["missing_start_orders"] += 1
            if state.retired_unprocessed_starts:
                bucket["stop_retirement_orders"] += 1
    return result


def stock_accepts(last: int, incoming: int) -> bool:
    """AUTWeapon::ValidateFireEventIndex, separate from the fixed int32 stream."""
    if not (0 <= last <= 255 and 0 <= incoming <= 255):
        raise ValueError("stock IDs must be uint8 values")
    return incoming == 255 or not (last >= incoming and last < incoming + 128)


def counter_increment_stock_trace(increment_releases: bool) -> Dict[str, object]:
    """Actual coupling: fixed Start permanently overwrites server's stock byte.

    Two stock charged cycles advance the client's stock byte to four. Three
    primary taps advance only its fixed counter. Fixed Starts write their low
    byte into server stock sequencing. The following charged Start sends five.
    """
    fixed = 0
    shots = []
    for _ in range(3):
        fixed += 1
        shots.append(fixed)
        if increment_releases:
            fixed += 1
    server_stock = shots[-1] & 255
    return {
        "fixed_shots": shots,
        "server_stock": server_stock,
        "next_client_stock_start": 5,
        "stock_start_accepted": stock_accepts(server_stock, 5),
    }


def main() -> None:
    import json

    example = (
        RPC("stop_copy", "stop", 1),
        RPC("start_original", "start", 1, 1),
        RPC("stop_original", "stop", 1, 2),
    )
    output = {
        "scope": "Source-semantics model; not an Unreal runtime proof.",
        "one_shot_two_copies_per_call": summarize(one_shot_events()),
        "two_shots_one_copy_per_call": summarize(two_shot_events()),
        "retry_stop_overtakes_start": {
            policy.value: [row.__dict__ for row in run(example, policy).rows]
            for policy in Policy
        },
        "stock_byte_counterexample": {
            "unchanged_counter": counter_increment_stock_trace(False),
            "increment_release_old_server": counter_increment_stock_trace(True),
        },
    }
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
