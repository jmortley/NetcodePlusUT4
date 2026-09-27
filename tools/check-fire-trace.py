#!/usr/bin/env python3
"""Join an instrumented client/server fire capture without treating ACKs as shots.

No clock-based matching: fixed requests join by shared run, replicated actor GUIDs,
mode and existing event index. Continuous fire has no per-pulse wire identity.
"""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import sys

MARKER = '[NCFireTrace] '
COMMON = {'schema', 'run', 'capture', 'seq'}
IDENTITY = {'side', 'world', 'driver', 'weapon', 'player', 'owner', 'mode',
            'event', 'generation', 'protocol', 'engine', 'build'}
CONTEXT = {'INPUT_PRESS', 'INPUT_RELEASE', 'STATE_REQUEST', 'STATE_CHANGED',
           'SWITCH_ATTEMPT', 'SWITCH_RESULT', 'EQUIP_BEGIN', 'PUTDOWN_CALLBACK',
           'PUTDOWN_TIMING', 'CHARGE_EVENT', 'RETRY_QUEUE_CLEAR', 'BLOCK_DISPATCH',
           'ACCEPT', 'CANCEL', 'REJECT', 'ACK_SENT', 'ACK_RECEIVED', 'RETRY_IGNORED',
           'STOCK_RECEIVE', 'STOP_RECEIVE', 'SEND_RETRY', 'SEND_STOP_RETRY'}


def integer(row, key, default=-1):
    try:
        return int(row.get(key, default))
    except (ValueError, TypeError):
        return default


def read_log(path):
    data = Path(path).read_bytes()
    enc = 'utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    return data.decode(enc, errors='replace').splitlines()


def parse(lines, filename, expected_side, run):
    rows, issues = [], []
    for line, text in enumerate(lines, 1):
        if MARKER not in text:
            continue
        payload = text.split(MARKER, 1)[1]
        fields = re.findall(r'(\w+)=([^\s]*)', payload)
        row = dict(fields)
        if row.get('run', run) != run:
            continue
        row.update(kind=payload.split()[0] if payload.split() else '',
                   file=filename, line=line, log_side=expected_side)
        required = COMMON if row['kind'] in {'END', 'LIMIT', 'NETWORK'} else COMMON | IDENTITY
        if (not required <= row.keys() or len(fields) != len(dict(fields))
                or row.get('schema') != '1' or integer(row, 'seq') < 1):
            issues.append(f'{filename}:{line}: malformed/unsupported trace record')
        if row.get('side', expected_side) != expected_side:
            issues.append(f'{filename}:{line}: expected {expected_side} records')
        rows.append(row)
    return rows, issues


def check_capture(rows, side):
    issues = []
    if not rows:
        return [f'{side}: no matching trace records'], False
    captures = {r.get('capture') for r in rows}
    if len(captures) != 1 or None in captures:
        issues.append(f'{side}: multiple/missing capture IDs; use a unique run label for each session')
    seqs = [integer(r, 'seq') for r in rows]
    if seqs != list(range(1, len(rows) + 1)):
        issues.append(f'{side}: missing, duplicated or reordered log records')
    if rows[-1]['kind'] != 'END' or sum(r['kind'] == 'END' for r in rows) != 1:
        issues.append(f'{side}: missing final ncp.FireTraceEnd marker (capture may be truncated)')
    if any(r['kind'] == 'LIMIT' or r.get('limited') == '1' for r in rows):
        issues.append(f'{side}: trace limit reached; evidence is incomplete')
    actors = [r for r in rows if IDENTITY <= r.keys()]
    worlds = {(r['world'], r['driver']) for r in actors}
    unambiguous = len(worlds) == 1 and len(captures) == 1
    if not unambiguous:
        issues.append(f'{side}: multiple worlds/drivers/captures; do not correlate across reconnects/travel')
    if any(r.get('build') != 'fire-trace-v1' for r in actors):
        issues.append(f'{side}: unsupported trace build')
        unambiguous = False
    return issues, unambiguous


def key(row):
    # Actor names and raw object pointers are process-local, never join keys.
    return tuple(row.get(k) for k in ('run', 'weapon', 'player', 'owner', 'mode', 'event'))


def location(row):
    return {'file': row['file'], 'line': row['line'], 'kind': row['kind'],
            **{k: v for k, v in row.items() if k not in {'file', 'line', 'kind', 'log_side'}}}


def context_key(row):
    return row.get('player'), None if integer(row, 'player') > 0 else row.get('actor')


def context_index(rows):
    groups = defaultdict(list)
    for row in rows:
        if row['kind'] in CONTEXT:
            groups[context_key(row)].append(row)
    for group in groups.values():
        group.sort(key=lambda r: integer(r, 'seq'))
    return {k: ([integer(r, 'seq') for r in group], group) for k, group in groups.items()}


