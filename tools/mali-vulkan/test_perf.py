"""Tiny mocked-clock metric test. No ICD, broker, Gamescope or Android build."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
PATCH = ROOT.parent / 'gamescope/patches/0121-mali-normal-wayland-session.patch'


def added_file(patch, name):
    section = patch.read_text().split('+++ b/src/' + name + '\n', 1)[1].split('\n--- a/', 1)[0]
    lines = section.splitlines()
    count = int(re.fullmatch(r'@@ -0,0 \+1,(\d+) @@', lines[0])[1])
    assert len(lines[1:]) == count and all(line.startswith('+') for line in lines[1:])
    return ''.join(line[1:] + '\n' for line in lines[1:])


class PerformanceTests(unittest.TestCase):
    def test_header_copies_and_wire_unchanged(self):
        for name in ('normal_api.h', 'normal_perf.h', 'normal_protocol.h', 'renderer_protocol.h', 'interop_protocol.h'):
            self.assertEqual(added_file(PATCH, name), (ROOT / name).read_text(), name)
        self.assertNotIn('Performance', (ROOT / 'normal_protocol.h').read_text())
        local = (ROOT / 'interop_icd.h').read_text().split('proxy_DroidDeckPerformanceMALI', 1)[1].split('proxy_DroidDeckWaylandMALI', 1)[0]
        self.assertNotIn('interop_rpc(', local)  # Snapshots add no round trips.
        self.assertIn('pthread_mutex_lock(&s->lock)', local)

    def test_normal_frame_scope_and_safe_optimizations(self):
        main = added_file(PATCH, 'mali_normal.cpp')
        frame = main.split('if (server.pending && server.surface', 1)[1].split('if (now - progress', 1)[0]
        self.assertLess(frame.index('vulkan_mali_normal_available()'), frame.index('vulkan_create_texture_from_wlr_buffer'))
        self.assertLess(frame.index('vulkan_mali_normal_frame_begin()'), frame.index('vulkan_create_texture_from_wlr_buffer'))
        self.assertLess(frame.index('source = nullptr'), frame.index('vulkan_mali_normal_frame_end(result == 0)'))
        self.assertIn('(perfBaseline ? std::chrono::steady_clock::now() : now)', frame)
        self.assertIn('MALI_VULKAN_PERF_BASELINE', main)
        service = (ROOT.parents[1] / 'app/src/main/java/com/droiddeck/launcher/session/SessionService.kt').read_text()
        normal = service.split('private fun runMaliSession', 1)[1].split('private fun stopMaliGuest', 1)[0]
        self.assertIn('extraEnv().lastOrNull { it == "MALI_VULKAN_PERF_BASELINE=0" || it == "MALI_VULKAN_PERF_BASELINE=1" }', normal)
        self.assertNotIn('guest.addAll(extraEnv())', normal)
        present = added_file(PATCH, 'mali_normal.inc').split('int vulkan_mali_normal_present', 1)[1].split('int vulkan_mali_normal_stop', 1)[0]
        for required in ('ownership.acquire', 'ownership.ready', 'ownership.presented', 'DD_SYNC_EXPORT', 'DD_SYNC_WAIT', 'MB_NORMAL_PUBLISH', 'g_device.wait(*sequence, false)', 'g_device.wait(*sequence, true)'):
            self.assertIn(required, present)
        self.assertIn('m_maliReadbackBuffer = VK_NULL_HANDLE', present)
        for forbidden in ('DD_AHB_INSPECT', 'DD_AHB_CREATE', 'DeviceWaitIdle', 'QueueWaitIdle'):
            self.assertNotIn(forbidden, present)

    def test_real_aggregator_with_mock_clock_and_rpc_snapshots(self):
        # Compile only the ~150-line metric helper with tiny stubs. This never
        # includes rendervulkan.cpp, Vulkan headers, the ICD, or native broker.
        with tempfile.TemporaryDirectory(prefix='mali-perf-') as directory:
            root = Path(directory)
            for name in ('mali_normal_perf.inc', 'normal_perf.h', 'renderer_protocol.h', 'interop_protocol.h'):
                (root / name).write_text(added_file(PATCH, name))
            wrapper = (ROOT / 'icd_proxy.c').read_text().split('static VkResult rpc(struct proxy_instance *s,', 1)[1].split('static void proxy_free_logical', 1)[0]
            (root / 'rpc_metric.inc').write_text('static VkResult rpc(struct proxy_instance *s,' + wrapper)
            (root / 'mali_output_pool.hpp').write_text(added_file(PATCH.with_name('0120-mali-persistent-session.patch'), 'mali_output_pool.hpp'))
            (root / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdint>
static uint64_t fakeNow = 1000000000;
static int fake_clock_gettime(clockid_t clock, timespec *t) {
    uint64_t time = clock == CLOCK_THREAD_CPUTIME_ID ? fakeNow / 4 : fakeNow;
    t->tv_sec = time / 1000000000; t->tv_nsec = time % 1000000000; return 0;
}
#define clock_gettime fake_clock_gettime
#include "normal_perf.h"
#include "mali_output_pool.hpp"
#include "interop_protocol.h"
using VkResult = int;
static constexpr int VK_SUCCESS = 0;
struct proxy_instance { int perf_enabled = 1; dd_perf_rpc perf{}; };
static int exchangeResult;
static VkResult rpc_exchange(proxy_instance *, uint32_t, const uint8_t *, uint32_t, uint8_t *, uint32_t *, uint32_t) {
    fakeNow += 1000; return exchangeResult;
}
static uint32_t mb_get_u32(const uint8_t *p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
#include "rpc_metric.inc"
#define VK_TRUE 1
#define MB_NORMAL_STATS 91u
#define MB_NORMAL_PUBLISH 89u
static dd_perf_rpc pending;
static int snapshot(int, dd_perf_rpc *out, int reset) { *out = pending; if (reset) pending = {}; return 0; }
static struct { int device() { return 1; } } g_device;
static struct { MaliOutputPool ownership; unsigned late = 0; decltype(&snapshot) performance = snapshot; } normalOutput;
struct dd_normal_output { unsigned presented = 1, owned = 2, counts[20]{}; };
static int NormalRPC(unsigned, int, dd_normal_output *) { return 0; }
static bool MaliResult(int result, const char *) { return result == 0; }
static void MaliRequire(bool ok, const char *) { assert(ok); }
#include "mali_normal_perf.inc"
int main() {
    dd_perf_rpc a{}, b{}; dd_perf_add(&a, 41, 3); dd_perf_add(&a, 41, 9);
    dd_perf_add(&b, 41, 4); b.upload_bytes = 4096; dd_perf_merge(&a, &b);
    assert(a.op[41].count == 3 && a.op[41].ns == 16 && a.op[41].worst == 9 && a.upload_bytes == 4096);
    dd_perf_add(&a, DD_PERF_OPS, 100); assert(a.op[41].count == 3);
    uint64_t samples[20]; for (unsigned i = 0; i < 20; ++i) samples[i] = 20 - i;
    assert(dd_perf_p95(samples, 20) == 19 && dd_perf_p95(samples, 0) == 0);
    proxy_instance connection; uint8_t request[20]{}; request[16] = 7;
    assert(rpc(&connection, 41, request, 20, nullptr, nullptr, 0) == 0);
    assert(connection.perf.upload_bytes == 7 && connection.perf.op[41].ns == 1000);
    exchangeResult = -9;
    assert(rpc(&connection, 41, request, 20, nullptr, nullptr, 0) == -9);
    assert(connection.perf.upload_bytes == 7 && connection.perf.op[41].count == 2 && connection.perf.op[41].worst == 1000);
    connection.perf_enabled = 0;
    assert(rpc(&connection, 41, request, 20, nullptr, nullptr, 0) == -9);
    assert(connection.perf.op[41].count == 2); fakeNow = 1000000000;
    for (unsigned i = 0; i < 3; ++i) {
        assert(normalOutput.ownership.acquire(i) == int(i));
        assert(normalOutput.ownership.ready(i) && normalOutput.ownership.presented(i));
    }
    vulkan_mali_normal_tick(); assert(!vulkan_mali_normal_available());
    fakeNow += 100000000; vulkan_mali_normal_tick();
    assert(normalPerf.outputWaitNs == 100000000 && normalPerf.maxAndroid == 3);
    for (unsigned i = 0; i < 3; ++i) assert(normalOutput.ownership.androidOwned(i));
    assert(normalOutput.ownership.released(0) && vulkan_mali_normal_available());
    for (unsigned i = 0; i < 2; ++i) {
        vulkan_mali_normal_frame_begin(); fakeNow += 2000000; vulkan_mali_normal_upload_done();
        dd_perf_add(&pending, 41, 1000000); dd_perf_add(&pending, 41, 1000000);
        dd_perf_add(&pending, 63, 5000000); dd_perf_add(&pending, 76, 1000000);
        pending.upload_bytes = 8192;
        fakeNow += i ? 48000000 : 18000000; vulkan_mali_normal_frame_end(true);
    }
    assert(normalPerf.frames == 2 && normalPerf.missed == 1 && normalOutput.late == 1);
    assert(normalPerf.rpc.op[41].count == 4 && normalPerf.frameNs == 70000000);
    assert(normalPerf.cpuNs == 17500000);
    assert(normalPerf.lastWorst == 5000000 && normalPerf.lastWorstOp == 63);
    fakeNow = 3000000000; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0 && normalPerf.rpc.op[41].count == 0);
    assert(normalPerf.lastEnd && normalPerf.previousPresented == 1);
    // A failed/backpressured attempt contributes work without inventing frames.
    vulkan_mali_normal_frame_begin(); fakeNow += 10000000; vulkan_mali_normal_frame_end(false);
    assert(normalPerf.frames == 0 && normalPerf.attempts == 1);
    fakeNow = 5000000000; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0);
    // Idle reports still fire and never invent rendered frames.
    fakeNow += 2000000000; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0);
}
''')
            binary = root / 'test'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + ['-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror', str(root / 'test.cpp'), '-o', str(binary)], check=True, timeout=15)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('committed-FPS=1.00 Android-FPS=0.50 frames=2', result.stdout)
            self.assertIn('work-avg/p95=35.000/50.000ms', result.stdout)
            self.assertIn('per-frame=4.00 total=8 total=16.000ms', result.stdout)
            self.assertIn('uploads/frame=8192bytes write-RPCs/frame=2.00', result.stdout)
            self.assertIn('main-thread-CPU=17.500ms off-CPU=52.500ms', result.stdout)
            self.assertEqual(result.stdout.count('MaliPerf frame:'), 3)
            self.assertIn('frames=0 attempts=1 work-avg/p95=10.000/10.000ms', result.stdout)
            self.assertIn('frames=0 attempts=0', result.stdout)


if __name__ == '__main__':
    unittest.main()
