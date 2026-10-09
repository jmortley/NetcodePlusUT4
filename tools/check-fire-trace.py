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
import math
import struct

MARKER = '[NCFireTrace] '
COMMON = {'schema', 'run', 'session', 'capture', 'seq'}
IDENTITY = {'side', 'world', 'driver', 'weapon', 'player', 'owner', 'mode',
            'event', 'generation', 'protocol', 'engine', 'build', 'connection', 'weaponLocal', 'local', 'identity'}
CONTEXT = {'INPUT_PRESS', 'INPUT_RELEASE', 'STATE_REQUEST', 'STATE_CHANGED',
           'SWITCH_ATTEMPT', 'SWITCH_RESULT', 'EQUIP_BEGIN', 'PUTDOWN_CALLBACK',
           'PUTDOWN_TIMING', 'CHARGE_EVENT', 'RETRY_QUEUE_CLEAR', 'BLOCK_DISPATCH',
           'ACCEPT', 'CANCEL', 'REJECT', 'ACK_SENT', 'ACK_RECEIVED', 'RETRY_IGNORED',
           'STOCK_RECEIVE', 'STOCK_RESULT', 'STOCK_VALIDATE', 'STOCK_SEND', 'SYNC_DECISION',
           'STOP_RECEIVE', 'STOP_RESULT', 'STOP_APPLY', 'STOP_SEND', 'SEND_RETRY', 'SEND_STOP_RETRY',
           'LOCAL_PROJECTILE', 'LAYOUT', 'HOLD_BEGIN', 'HOLD_END', 'CHARGE_COMMIT', 'CHARGE_END'}


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
        required = COMMON if row['kind'] in {'BEGIN', 'END', 'LIMIT', 'NETWORK', 'ABORT'} else COMMON | IDENTITY
        if (not required <= row.keys() or len(fields) != len(dict(fields))
                or row.get('schema') != '2' or integer(row, 'seq') < 1):
            issues.append(f'{filename}:{line}: malformed/unsupported trace record')
        if row.get('side', expected_side) != expected_side:
            issues.append(f'{filename}:{line}: expected {expected_side} records')
        rows.append(row)
    return rows, issues


def check_capture(rows, side):
    issues = []
    if not rows:
        return [f'{side}: no matching trace records'], False
    if rows[0]['kind'] != 'BEGIN' or rows[0].get('explicit') != '1' or sum(r['kind'] == 'BEGIN' for r in rows) != 1:
        issues.append(f'{side}: missing unique explicit ncp.FireTraceBegin marker')
    captures = {r.get('capture') for r in rows}
    if len(captures) != 1 or None in captures:
        issues.append(f'{side}: multiple/missing capture IDs; use a unique run label for each session')
    seqs = [integer(r, 'seq') for r in rows]
    if seqs != list(range(1, len(rows) + 1)):
        issues.append(f'{side}: missing, duplicated or reordered log records')
    if rows[-1]['kind'] != 'END' or sum(r['kind'] == 'END' for r in rows) != 1:
        issues.append(f'{side}: missing final ncp.FireTraceEnd marker (capture may be truncated)')
    if any(r['kind'] in {'LIMIT', 'ABORT'} or r.get('limited') == '1' for r in rows):
        issues.append(f'{side}: trace limited or aborted; evidence is incomplete')
    actors = [r for r in rows if IDENTITY <= r.keys()]
    worlds = {(r['world'], r['driver']) for r in actors}
    unambiguous = len(worlds) <= 1 and len(captures) == 1
    if not unambiguous:
        issues.append(f'{side}: multiple worlds/drivers/captures; do not correlate across reconnects/travel')
    if any(r.get('build') != 'fire-trace-v2' for r in actors):
        issues.append(f'{side}: unsupported trace build')
        unambiguous = False
    return issues, unambiguous


def key(row):
    # Actor names and raw object pointers are process-local, never join keys.
    return tuple(row.get(k) for k in ('session', 'weapon', 'mode', 'event'))


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
    direct = [r for r in peers if r['kind'] == 'DIRECT_SPAWN']
    if direct:
        if len(direct) == 1 and not (accepts or dispatches or cancels) and receives and direct[0].get('result') == 'ok':
            return 'fired', 'server_trade_grace_projectile_spawn'
        return 'unexplained', 'ambiguous_or_failed_direct_spawn'
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