def nearby(index, anchor, count):
    seqs, candidates = index.get(context_key(anchor), ([], []))
    # Include the pending/outgoing weapon's events, not just the shot's weapon.
    split = bisect_right(seqs, integer(anchor, 'seq'))
    before = candidates[max(0, split-count):split]
    after = candidates[split:split+count]
    return [location(r) for r in before + after] if count else []


def classify(pred, peers):
    receives = [r for r in peers if r['kind'] == 'RECEIVE']
    accepts = [r for r in peers if r['kind'] == 'ACCEPT']
    dispatches = [r for r in peers if r['kind'] == 'DISPATCH']
    cancels = [r for r in peers if r['kind'] == 'CANCEL']
    rejects = [r for r in peers if r['kind'] == 'REJECT']
    # A reset/wrap or two accepted lifetimes must never collapse to one shot.
    if len(accepts) > 1 or len(dispatches) > 1:
        return 'unexplained', 'ambiguous_reused_event_or_multiple_dispatches'
    if dispatches:
        shot = dispatches[0]
        if (len(accepts) != 1 or integer(shot, 'generation') <= 0
                or shot['generation'] != accepts[0].get('generation')
                or integer(accepts[0], 'seq') >= integer(shot, 'seq')):
            return 'unexplained', 'dispatch_missing_matching_acceptance'
        ends = [r for r in peers if r['kind'] == 'DISPATCH_END'
                and r.get('scope') == shot.get('scope') and r.get('generation') == shot.get('generation')]
        if len(ends) != 1 or integer(ends[0], 'seq') <= integer(shot, 'seq'):
            return 'unexplained', 'dispatch_incomplete'
        outcomes = [r for r in peers if r.get('scope') == shot.get('scope')
                    and r.get('generation') == shot.get('generation')
                    and integer(shot, 'seq') < integer(r, 'seq') < integer(ends[0], 'seq')]
        projectiles = sum(r['kind'] == 'PROJECTILE' and r.get('result') == 'ok' for r in outcomes)
        traces = sum(r['kind'] == 'HITSCAN' for r in outcomes)
        damaging = sum(r['kind'] == 'HITSCAN' and r.get('damagePath') == '1' for r in outcomes)
        if (projectiles, traces, damaging) != tuple(integer(ends[0], k) for k in ('projectiles', 'traces', 'damagingTraces')):
            return 'unexplained', 'missing_or_inconsistent_outcome_records'
        if cancels:
            return 'unexplained', 'cancellation_conflicts_with_dispatch'
        if projectiles or damaging:
            return 'fired', 'server_projectile_spawn' if projectiles else 'server_hitscan_execution'
        return 'unexplained', 'dispatch_without_verified_firing_outcome'
    if cancels:
        if (len(accepts) == 1 and len(cancels) == 1
                and integer(accepts[0], 'generation') > 0
                and accepts[0].get('generation') == cancels[0].get('generation')
                and integer(cancels[0], 'seq') > integer(accepts[0], 'seq')):
            return 'cancelled', cancels[0].get('reason', 'unspecified_cancellation')
        return 'unexplained', 'cancellation_without_unique_acceptance'
    if accepts:
        return 'unexplained', 'accepted_without_outcome'
    if rejects and receives:
        # Every observed request attempt must have an explicit terminal reject.
        ordered = sorted(receives + rejects, key=lambda r: integer(r, 'seq'))
        pending = False
        for r in ordered:
            pending = r['kind'] == 'RECEIVE'
        if not pending and len(rejects) >= len(receives):
            return 'rejected', ','.join(sorted({r.get('reason', 'unspecified') for r in rejects}))
    return 'unexplained', 'received_without_outcome' if receives else 'no_matching_server_request'


