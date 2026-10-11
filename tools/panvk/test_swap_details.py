"""Swap attribution arithmetic/protocol checks. No phone or graphics calls."""
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location('swap_details', Path(__file__).resolve().parents[1] /
                                           'mali-vulkan/steam_swap_details.py')
swap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(swap)


class SwapTests(unittest.TestCase):
    def test_nested_inclusive_waits_are_not_double_counted(self):
        self.assertEqual(swap.union_us([(0, 20), (5, 15), (18, 25), (30, 40)], 10, 35), 20)

    def test_thread_cpu_from_synchronous_begin_end(self):
        events = [dict(pid=1, tid=2, ph='B', name='swap', ts=10, tts=100),
                  dict(pid=1, tid=2, ph='B', name='child', ts=12, tts=101),
                  dict(pid=1, tid=2, ph='E', ts=14, tts=102),
                  dict(pid=1, tid=2, ph='E', ts=30, tts=105)]
        scopes = {row['name']: row for row in swap.complete_scopes(events)}
        self.assertEqual(scopes['swap']['dur'], 20)
        self.assertEqual(scopes['swap']['tdur'], 5)
        self.assertEqual(scopes['child']['dur'], 2)

    def test_cpu_bounds_preserve_wall_time_under_clock_uncertainty(self):
        samples = [dict(begin_ns=i*1000000, end_ns=i*1000000+1000,
                        cpu_ns=i*500000, runqueue_ns=i*10000) for i in range(0, 41, 5)]
        exact = swap.cpu_bounds(samples, 12000, 28000)
        uncertain = swap.cpu_bounds(samples, 12000, 28000, 4000)
        self.assertLessEqual(uncertain['cpu_ms_lower'], exact['cpu_ms_lower'])
        self.assertGreaterEqual(uncertain['cpu_ms_upper'], exact['cpu_ms_upper'])
        self.assertEqual(uncertain['off_cpu_ms_lower']+uncertain['cpu_ms_upper'], 16)
        self.assertIsNone(swap.cpu_bounds(samples, -1000, 28000))

    def test_queue_submit_is_not_gpu_execution(self):
        self.assertEqual(swap.scope_kind('VulkanQueueSubmitHook'), 'vulkan-submit-present-api-scope')
        self.assertIsNone(swap.scope_kind('SwapBuffers'))
        self.assertEqual(swap.scope_kind('vkWaitForFences'), 'gpu-completion-wait-scope')

    def test_same_swap_queue_bounds_leave_nonrunnable_residual_not_gpu_time(self):
        row = swap.sleep_queue_bounds(30, dict(runqueue_ms_lower=4, runqueue_ms_upper=12))
        self.assertEqual((row['nonrunnable_ms_lower'], row['nonrunnable_ms_upper']), (18, 26))
        self.assertIsNone(row['exact_poll_sleep_ms'])
        self.assertIsNone(row['exact_poll_wakeup_to_run_ms'])
        clipped = swap.sleep_queue_bounds(30, dict(runqueue_ms_lower=4, runqueue_ms_upper=35))
        self.assertEqual(clipped['nonrunnable_ms_lower'], 0)
        self.assertIsNone(swap.sleep_queue_bounds(30, None))

    def test_pending_runqueue_accounting_cannot_be_treated_as_live_counter(self):
        samples = [dict(begin_ns=i*1000000, end_ns=i*1000000+1000,
                        cpu_ns=0, runqueue_ns=0, state='R') for i in range(0, 41, 5)]
        bounds = swap.cpu_bounds(samples, 12000, 28000)
        self.assertEqual(bounds['runqueue_ms_lower'], 0)
        self.assertEqual(bounds['runqueue_ms_upper'], 16)

    def sampler(self, text):
        sampler = swap.ThreadSampler.__new__(swap.ThreadSampler)
        sampler.mem = {1: 99}
        sampler.unix = {}
        sampler.paths = {(1, 2): {}}
        sampler.read = Mock(return_value=text)
        return sampler

    def test_pollfd_read_is_bounded_and_identifies_mali(self):
        text = '73 0x1000 0x10000 0x0 0x0 0x0 0x0'
        sampler = self.sampler(text)
        payload = struct.pack('=ihh', 7, 1, 0)*8
        with patch.object(swap.os, 'pread', return_value=payload) as read, \
             patch.object(swap.os, 'readlink', return_value='/dev/mali0'):
            detail = sampler.syscall_detail(1, 2, text)
        read.assert_called_once_with(99, 64, 0x1000)
        self.assertTrue(detail['pollfd_count_capped'])
        self.assertEqual(detail['fds'][0]['kind'], 'mali-device')
        self.assertEqual(detail['fds'][0]['poll_events'], 1)

    def test_changed_syscall_discards_racy_fd_attribution(self):
        text = '73 0x1000 0x1 0x0 0x0 0x0 0x0'
        sampler = self.sampler('running')
        with patch.object(swap.os, 'pread', return_value=struct.pack('=ihh', 7, 1, 0)), \
             patch.object(swap.os, 'readlink', return_value='/dev/mali0'):
            detail = sampler.syscall_detail(1, 2, text)
        self.assertFalse(detail['stable_observation'])
        self.assertEqual(detail['fds'], [])


if __name__ == '__main__':
    unittest.main()
