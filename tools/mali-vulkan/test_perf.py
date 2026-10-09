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
        for name in ('normal_api.h', 'normal_perf.h', 'normal_opcode_perf.h', 'normal_protocol.h', 'renderer_protocol.h', 'interop_protocol.h'):
            self.assertEqual(added_file(PATCH, name), (ROOT / name).read_text(), name)
        self.assertNotIn('Performance', (ROOT / 'normal_protocol.h').read_text())
        local = (ROOT / 'interop_icd.h').read_text().split('proxy_DroidDeckPerformanceMALI', 1)[1].split('proxy_DroidDeckWaylandMALI', 1)[0]
        self.assertNotIn('interop_rpc(', local)  # Snapshots add no round trips.
        self.assertIn('pthread_mutex_lock(&s->lock)', local)

    def test_opcode_names_match_every_wire_opcode(self):
        # Parse headers as text: no Vulkan/Android header dependency or build.
        wire = {}
        for name in ('protocol.h', 'device_protocol.h', 'interop_protocol.h', 'renderer_protocol.h', 'normal_protocol.h'):
            wire.update((name, int(value)) for name, value in re.findall(
                r'^#define (MB_\w+) (\d+)u\b', (ROOT / name).read_text(), re.M))
        submit = (ROOT / 'submit_protocol.h').read_text().split('enum {', 1)[1].split('}', 1)[0]
        wire.update((name, number) for number, name in enumerate(re.findall(r'MB_\w+', submit), 17))
        table = re.findall(r'\{"(\w+)", DD_PERF_(\w+)\}, /\* (\d+) \*/', (ROOT / 'normal_opcode_perf.h').read_text())
        self.assertEqual([int(number) for _, _, number in table], list(range(92)))
        self.assertEqual(len({name for name, _, _ in table}), 92)
        self.assertEqual(table[0], ('UNKNOWN', 'OTHER', '0'))
        for name, _, number in table[1:]:
            self.assertEqual(wire[name], int(number), name)
        categories = {
            'MEMORY': set(range(38, 44)),
            'RECORDING': {21, 22, 26, 48, 49, 50, 51, 73, 74, 75, 77, 79, 80, 81},
            'UPDATES': {37, 47, 70}, 'SUBMIT': {31, 76},
            'SYNC': {25, 29, 30, 55, 56, 57, 62, 63, 82},
            'RESOURCES': {2, 3, *range(14, 21), 23, 24, 27, 28, *range(32, 37),
                          44, 45, 46, 54, 60, 61, *range(64, 70), 71, 72, 78},
            'PRESENT': {52, 53, 58, 59, *range(83, 92)},
            'OTHER': {0, 1, *range(4, 14)},
        }
        for category, expected in categories.items():
            self.assertEqual({int(number) for _, c, number in table if c == category}, expected, category)

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
            for name in ('mali_normal_perf.inc', 'normal_perf.h', 'normal_opcode_perf.h', 'renderer_protocol.h', 'interop_protocol.h'):
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
static int NormalRPC(unsigned op, int, dd_normal_output *) {
    // Reporting STATS costs client wall time too, outside frame attempts.
    dd_perf_add(&pending, op, 2000000); fakeNow += 2000000; return 0;
}
static bool MaliResult(int result, const char *) { return result == 0; }
static void MaliRequire(bool ok, const char *) { assert(ok); }
#include "mali_normal_perf.inc"
int main() {
    dd_perf_rpc a{}, b{}; dd_perf_add(&a, 41, 3); dd_perf_add(&a, 41, 9);
    dd_perf_add(&b, 41, 4); b.upload_bytes = 4096; dd_perf_merge(&a, &b);
    assert(a.op[41].count == 3 && a.op[41].ns == 16 && a.op[41].worst == 9 && a.upload_bytes == 4096);
    dd_perf_add(&a, DD_PERF_OPS, 100); assert(a.op[41].count == 3);
    dd_perf_rpc all{}; dd_perf_op categories[DD_PERF_CATEGORIES];
    for (unsigned i = 0; i < DD_PERF_OPS; ++i) dd_perf_add(&all, i, i + 1);
    auto total = dd_perf_categories(&all, categories);
    assert(total.count == 92 && total.ns == 4278 && total.worst == 92);
    dd_perf_op sum{};
    for (auto &category : categories) dd_perf_op_merge(&sum, &category);
    assert(sum.count == total.count && sum.ns == total.ns && sum.worst == total.worst);
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
    // Initialization is not steady-state traffic.
    dd_perf_add(&pending, 87, 9000000);
    for (unsigned i = 0; i < 3; ++i) {
        assert(normalOutput.ownership.acquire(i) == int(i));
        assert(normalOutput.ownership.ready(i) && normalOutput.ownership.presented(i));
    }
    vulkan_mali_normal_tick(); assert(!vulkan_mali_normal_available());
    assert(pending.op[87].count == 0);
    fakeNow += 100000000; vulkan_mali_normal_tick();
    assert(normalPerf.outputWaitNs == 100000000 && normalPerf.maxAndroid == 3);
    for (unsigned i = 0; i < 3; ++i) assert(normalOutput.ownership.androidOwned(i));
    assert(normalOutput.ownership.released(0) && vulkan_mali_normal_available());
    dd_perf_add(&pending, 62, 3000000); // Outside a frame, collected at begin.
    for (unsigned i = 0; i < 2; ++i) {
        vulkan_mali_normal_frame_begin(); fakeNow += 2000000; vulkan_mali_normal_upload_done();
        dd_perf_add(&pending, 41, 500000); dd_perf_add(&pending, 41, 1500000);
        dd_perf_add(&pending, 63, 3000000); dd_perf_add(&pending, 76, 1000000);
        dd_perf_add(&pending, 79, 2000000);
        pending.upload_bytes = 8192;
        fakeNow += i ? 48000000 : 18000000; vulkan_mali_normal_frame_end(true);
    }
    assert(normalPerf.frames == 2 && normalPerf.missed == 1 && normalOutput.late == 1);
    assert(normalPerf.rpc.op[41].count == 4 && normalPerf.frameNs == 70000000);
    assert(normalPerf.cpuNs == 17500000);
    assert(normalPerf.lastWorst == 3000000 && normalPerf.lastWorstOp == 63);
    assert(normalPerf.outside.op[62].count == 1 && normalPerf.rpc.op[62].count == 0);
    dd_perf_add(&pending, 0, 1000000); // Unknown opcode remains accounted as other.
    fakeNow = 3000000000; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0 && normalPerf.rpc.op[41].count == 0 && normalPerf.outside.op[62].count == 0);
    assert(pending.op[91].count == 0); // STATS must not leak into next frame.
    assert(normalPerf.lastEnd && normalPerf.previousPresented == 1);
    // A failed/backpressured attempt contributes work without inventing frames.
    vulkan_mali_normal_frame_begin(); fakeNow += 10000000; vulkan_mali_normal_frame_end(false);
    assert(normalPerf.frames == 0 && normalPerf.attempts == 1);
    fakeNow = normalPerf.report + DD_PERF_INTERVAL_NS; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0);
    // Idle reports still fire and never invent rendered frames.
    fakeNow += 2000000000; vulkan_mali_normal_tick();
    assert(normalPerf.frames == 0);
    // Truly empty window is finite, emits no opcode rows, and has no leaders.
    dd_perf_rpc empty{};
    dd_perf_report_opcodes(stdout, &empty, &empty, 0, 0);
}
''')
            binary = root / 'test'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + ['-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror', str(root / 'test.cpp'), '-o', str(binary)], check=True, timeout=15)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('committed-FPS=1.00 Android-FPS=0.50 frames=2', result.stdout)
            self.assertIn('work-avg/p95=35.000/50.000ms', result.stdout)
            self.assertIn('per-frame=5.00 total=10 total=16.000ms', result.stdout)
            self.assertIn('uploads/frame=8192bytes write-RPCs/frame=2.00', result.stdout)
            self.assertIn('main-thread-CPU=17.500ms off-CPU=52.500ms', result.stdout)
            self.assertEqual(result.stdout.count('MaliPerf frame:'), 3)
            self.assertIn('frames=0 attempts=1 work-avg/p95=10.000/10.000ms', result.stdout)
            self.assertIn('frames=0 attempts=0', result.stdout)
            windows = result.stdout.split('MaliPerf opcode-window: ')[1:]
            self.assertEqual(len(windows), 4)
            self.assertIn('count=13 frame-count=10 outside-count=3 frame-RPCs/frame=5.00 client-RTT=22.000ms', windows[0])
            self.assertIn('name=MB_MEMORY_WRITE category=mapped-memory-transfer count=4 count-pct=30.77 frame-count=4 outside-count=0 frame-RPCs/frame=2.00 RTT-total=4.000ms RTT-avg=1.000ms RTT-max=1.500ms', windows[0])
            self.assertIn('name=MB_NORMAL_STATS category=AHB-presentation count=1', windows[0])
            self.assertIn('most-frame-count-op=MB_MEMORY_WRITE largest-RTT-op=MB_RENDERER_WAIT largest-RTT-category=timeline-synchronization command-recording-RTT-pct=18.18', windows[0])
            # Disjoint categories cover the full window, without double counting
            # frame snapshots or STATS. Category maximum is a max, not a sum.
            for index, window in enumerate(windows):
                header = window.splitlines()[0]
                total = int(re.search(r'\bcount=(\d+)', header)[1])
                op_rows = [line for line in window.splitlines() if line.startswith('MaliPerf opcode:')]
                category_rows = [line for line in window.splitlines() if line.startswith('MaliPerf category:')]
                self.assertEqual(len(category_rows), 8)
                self.assertEqual(sum(int(re.search(r'\bcount=(\d+)', line)[1]) for line in op_rows), total)
                self.assertEqual(sum(int(re.search(r'\bcount=(\d+)', line)[1]) for line in category_rows), total)
                self.assertAlmostEqual(sum(float(re.search(r'count-pct=([\d.]+)', line)[1]) for line in op_rows), 100 if total else 0, delta=0.06)
                self.assertAlmostEqual(sum(float(re.search(r'RTT-total=([\d.]+)ms', line)[1]) for line in op_rows), float(re.search(r'client-RTT=([\d.]+)ms', header)[1]))
                if index in (1, 2):
                    self.assertEqual(total, 1)  # Only that window's STATS.
                    self.assertNotIn('name=MB_MEMORY_WRITE', window)
                self.assertNotRegex(window.lower(), r'\b(nan|inf)\b')
            self.assertIn('name=timeline-synchronization count=3 count-pct=23.08 frame-count=2 outside-count=1 frame-RPCs/frame=1.00 RTT-total=9.000ms RTT-pct=40.91 RTT-avg=3.000ms RTT-max=3.000ms', windows[0])
            self.assertIn('name=UNKNOWN category=other count=1', windows[0])
            self.assertNotIn('MaliPerf opcode:', windows[-1])
            self.assertIn('most-frame-count-op=none largest-RTT-op=none largest-RTT-category=none', windows[-1])


if __name__ == '__main__':
    unittest.main()