def wire_bits(row):
    """Validate the decimal and exact IEEE-754 single-precision wire representation."""
    try:
        value = float(row['clientT'])
        bits = row['clientBits'].lower()
        if not math.isfinite(value) or not re.fullmatch(r'[0-9a-f]{8}', bits):
            return None
        return bits if struct.pack('>f', value).hex() == bits else None
    except (KeyError, ValueError, OverflowError):
        return None


def selected_lifetime(rows):
    connections = {r.get('connection') for r in rows if integer(r, 'connection', 0) > 0}
    objects = defaultdict(set)
    for r in rows:
        if integer(r, 'weapon', 0) > 0:
            objects[r['weapon']].add(r.get('weaponLocal'))
    return (not rows or len(connections) == 1) and all(len(v) == 1 and None not in v for v in objects.values())


def ownership_consistent(pred, peers):
    # Unknown ownership is evidence to inspect, never a substitute for a lifetime check.
    return all(integer(r, 'owner') > 0 and integer(r, 'player') > 0
               and r['owner'] == pred.get('owner') and r['player'] == pred.get('player') for r in peers)


def pulse_counts(rows):
    return dict(Counter(r['kind'] for r in rows
                        if r['kind'] in {'PREDICT', 'DISPATCH', 'BEAM_SAMPLE', 'CHARGE_COMMIT'})) | {
        'successful_projectiles': sum(r['kind'] == 'PROJECTILE' and r.get('result') == 'ok' for r in rows),
        'damaging_traces': sum(r['kind'] == 'HITSCAN' and r.get('damagePath') == '1' for r in rows)}


def local_hold_rows(rows, anchor, hold):
    if hold <= 0:
        return []
    return [r for r in rows if r.get('weapon') == anchor.get('weapon') and r.get('mode') == anchor.get('mode')
            and r.get('owner') == anchor.get('owner') and (integer(r, 'hold') == hold or integer(r, 'volleyHold') == hold)]


def stock_peers(send, server):
    return [r for r in server if r['kind'] == 'STOCK_RECEIVE' and r.get('origin') == 'wire'
            and key(r) == key(send) and r.get('start') == send.get('start')]


def stock_results(receives, server):
    ids = {r.get('rpc') for r in receives if integer(r, 'rpc', 0) > 0}
    return [r for r in server if r['kind'] == 'STOCK_RESULT' and r.get('rpc') in ids]


def summarize_holds(client, server, pair_safe):
    """Match unique stock Start/Stop IDs per hold, never individual pulse ordinals."""
    starts = [r for r in client if r['kind'] == 'STOCK_SEND' and r.get('start') == '1']
    multiplicity = Counter(key(r) for r in starts)
    reports, paired_server_holds = [], set()
    for start in starts:
        receives = stock_peers(start, server)
        results = stock_results(receives, server)
        cb = [r for r in client if r['kind'] == 'HOLD_BEGIN' and r.get('weapon') == start.get('weapon')
              and r.get('mode') == start.get('mode') and integer(r, 'action', 0) > 0 and r.get('action') == start.get('action')]
        ch = integer(cb[0], 'hold', 0) if len(cb) == 1 else 0
        crows = local_hold_rows(client, start, ch)
        accepted = [r for r in results if r.get('accepted') == '1']
        sb = [r for r in server if r['kind'] == 'HOLD_BEGIN' and r.get('weapon') == start.get('weapon')
              and r.get('mode') == start.get('mode') and r.get('action') in {x.get('rpc') for x in accepted}]
        sh = integer(sb[0], 'hold', 0) if len(sb) == 1 else 0
        srows = local_hold_rows(server, start, sh)
        safe = (pair_safe and multiplicity[key(start)] == 1 and integer(start, 'event') in range(255)
                and ownership_consistent(start, receives + results + sb) and len(accepted) <= 1)
        status = ('ambiguous_identity' if not safe else 'missing_server_start' if not receives
                  else 'incomplete_stock_result' if len(results) != len(receives)
                  else ('accepted' if integer(accepted[0], 'applied') > 0 else 'accepted_without_sequence') if accepted else 'rejected' if all(r.get('accepted') == '0' for r in results)
                  else 'validation_not_observed')
        if safe and sh and status == 'accepted':
            paired_server_holds.add((start.get('weapon'), start.get('mode'), sh))
        stops = [r for r in client if r['kind'] == 'STOCK_SEND' and r.get('start') == '0'
                 and r.get('weapon') == start.get('weapon') and r.get('mode') == start.get('mode')
                 and ch > 0 and integer(r, 'lastHold') == ch]
        stop_evidence = []
        for stop in stops:
            peers = stock_peers(stop, server)
            stop_evidence.append(dict(send=location(stop), results=[location(r) for r in stock_results(peers, server)],
                                      receives=[location(r) for r in peers]))
        # Sync has no Start byte identity (255). List it in a bounded, server-local
        # request window; never claim these pulses belong to the rejected client hold.
        later = [r for r in server if r['kind'] == 'STOCK_RECEIVE' and r.get('origin') == 'wire'
                 and r.get('start') == '1' and r.get('weapon') == start.get('weapon')
                 and r.get('mode') == start.get('mode') and key(r) != key(start)
                 and receives and integer(r, 'seq') > integer(receives[0], 'seq')]
        low = integer(receives[0], 'seq') if receives else -1
        high = min((integer(r, 'seq') for r in later), default=10**30)
        sync = [r for r in server if safe and low >= 0 and low < integer(r, 'seq') < high
                and r.get('weapon') == start.get('weapon') and r.get('mode') == start.get('mode')
                and (r['kind'] == 'SYNC_DECISION' or r.get('origin') == 'sync')]
        reports.append(dict(start=location(start), status=status, client_hold=ch, server_hold=sh if safe else 0,
                            client_counts=pulse_counts(crows), server_counts=pulse_counts(srows) if safe else {},
                            start_receives=[location(r) for r in receives], start_results=[location(r) for r in results],
                            stops=stop_evidence, sync_window_evidence=[location(r) for r in sync],
                            note='Hold counts are observations; sync-window association is not pulse identity.'))
    return reports, paired_server_holds


