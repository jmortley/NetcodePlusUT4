"""Regression cases from the paired-diagnostics audit; no Unreal runtime implied."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_fire_trace import analyze, log, CLIENT, FIRED, SESSION
from test_fire_provenance import report as legacy_report, row, codes


def stock(kind, **values):
    return kind, dict(dict(protocol='stock', generation=0, event=-1), **values)


def hold_capture(accepted=1):
    # A synchronous first pulse precedes the client's STOCK_SEND/QueueResendFire.
    client = [stock('HOLD_BEGIN', hold=8, lastHold=8, action=7),
              ('PREDICT', dict(protocol='stream', event=-1, hold=8, action=7)),
              stock('STOCK_SEND', start=1, **{'event': 12}, action=7, hold=8, lastHold=8),
              stock('HOLD_END', hold=8, lastHold=8, action=9),
              stock('STOCK_SEND', start=0, **{'event': 13}, action=9, hold=0, lastHold=8)]
    server = [stock('STOCK_RECEIVE', **{'event': 12}, start=1, rpc=90, origin='wire', action=90),
              stock('STOCK_VALIDATE', **{'event': 12}, rpc=90, accepted=accepted, byteBefore=30, byteAfter=12 if accepted else 30)]
    if accepted:
        server += [stock('HOLD_BEGIN', hold=91, lastHold=91, action=90),
                   ('DISPATCH', dict(protocol='stream', event=-1, hold=91))]
    server += [stock('STOCK_RESULT', **{'event': 12}, start=1, rpc=90, origin='wire',
                     accepted=accepted, applied=accepted, applyResult=accepted),
               stock('STOCK_RECEIVE', **{'event': 13}, start=0, rpc=94, origin='wire'),
               stock('STOCK_RESULT', **{'event': 13}, start=0, rpc=94, origin='wire', accepted=0, applied=0, applyResult=-1)]
    return client, server


class ReviewRegressionTests(unittest.TestCase):
    def run_pair(self, client=CLIENT, server=FIRED, **kwargs):
        return analyze(log('client', client), log('server', server), 'test', **kwargs)

    def test_stock_firing_state_keeps_fixed_request(self):
        server = [('LAYOUT', dict(protocol='state', event=-1, firingState='UTWeaponStateFiring'))] + FIRED
        result = self.run_pair(server=server)
        self.assertEqual(result['counts'], {'fired': 1})
        self.assertEqual(result['layouts'][0]['firingState'], 'UTWeaponStateFiring')

    def test_different_wire_timestamp_cannot_pass(self):
        server = [(k, dict(v, clientT='2', clientBits='40000000') if k == 'RECEIVE' else v) for k, v in FIRED]
        result = self.run_pair(server=server)
        self.assertEqual(result['shots'][0]['reason'], 'wire_timestamp_missing_or_mismatched')
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')

    def test_inconsistent_decimal_and_bits_cannot_pass(self):
        server = [(k, dict(v, clientT='2') if k == 'RECEIVE' else v) for k, v in FIRED]
        self.assertEqual(self.run_pair(server=server)['counts'], {'unexplained': 1})

    def test_missing_original_send_cannot_pass(self):
        self.assertEqual(self.run_pair(client=CLIENT[1:])['counts'], {'unexplained': 1})

    def test_missing_receive_cannot_certify_wire_equality(self):
        result = self.run_pair(server=FIRED[1:])
        self.assertEqual(result['shots'][0]['reason'], 'missing_server_wire_receipt')
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')

    def test_duplicate_original_send_cannot_pass(self):
        result = self.run_pair(client=[('SEND', {})] + CLIENT)
        self.assertEqual(result['counts'], {'unexplained': 1})

    def test_unknown_connection_lifetime_cannot_pass(self):
        server = [(k, dict(v, connection=0)) for k, v in FIRED]
        self.assertEqual(self.run_pair(server=server)['counts'], {'unexplained': 1})

    def test_mismatched_session_nonce_cannot_pass(self):
        server = [s.replace(SESSION, 'a' * 32) for s in log('server', FIRED)]
        result = analyze(log('client', CLIENT), server, 'test')
        self.assertFalse(result['capture_complete'])
        self.assertEqual(result['counts'], {'unexplained': 1})

    def test_missing_nonce_cannot_pass(self):
        server = [s.replace('session=' + SESSION, 'session=unset') for s in log('server', FIRED)]
        self.assertEqual(analyze(log('client', CLIENT), server, 'test')['status'], 'REVIEW_REQUIRED')

    def test_reconnect_same_driver_new_connection_cannot_pass(self):
        result = self.run_pair(server=FIRED + [('STATE_CHANGED', dict(connection=99))])
        self.assertEqual(result['counts'], {'unexplained': 1})

    def test_reused_guid_different_local_object_cannot_pass(self):
        result = self.run_pair(server=FIRED + [('STATE_CHANGED', dict(weaponLocal=999))])
        self.assertEqual(result['counts'], {'unexplained': 1})

    def test_other_players_and_bots_filtered(self):
        extra = [('DISPATCH', dict(protocol='stream', event=-1, player=999, owner=888, weapon=777, connection=999)),
                 ('BEAM_SAMPLE', dict(protocol='beam', player=999, owner=888, weapon=777))]
        result = self.run_pair(server=FIRED + extra)
        self.assertEqual(result['status'], 'ALL_NUMBERED_PREDICTIONS_FIRED')
        self.assertEqual(result['continuous_observations'], [])
        self.assertEqual(result['server_only_events'], [])

    def test_multiple_local_players_require_selection(self):
        client = CLIENT + [('PREDICT', dict(player=99, owner=88, weapon=222))]
        self.assertEqual(self.run_pair(client=client)['status'], 'REVIEW_REQUIRED')
        self.assertEqual(self.run_pair(client=client, player=33)['counts'], {'fired': 1})

    def test_retained_owner_trade_grace_is_fired(self):
        server = [('RECEIVE', dict(identity='retained')),
                  ('DIRECT_SPAWN', dict(identity='retained', result='ok', source='trade_grace'))]
        self.assertEqual(self.run_pair(server=server)['counts'], {'fired': 1})

    def test_retained_owner_grace_rejection_is_rejected(self):
        server = [('RECEIVE', dict(identity='retained')),
                  ('REJECT', dict(identity='retained', reason='owner_lost_grace'))]
        self.assertEqual(self.run_pair(server=server)['counts'], {'rejected': 1})

    def test_unknown_owner_preserves_evidence_without_claiming_match(self):
        server = [('RECEIVE', dict(owner=0, player=0)), ('REJECT', dict(owner=0, player=0, reason='owner_lost_grace'))]
        result = self.run_pair(server=server)
        self.assertEqual(result['shots'][0]['reason'], 'unknown_or_conflicting_ownership')
        self.assertEqual(len(result['shots'][0]['server_evidence']), 2)

    def test_server_only_stream_on_fixed_weapon_is_listed(self):
        result = self.run_pair(server=FIRED + [('DISPATCH', dict(protocol='stream', event=-1, generation=0))])
        self.assertEqual(len(result['server_only_events']), 1)
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')

    def test_stock_acceptance_not_inferred_from_state_change(self):
        client, server = hold_capture(0)
        server += [('STATE_CHANGED', dict(protocol='state', state='Firing', held0=1))]
        result = self.run_pair(client, server)
        self.assertEqual(result['holds'][0]['status'], 'rejected')
        self.assertEqual(result['holds'][0]['server_counts'].get('DISPATCH', 0), 0)
        self.assertEqual(result['holds'][0]['stops'][0]['results'][0]['accepted'], '0')

    def test_stock_hold_counts_first_synchronous_pulse(self):
        client, server = hold_capture()
        result = self.run_pair(client, server)
        self.assertEqual(result['holds'][0]['status'], 'accepted')
        self.assertEqual(result['holds'][0]['client_counts']['PREDICT'], 1)
        self.assertEqual(result['holds'][0]['server_counts']['DISPATCH'], 1)
        self.assertFalse(result['server_only_events'])
        self.assertEqual(result['shots'][0]['outcome'], 'unexplained')  # Still no per-pulse identity.

    def test_sync_recovery_is_not_mistaken_for_wire_acceptance(self):
        client, server = hold_capture(0)
        server += [stock('SYNC_DECISION', incoming=1, pendingBit=0, filtered=0),
                   stock('STOCK_RECEIVE', **{'event': 255}, start=1, rpc=98, origin='sync'),
                   stock('STOCK_RESULT', **{'event': 255}, start=1, rpc=98, origin='sync', accepted=1, applied=1)]
        result = self.run_pair(client, server)
        self.assertEqual(result['holds'][0]['status'], 'rejected')
        self.assertEqual(len(result['holds'][0]['sync_window_evidence']), 3)

    def test_stock_byte_reuse_is_ambiguous(self):
        client, server = hold_capture()
        client += [stock('STOCK_SEND', **{'event': 12}, start=1, action=999, hold=1000, lastHold=1000)]
        result = self.run_pair(client, server)
        self.assertTrue(all(h['status'] == 'ambiguous_identity' for h in result['holds']))

    def test_delayed_fake_cancellation_is_attached(self):
        client = CLIENT + [('LOCAL_PROJECTILE', dict(protocol='local', reason='scheduled', reservation=8)),
                           ('LOCAL_PROJECTILE', dict(protocol='local', reason='ack_cancel', reservation=8))]
        result = self.run_pair(client=client)
        self.assertEqual([r['reason'] for r in result['shots'][0]['local_projectile_evidence']], ['scheduled', 'ack_cancel'])

    def test_empty_capture_has_end_but_is_not_a_successful_test(self):
        result = self.run_pair([], [])
        self.assertEqual(result['status'], 'REVIEW_REQUIRED')
        self.assertFalse(any('missing final' in s for s in result['issues']))

    def test_direct_toggle_auto_begin_is_incomplete(self):
        client = [s.replace('explicit=1', 'explicit=0') for s in log('client', CLIENT)]
        self.assertFalse(analyze(client, log('server', FIRED), 'test')['capture_complete'])

    def test_aborted_capture_cannot_pass_even_with_end(self):
        result = self.run_pair(client=CLIENT + [('ABORT', dict(reason='toggle_off'))])
        self.assertFalse(result['capture_complete'])

    def test_stored_cpp_format_lines(self):
        folder = Path(__file__).parent / 'fixtures/fire_trace_v2'
        result = analyze((folder / 'client.log').read_text().splitlines(),
                         (folder / 'server.log').read_text().splitlines(), 'golden')
        self.assertEqual(result['status'], 'ALL_NUMBERED_PREDICTIONS_FIRED')
        evidence = result['shots'][0]['server_evidence']
        self.assertNotIn('scope', next(r for r in evidence if r['kind'] == 'ACCEPT'))
        self.assertEqual(next(r for r in evidence if r['kind'] == 'RECEIVE')['generation'], '0')

    def test_charged_volley_can_precede_fixed_stop(self):
        client, server = hold_capture()
        # Stock release/automatic release can commit before the fixed Stop is sent/received.
        client += [('CHARGE_COMMIT', dict(protocol='stream', event=-1, volley=31, volleyHold=8)),
                   ('PROJECTILE', dict(protocol='stream', event=-1, volley=31, volleyHold=8, result='ok')),
                   ('CHARGE_END', dict(protocol='stream', event=-1, volley=31, volleyHold=8)),
                   ('STOP_SEND', dict(protocol='fixed_stop', event=51, charged=1, lastHold=8))]
        server += [('CHARGE_COMMIT', dict(protocol='stream', event=-1, volley=101, volleyHold=91)),
                   ('PROJECTILE', dict(protocol='stream', event=-1, volley=101, volleyHold=91, result='ok')),
                   ('CHARGE_END', dict(protocol='stream', event=-1, volley=101, volleyHold=91)),
                   ('STOP_RECEIVE', dict(protocol='fixed_stop', event=51, lastHold=91)),
                   ('STOP_RESULT', dict(protocol='fixed_stop', event=51, lastHold=91, accepted=1))]
        result = self.run_pair(client, server)
        volley = result['volleys'][0]
        self.assertEqual(volley['status'], 'paired_hold_release')
        self.assertEqual(volley['client_counts']['successful_projectiles'], 1)
        self.assertEqual(volley['server_counts']['successful_projectiles'], 1)
        self.assertTrue(volley['server_ended'])
        client += [('STOP_SEND', dict(protocol='fixed_stop', event=51, charged=1, lastHold=8))]
        self.assertTrue(all(v['status'] == 'unexplained_release' for v in self.run_pair(client, server)['volleys']))

    def test_new_cancellations_keep_legacy_exit_code_and_are_visible(self):
        for reason in ('switch_before_commit', 'inactive', 'waiting_state_left'):
            with self.subTest(reason=reason):
                result = legacy_report(row('ACCEPT'), row('CANCEL', reason=reason))
                self.assertEqual(result.exit_code, 0)
                self.assertIn('CLIENT_VISIBLE_CANCEL', codes(result))

    def test_cli_lists_server_only_dispatch(self):
        with tempfile.TemporaryDirectory() as folder:
            client, server = Path(folder) / 'c.log', Path(folder) / 's.log'
            client.write_text('\n'.join(log('client', CLIENT)))
            server.write_text('\n'.join(log('server', FIRED + [('DISPATCH', dict(protocol='stream', event=-1))])))
            process = subprocess.run([sys.executable, '-B', str(Path(__file__).parents[1] / 'check-fire-trace.py'),
                                      '--client', str(client), '--server', str(server), '--run', 'test'], capture_output=True, text=True)
            self.assertEqual(process.returncode, 1)
            self.assertIn('SERVER-ONLY', process.stdout)


if __name__ == '__main__':
    unittest.main()
