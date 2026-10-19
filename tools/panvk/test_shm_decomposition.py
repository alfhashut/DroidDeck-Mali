"""Synthetic same-frame journal contracts; Python only, no native build or phone."""
import contextlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch

import analyze_frame_journal as journal

NS = 1_000_000
TOOLS = Path(__file__).resolve().parents[1]


class ShmDecompositionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='dd-shm-analysis-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = []
        self.add('frame', 5, 125)
        self.add('shm_import', 10, 110)
        for stage, begin, end in [
            ('shm_lookup',11,12), ('shm_source',13,18), ('staging_acquire',20,40),
            ('staging_create',22,30), ('staging_map',25,28), ('upload_wait',32,35),
            ('staging_validate',41,42), ('shm_copy',43,63), ('shm_source_release',64,65),
            ('texture_create',66,76), ('upload_record',77,82), ('upload_prepare',83,84),
            ('upload_submit',85,105), ('upload_finalize',86,90), ('upload_queue',92,103),
        ]:
            self.add(stage, begin, end)
        publication = self.event('publish',120)
        publication.update(key=7, frame=1, sync=456, fence=455)
        self.game.append(publication)
        self.native = [dict(publication, mono_ns=119*NS, attempt=0, client=0)]

    def event(self, stage, ms, **extra):
        row = dict(type='event', event_seq=0, stage=stage, mono_ns=ms*NS,
                   cpu_ns=ms*NS//2, tid=11, client=18, attempt=8, scope=0,
                   key=0, frame=0, sync=0, fence=0, object=0, value=0, status=0)
        row.update(extra)
        return row

    def add(self, stage, begin, end, **extra):
        self.game.extend([self.event(stage+'_begin',begin,**extra), self.event(stage+'_end',end,**extra)])

    def edge(self, stage):
        return next(row for row in self.game if row['stage']==stage)

    def paths(self, dropped=0):
        paths = []
        for role, pid, events in [('gamescope',1,self.game),('android',2,self.native)]:
            header = dict(type='header',version=1,clock='CLOCK_MONOTONIC',run=42,
                          start_ns=0,end_ns=400*NS,ready_ns=0,pid=pid,role=role)
            events = sorted(events,key=lambda e:e['mono_ns'])
            for sequence, row in enumerate(events,1):
                row['event_seq'] = sequence
            footer = dict(type='footer',count=len(events),complete=True,dropped=dropped)
            path = self.root/f'trace-42-{pid}-{role}.jsonl'
            path.write_text('\n'.join(json.dumps(r) for r in [header,*events,footer]))
            paths.append(path)
        return paths

    def analyze(self, **kwargs):
        return journal.analyze(self.paths(**kwargs))

    def test_new_names_and_native_enum_table_stay_in_sync(self):
        header = (TOOLS/'mali-vulkan/frame_trace.h').read_text().split('enum dd_ft_stage {',1)[1].split('};',1)[0]
        enums = re.findall(r'\bDD_FT_[A-Z_]+\b',header)
        source = (TOOLS/'mali-vulkan/frame_trace.c').read_text().split('dd_ft_names[] = {',1)[1].split('};',1)[0]
        names = re.findall(r'"([a-z_]+)"',source)
        self.assertEqual(len(enums)-1,len(names))
        self.assertEqual(names[32:34],['shm_copy_begin','shm_copy_end'])
        for stage in journal.SHM_CHILD_STAGES:
            self.assertIn(stage+'_begin',names)
            self.assertIn(stage+'_end',names)

    def test_all_stages_parse_pair_and_keep_explicit_attempt_identity(self):
        result = self.analyze()
        self.assertEqual(result['joined_frames'],1)
        self.assertEqual(result['span_diagnostics'],[])
        for stage in journal.SHM_CHILD_STAGES:
            if stage=='shm_cleanup':  # No fallback cleanup operation in this path.
                self.assertNotIn('gamescope/'+stage,result['stage_spans'])
                continue
            self.assertEqual(result['stage_spans']['gamescope/'+stage]['count'],1)
        nested = next(s for s in result['frames'][0]['spans'] if s['stage']=='upload_queue')
        self.assertEqual((nested['attempt'],nested['client'],nested['key'],nested['frame']),(8,18,0,0))
        self.assertEqual((nested['joined_key'],nested['joined_frame']),(7,1))

    def test_stage_cpu_and_off_cpu_use_same_paired_subset(self):
        result = self.analyze()['stage_spans']['gamescope/shm_copy']
        self.assertEqual(result,dict(count=1,wall_mean_ms=20,wall_p95_ms=20,cpu_samples=1,
                                     cpu_mean_ms=10,paired_wall_mean_ms=20,off_cpu_mean_ms=10))
        self.edge('shm_copy_end')['cpu_ns'] = 0
        result = self.analyze()['stage_spans']['gamescope/shm_copy']
        self.assertEqual(result['cpu_samples'],0)
        self.assertIsNone(result['cpu_mean_ms'])
        self.assertIsNone(result['paired_wall_mean_ms'])
        self.assertIsNone(result['off_cpu_mean_ms'])

    def test_nested_scopes_use_disjoint_interval_union_not_sum(self):
        result = self.analyze()['shm_import_decomposition']
        self.assertEqual(result['count'],1)
        self.assertEqual(result['remainder_samples'],1)
        self.assertEqual(result['total_import_mean_ms'],100)
        self.assertEqual(result['covered_import_mean_ms'],84)
        self.assertEqual(result['unattributed_inside_import_mean_ms'],16)
        self.assertGreater(sum(result['stage_inclusive_mean_ms'].values()),100)
        self.assertEqual(result['stage_inclusive_mean_ms']['staging_acquire'],20)
        self.assertEqual(result['stage_inclusive_mean_ms']['staging_map'],3)
        self.assertEqual(result['stage_inclusive_mean_ms']['shm_cleanup'],0)

    def test_paired_cpu_subset_does_not_use_all_span_wall_mean(self):
        self.add('shm_copy',210,250,attempt=16,client=19,cpu_ns=0)
        pub = self.event('publish',270,attempt=16,client=19,key=7,frame=2,sync=458,fence=457)
        self.game.append(pub)
        self.native.append(dict(pub,mono_ns=269*NS,attempt=0,client=0))
        result = self.analyze()['stage_spans']['gamescope/shm_copy']
        self.assertEqual(result['count'],2)
        self.assertEqual(result['wall_mean_ms'],30)
        self.assertEqual(result['cpu_samples'],1)
        self.assertEqual(result['paired_wall_mean_ms'],20)
        self.assertEqual(result['cpu_mean_ms'],10)
        self.assertEqual(result['off_cpu_mean_ms'],10)

    def test_repeated_sequential_waits_pair_separately(self):
        self.add('upload_wait',36,37,object=123,value=88)
        result = self.analyze()
        self.assertEqual(result['stage_spans']['gamescope/upload_wait']['count'],2)
        self.assertEqual(result['shm_import_decomposition']['stage_inclusive_mean_ms']['upload_wait'],4)
        self.assertEqual(result['shm_import_decomposition']['unattributed_inside_import_mean_ms'],16)

    def assert_ambiguous(self):
        result = self.analyze()
        decomposition = result['shm_import_decomposition']
        self.assertEqual(decomposition['remainder_samples'],0)
        self.assertEqual(decomposition['ambiguous_imports'],1)
        self.assertIsNone(decomposition['unattributed_inside_import_mean_ms'])
        self.assertTrue(decomposition['per_import'][0]['remainder_unavailable_reasons'])
        return result

    def test_missing_end_disables_remainder_and_reports_unpaired_stage(self):
        self.game.remove(self.edge('staging_map_end'))
        result = self.assert_ambiguous()
        self.assertTrue(any(i['stage']=='staging_map_begin' and i['reason']=='unpaired begin'
                            for i in result['span_diagnostics']))

    def test_duplicate_begin_never_silently_overwrites_a_pair(self):
        self.game.append(self.event('staging_map_begin',26))
        result = self.assert_ambiguous()
        self.assertNotIn('gamescope/staging_map',result['stage_spans'])
        self.assertTrue(any('duplicate begin' in i['reason'] for i in result['span_diagnostics']))

    def test_wrong_local_object_cannot_pair(self):
        self.edge('staging_map_end')['object'] = 123
        result = self.assert_ambiguous()
        self.assertNotIn('gamescope/staging_map',result['stage_spans'])

    def test_client_identity_change_inside_scope_is_rejected(self):
        self.edge('staging_map_end')['client'] = 99
        result = self.assert_ambiguous()
        self.assertTrue(any('identity changed' in i['reason'] for i in result['span_diagnostics']))

    def test_known_frame_identity_change_inside_scope_is_rejected(self):
        self.edge('staging_map_begin').update(key=7,frame=1)
        self.edge('staging_map_end').update(key=8,frame=1)
        result = self.assert_ambiguous()
        self.assertTrue(any('known frame identity changed' in i['reason'] for i in result['span_diagnostics']))

    def test_import_client_must_match_successful_publication(self):
        for row in self.game:
            if row['stage']!='publish':
                row['client'] = 99
        result = self.assert_ambiguous()['shm_import_decomposition']
        self.assertIn('import client differs from successful publication',result['per_import'][0]['remainder_unavailable_reasons'])

    def test_reverse_time_pair_remains_unpaired(self):
        self.edge('staging_map_end')['mono_ns'] = 24*NS
        self.assert_ambiguous()

    def test_crossing_child_intervals_make_remainder_ambiguous(self):
        self.edge('staging_map_end')['mono_ns'] = 45*NS
        result = self.assert_ambiguous()['shm_import_decomposition']
        self.assertIn('crossing child scopes; remainder ambiguous',result['per_import'][0]['remainder_unavailable_reasons'])

    def test_child_outside_import_is_not_clipped_into_a_false_remainder(self):
        self.edge('upload_queue_end')['mono_ns'] = 115*NS
        result = self.assert_ambiguous()['shm_import_decomposition']
        self.assertIn('child crosses import boundary',result['per_import'][0]['remainder_unavailable_reasons'])

    def test_other_thread_child_cannot_be_subtracted_from_import(self):
        for edge in ('staging_map_begin','staging_map_end'):
            self.edge(edge)['tid'] = 22
        self.assert_ambiguous()

    def test_timestamp_neighbors_with_other_attempt_are_not_attributed(self):
        self.add('staging_map',25,28,attempt=9,client=99)
        result = self.analyze()
        self.assertEqual(result['stage_spans']['gamescope/staging_map']['count'],1)
        self.assertEqual(result['shm_import_decomposition']['unattributed_inside_import_mean_ms'],16)

    def test_unknown_stage_is_reported_and_not_guessed(self):
        self.add('invented',46,47)
        result = self.analyze()
        self.assertNotIn('gamescope/invented',result['stage_spans'])
        self.assertEqual(sum(i['reason']=='unknown scope' for i in result['span_diagnostics']),2)

    def test_invalid_cpu_does_not_turn_into_negative_off_cpu(self):
        self.edge('shm_copy_end')['cpu_ns'] = 90*NS
        result = self.analyze()
        self.assertIsNone(result['stage_spans']['gamescope/shm_copy']['off_cpu_mean_ms'])
        self.assertEqual(result['shm_import_decomposition']['unattributed_inside_import_mean_ms'],16)
        self.assertTrue(any('invalid CPU samples' in i['reason'] for i in result['span_diagnostics']))

    def test_loss_disables_remainder(self):
        result = self.analyze(dropped=1)['shm_import_decomposition']
        self.assertEqual(result['remainder_samples'],0)
        self.assertIsNone(result['unattributed_inside_import_mean_ms'])

    def test_old_journal_with_only_copy_does_not_fake_decomposition(self):
        self.game = [e for e in self.game if e['stage'].split('_begin')[0].split('_end')[0]
                     in ('frame','shm_import','shm_copy','publish')]
        self.assert_ambiguous()

    def test_no_full_import_inside_sample_has_no_remainder(self):
        result = journal.analyze(self.paths(),(30*NS,130*NS))['shm_import_decomposition']
        self.assertEqual(result['count'],0)
        self.assertIsNone(result['unattributed_inside_import_mean_ms'])

    def test_direct_journal_directory_retains_parent_external_sample_bounds(self):
        directory = self.root/'frame-journal'
        directory.mkdir()
        for path in self.paths():
            path.replace(directory/path.name)
        (self.root/'guest-report.txt').write_text(
            'PERF_SAMPLING_START='+json.dumps(dict(monotonic_ns=30*NS))+'\n'+
            'PERF_SAMPLING_END='+json.dumps(dict(monotonic_ns=130*NS))+'\n')
        stream = io.StringIO()
        with patch.object(sys,'argv',['analyze_frame_journal.py',str(directory)]), \
             contextlib.redirect_stdout(stream):
            journal.main()
        result = json.loads(stream.getvalue())
        self.assertEqual(result['window_ns'],[30*NS,130*NS])
        self.assertEqual(result['shm_import_decomposition']['count'],0)


if __name__=='__main__':
    unittest.main()