def summarize_volleys(client, server, holds, pair_safe):
    reports = []
    releases = [r for r in client if r['kind'] == 'STOP_SEND' and r.get('charged') == '1']
    multiplicity = Counter(key(r) for r in releases)
    for release in releases:
        peers = [r for r in server if r['kind'] in {'STOP_RECEIVE', 'STOP_RESULT', 'STOP_APPLY'} and key(r) == key(release)]
        ch = integer(release, 'lastHold', 0)
        hold = [h for h in holds if h['client_hold'] == ch and ch > 0 and h['start'].get('weapon') == release.get('weapon')]
        sh = hold[0]['server_hold'] if len(hold) == 1 else 0
        cc = [r for r in local_hold_rows(client, release, ch) if r['kind'] == 'CHARGE_COMMIT']
        sc = [r for r in local_hold_rows(server, release, sh) if r['kind'] == 'CHARGE_COMMIT']
        safe = (pair_safe and multiplicity[key(release)] == 1 and ownership_consistent(release, peers)
                and len(cc) == 1 and len(sc) == 1 and sh > 0
                and any(r['kind'] == 'STOP_RECEIVE' for r in peers))
        def volley_rows(rows, commits):
            return [r for r in rows if len(commits) == 1 and integer(r, 'volley', 0) > 0
                    and r.get('volley') == commits[0].get('volley') and r.get('weapon') == release.get('weapon')]
        cr, sr = volley_rows(client, cc), volley_rows(server, sc)
        results = [r for r in peers if r['kind'] == 'STOP_RESULT']
        received = [r for r in peers if r['kind'] == 'STOP_RECEIVE']
        release_outcome = ('unexplained' if not safe or len(results) != len(received)
                           else 'accepted' if sum(r.get('accepted') == '1' for r in results) == 1
                           else 'rejected' if results and all(r.get('accepted') == '0' for r in results)
                           else 'unexplained')
        reports.append(dict(release=location(release), status='paired_hold_release' if safe else 'unexplained_release',
                            release_outcome=release_outcome, stop_evidence=[location(r) for r in peers], client_commits=[location(r) for r in cc],
                            server_commits=[location(r) for r in sc], client_counts=pulse_counts(cr),
                            server_counts=pulse_counts(sr) if safe else {},
                            client_ended=any(r['kind'] == 'CHARGE_END' for r in cr),
                            server_ended=any(r['kind'] == 'CHARGE_END' for r in sr) if safe else False,
                            note='Fixed Stop is a release watermark, not proof it caused the first rocket; stock Stop/automatic commit may precede it.'))
    return reports


