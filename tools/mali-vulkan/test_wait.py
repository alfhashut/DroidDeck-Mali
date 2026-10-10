"""Wait attribution helpers only: fake Vulkan/clock/transport; no project build."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
from test_perf import PATCH, added_file

ROOT = Path(__file__).resolve().parent


class WaitAttributionTests(unittest.TestCase):
    def test_native_proxy_measurements_and_error_semantics(self):
        with tempfile.TemporaryDirectory(prefix='mali-wait-') as directory:
            root = Path(directory)
            native = (ROOT.parents[1] / 'app/src/main/cpp/malivulkan').resolve()
            (root / 'native_wait.inc').write_text((native / 'normal_wait_native.h').read_text())
            code = (native / 'renderer_commands.h').read_text()
            case = code.split('    case MB_RENDERER_COUNTER: case MB_RENDERER_WAIT: {', 1)[1].split('    case MB_RENDERER_VIEW:', 1)[0]
            (root / 'native_case.inc').write_text('    case MB_RENDERER_COUNTER: case MB_RENDERER_WAIT: {' + case)
            code = (ROOT / 'renderer_icd.h').read_text()
            fn = code.split('static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckProfiledWaitMALI', 1)[1].split('static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateSampler', 1)[0]
            (root / 'proxy_wait.inc').write_text('static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckProfiledWaitMALI' + fn)
            code = (ROOT / 'icd_proxy.c').read_text()
            fn = code.split('static VkResult rpc(struct proxy_instance *s,', 1)[1].split('static void proxy_free_logical', 1)[0]
            (root / 'metric.inc').write_text('static VkResult rpc(struct proxy_instance *s,' + fn)
            binary = root / 'wait'
            subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
                '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O0', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root), '-I' + str(ROOT), str(ROOT / 'tests/wait_attribution.c'),
                '-pthread', '-o', str(binary)], check=True, timeout=15)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('wait attribution PASS', result.stdout)
            self.assertIn('wait-reason=shm-upload-staging-destroy count=1 frame-count=1 outside-count=0 per-frame=1.00 RTT-total/avg/max=2.600/2.600/2.600ms native-wait-total/avg/max=2.000/2.000/2.000ms native-CPU=0.100ms native-off-CPU-est=1.900ms counter-query=0.100ms samples=1 already-satisfied=0 briefly-blocked=0 materially-blocked=1', result.stdout)
            self.assertIn('last-target=42 before=41 before-result=0x0 semaphore=10 detail=20 latest-submitted=42 producer-found=1 command=8 image=30 buffer=20 memory=21 descriptor-set=31 fence=40', result.stdout)
            self.assertIn('wait-reason=descriptor-set-reuse', result.stdout)
            self.assertIn('already-satisfied=1', result.stdout)
            self.assertIn('wait-reason=output-producer-completion', result.stdout)
            self.assertIn('briefly-blocked=1', result.stdout)
            self.assertIn('typed-frame=12 other-management-frame=0 typed-outside=4 other-management-outside=0', result.stdout)
            self.assertIn('type=image-view frame-create/query/destroy=2/0/0 outside-create/query/destroy=0/0/2', result.stdout)
            self.assertIn('type=memory frame-create/query/destroy=2/0/1 outside-create/query/destroy=0/0/1', result.stdout)

    def test_scope_restoration_and_unchanged_non_normal_waits(self):
        # Compile only the scope helper, with a minimal Vulkan type header.
        with tempfile.TemporaryDirectory(prefix='mali-wait-scope-') as directory:
            root = Path(directory)
            (root / 'vulkan').mkdir()
            (root / 'vulkan/vulkan.h').write_text('''#pragma once
#include <stdint.h>
#define VKAPI_PTR
using VkDevice = void *; using VkResult = int; using VkBool32 = uint32_t;
struct VkSemaphoreWaitInfo { unsigned flags; }; using VkSemaphoreWaitInfoKHR = VkSemaphoreWaitInfo;
using PFN_vkVoidFunction = void (*)();
using PFN_vkWaitSemaphores = VkResult (*)(VkDevice, const VkSemaphoreWaitInfo *, uint64_t);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction (*)(VkDevice, const char *);
''')
            (root / 'scope.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include "mali_wait_profile.hpp"
static int originals, profiles, lookups; static bool missing;
static const VkSemaphoreWaitInfo info{}; static const uint64_t timeout = 5000000000;
static uint32_t reason; static uint64_t resource;
static int original(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t) {
    assert(d == (void *)1 && i == &info && t == timeout); ++originals; return 2;
}
static int profiled(VkDevice d, const VkSemaphoreWaitInfo *i, uint64_t t, uint32_t r, uint64_t detail) {
    assert(d == (void *)1 && i == &info && t == timeout); ++profiles; reason = r; resource = detail; return 2;
}
static PFN_vkVoidFunction lookup(VkDevice, const char *name) {
    assert(!strcmp(name, "vkDroidDeckProfiledWaitMALI")); ++lookups;
    return missing ? nullptr : reinterpret_cast<PFN_vkVoidFunction>(profiled);
}
int main(int argc, char **) {
    missing = argc > 1;
    assert(MaliProfiledWait(original, lookup, (void *)1, &info, timeout) == 2);
    assert(originals == 1 && !lookups && !profiles);
    maliWaitProfilingActive = true;
    { MaliWaitScope scope(DD_WAIT_SHM_STAGING_DESTROY, 12345);
      assert(MaliProfiledWait(original, lookup, (void *)1, &info, timeout) == 2);
      if (!missing) assert(reason == DD_WAIT_SHM_STAGING_DESTROY && resource == 12345);
      { MaliWaitScope inner(DD_WAIT_DESCRIPTOR_REUSE, 3);
        assert(MaliProfiledWait(original, lookup, (void *)1, &info, timeout) == 2);
        if (!missing) assert(reason == DD_WAIT_DESCRIPTOR_REUSE && resource == 3);
      }
      assert(maliWaitContext.reason == DD_WAIT_SHM_STAGING_DESTROY && maliWaitContext.resource == 12345);
    }
    assert(maliWaitContext.reason == DD_WAIT_UNATTRIBUTED && !maliWaitContext.resource);
    assert(lookups == 1 && (missing ? originals == 3 && !profiles : profiles == 2 && originals == 1));
    maliWaitProfilingActive = false;
    assert(MaliProfiledWait(original, lookup, (void *)1, &info, timeout) == 2);
    assert(lookups == 1 && originals == (missing ? 4 : 2));
}
''')
            binary = root / 'scope'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + [
                '-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror', '-I' + str(root),
                '-I' + str(ROOT), str(root / 'scope.cpp'), '-o', str(binary)], check=True, timeout=15)
            for arguments in ([], ['missing-entrypoint']):
                subprocess.run([str(binary), *arguments], check=True, timeout=2)

    def test_real_source_sites_and_normal_only_gate(self):
        patch = PATCH.read_text()
        self.assertIn('MaliWaitScope site(DD_WAIT_DESCRIPTOR_REUSE, uIndex)', patch)
        self.assertIn('MaliWaitScope site(DD_WAIT_SHM_STAGING_DESTROY, (uint64_t)(uintptr_t)buffer)', patch)
        normal = added_file(PATCH, 'mali_normal.inc')
        self.assertIn('{ MaliWaitScope site(DD_WAIT_OUTPUT_COMPLETION, index); g_device.wait(*sequence, false); }', normal)
        self.assertIn('{ MaliWaitScope site(DD_WAIT_OUTPUT_RETIRE, index); g_device.wait(*sequence, true); }', normal)
        self.assertIn('maliWaitProfilingActive = false;', normal.split('int vulkan_mali_normal_stop()', 1)[1])
        perf = added_file(PATCH, 'mali_normal_perf.inc')
        self.assertIn('maliWaitProfilingActive = true;', perf.split('void vulkan_mali_normal_frame_begin()', 1)[1])
        self.assertIn('MaliPerf wait coverage:', perf)
        # No additional socket exchange for tagging, counters, or report snapshots.
        scope = (ROOT / 'mali_wait_profile.hpp').read_text()
        self.assertNotIn('rpc(', scope)
        native = (ROOT.parents[1] / 'app/src/main/cpp/malivulkan/normal_wait_native.h').read_text()
        self.assertEqual(native.count('counter(device,'), 1)
        self.assertEqual(native.count('wait(device,'), 1)
        self.assertNotIn('if (', native)  # The observation cannot bypass the real wait.


if __name__ == '__main__':
    unittest.main()
