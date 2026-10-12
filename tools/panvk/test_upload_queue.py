#!/usr/bin/env python3
"""Bounded upload queue decomposition and exact transport attribution, no GPU."""
import unittest
import analyze_frame_journal as journal
import test_shm_decomposition as fixture

# Reuse only the fixture helpers, without rediscovering its test methods.
class UploadQueueTests(unittest.TestCase):
    setUp = fixture.ShmDecompositionTests.setUp
    event = fixture.ShmDecompositionTests.event
    add = fixture.ShmDecompositionTests.add
    paths = fixture.ShmDecompositionTests.paths
    analyze = fixture.ShmDecompositionTests.analyze

    def details(self):
        for name,start,end in [
            ('proxy_prepare',92,93),('proxy_lock',92.2,92.3),
            ('proxy_lock',93,93.1),('proxy_mirror',93.2,98),
            ('proxy_bulk_setup',93.3,93.4),('proxy_bulk_pack',93.5,94),
            ('proxy_bulk_rpc',94,97),('rpc_exchange',94.05,97),('rpc_header',94.1,94.2),
            ('rpc_send',94.3,94.5),('rpc_receive',94.6,96.9),
            ('rpc_decode',96.9,97),('proxy_bulk_ack',97.1,97.3),
            ('proxy_submit_pack',98.2,98.4),('proxy_submit_rpc',98.5,102),
            ('rpc_exchange',98.7,101.6),('proxy_lock',98.6,98.7),('proxy_submit_pack',98.5,98.6),
            ('rpc_header',98.7,98.8),('rpc_send',98.9,99),
            ('rpc_receive',99,101.5),('rpc_decode',101.5,101.6),
            ('proxy_reply_handle',101.6,102),('proxy_finalize',102.2,102.8),
        ]:
            self.add(name,start,end,scope=31)

    def test_new_stage_names_pair_and_cpu(self):
        for name in journal.UPLOAD_QUEUE_CHILD_STAGES+journal.BROKER_STAGES:
            a=self.event(name+'_begin',1,pid=1,role='gamescope',scope=31)
            b=self.event(name+'_end',5,pid=1,role='gamescope',scope=31)
            spans,issues=journal.pair_spans([a,b])
            self.assertEqual(issues,[])
            self.assertEqual(spans[0]['cpu_ns'],2_000_000)
            self.assertEqual(spans[0]['off_cpu_ns'],2_000_000)
            self.assertEqual(spans[0]['attempt'],8)

    def test_nested_union_remainder_and_summaries(self):
        self.details()
        out=self.analyze()
        d=out['upload_queue_decomposition']
        self.assertEqual(d['remainder_samples'],1)
        self.assertAlmostEqual(d['total_queue_mean_ms'],11)
        self.assertAlmostEqual(d['covered_queue_mean_ms'],10.2)
        self.assertAlmostEqual(d['unattributed_inside_queue_mean_ms'],0.8)
        self.assertAlmostEqual(d['before_submit_rpc_mean_ms'],6.7)
        self.assertGreater(sum(d['stage_inclusive_mean_ms'].values()),11)
        summary=out['stage_spans']['gamescope/rpc_receive']
        self.assertEqual(summary['count'],2)
        self.assertEqual(summary['cpu_samples'],2)
        self.assertAlmostEqual(summary['paired_wall_mean_ms'],2.4)
        self.assertAlmostEqual(summary['off_cpu_mean_ms'],1.2)

    def test_unpaired_child_disables_remainder(self):
        self.details();self.game=[e for e in self.game if e['stage']!='proxy_bulk_ack_end']
        out=self.analyze()
        self.assertEqual(out['upload_queue_decomposition']['remainder_samples'],0)
        self.assertTrue(any(i['reason']=='unpaired begin' for i in out['span_diagnostics']))

    def test_loss_and_unsampled_captures_unavailable(self):
        self.assertEqual(self.analyze()['upload_queue_decomposition']['remainder_samples'],0)
        self.details()
        self.assertEqual(self.analyze(dropped=1)['upload_queue_decomposition']['remainder_samples'],0)

    def test_crossing_spans_rejected(self):
        self.details();self.add('rpc_send',95,98.1,scope=31)
        self.assertEqual(self.analyze()['upload_queue_decomposition']['remainder_samples'],0)

    def test_child_crossing_parent_rejected(self):
        self.details();self.add('rpc_send',102.9,103.1,scope=31)
        self.assertEqual(self.analyze()['upload_queue_decomposition']['remainder_samples'],0)

    def transport_fixture(self):
        self.details()
        # Published output R anchors the client/native connection namespaces.
        self.game.append(self.event('proxy_submit_link',115,scope=31,object=50,value=100,
                                    key=7,frame=1,fence=455))
        self.add('proxy_submit_rpc',115,118,scope=31,object=50,value=100,key=7,frame=1,fence=455)
        for edge,ms in [('begin',116),('end',117)]:
            self.native.append(self.event('vk_submit_'+edge,ms,tid=22,attempt=0,client=0,scope=72,
                                          object=50,value=100,key=7,frame=0,fence=455))
        # The U submit has no key or fence; shared semaphore/value identifies it.
        self.game.append(self.event('proxy_submit_link',98.4,scope=31,object=50,value=99))
        for row in self.game:
            if row['stage'].startswith('proxy_submit_rpc_') and row['mono_ns']<110_000_000:
                row.update(object=50,value=99)
            if row['stage'].startswith('proxy_bulk_rpc_'):
                row.update(object=60,value=524288)
        for name,begin,end,obj,value in [
            ('broker_receive',99.01,99.08,101,28),
            ('broker_dispatch',99.1,101.1,50,99),('vk_submit',99.3,100.8,50,99),
            ('broker_reply',101.2,102.1,50,99),  # finish can race client read
            ('broker_receive',94.55,94.65,100,524308),
            ('broker_dispatch',94.7,96.5,60,524288),
            ('broker_mapped_copy',95,96,60,524288),('broker_reply',96.6,96.8,60,524288),
        ]:
            for edge,ms in [('begin',begin),('end',end)]:
                self.native.append(self.event(name+'_'+edge,ms,tid=22,attempt=0,client=0,scope=72,
                                              object=obj,value=value))

    def test_namespace_timeline_and_bulk_containment_join(self):
        self.transport_fixture();out=self.analyze()
        self.assertEqual(out['proxy_native_correlation']['connection_scope_map'],{31:72})
        native=out['frames'][0]['spans']
        upload=[s for s in native if s['role']=='android' and s['stage']=='vk_submit' and s['value']==99]
        self.assertEqual(len(upload),1)
        self.assertEqual(upload[0]['joined_frame'],1)
        copies=[s for s in native if s['stage']=='broker_mapped_copy']
        self.assertEqual(len(copies),1)
        d=out['upload_queue_decomposition']
        self.assertAlmostEqual(d['broker_stage_inclusive_mean_ms']['vk_submit'],1.5)
        self.assertAlmostEqual(d['rpc_wall_mean_ms'],6.5)
        self.assertAlmostEqual(d['observed_broker_inside_rpc_mean_ms'],4.17)
        self.assertAlmostEqual(d['rpc_outside_observed_broker_mean_ms'],2.33)
        # Native work is concurrent with local waits, never added to coverage.
        self.assertAlmostEqual(d['unattributed_inside_queue_mean_ms'],0.8)
        self.assertIn('android/broker_reply',out['stage_spans'])
        self.assertEqual(d['broker_complete_samples'],1)

    def test_different_native_namespace_not_joined(self):
        self.transport_fixture()
        for row in self.native:
            if row['object']==60: row['scope']=73
        out=self.analyze()
        self.assertNotIn('android/broker_mapped_copy',out['stage_spans'])

    def test_duplicate_bulk_rpc_stays_unassigned(self):
        self.transport_fixture()
        # Different thread, same resource/range and interval: ambiguous.
        self.add('proxy_bulk_rpc',94,97,scope=31,object=60,value=524288,tid=12)
        self.assertNotIn('android/broker_mapped_copy',self.analyze()['stage_spans'])

    def test_ambiguous_timeline_link_stays_unassigned(self):
        self.transport_fixture()
        self.game.append(self.event('proxy_submit_link',98.45,scope=31,object=50,value=99))
        out=self.analyze()
        self.assertFalse(any(s['role']=='android' and s['stage']=='vk_submit' and s['value']==99
                             for s in out['frames'][0]['spans']))

    def test_no_anchor_no_native_upload_join(self):
        self.transport_fixture()
        self.game=[e for e in self.game if not(e['stage']=='proxy_submit_link' and e['value']==100)]
        out=self.analyze()
        self.assertEqual(out['proxy_native_correlation']['connection_scope_map'],{})
        self.assertEqual(out['upload_queue_decomposition']['broker_complete_samples'],0)
        self.assertIsNone(out['upload_queue_decomposition']['observed_broker_inside_rpc_mean_ms'])
        self.assertNotIn('android/broker_mapped_copy',out['stage_spans'])

    def test_wrong_bulk_offset_not_joined(self):
        self.transport_fixture()
        for row in self.native:
            if row['object']==60: row['value']+=512
        self.assertNotIn('android/broker_mapped_copy',self.analyze()['stage_spans'])

    def test_conflicting_connection_anchors_rejected(self):
        self.transport_fixture()
        anchor=next(e for e in self.game if e['stage']=='proxy_submit_link' and e['value']==100)
        self.game.append(dict(anchor,scope=32))
        self.assertEqual(self.analyze()['proxy_native_correlation']['connection_scope_map'],{})

    def test_partial_broker_dispatch_is_not_complete_coverage(self):
        self.transport_fixture()
        for row in self.native:
            if row['stage']=='broker_dispatch_begin' and row['object']==60:
                row['mono_ns']=93_900_000
        d=self.analyze()['upload_queue_decomposition']
        self.assertEqual(d['broker_complete_samples'],0)
        self.assertIsNone(d['observed_broker_inside_rpc_mean_ms'])

    def test_foreign_attempt_cannot_supply_queue_children(self):
        self.details()
        for row in self.game:
            if row['stage'].rsplit('_',1)[0] in journal.UPLOAD_QUEUE_CHILD_STAGES:
                row['attempt']=16
        self.assertEqual(self.analyze()['upload_queue_decomposition']['remainder_samples'],0)

    def test_body_receive_binds_by_dispatch_order(self):
        self.transport_fixture()
        out=self.analyze()
        body=[s for s in out['frames'][0]['spans'] if s['stage']=='broker_receive']
        self.assertEqual(len(body),2)
        self.assertEqual({s['value'] for s in body},{28,524308})
        event=next(e for e in out['frames'][0]['events'] if e['stage']=='broker_receive_begin')
        self.assertIn('immediately following',event['identity_source'])

    def test_body_receive_with_wrong_namespace_stays_unassigned(self):
        self.transport_fixture()
        for row in self.native:
            if row['stage'].startswith('broker_receive_'): row['scope']=73
        self.assertNotIn('android/broker_receive',self.analyze()['stage_spans'])

    def test_missing_cpu_samples_not_zero(self):
        self.details()
        for row in self.game:
            if row['stage']=='rpc_receive_end': row['cpu_ns']=0
        summary=self.analyze()['stage_spans']['gamescope/rpc_receive']
        self.assertEqual(summary['cpu_samples'],0)
        self.assertIsNone(summary['off_cpu_mean_ms'])

if __name__=='__main__': unittest.main()