def analyze(client_lines, server_lines, run, client_name='client.log', server_name='server.log', context=4, player=None):
    if not run or run == 'unset' or re.search(r'\s', run):
        raise ValueError('Use the same explicit, unique run label on both processes (no whitespace).')
    client_all, issues = parse(client_lines, client_name, 'client', run)
    server_all, server_issues = parse(server_lines, server_name, 'server', run)
    issues += server_issues
    ci, client_unique = check_capture(client_all, 'client')
    si, server_unique = check_capture(server_all, 'server')
    issues += ci + si
    sessions = {r.get('session') for r in client_all + server_all}
    session_safe = len(sessions) == 1 and all(re.fullmatch(r'[0-9a-f]{32}', x or '') for x in sessions)
    if not session_safe:
        issues.append('Mismatched/missing session nonce; generate a fresh nonce for each capture pair')
    engines = {r['engine'] for r in client_all + server_all if 'engine' in r}
    versions_match = len(engines) == 1
    if not versions_match:
        issues.append('Pair must come from the same engine version, not a 4.15/4.27 mixture')
    candidates = {r.get('player') for r in client_all if r.get('local') == '1' and integer(r, 'player') > 0
                  and r['kind'] in {'INPUT_PRESS', 'PREDICT', 'SEND', 'STOCK_SEND'}}
    selected = str(player) if player is not None else next(iter(candidates)) if len(candidates) == 1 else None
    if selected not in candidates:
        issues.append('No unique local client player; select a captured player with --player NETGUID')
        selected = None
    client = [r for r in client_all if r.get('player') == selected] if selected else []
    weapons = {r.get('weapon') for r in client if integer(r, 'weapon', 0) > 0}
    server = [r for r in server_all if selected and (r.get('player') == selected
              or (integer(r, 'player', 0) == 0 and r.get('weapon') in weapons))]
    lifetime_safe = selected_lifetime(client) and selected_lifetime(server)
    if not lifetime_safe:
        issues.append('Selected player reconnect or reused weapon GUID detected; split the capture')
    pair_safe = client_unique and server_unique and versions_match and session_safe and lifetime_safe
    grouped, sent, local_fakes = defaultdict(list), defaultdict(list), defaultdict(list)
    for r in server:
        if r.get('protocol') == 'fixed':
            grouped[key(r)].append(r)
    for r in client:
        if r['kind'] == 'LOCAL_PROJECTILE':
            local_fakes[key(r)].append(r)
        if r['kind'] in {'SEND', 'SEND_RETRY'}:
            sent[key(r)].append(r)
    predicted = [r for r in client if r['kind'] == 'PREDICT' and r.get('local') == '1']
    if not predicted:
        issues.append('No client predictions observed; this is not a successful shot test')
    duplicates = Counter(key(r) for r in predicted if r.get('protocol') == 'fixed')
    received_watermarks = {key(r) for r in client if r['kind'] == 'ACK_RECEIVED'}
    client_context, server_context = context_index(client), context_index(server)
    result = []
    for pred in predicted:
        peers = grouped[key(pred)] if pred.get('protocol') == 'fixed' else []
        receives = [r for r in peers if r['kind'] == 'RECEIVE']
        bits = {wire_bits(r) for r in sent[key(pred)] + receives}
        if pred.get('protocol') != 'fixed':
            outcome, reason = 'unexplained', 'continuous_or_charged_pulse_has_no_per_shot_wire_identity'
        elif any(integer(pred, k) <= 0 for k in ('weapon', 'player', 'owner')) or integer(pred, 'event') < 0:
            outcome, reason = 'unexplained', 'missing_replicated_identity'
        elif not pair_safe or duplicates[key(pred)] != 1:
            outcome, reason = 'unexplained', 'ambiguous_capture_or_prediction_identity'
        elif not ownership_consistent(pred, peers):
            outcome, reason = 'unexplained', 'unknown_or_conflicting_ownership'
        elif not receives:
            outcome, reason = 'unexplained', 'missing_server_wire_receipt' if peers else 'no_matching_server_request'
        elif sum(r['kind'] == 'SEND' for r in sent[key(pred)]) != 1 or None in bits or len(bits) != 1:
            outcome, reason = 'unexplained', 'wire_timestamp_missing_or_mismatched'
        else:
            outcome, reason = classify(pred, peers)
        item = {'outcome': outcome, 'reason': reason, 'prediction': location(pred),
                'server_evidence': [location(r) for r in peers],
                'local_projectile_evidence': [location(r) for r in local_fakes[key(pred)]],
                'ack_sent': any(r['kind'] == 'ACK_SENT' for r in peers),
                'exact_watermark_received': key(pred) in received_watermarks,
                'client_context': nearby(client_context, pred, context)}
        item['server_context'] = nearby(server_context, peers[0], context) if peers else []
        result.append(item)
    counts = dict(Counter(r['outcome'] for r in result))
    stream_counts = []
    for side, rows in [('client', client), ('server', server)]:
        groups = Counter((r.get('class'), r.get('mode'), r['kind']) for r in rows
                         if r.get('protocol') in {'stock', 'stream', 'beam'}
                         and r['kind'] in {'PREDICT', 'DISPATCH', 'BEAM_SAMPLE', 'PROJECTILE', 'HITSCAN', 'STOCK_RECEIVE', 'STOCK_RESULT'})
        stream_counts += [dict(side=side, weapon_class=c, mode=m, kind=k, count=n)
                          for (c, m, k), n in sorted(groups.items(), key=lambda x: str(x[0]))]
    holds, paired_holds = summarize_holds(client, server, pair_safe)
    volleys = summarize_volleys(client, server, holds, pair_safe)
    observed_keys = {key(r) for r in predicted if r.get('protocol') == 'fixed'}
    server_only = [location(r) for r in server
                   if ((r.get('protocol') == 'fixed' and r['kind'] in {'ACCEPT', 'DISPATCH', 'CANCEL', 'DIRECT_SPAWN'}
                        and key(r) not in observed_keys)
                       or (r['kind'] == 'DISPATCH' and r.get('protocol') != 'fixed'
                           and (r.get('weapon'), r.get('mode'), integer(r, 'hold', 0) or integer(r, 'volleyHold', 0)) not in paired_holds))]
    return {'schema': 2, 'run': run, 'selected_player': selected, 'capture_complete': not issues,
            'status': 'REVIEW_REQUIRED' if issues or server_only or any(r['outcome'] != 'fired' for r in result) else 'ALL_NUMBERED_PREDICTIONS_FIRED',
            'counts': counts, 'issues': issues, 'shots': result, 'continuous_observations': stream_counts,
            'holds': holds, 'volleys': volleys, 'server_only_events': server_only,
            'layouts': [location(r) for r in client + server if r['kind'] == 'LAYOUT'],
            'network_samples': [location(r) for r in client_all + server_all if r['kind'] == 'NETWORK'],
            'limits': ['Fired means hitscan execution/projectile spawn, not damage confirmation.',
                       'ACKs are watermarks, not proof of a fired shot.',
                       'Stream/beam/charged pulse ordinals are local only; hold/volley counts cannot prove a one-to-one server outcome.',
                       'Raw network counters are driver-wide: inPacketsRaw/inLostRaw reset with stats except Shipping clients; outOfOrderTotal is cumulative.',
                       'A nonce is operator-supplied, not authenticated: never reuse it across connections or sessions.']}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--run', required=True)
    parser.add_argument('--context', type=int, default=4, help='Local events before/after each unmatched prediction (0..20)')
    parser.add_argument('--player', type=int, help='Local client PlayerState NetGUID (required for multiple local players)')
    parser.add_argument('--json', action='store_true', help='Print every prediction, evidence, and context as JSON')
    args = parser.parse_args(argv)
    if not 0 <= args.context <= 20:
        parser.error('--context must be 0..20')
    try:
        report = analyze(read_log(args.client), read_log(args.server), args.run,
                         str(args.client), str(args.server), args.context, args.player)
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
        for row in report['server_only_events']:
            print(f"SERVER-ONLY {row['file']}:{row['line']} {row['kind']} weapon={row.get('weapon')} mode={row.get('mode')} event={row.get('event')} protocol={row.get('protocol')}")
        for hold in report['holds']:
            print('HOLD', hold['status'], 'byte=' + hold['start'].get('event', '?'), hold['client_counts'], hold['server_counts'])
            for stop in hold['stops']:
                print('  STOP', stop['send'].get('event'), [(r.get('accepted'), r.get('applied')) for r in stop['results']])
            for row in hold['sync_window_evidence']:
                print(f"  SYNC {row['file']}:{row['line']} {row['kind']} incoming={row.get('incoming')} accepted={row.get('accepted')}")
        for volley in report['volleys']:
            print('VOLLEY', volley['status'], volley['release_outcome'], 'release=' + volley['release'].get('event', '?'), volley['client_counts'], volley['server_counts'])
        for note in report['limits']:
            print(note)
    return 0 if report['status'] == 'ALL_NUMBERED_PREDICTIONS_FIRED' else 1


if __name__ == '__main__':
    sys.exit(main())
