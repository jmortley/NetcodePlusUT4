"""Paired-shot evidence tests: a received/ACKed request is not a fired shot."""
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

analyze = runpy.run_path(str(Path(__file__).parents[1] / 'check-fire-trace.py'))['analyze']


def log(side, events, *, end=True, overrides=None):
    rows = []
    for seq, (kind, extra) in enumerate(events, 1):
        values = dict(schema=1, run='test', capture=side, seq=seq, side=side,
                      world=1 if side == 'client' else 12, driver=2 if side == 'client' else 25,
                      weapon=111, player=33, owner=77, mode=0, event=42,
                      generation=0 if side == 'client' else 7, protocol='fixed',
                      engine='4.27', build='fire-trace-v1', scope=20 if side == 'client' else 40,
                      actor='local_' + side, **(overrides or {}))
        values.update(extra)
        rows.append('[NCFireTrace] ' + kind + ' ' + ' '.join(f'{k}={v}' for k, v in values.items()))
    if end:
        rows.append(f'[NCFireTrace] END schema=1 run=test capture={side} seq={len(rows)+1} limited=0')
    return rows


CLIENT = [('PREDICT', {}), ('DISPATCH_END', dict(traces=1, damagingTraces=1, projectiles=0))]
FIRED = [('RECEIVE', {}), ('ACCEPT', {}), ('DISPATCH', {}),
         ('HITSCAN', dict(damagePath=1, blocking=0)),
         ('DISPATCH_END', dict(traces=1, damagingTraces=1, projectiles=0)), ('ACK_SENT', {})]


