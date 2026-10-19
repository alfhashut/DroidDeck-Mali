"""Frame-clock, history filtering, and recorder isolation checks; no phone."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import analyze_steam_frames as frames
import stage_phone_test as phone
from perf_capture import PerformanceCapture
from system_frame_capture import SystemFrameCapture, layer_names, trace_config


class FrameTests(unittest.TestCase):
    def test_clock_discontinuity_refuses_cross_domain_join(self):
        report = {'frame-clock-start': [dict(monotonic_before_ns=10, monotonic_after_ns=12, boottime_ns=30)],
                  'frame-clock-end': [dict(monotonic_before_ns=100, monotonic_after_ns=102, boottime_ns=120)],
                  'PERF_SAMPLING_START': [dict(monotonic_ns=11)],
                  'PERF_SAMPLING_END': [dict(monotonic_ns=101)]}
        self.assertEqual(frames.window(report)['boot_minus_monotonic_ns'], 19)
        report['frame-clock-end'][0]['boottime_ns'] += 5
        with self.assertRaisesRegex(ValueError, 'clock offsets disagree'):
            frames.window(report)

    def test_history_rejects_old_pending_zero_and_java_activity_rows(self):
        row = dict(layer='droiddeck_wayland_game#5', returncode=0,
                   output='now = 300\n16666667\n0 0 0\n10 20 10\n100 110 105\n'
                          '100 9223372036854775807 105\n')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'surfaceflinger-latency-before.json').write_text(json.dumps([row]))
            java = dict(row, layer='SurfaceView[com.droiddeck.launcher]')
            (root/'surfaceflinger-latency-samples.jsonl').write_text(json.dumps(row)+'\n'+json.dumps(java)+'\n')
            result = frames.latency_rows(root, dict(monotonic_start_ns=100, monotonic_end_ns=200))
        self.assertEqual(len(result), 1)  # Snapshot duplicates are not extra frames.
        self.assertEqual(result[0]['actual_present_ns'], 110)

    def test_timestamp_neighbors_remain_ambiguous(self):
        rows = [dict(monotonic_ns=99, id=1), dict(monotonic_ns=101, id=2), dict(monotonic_ns=500, id=3)]
        self.assertEqual(frames.candidates(100, rows, 10), rows[:2])

    def test_live_renderer_proof_rejects_fallback_and_wrong_backend(self):
        good = dict(displayType='ANGLE_VULKAN', glRenderer='ANGLE Mali-G52 panvk-26.2.99')
        frames.validate_renderer(good)
        for bad in (dict(good, displayType='ANGLE_OPENGL'), dict(good, glRenderer='ANGLE llvmpipe'),
                    dict(good, glRenderer='ANGLE Mali-G52 SwiftShader panvk')):
            with self.assertRaisesRegex(ValueError, 'fresh live CEF renderer'):
                frames.validate_renderer(bad)

    def test_pending_depth_ambiguity_is_not_assigned_per_frame_latency(self):
        rows = [dict(ts=stamp, value=value, name='native') for stamp, value in
                [(1, 1), (2, 2), (3, 1), (4, 0), (5, 1), (9, 0), (10, 1)]]
        # 2->1 is not a newly arrived buffer. A later complete 0->1->0
        # cycle is observable; a trailing pending buffer is incomplete.
        self.assertEqual(frames.pending_intervals(rows, 0, 20),
                         [dict(track='native', begin_boottime_ns=5, end_boottime_ns=9, pending_ms=0.000004)])

    def test_default_recorder_avoids_system_wide_scheduler_overhead(self):
        self.assertNotIn('sched/sched_switch', trace_config(10))
        self.assertIn('sched/sched_switch', trace_config(10, scheduler=True))
        self.assertIn('compact_sched { enabled: true }', trace_config(10, scheduler=True))
        self.assertIn('duration_ms: 40000', trace_config(10))
        self.assertIn('max_file_size_bytes:', trace_config(10))
        self.assertNotIn('raw_syscalls', trace_config(10))

    def test_recorder_never_signals_reused_pid_or_foreign_trace(self):
        for argv in ('perfetto\0-o\0/somebody-elses-trace\0', 'sh\0perfetto\0'):
            with self.subTest(argv=argv), tempfile.TemporaryDirectory() as directory:
                capture = SystemFrameCapture(['adb'], Path(directory), 10)
                capture.pid = 42
                with patch.object(capture, 'command', return_value=subprocess.CompletedProcess([], 0, argv, '')) as command, \
                     patch('system_frame_capture.subprocess.run') as run, \
                     contextlib.redirect_stdout(io.StringIO()):
                    capture.close()
                self.assertEqual(command.call_count, 1)
                run.assert_not_called()
                self.assertIn('identity mismatch', capture.metadata['close_error'])

    def test_recorder_stops_only_exact_uuid_output_and_pulls_its_file(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = SystemFrameCapture(['adb'], Path(directory), 10)
            capture.pid = 42
            responses = [subprocess.CompletedProcess([], 0, 'perfetto\0-o\0'+capture.trace_remote+'\0', ''),
                         subprocess.CompletedProcess([], 0, '', ''),
                         subprocess.CompletedProcess([], 1, '', '')]
            with patch.object(capture, 'command', side_effect=responses) as command, \
                 patch.object(capture, 'latency') as latency, \
                 patch('system_frame_capture.subprocess.run', return_value=subprocess.CompletedProcess([], 0, '', '')) as run:
                capture.close()
            self.assertEqual(command.call_args_list[1].args[0], ['kill', '-INT', '42'])
            self.assertIn(capture.trace_remote, run.call_args.args[0])
            latency.assert_called_once_with('after')

    def test_system_collection_failure_still_stops_other_capture_workers(self):
        capture = PerformanceCapture(['adb'], Path('/tmp/not-created-by-test'))
        capture.system = Mock()
        capture.system.close.side_effect = OSError('trace output disk full')
        capture.thread, capture.logcat, capture.log = Mock(), Mock(), Mock()
        with self.assertRaisesRegex(OSError, 'disk full'):
            capture.__exit__(None, None, None)
        self.assertTrue(capture.stop.is_set())
        capture.thread.join.assert_called_once()
        capture.logcat.terminate.assert_called_once()
        capture.log.close.assert_called_once()

    def test_frame_mode_refuses_non_ff_affinity_and_missing_run(self):
        for flags in (['--frame-diagnostics'], ['--frame-diagnostics', '--run', '--client-affinity', '0xfc'],
                      ['--frame-scheduler', '--run']):
            with self.subTest(flags=flags), patch.object(sys, 'argv', ['stage_phone_test.py']+flags), \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                phone.main()
            self.assertEqual(error.exception.code, 2)

    def test_layer_filter_avoids_unrelated_apps(self):
        self.assertEqual(layer_names('OtherApp\ndroiddeck_wayland_game#1\n'), ['droiddeck_wayland_game#1'])


if __name__ == '__main__':
    unittest.main()
