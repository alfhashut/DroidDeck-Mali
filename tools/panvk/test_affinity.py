"""Affinity restoration and same-scope sleep/queue bounds; no phone calls."""
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('steam_affinity', Path(__file__).resolve().parents[1] /
                                           'mali-vulkan/steam_affinity.py')
affinity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(affinity)


class AffinityTests(unittest.TestCase):
    def fake_tasks(self, root, masks):
        proc = root / '10'
        proc.mkdir()
        (proc / 'cmdline').write_text('steamwebhelper\0')
        (proc / 'environ').write_text('PANVK_ATOM_STRIDE=56\0')
        for tid in masks:
            task = proc / 'task' / str(tid)
            task.mkdir(parents=True)
            (task / 'comm').write_text('leader' if tid == 10 else 'custom')

    def test_partial_apply_failure_restores_already_changed_threads(self):
        original = {10: {2, 3, 4, 5, 6}, 11: set(range(8))}
        actual = {tid: set(mask) for tid, mask in original.items()}
        worker = affinity.Masks({}, {2, 3, 4, 5, 6, 7})
        failed = False

        def put(tid, cpus):
            nonlocal failed
            if tid == 11 and not failed:
                failed = True
                raise PermissionError('injected partial failure')
            actual[tid] = set(cpus)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fake_tasks(root, actual)
            with patch.object(affinity, 'Path', side_effect=lambda value: root if value == '/proc' else Path(value)), \
                 patch.object(affinity, 'targets', return_value=[10]), \
                 patch.object(affinity, 'identity', return_value=30), \
                 patch.object(affinity.os, 'sched_getaffinity', side_effect=lambda tid: actual[tid]), \
                 patch.object(affinity.os, 'sched_setaffinity', side_effect=put), \
                 patch.object(affinity, 'emit'):
                with self.assertRaises(PermissionError):
                    worker.refresh()
                self.assertTrue(worker.restore())
        self.assertEqual(actual, original)

    def test_new_thread_uses_same_name_original_policy(self):
        actual = {10: {2, 3, 4, 5, 6}, 11: set(range(8))}
        worker = affinity.Masks({}, {2, 3, 4, 5, 6, 7})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fake_tasks(root, actual)
            with patch.object(affinity, 'Path', side_effect=lambda value: root if value == '/proc' else Path(value)), \
                 patch.object(affinity, 'targets', return_value=[10]), \
                 patch.object(affinity, 'identity', return_value=30), \
                 patch.object(affinity.os, 'sched_getaffinity', side_effect=lambda tid: actual[tid]), \
                 patch.object(affinity.os, 'sched_setaffinity', side_effect=lambda tid, mask: actual.update({tid: set(mask)})), \
                 patch.object(affinity, 'emit'):
                worker.refresh()
                task = root / '10/task/12'
                task.mkdir()
                (task / 'comm').write_text('custom')
                actual[12] = set(worker.cpus)
                worker.refresh()
                self.assertEqual(worker.saved[(10, 12, 30)], set(range(8)))
                self.assertTrue(worker.restore())
        self.assertEqual(actual[10], {2, 3, 4, 5, 6})
        self.assertEqual(actual[11], set(range(8)))
        self.assertEqual(actual[12], set(range(8)))

    def test_restore_preserves_each_original_mask_and_skips_recycled_tid(self):
        masks = affinity.Masks({}, {0, 1})
        masks.saved = {(1, 2, 30): {2, 3, 4, 5, 6}, (1, 4, 50): {6, 7}, (1, 8, 90): {1}}
        with patch.object(affinity, 'identity', side_effect=[30, 50, 91]), \
             patch.object(affinity.os, 'sched_setaffinity') as put, \
             patch.object(affinity.os, 'sched_getaffinity', side_effect=[{2, 3, 4, 5, 6}, {6, 7}]), \
             patch.object(affinity, 'emit'):
            self.assertTrue(masks.restore())
        self.assertEqual([call.args for call in put.call_args_list], [(2, {2, 3, 4, 5, 6}), (4, {6, 7})])

    def test_restore_failure_is_visible_and_does_not_skip_remaining_threads(self):
        masks = affinity.Masks({}, {0, 1})
        masks.saved = {(1, 2, 30): {2}, (1, 4, 50): {4}}
        with patch.object(affinity, 'identity', side_effect=[30, 50]), \
             patch.object(affinity.os, 'sched_setaffinity', side_effect=[PermissionError('denied'), None]) as put, \
             patch.object(affinity.os, 'sched_getaffinity', return_value={4}), \
             patch.object(affinity, 'emit') as output:
            self.assertFalse(masks.restore())
        self.assertEqual(put.call_count, 2)
        self.assertFalse(output.call_args.args[1]['ok'])

    def test_parent_death_causes_detached_watchdog_restore(self):
        # Fake mutation uses a file so this tests real fork/pipe lifecycle without
        # changing host scheduling. The experimental parent exits without close.
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / 'state'

            class FakeMasks:
                def __init__(self, *_):
                    pass

                def refresh(self):
                    state.write_text('temporary')

                def snapshot(self, *_):
                    pass

                def restore(self):
                    state.write_text('restored')
                    return True

            with patch.object(affinity, 'Masks', FakeMasks), patch.object(affinity, 'emit'):
                pid = os.fork()
                if pid == 0:
                    try:
                        affinity.Experiment({'BL_LOG': 'test', 'HOME': directory}, '0xff', 5)
                        os._exit(0)
                    except BaseException:
                        os._exit(1)
                _, status = os.waitpid(pid, 0)
                self.assertEqual(status, 0)
                deadline = time.monotonic()+3
                while time.monotonic() < deadline and (not state.exists() or state.read_text() != 'restored'):
                    time.sleep(0.01)
                self.assertEqual(state.read_text(), 'restored')


if __name__ == '__main__':
    unittest.main()