class PairedFireTests(unittest.TestCase):
    def report(self, server=FIRED, client=CLIENT, **kwargs):
        return analyze(log('client', client), log('server', server), 'test', **kwargs)

    def outcome(self, server=FIRED, client=CLIENT):
        return self.report(server, client)['shots'][0]['outcome']

    def test_hitscan_miss_still_fired(self):
        result = self.report()
        self.assertEqual(result['counts'], {'fired': 1})
        self.assertTrue(result['capture_complete'])

    def test_ack_is_not_commit(self):
        self.assertEqual(self.outcome([('RECEIVE', {}), ('ACCEPT', {}), ('ACK_SENT', {})]), 'unexplained')

    def test_accepted_acked_cancel_is_visible(self):
        result = self.report([('RECEIVE', {}), ('ACCEPT', {}), ('ACK_SENT', {}),
                              ('CANCEL', dict(reason='switch_before_commit'))])
        self.assertEqual(result['counts'], {'cancelled': 1})
        self.assertTrue(result['shots'][0]['ack_sent'])
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')

    def test_stale_equip_reject(self):
        self.assertEqual(self.outcome([('RECEIVE', {}), ('REJECT', dict(reason='stale_lifetime'))]), 'rejected')

    def test_retry_reject_does_not_erase_fired(self):
        self.assertEqual(self.outcome(FIRED + [('RECEIVE', {}), ('REJECT', dict(reason='sequence'))]), 'fired')

    def test_late_unresolved_retry_not_called_rejected(self):
        self.assertEqual(self.outcome([('RECEIVE', {}), ('REJECT', dict(reason='rate')), ('RECEIVE', {})]), 'unexplained')

    def test_ack_only_unmatched(self):
        self.assertEqual(self.outcome([('ACK_SENT', {})]), 'unexplained')

    def test_projectile_pellets_count_as_one_shot(self):
        server = FIRED[:3] + [('PROJECTILE', dict(result='ok'))] * 9
        server += [('DISPATCH_END', dict(projectiles=9, traces=0, damagingTraces=0))]
        self.assertEqual(self.outcome(server), 'fired')

    def test_null_projectile_is_not_fired(self):
        server = FIRED[:3] + [('PROJECTILE', dict(result='null')),
                              ('DISPATCH_END', dict(projectiles=0, traces=0, damagingTraces=0))]
        self.assertEqual(self.outcome(server), 'unexplained')

    def test_missing_spawn_record_is_not_proof(self):
        server = FIRED[:3] + [('DISPATCH_END', dict(projectiles=1, traces=0, damagingTraces=0))]
        self.assertEqual(self.outcome(server), 'unexplained')

    def test_trace_without_damage_path_is_not_authority_shot(self):
        server = FIRED[:3] + [('HITSCAN', dict(damagePath=0)),
                              ('DISPATCH_END', dict(projectiles=0, traces=1, damagingTraces=0))]
        self.assertEqual(self.outcome(server), 'unexplained')

    def test_no_generation_no_commit_proof(self):
        server = [(kind, {**extra, 'generation': 0}) for kind, extra in FIRED]
        self.assertEqual(self.outcome(server), 'unexplained')

    def test_streams_have_no_invented_match(self):
        client = [('PREDICT', dict(protocol='stream', event=-1))]
        self.assertEqual(self.outcome(FIRED, client), 'unexplained')

    def test_beam_pulse_remains_observation(self):
        result = self.report(client=[('PREDICT', dict(protocol='beam', event=-1))])
        self.assertEqual(result['counts'], {'unexplained': 1})
        self.assertTrue(result['continuous_observations'])

    def test_zero_guid_is_not_matched_by_actor_name(self):
        client = [('PREDICT', dict(weapon=0))]
        self.assertEqual(self.outcome(client=client), 'unexplained')

    def test_respawn_identity_is_not_reused(self):
        self.assertEqual(self.outcome(client=[('PREDICT', dict(owner=999))]), 'unexplained')

    def test_different_player_is_not_joined(self):
        self.assertEqual(self.outcome(client=[('PREDICT', dict(player=999))]), 'unexplained')

    def test_two_acceptances_not_collapsed(self):
        self.assertEqual(self.outcome(FIRED + [('ACCEPT', dict(generation=8))]), 'unexplained')

    def test_duplicate_client_event_ambiguous(self):
        self.assertEqual(self.outcome(client=CLIENT + [('PREDICT', dict(scope=21))]), 'unexplained')

    def test_truncated_dispatch(self):
        self.assertEqual(self.outcome(FIRED[:4]), 'unexplained')

    def test_missing_end_requires_review_even_for_verified_shot(self):
        result = analyze(log('client', CLIENT), log('server', FIRED, end=False), 'test')
        self.assertEqual(result['counts'], {'fired': 1})
        self.assertFalse(result['capture_complete'])
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')

    def test_log_sequence_gap(self):
        server = log('server', FIRED)
        server.pop(0)
        result = analyze(log('client', CLIENT), server, 'test')
        self.assertFalse(result['capture_complete'])

    def test_limit_is_incomplete(self):
        result = self.report(FIRED + [('LIMIT', {})])
        self.assertFalse(result['capture_complete'])

    def test_reconnect_capture_ambiguous(self):
        result = self.report(FIRED + [('STATE_CHANGED', dict(driver=999))])
        self.assertEqual(result['counts'], {'unexplained': 1})

    def test_engine_mismatch(self):
        client = [(kind, {**extra, 'engine': '4.15'}) for kind, extra in CLIENT]
        self.assertEqual(self.outcome(client=client), 'unexplained')

    def test_clock_offsets_not_matching_criteria(self):
        server = [(kind, {**extra, 't': 5000, 'mono': 40000}) for kind, extra in FIRED]
        client = [(kind, {**extra, 't': 2, 'mono': 20}) for kind, extra in CLIENT]
        self.assertEqual(self.outcome(server, client), 'fired')

    def test_pending_weapon_context_included(self):
        client = [('SWITCH_ATTEMPT', dict(weapon=222)), *CLIENT,
                  ('EQUIP_BEGIN', dict(weapon=333))]
        context = self.report(client=client)['shots'][0]['client_context']
        self.assertEqual([c['weapon'] for c in context], ['222', '333'])

    def test_cancel_after_dispatch_is_conflicting(self):
        self.assertEqual(self.outcome(FIRED + [('CANCEL', dict(reason='unexpected'))]), 'unexplained')

    def test_no_predicted_shots_not_pass(self):
        self.assertEqual(self.report(client=[])['status'], 'REVIEW_REQUIRED')

    def test_unset_label_rejected(self):
        with self.assertRaises(ValueError):
            analyze([], [], 'unset')

    def test_malformed_row_requires_review(self):
        server = log('server', FIRED)
        server[0] += ' event=42'
        result = analyze(log('client', CLIENT), server, 'test')
        self.assertFalse(result['capture_complete'])

    def test_higher_ack_is_not_exact_receipt(self):
        result = self.report(client=CLIENT + [('ACK_RECEIVED', dict(event=43))])
        self.assertFalse(result['shots'][0]['exact_watermark_received'])

    def test_other_run_excluded(self):
        server = log('server', FIRED)
        server += [s.replace('run=test', 'run=unrelated') for s in log('server', FIRED)]
        result = analyze(log('client', CLIENT), server, 'test')
        self.assertEqual(result['counts'], {'fired': 1})
        self.assertTrue(result['capture_complete'])

    def test_utf16_cli_and_old_entry_point(self):
        with tempfile.TemporaryDirectory() as folder:
            client, server = Path(folder) / 'client.log', Path(folder) / 'server.log'
            client.write_text('\n'.join(log('client', CLIENT)), encoding='utf-16')
            server.write_text('\n'.join(log('server', FIRED)), encoding='utf-8-sig')
            tool = Path(__file__).parents[1] / 'check-fire-provenance.py'
            process = subprocess.run([sys.executable, '-B', str(tool), '--client', str(client),
                                      '--server', str(server), '--run', 'test', '--json'],
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr + process.stdout)
            self.assertIn('"fired": 1', process.stdout)


if __name__ == '__main__':
    unittest.main()
