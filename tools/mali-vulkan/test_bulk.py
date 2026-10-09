"""Standalone mapped-transfer tests; no ICD/broker/Gamescope/Android build."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class BulkMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mali-bulk-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        proxy = (ROOT / 'interop_icd.h').read_text()
        upload = proxy.split('static VkResult interop_upload_mapping', 1)[1].split('static VKAPI_ATTR VkResult VKAPI_CALL proxy_MapMemory', 1)[0]
        (build / 'upload.inc').write_text('static VkResult interop_upload_mapping' + upload)
        native = (ROOT.parents[1] / 'app/src/main/cpp/malivulkan/interop_commands.h').read_text()
        memory = native.split('    case MB_MEMORY_MAP: case MB_MEMORY_UNMAP:', 1)[1].split('    case MB_COMMAND_FILL:', 1)[0]
        (build / 'native_memory.inc').write_text('    case MB_MEMORY_MAP: case MB_MEMORY_UNMAP:' + memory)
        renderer = (ROOT / 'renderer_icd.h').read_text().split('static VkResult renderer_QueueSubmit', 1)[1].split('static void renderer_Barrier', 1)[0]
        (build / 'submit.inc').write_text('static VkResult renderer_QueueSubmit' + renderer)
        metric = (ROOT / 'icd_proxy.c').read_text().split('static VkResult rpc(struct proxy_instance *s,', 1)[1].split('static void proxy_free_logical', 1)[0]
        (build / 'metric.inc').write_text('static VkResult rpc(struct proxy_instance *s,' + metric)
        cleanup = (ROOT / 'submit_icd.h').read_text().split('static void proxy_free_resources', 1)[1].split('static struct proxy_resource *submit_find', 1)[0]
        (build / 'cleanup.inc').write_text('static void proxy_free_resources' + cleanup)
        cls.binary = build / 'bulk'
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
            '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O0', '-Wall', '-Wextra', '-Werror',
            '-I' + str(build), '-I' + str(ROOT), str(ROOT / 'tests/memory_bulk.c'),
            '-pthread', '-o', str(cls.binary)], check=True, timeout=15)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ' PASS', result.stdout)

    def test_bulk_counts_bytes_large_ranges_and_request_reuse(self):
        self.run_case('bulk')

    def test_all_coherent_uploads_acknowledged_before_each_submit(self):
        self.run_case('ordering')

    def test_upload_errors_bad_ack_and_allocation_failure_prevent_submit(self):
        self.run_case('errors')

    def test_native_and_proxy_length_offset_mapping_and_overflow_bounds(self):
        self.run_case('bounds')

    def test_v6_writes_and_v7_reads_keep_4k_behavior(self):
        self.run_case('legacy')


if __name__ == '__main__':
    unittest.main()