def analyze(client_lines, server_lines, run, client_name='client.log', server_name='server.log', context=4):
    if not run or run == 'unset' or re.search(r'\s', run):
        raise ValueError('Use the same explicit, unique run label on both processes (no whitespace).')
    client, issues = parse(client_lines, client_name, 'client', run)
    server, server_issues = parse(server_lines, server_name, 'server', run)
    issues += server_issues
    ci, client_unique = check_capture(client, 'client')
    si, server_unique = check_capture(server, 'server')
    issues += ci + si
    engines = {r['engine'] for r in client + server if 'engine' in r}
    versions_match = len(engines) == 1
    if not versions_match:
        issues.append('Pair must come from the same engine version, not a 4.15/4.27 mixture')
    grouped = defaultdict(list)
    for r in server:
        if r.get('protocol') == 'fixed':
            grouped[key(r)].append(r)
    predicted = [r for r in client if r['kind'] == 'PREDICT']
    if not predicted:
        issues.append('No client predictions observed; this is not a successful shot test')
    duplicates = Counter(key(r) for r in predicted if r.get('protocol') == 'fixed')
    received_watermarks = {key(r) for r in client if r['kind'] == 'ACK_RECEIVED'}
    client_context, server_context = context_index(client), context_index(server)
    result = []
    for pred in predicted:
        peers = grouped[key(pred)] if pred.get('protocol') == 'fixed' else []
        if pred.get('protocol') != 'fixed':
            outcome, reason = 'unexplained', 'continuous_or_charged_pulse_has_no_per_shot_wire_identity'
        elif any(integer(pred, k) <= 0 for k in ('weapon', 'player', 'owner')) or integer(pred, 'event') < 0:
            outcome, reason = 'unexplained', 'missing_replicated_identity'
        elif not (client_unique and server_unique and versions_match) or duplicates[key(pred)] != 1:
            outcome, reason = 'unexplained', 'ambiguous_capture_or_prediction_identity'
        else:
            outcome, reason = classify(pred, peers)
        item = {'outcome': outcome, 'reason': reason, 'prediction': location(pred),
                'server_evidence': [location(r) for r in peers],
                'ack_sent': any(r['kind'] == 'ACK_SENT' for r in peers),
                'exact_watermark_received': key(pred) in received_watermarks,
                'client_context': nearby(client_context, pred, context)}
        # No global time sorting or fabricated server timestamp for a missing event.
        item['server_context'] = nearby(server_context, peers[0], context) if peers else []
        result.append(item)
    counts = dict(Counter(r['outcome'] for r in result))
    stream_counts = []
    for side, rows in [('client', client), ('server', server)]:
        groups = Counter((r.get('class'), r.get('mode'), r['kind']) for r in rows
                         if r.get('protocol') in {'stock', 'stream', 'beam'}
                         and r['kind'] in {'PREDICT', 'DISPATCH', 'BEAM_SAMPLE', 'PROJECTILE', 'HITSCAN', 'STOCK_RECEIVE'})
        stream_counts += [dict(side=side, weapon_class=c, mode=m, kind=k, count=n)
                          for (c, m, k), n in sorted(groups.items(), key=lambda x: str(x[0]))]
    observed_keys = {key(r) for r in predicted if r.get('protocol') == 'fixed'}
    server_only = [location(r) for r in server if r.get('protocol') == 'fixed'
                   and r['kind'] in {'ACCEPT', 'DISPATCH', 'CANCEL', 'DIRECT_SPAWN'} and key(r) not in observed_keys]
    return {'schema': 1, 'run': run, 'capture_complete': not issues,
            'status': 'REVIEW_REQUIRED' if issues or any(r['outcome'] != 'fired' for r in result) else 'ALL_NUMBERED_PREDICTIONS_FIRED',
            'counts': counts, 'issues': issues, 'shots': result, 'continuous_observations': stream_counts,
            'server_only_events': server_only,
            'network_samples': [location(r) for r in client + server if r['kind'] == 'NETWORK'],
            'limits': ['Fired means hitscan execution/projectile spawn, not damage confirmation.',
                       'ACKs are watermarks, not proof of a fired shot.',
                       'Stream/beam/charged pulse ordinals are local only; they cannot prove a one-to-one server outcome.',
                       'Missing records do not prove packet loss; sampled network counters are driver-wide.']}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--run', required=True)
    parser.add_argument('--context', type=int, default=4, help='Local events before/after each unmatched prediction (0..20)')
    parser.add_argument('--json', action='store_true', help='Print every prediction, evidence, and context as JSON')
    args = parser.parse_args(argv)
    if not 0 <= args.context <= 20:
        parser.error('--context must be 0..20')
    try:
        report = analyze(read_log(args.client), read_log(args.server), args.run,
                         str(args.client), str(args.server), args.context)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(report['status'], report['counts'])
        for issue in report['issues']:
            print('INCOMPLETE:', issue)
        for shot in report['shots']:
            if shot['outcome'] == 'fired':
                continue
            p = shot['prediction']
            print(f"{shot['outcome'].upper()} {p['file']}:{p['line']} {p.get('class')} mode={p.get('mode')} event={p.get('event')}: {shot['reason']}")
            for side in ('client', 'server'):
                for row in shot[side + '_context']:
                    print(f"  {side} {row['file']}:{row['line']} {row['kind']} actor={row.get('actor')} state={row.get('state')} pending={row.get('pending')} reason={row.get('reason', '-')} t={row.get('t', '-')}")
        for note in report['limits']:
            print(note)
    return 0 if report['status'] == 'ALL_NUMBERED_PREDICTIONS_FIRED' else 1


if __name__ == '__main__':
    sys.exit(main())
