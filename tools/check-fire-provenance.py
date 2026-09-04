#!/usr/bin/env python3
"""Check default-off NCFireAuth server telemetry without inferring shots from time.

Usage: python tools/check-fire-provenance.py server.log [--rocket-primary] [--json]
Exit codes: 0 = observed records reconcile; 1 = violation; 2 = incomplete evidence.
Files are separate captures; event identities are never joined across files.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys
from typing import Iterable


NUMBERED_SOURCES = frozenset({
    "FixedInitial", "FixedRetry", "DeferredEquip", "DeferredChargedTail", "DeferredStateTail",
})
PERMITTED_SOURCES = NUMBERED_SOURCES | {
    "StockManaged", "ChargedDirect", "Bot", "ListenHost", "Standalone",
}
LIFECYCLE_REASONS = frozenset({
    "switch", "death", "drop", "destroyed", "destroy", "travel", "removed",
    "detach", "owner_changed", "equip_lifetime", "bringup",
})
MARKER = re.compile(r"\[NCFireAuth\]\s+([A-Z_]+)\b(.*)")
FIELDS = re.compile(r'''(\w+)=("(?:\\.|[^"\\])*"|'[^']*'|[^\s]+)''')
IDENTITY_EVENTS = frozenset({
    "ACCEPT", "SHOT", "SHOT_END", "SPAWN", "PENDING", "CANCEL", "CANCELED", "CANCELLED",
})


@dataclass(frozen=True)
class Record:
    capture: str
    line: int
    kind: str
    values: dict[str, str]

    def integer(self, name: str) -> int:
        return int(self.values[name])

    @property
    def key(self) -> tuple[str, str, int, int]:
        return (self.capture, self.values["weapon"], self.integer("mode"), self.integer("event"))

    @property
    def context_key(self) -> tuple[str, str, int, int, int]:
        return (*self.key, self.integer("generation"))


@dataclass
class Report:
    counts: Counter = field(default_factory=Counter)
    findings: list[dict] = field(default_factory=list)

    def add(self, severity: str, code: str, message: str, record: Record | None = None) -> None:
        finding = {"severity": severity, "code": code, "message": message}
        if record is not None:
            finding.update(capture=record.capture, line=record.line)
        self.findings.append(finding)

    @property
    def exit_code(self) -> int:
        if any(item["severity"] == "violation" for item in self.findings):
            return 1
        if any(item["severity"] == "unresolved" for item in self.findings):
            return 2
        return 0

    def as_dict(self) -> dict:
        return {
            "status": {0: "observed_records_reconcile", 1: "violations", 2: "incomplete_evidence"}[self.exit_code],
            "exit_code": self.exit_code,
            "counts": dict(sorted(self.counts.items())),
            "findings": self.findings,
            "scope": "Observed labelled dispatches only; timestamps do not prove spawns. "
                     "SPAWN/SHOT_END cover instrumented NCP spawn paths, not every engine or Blueprint path.",
        }


def parse_records(lines: Iterable[str], capture: str = "<memory>") -> list[Record]:
    records = []
    for number, line in enumerate(lines, 1):
        match = MARKER.search(line)
        if match is None:
            continue
        values = {}
        for key, value in FIELDS.findall(match.group(2)):
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key] = value
        records.append(Record(capture, number, match.group(1), values))
    return records


def describe(record: Record) -> str:
    return (f"weapon={record.values['weapon']} mode={record.integer('mode')} "
            f"event={record.integer('event')} generation={record.integer('generation')}")


def check_spawn_counters(records: Iterable[Record], report: Report) -> None:
    """Compare each completed synchronous observation scope, including reentry.

    Missing result rows remain incomplete evidence. Present rows contradicting a
    counter are violations of telemetry consistency, not proof of engine damage.
    """
    records = list(records)
    shot_contexts = set()
    for record in records:
        if record.kind == "SHOT":
            try:
                shot_contexts.add(record.context_key)
            except (KeyError, ValueError):
                pass
    open_scopes = defaultdict(list)
    for record in records:
        if record.kind not in {"SHOT", "SHOT_END", "SPAWN"}:
            continue
        try:
            context = record.context_key
        except (KeyError, ValueError):
            continue
        if record.kind == "SHOT":
            open_scopes[context].append([])
        elif record.kind == "SPAWN":
            if open_scopes[context]:
                open_scopes[context][-1].append(record)
            elif context in shot_contexts:
                report.add("unresolved", "SPAWN_OUTSIDE_SHOT",
                           f"SPAWN appears outside an open SHOT/SHOT_END scope: {describe(record)}.", record)
        elif open_scopes[context]:
            outcomes = open_scopes[context].pop()
            try:
                attempts, successes = record.integer("attempts"), record.integer("spawns")
                if attempts < 0 or successes < 0 or successes > attempts:
                    continue  # Already reported while parsing SHOT_END.
            except (KeyError, ValueError):
                continue
            results = [outcome.values.get("result") for outcome in outcomes]
            if any(result not in {"ok", "null", "suppressed"} for result in results):
                continue  # UNKNOWN_SPAWN_RESULT already leaves this unresolved.
            observed_successes = results.count("ok")
            if len(results) > attempts or observed_successes > successes:
                report.add("violation", "SPAWN_COUNTER_MISMATCH",
                           f"{describe(record)}: SHOT_END reports attempts={attempts} spawns={successes}, "
                           f"but enclosed SPAWN records show attempts={len(results)} spawns={observed_successes}.", record)
            elif len(results) < attempts:
                report.add("unresolved", "MISSING_SPAWN_TELEMETRY",
                           f"{describe(record)}: SHOT_END reports {attempts} attempts but only "
                           f"{len(results)} enclosed SPAWN results are present.", record)
            elif observed_successes != successes:
                report.add("violation", "SPAWN_COUNTER_MISMATCH",
                           f"{describe(record)}: all {attempts} SPAWN results are present but SHOT_END "
                           f"reports {successes} successes instead of {observed_successes}.", record)
            else:
                report.counts["verified_spawn_observations"] += 1
        elif context in shot_contexts:
            report.add("unresolved", "END_OUTSIDE_SHOT",
                       f"SHOT_END has no preceding open SHOT scope: {describe(record)}.", record)


def analyze(records: Iterable[Record], rocket_primary: bool = False) -> Report:
    report = Report()
    records = list(records)
    report.counts["telemetry_records"] = len(records)
    if not records:
        report.add("unresolved", "NO_TELEMETRY", "No NCFireAuth records; firing provenance cannot be verified.")
        return report

    if rocket_primary:
        rocket_weapons = {
            (record.capture, record.values.get("weapon", ""))
            for record in records
            if "rocket" in (record.values.get("weapon", "") + " " + record.values.get("class", "")).lower()
        }
        records = [record for record in records
                   if (record.capture, record.values.get("weapon", "")) in rocket_weapons
                   and record.values.get("mode") == "0"]
        if not records:
            report.add("unresolved", "NO_ROCKET_PRIMARY_TELEMETRY",
                       "No mode-0 records identifiable as Rocket by weapon/class name; scope cannot be verified.")
            return report
    report.counts["scoped_records"] = len(records)

    accepted = defaultdict(list)
    shots = defaultdict(list)
    ends = defaultdict(list)
    spawns = defaultdict(list)
    canceled = defaultdict(list)
    pending = set()
    for record in records:
        if record.kind in {"BLOCK_ENTRY", "BLOCK_SHOT"}:
            report.counts[record.kind.lower()] += 1
            continue
        if record.kind not in IDENTITY_EVENTS:
            continue
        try:
            record.context_key
            if not record.values["weapon"]:
                raise ValueError("empty weapon")
        except (KeyError, ValueError):
            report.add("unresolved", "MALFORMED_IDENTITY",
                       f"{record.kind} lacks weapon/mode/event/generation identity.", record)
            continue

        if record.kind == "ACCEPT":
            accepted[record.key].append(record)
            if record.values.get("source") not in NUMBERED_SOURCES or record.integer("generation") <= 0:
                report.add("violation", "INVALID_ACCEPT_PROVENANCE", describe(record), record)
        elif record.kind == "SHOT":
            source = record.values.get("source")
            report.counts["dispatches"] += 1
            if source not in PERMITTED_SOURCES:
                report.add("violation", "UNPERMITTED_SHOT_SOURCE",
                           f"source={source or '<missing>'} {describe(record)}", record)
            if source in NUMBERED_SOURCES:
                report.counts["numbered_dispatches"] += 1
                if record.integer("generation") <= 0:
                    report.add("violation", "MISSING_REQUEST_GENERATION", describe(record), record)
            else:
                report.counts["other_dispatches"] += 1
            shots[record.context_key].append(record)
        elif record.kind == "SHOT_END":
            ends[record.context_key].append(record)
            try:
                attempts, successes = record.integer("attempts"), record.integer("spawns")
                if attempts < 0 or successes < 0 or successes > attempts:
                    raise ValueError("invalid counts")
                report.counts["reported_spawn_attempts"] += attempts
                report.counts["reported_spawn_successes"] += successes
            except (KeyError, ValueError):
                report.add("unresolved", "INVALID_SHOT_END_COUNTS",
                           "SHOT_END requires nonnegative attempts/spawns with spawns <= attempts.", record)
        elif record.kind == "SPAWN":
            result = record.values.get("result")
            report.counts["spawn_records"] += 1
            if result not in {"ok", "null", "suppressed"}:
                report.add("unresolved", "UNKNOWN_SPAWN_RESULT", f"result={result or '<missing>'}", record)
            else:
                report.counts[f"spawn_{result}"] += 1
            spawns[record.context_key].append(record)
        elif record.kind == "PENDING":
            pending.add(record.context_key)
        else:
            canceled[record.context_key].append(record)
            if record.values.get("reason", "").lower() not in LIFECYCLE_REASONS:
                report.add("unresolved", "UNCLASSIFIED_CANCEL",
                           f"Cancellation needs an explicit lifecycle reason: {describe(record)}", record)

    numbered_by_event = defaultdict(list)
    for context, entries in shots.items():
        for record in entries:
            if record.values.get("source") in NUMBERED_SOURCES:
                numbered_by_event[record.key].append(record)
        end_entries = ends.get(context, [])
        if len(end_entries) != len(entries):
            report.add("unresolved", "UNMATCHED_SHOT_END",
                       f"{describe(entries[0])}: {len(entries)} SHOT versus {len(end_entries)} SHOT_END records.", entries[0])
        else:
            report.counts["completed_dispatches"] += len(entries)

    for key, entries in numbered_by_event.items():
        dispatches_by_context = defaultdict(list)
        for entry in entries:
            dispatches_by_context[entry.context_key].append(entry)
        for dispatches in dispatches_by_context.values():
            if len(dispatches) > 1:
                report.add("violation", "DUPLICATE_DISPATCH",
                           f"{describe(dispatches[0])}: {len(dispatches)} numbered dispatches for one request.", dispatches[1])
        reservations = accepted.get(key, [])
        for shot in entries:
            if not reservations:
                report.add("unresolved", "DISPATCH_WITHOUT_ACCEPT",
                           f"No ACCEPT in this capture for {describe(shot)}; capture may start late.", shot)
            elif not any(item.context_key == shot.context_key for item in reservations):
                report.add("violation", "REQUEST_GENERATION_MISMATCH", describe(shot), shot)

    report.counts["accepted_events"] = len({reservation.context_key
                                           for reservations in accepted.values() for reservation in reservations})
    for key, reservations in accepted.items():
        reservations_by_context = defaultdict(list)
        for reservation in reservations:
            reservations_by_context[reservation.context_key].append(reservation)
        if len(reservations_by_context) > 1:
            report.add("unresolved", "EVENT_ID_REUSED",
                       f"weapon={key[1]} mode={key[2]} event={key[3]} appears with multiple generations. "
                       "Actor-name reuse across lifetimes and repeated acceptance cannot be distinguished from this schema.",
                       reservations[-1])
        for duplicates in reservations_by_context.values():
            reservation = duplicates[0]
            if len(duplicates) > 1:
                report.add("violation", "DUPLICATE_ACCEPT",
                           f"{describe(reservation)}: {len(duplicates)} ACCEPT records for one request.", duplicates[1])
            dispatches = [shot for shot in numbered_by_event.get(key, [])
                          if shot.context_key == reservation.context_key]
            cancellations = canceled.get(reservation.context_key, [])
            lifecycle_canceled = any(item.values.get("reason", "").lower() in LIFECYCLE_REASONS
                                     for item in cancellations)
            if dispatches:
                report.counts["accepted_events_dispatched"] += 1
                if lifecycle_canceled:
                    report.add("violation", "DISPATCHED_AND_CANCELED",
                               f"Both dispatch and reservation cancellation: {describe(reservation)}", reservation)
            elif lifecycle_canceled:
                report.counts["accepted_events_lifecycle_canceled"] += 1
            else:
                report.counts["accepted_events_unresolved"] += 1
                explicit = reservation.context_key in pending
                report.add("unresolved", "ACCEPT_STILL_PENDING" if explicit else "ACCEPT_WITHOUT_DISPATCH",
                           f"{'Explicitly pending' if explicit else 'No dispatch or lifecycle cancellation in capture'}: "
                           f"{describe(reservation)}; a truncated capture cannot prove loss.", reservation)

    for context, entries in ends.items():
        if context not in shots:
            report.add("unresolved", "END_WITHOUT_SHOT",
                       f"No SHOT in this capture for {describe(entries[0])}.", entries[0])
    for context, entries in spawns.items():
        if context not in shots:
            for entry in entries:
                if (entry.values.get("source") == "ChargedDirect" and entry.integer("mode") == 1
                        and entry.integer("event") == -1 and entry.integer("generation") == 0):
                    # Charged volleys bypass FireShot by design. These rows are
                    # observed outcomes, never counted as numbered dispatches.
                    report.counts["permitted_direct_outcomes"] += 1
                else:
                    report.add("unresolved", "SPAWN_WITHOUT_SHOT",
                               f"Spawn path has no enclosing SHOT record: {describe(entry)}.", entry)
    check_spawn_counters(records, report)
    if not accepted and not shots:
        if report.counts["permitted_direct_outcomes"]:
            report.add("info", "DIRECT_OUTCOMES_ONLY",
                       "Only permitted charged-direct outcomes were observed; there are no numbered dispatches to reconcile.")
        else:
            report.add("unresolved", "NO_DISPATCH_TELEMETRY",
                       "No ACCEPT/SHOT records in scope; blocked entries or unscoped spawn rows cannot establish firing acceptance.")
    return report


def read_log(path: Path) -> list[str]:
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    return data.decode(encoding, errors="replace").splitlines()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="Server log files (each is a separate capture).")
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--rocket-primary", action="store_true",
                       help="Mode 0 only, for weapons identifiable by Rocket in weapon/class name.")
    scope.add_argument("--all", action="store_true", help="All observed weapons and modes (default).")
    parser.add_argument("--json", action="store_true", help="Print machine-readable report.")
    args = parser.parse_args(argv)
    records = []
    try:
        for path in dict.fromkeys(path.resolve() for path in args.logs):
            records.extend(parse_records(read_log(path), str(path)))
    except OSError as error:
        parser.error(str(error))
    report = analyze(records, rocket_primary=args.rocket_primary)
    result = report.as_dict()
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(result["status"])
        for key, value in result["counts"].items():
            print(f"  {key}: {value}")
        for finding in report.findings:
            location = f" {finding['capture']}:{finding['line']}" if "capture" in finding else ""
            print(f"{finding['severity'].upper()} {finding['code']}{location}: {finding['message']}")
        print(result["scope"])
    return report.exit_code


if __name__ == "__main__":
    sys.exit(main())
