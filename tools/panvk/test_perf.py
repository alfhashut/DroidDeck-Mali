"""Lightweight sampler correctness checks; no phone, graphics APIs or network."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import stage_phone_test as phone

spec = importlib.util.spec_from_file_location('steam_perf', Path(__file__).resolve().parents[1] /
                                           'mali-vulkan/diagnose_steam_perf.py')
perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(perf)


class PerformanceTests(unittest.TestCase):
    def test_cpu_pid_reuse_and_core_units(self):
        old = dict(start=100, ticks=200)
        self.assertEqual(perf.cpu_delta(old, dict(start=100, ticks=500), 2, 100), 150)
        self.assertIsNone(perf.cpu_delta(old, dict(start=101, ticks=500), 2, 100))
        self.assertIsNone(perf.cpu_delta(old, dict(start=100, ticks=199), 2, 100))

    def test_proc_comm_with_spaces_and_parentheses(self):
        fields = ['S'] + [str(i) for i in range(4, 53)]
        row = perf.stat_fields('7 (name with ) chars) ' + ' '.join(fields))
        self.assertEqual(row['name'], 'name with ) chars')
        self.assertEqual(row['ticks'], 14 + 15)
        self.assertEqual(row['start'], 22)

    def test_trace_end_is_not_another_frame_and_coverage_is_explicit(self):
        trace = perf.TraceCounts()
        trace.add([dict(pid=1, tid=2, name='DrawFrame', ph=phase, ts=ts)
                   for phase, ts in [('B', 1000000), ('E', 1100000), ('B', 2000000)]])
        with contextlib.redirect_stdout(io.StringIO()) as logged:
            trace.report(25)
        lines = logged.getvalue().splitlines()
        row = json.loads(lines[0].split('=', 1)[1])
        self.assertEqual(row['count'], 2)
        self.assertEqual(row['retained_span_s'], 1)
        self.assertEqual(row['retained_span_event_hz'], 1)
        self.assertEqual(row['count_div_sample_s_lower_bound'], 0.08)
        timestamps = json.loads(lines[1].split('=', 1)[1])
        self.assertEqual([item['ts_us'] for item in timestamps['samples']], [1000000, 2000000])
        self.assertEqual(timestamps['interval_p95_ms'], 1000)

    def test_log_warning_is_visible_and_only_new_lines_are_fresh(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'log'
            path.write_text('KBASE_IOCTL_MEM_JIT_INIT historical\n')
            with contextlib.redirect_stdout(io.StringIO()) as logged:
                tail = perf.LogTail(path)
                with path.open('a') as stream:
                    stream.write('KBASE_IOCTL_MEM_JIT_INIT fresh\n[KFAULT] atom=0 code=0x40 core_req=0x0\n')
                tail.poll()
                tail.poll()
                tail.close()
            rows = [json.loads(line.split('=', 1)[1]) for line in logged.getvalue().splitlines()
                    if line.startswith('fresh-log-lines=')]
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['rows'], ['KBASE_IOCTL_MEM_JIT_INIT fresh',
                                              '[KFAULT] atom=0 code=0x40 core_req=0x0'])

    def test_perf_completion_cannot_pass_gpu_proof(self):
        output = 'DROIDDECK_PERF_DIAGNOSTICS_DONE\nDROIDDECK_PANVK_PROBE_DONE exit=0\n'
        for diagnostic in (False, True):
            with patch.object(phone.subprocess, 'run', side_effect=[
                    subprocess.CompletedProcess([], 0, 'result=1', ''),
                    subprocess.CompletedProcess([], 0, output, '')]), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(phone.run_phone(['adb'], '/storage/test', perf_diagnostics=diagnostic),
                                 0 if diagnostic else 1)

    def test_log_observer_refuses_fifo_without_consuming_it(self):
        with tempfile.TemporaryDirectory() as directory:
            fifo = Path(directory) / 'pipe'
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, 'not a regular log file'):
                perf.LogTail(fifo)

    def test_trace_cleanup_leaves_foreign_trace_running(self):
        trace = perf.Trace.__new__(perf.Trace)
        trace.started = False
        trace.ws = Mock()
        trace.call = Mock()
        trace.close()
        trace.call.assert_not_called()
        trace.ws.close.assert_called_once()

    def test_trace_cleanup_closes_socket_after_end_error(self):
        trace = perf.Trace.__new__(perf.Trace)
        trace.started = True
        trace.ws = Mock()
        trace.call = Mock(side_effect=RuntimeError('disconnected'))
        with self.assertRaisesRegex(RuntimeError, 'disconnected'):
            trace.close()
        trace.ws.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()
