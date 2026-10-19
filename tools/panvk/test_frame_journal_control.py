"""Pure Python journal-control tests; no phone, native compilation or GPU work."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import frame_journal as host

SOURCE = Path(__file__).resolve().parents[1] / 'mali-vulkan/steam_frame_journal.py'
SPEC = importlib.util.spec_from_file_location('guest_frame_journal', SOURCE)
guest = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guest)


class JournalControlTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='dd-journal-control-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'shared storage'
        self.root.mkdir()
        self.proc = Path(temporary.name) / 'guest-proc'
        self.proc.mkdir()
        self.messages = []
        self.armed = self.root / 'armed'
        self.armed.touch()
        self.arm_ns = self.armed.stat().st_mtime_ns

    def marker(self, pid, role, *, stale=False, live=False):
        path = self.root / f'ready-{pid}-{role}'
        path.write_text(f'version=1 pid={pid} role={role}\n')
        timestamp = self.arm_ns + (-1_000_000 if stale else 1_000_000)
        os.utime(path, ns=(timestamp, timestamp))
        if live:
            (self.proc / str(pid)).mkdir(exist_ok=True)
        return path

    def pair(self):
        self.marker(12345, 'gamescope', live=True)
        self.marker(67890, 'android')  # Intentionally absent from guest /proc.

    def emit(self, *message):
        self.messages.append(message)

    def schedule(self):
        return guest.schedule(10, self.emit, self.root, proc_root=self.proc)

    def assert_unavailable(self):
        self.assertIsNone(self.schedule())
        self.assertEqual(self.messages[-1][0], 'frame-journal-unavailable')
        self.assertFalse((self.root / 'control').exists())
        self.assertFalse(list(self.root.glob('capture-*.json')))

    def test_rearm_cleans_stale_workers_control_and_preserves_captures(self):
        self.pair()
        (self.root / 'control').write_text('stale control\n')
        retained = {'trace-42-12345-gamescope.jsonl': 'old trace\n',
                    'trace-42-67890-android.jsonl': 'old Android trace\n',
                    'capture-42.json': '{"run":42}\n'}
        for name, contents in retained.items():
            (self.root / name).write_text(contents)
        os.utime(self.armed, ns=(1_000_000_000, 1_000_000_000))
        real_run = subprocess.run
        calls = []

        def android_shell(argv, **kwargs):
            self.assertEqual(argv[:4], ['adb', '-s', 'test-phone', 'shell'])
            calls.append(argv)
            # Exercise the exact cleanup/globbing/quoting shell sent to Android.
            return real_run(['sh', '-c', argv[4]], **kwargs)

        with patch.object(host, 'ROOT', str(self.root)), \
             patch.object(sys, 'argv', ['frame_journal.py', 'arm', '--serial', 'test-phone']), \
             patch.object(host, 'resolve_serial', return_value='test-phone'), \
             patch.object(host.subprocess, 'run', side_effect=android_shell), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(host.main(), 0)
        self.assertEqual(len(calls), 1)
        self.assertFalse(list(self.root.glob('ready-*')))
        self.assertFalse((self.root / 'control').exists())
        self.assertGreater(self.armed.stat().st_mtime_ns, 1_000_000_000)
        self.assertLess(calls[0][4].index('/ready-*-android'), calls[0][4].index('touch '))
        for name, contents in retained.items():
            self.assertEqual((self.root / name).read_text(), contents)

    def test_fresh_android_without_guest_proc_and_live_gamescope_accepted(self):
        self.pair()
        self.assertFalse((self.proc / '67890').exists())
        config = self.schedule()
        self.assertIsNotNone(config)
        selected = [dict(pid=12345, role='gamescope'), dict(pid=67890, role='android')]
        self.assertEqual(config['ready'], selected)
        self.assertEqual(config['arm_mtime_ns'], self.arm_ns)
        control = (self.root / 'control').read_text()
        self.assertEqual(control, f"{config['run']} {config['start_ns']} {config['end_ns']} 8\n")
        self.assertEqual(json.loads((self.root / f"capture-{config['run']}.json").read_text()), config)
        self.assertEqual(self.messages[-1], ('frame-journal-control', config))

    def test_equal_timestamp_storage_resolution_is_accepted_after_arm_cleanup(self):
        self.pair()
        for path in self.root.glob('ready-*'):
            os.utime(path, ns=(self.arm_ns, self.arm_ns))
        self.assertIsNotNone(self.schedule())

    def test_stale_marker_for_either_role_is_rejected(self):
        for role in ('gamescope', 'android'):
            with self.subTest(role=role):
                self.pair()
                self.marker(12345 if role == 'gamescope' else 67890, role, stale=True)
                self.assert_unavailable()
                self.assertIn('predates current armed', self.messages[-1][1]['rejected'][0]['reason'])

    def test_stale_additional_marker_cannot_hide_behind_fresh_pair(self):
        self.pair()
        self.marker(11111, 'android', stale=True)
        self.assert_unavailable()

    def test_gamescope_pid_missing_from_guest_proc_is_rejected(self):
        self.marker(12345, 'gamescope')
        self.marker(67890, 'android')
        self.assert_unavailable()
        self.assertIn('Gamescope PID absent', self.messages[-1][1]['rejected'][0]['reason'])

    def test_duplicate_markers_for_either_role_are_rejected(self):
        for role in ('gamescope', 'android'):
            with self.subTest(role=role):
                self.pair()
                extra = self.marker(11111, role, live=role == 'gamescope')
                self.assert_unavailable()
                self.assertEqual(self.messages[-1][1]['duplicate_roles'], [role])
                extra.unlink()

    def test_malformed_ready_names_or_contents_are_not_silently_ignored(self):
        self.pair()
        for name, contents in [('ready-bad', ''), ('ready-0-android', ''),
                               ('ready-99999-unknown', ''), ('ready-99999-android', 'pid mismatch')]:
            with self.subTest(name=name):
                marker = self.root / name
                marker.write_text(contents)
                os.utime(marker, ns=(self.arm_ns + 1_000_000, self.arm_ns + 1_000_000))
                self.assert_unavailable()
                self.assertEqual(self.messages[-1][1]['rejected'][0]['marker'], name)
                marker.unlink()

    def test_arm_cycle_change_during_selection_is_rejected(self):
        self.pair()
        original = Path.read_text

        def rearm(path, *args, **kwargs):
            text = original(path, *args, **kwargs)
            os.utime(self.armed, ns=(self.arm_ns + 1, self.arm_ns + 1))
            return text

        with patch.object(Path, 'read_text', rearm):
            self.assert_unavailable()
        self.assertIn('arm cycle changed', self.messages[-1][1])

    def test_unarmed_does_not_create_control(self):
        self.armed.unlink()
        self.assert_unavailable()

    def test_missing_role_does_not_create_control(self):
        self.marker(12345, 'gamescope', live=True)
        self.assert_unavailable()

    def finish(self, config):
        with patch.object(guest.time, 'monotonic_ns', return_value=config['end_ns'] + 1_000_000_000):
            guest.finish(config, self.emit, self.root)
        return self.messages[-1][1]

    def test_finish_requires_selected_android_output_even_with_two_files(self):
        self.pair()
        config = self.schedule()
        game = f"trace-{config['run']}-12345-gamescope.jsonl"
        wrong = f"trace-{config['run']}-99999-android.jsonl"
        for name in (game, wrong):
            (self.root / name).touch()
        result = self.finish(config)
        self.assertFalse(result['complete_pair_available'])
        self.assertEqual(result['missing_files'], [f"trace-{config['run']}-67890-android.jsonl"])
        self.assertEqual(result['unexpected_files'], [wrong])

    def test_finish_accepts_only_exact_current_run_pid_role_pair(self):
        self.pair()
        config = self.schedule()
        expected = [f"trace-{config['run']}-{row['pid']}-{row['role']}.jsonl" for row in config['ready']]
        for name in expected + ['trace-1-12345-gamescope.jsonl', 'trace-1-67890-android.jsonl']:
            (self.root / name).touch()
        result = self.finish(config)
        self.assertTrue(result['complete_pair_available'])
        self.assertEqual(result['files'], sorted(expected))
        self.assertEqual(result['missing_files'], [])
        self.assertEqual(result['unexpected_files'], [])


if __name__ == '__main__':
    unittest.main()
