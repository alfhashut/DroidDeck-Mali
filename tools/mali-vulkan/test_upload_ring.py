"""Tiny private-header/patch checks; no Gamescope, ICD or Android build."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from test_perf import PATCH

ROOT = Path(__file__).resolve().parent
RING_PATCH = PATCH.with_name('0123-mali-upload-ring-ranges.patch')


class UploadRingTests(unittest.TestCase):
    def test_patch_header_parity_all_writer_hooks_and_normal_only_registration(self):
        for name in ('upload_ring.h', 'upload_ring_api.h'):
            section = RING_PATCH.read_text().split('+++ b/src/' + name + '\n', 1)[1].splitlines()
            count = int(re.fullmatch(r'@@ -0,0 \+1,(\d+) @@', section[0])[1])
            self.assertTrue(all(line.startswith('+') for line in section[1:count + 1]))
            self.assertEqual(''.join(line[1:] + '\n' for line in section[1:count + 1]), (ROOT / name).read_text())
        patch = RING_PATCH.read_text()
        self.assertFalse([s for s in patch.splitlines() if s.startswith('-') and not s.startswith('---')])
        self.assertIn('if (m_maliDiagnostic && mali_normal_active())', patch)
        self.assertIn('m_uploadRingApi = nullptr;', patch)
        self.assertIn('uploadRingOperation(DD_RING_RESERVE, uOffset, size)', patch)
        for marker in ('offset, sizeof(data)', 'base_offset, lut1d_size + lut3d_size',
                       'offset, uint64_t(width) * height * 4', 'offset, size'):
            self.assertIn('uploadRingOperation(DD_RING_MODIFIED, ' + marker + ')', patch)
        self.assertIn('uploadRingOperation(DD_RING_SEAL, 0, 0, rawCmdBuffer)', patch)
        self.assertNotIn('584', patch)  # size comes from actual writer, never the protocol
        for name in ('device_icd.h', 'icd_proxy.c'):
            self.assertIn('if (!strcmp(name, "vkDroidDeckUploadRingMALI")) return (PFN_vkVoidFunction)proxy_DroidDeckUploadRingMALI;',
                          (ROOT / name).read_text())
        added = '\n'.join(s[1:] for s in patch.splitlines() if s.startswith('+') and not s.startswith('+++'))
        for operation in ('QueueSubmit(', 'WaitSemaphores', 'DeviceWaitIdle', 'CmdCopy', 'MB_MEMORY_WRITE', 'NormalRPC('):
            self.assertNotIn(operation, added)

    def test_cpp_private_ABI_bounded_ranges_and_saturated_epoch_fallback(self):
        with tempfile.TemporaryDirectory(prefix='mali-ring-') as directory:
            root = Path(directory)
            (root / 'vulkan').mkdir()
            (root / 'vulkan/vulkan.h').write_text('''#pragma once
#include <stdint.h>
#define VKAPI_PTR
using VkResult = int; using VkDevice = void *;
using VkDeviceMemory = uint64_t; using VkBuffer = uint64_t;
using VkCommandBuffer = void *; using VkDeviceSize = uint64_t;
''')
            (root / 'ring.cpp').write_text(r'''
#include <cassert>
#include "upload_ring_api.h"
static int probe(VkDevice, VkDeviceMemory, VkBuffer, VkCommandBuffer, uint32_t, VkDeviceSize, VkDeviceSize) { return 0; }
int main() {
    PFN_vkDroidDeckUploadRingMALI api = probe; assert(api(nullptr, 0, 0, nullptr, DD_RING_PROBE, 0, 0) == 0);
    dd_upload_ring r; dd_ring_init(&r, 524288);
    assert(dd_ring_fallback(&r, 1) & DD_RING_BASELINE);
    dd_ring_seal(&r, 1); dd_ring_uploaded(&r, 1); dd_ring_submitted(&r, 1);
    assert(!dd_ring_reserve(&r, UINT64_MAX, 1));
    dd_ring_seal(&r, 1); dd_ring_uploaded(&r, 1); dd_ring_submitted(&r, 1);
    assert(dd_ring_reserve(&r, 32, 8)); assert(dd_ring_modified(&r, 32, 8));
    assert(dd_ring_reserve(&r, 48, 8)); assert(dd_ring_modified(&r, 48, 8));
    assert(dd_ring_reserve(&r, 40, 8)); assert(dd_ring_modified(&r, 40, 8));
    assert(r.count == 1 && r.ranges[0].offset == 32 && r.ranges[0].length == 24);
    dd_ring_seal(&r, 1); assert(!dd_ring_fallback(&r, 1));
    dd_ring_uploaded(&r, 1); dd_ring_submitted(&r, 2); assert(r.count == 1);
    dd_ring_submitted(&r, 1); assert(!r.count);
    assert(!dd_ring_modified(&r, 64, 1)); // store without reservation is uncertain
    assert(dd_ring_fallback(&r, 1) & DD_RING_UNFINISHED);
    r.generation = UINT64_MAX;
    assert(dd_ring_reserve(&r, 0, 1)); assert(dd_ring_modified(&r, 0, 1));
    dd_ring_seal(&r, 1); dd_ring_uploaded(&r, 1); dd_ring_submitted(&r, 1);
    assert(r.count == 1 && dd_ring_fallback(&r, 1)); // no generation wrap can lose data
}
''')
            binary = root / 'ring'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + [
                '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fno-exceptions',
                '-I' + str(root), '-I' + str(ROOT), str(root / 'ring.cpp'), '-o', str(binary)],
                check=True, timeout=10)
            subprocess.run([str(binary)], check=True, timeout=2)


if __name__ == '__main__':
    unittest.main()
