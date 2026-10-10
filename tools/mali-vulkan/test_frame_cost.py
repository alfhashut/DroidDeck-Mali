"""Small attribution helpers only: no Gamescope/ICD/broker project build."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
from test_perf import PATCH, added_file

ROOT = Path(__file__).resolve().parent
DETAIL_PATCH = PATCH.with_name('0122-mali-frame-cost-attribution.patch')


class FrameCostTests(unittest.TestCase):
    def test_exclusive_scopes_wall_cpu_sampling_output_and_disabled_path(self):
        with tempfile.TemporaryDirectory(prefix='mali-frame-cost-') as directory:
            root = Path(directory)
            (root / 'test.cpp').write_text(r'''
#include <cassert>
#include <ctime>
#include <cstdint>
static uint64_t wall = 1000000000, cpu = 1000;
static unsigned wallCalls, cpuCalls;
static int clockStub(clockid_t id, timespec *t) {
    uint64_t n = wall;
    if (id == CLOCK_THREAD_CPUTIME_ID) { n = cpu; ++cpuCalls; }
    else if (id == CLOCK_PROCESS_CPUTIME_ID) n = cpu;
    else ++wallCalls;
    t->tv_sec = n / 1000000000; t->tv_nsec = n % 1000000000; return 0;
}
#define clock_gettime clockStub
#include "normal_detail_perf.hpp"
static void advance(unsigned w, unsigned c) { wall += w * 1000000ull; cpu += c * 1000000ull; }
int main() {
    { MaliCpuScope off(DD_CPU_DESCRIPTORS); off.set(DD_CPU_COMMANDS); }
    assert(!wallCalls && !cpuCalls); // Diagnostics/non-normal paths read no clocks.
    maliCpuDetail.begin();
    {
        MaliCpuScope comp(DD_CPU_SCENE, DD_CPU_COMPOSITE); advance(10, 2);
        { MaliCpuScope desc(DD_CPU_DESCRIPTORS); advance(20, 3); }
        { MaliCpuScope uniforms(DD_CPU_UNIFORMS); advance(5, 1); }
        comp.set(DD_CPU_COMMANDS); advance(15, 4);
    }
    maliCpuDetail.end();
    auto &p = maliCpuDetail;
    auto &stages = p.samples[DD_CPU_COMPOSITE];
    assert(stages[DD_CPU_SCENE].wall == 10000000 && stages[DD_CPU_SCENE].cpu == 2000000);
    assert(stages[DD_CPU_DESCRIPTORS].wall == 20000000 && stages[DD_CPU_DESCRIPTORS].cpu == 3000000);
    uint64_t total=0, execution=0; for (auto &s: stages) { total+=s.wall; execution+=s.cpu; }
    assert(total == 50000000 && execution == 10000000 && p.cpuSamples == 1);
    p.shmBytes = 230400; p.uniformBytes = 584; p.uniformWrites = 1; p.uniformSize = 584;
    p.report(1, 50);
    assert(!p.samples[DD_CPU_COMPOSITE][DD_CPU_SCENE].wall && p.serial == 1);
    unsigned before = cpuCalls;
    for (unsigned i=0; i<15; ++i) { p.begin(); { MaliCpuScope scope(DD_CPU_COMMANDS); advance(1, 1); } p.end(); }
    assert(cpuCalls == before && p.cpuSamples == 0);
    p.begin(); advance(1, 1); p.end(); assert(cpuCalls > before && p.cpuSamples == 1);
    before = wallCalls; { MaliCpuScope off(DD_CPU_RETIRE); } assert(wallCalls == before);
}
''')
            binary = root / 'test'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + [
                '-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
                str(root / 'test.cpp'), '-o', str(binary)], check=True, timeout=10)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            line = next(s for s in result.stdout.splitlines() if s.startswith('MaliPerf compositor-cpu:'))
            for item in ('scene=10.000/2.000ms', 'descriptors=20.000/3.000ms', 'uniforms=5.000/1.000ms',
                         'commands=15.000/4.000ms', 'total=50.000ms thread-CPU=10.000ms off-CPU-est=40.000ms',
                         'parent=50.000ms parent-minus-stages=0.000ms', 'CPU-samples=1'):
                self.assertIn(item, line)
            self.assertIn('staging-SHM-copy=230400bytes/attempt uniform-store=584bytes/attempt', result.stdout)
            self.assertIn('thermal not sampled; no throttling inference', result.stdout)

    def test_actual_handle_lookup_traversal_and_disabled_path(self):
        source = (ROOT / 'submit_icd.h').read_text()
        finder = 'static struct proxy_resource *submit_find' + source.split('static struct proxy_resource *submit_find', 1)[1].split('static VkResult submit_rpc', 1)[0]
        with tempfile.TemporaryDirectory(prefix='mali-lookup-') as directory:
            root = Path(directory)
            (root / 'finder.inc').write_text(finder)
            (root / 'test.c').write_text(r'''
#include <assert.h>
#include <pthread.h>
#include "normal_memory_perf.h"
enum proxy_resource_kind { PROXY_MEMORY };
struct proxy_resource { struct proxy_resource *next; enum proxy_resource_kind kind; int live; };
struct proxy_instance { pthread_mutex_t lock; int perf_enabled; struct dd_memory_profile memory_perf; };
struct proxy_logical { struct proxy_instance *owner; struct proxy_resource *resources; };
#include "finder.inc"
int main(void) {
    struct proxy_instance s = {.lock=PTHREAD_MUTEX_INITIALIZER,.perf_enabled=1};
    struct proxy_resource tail={.live=1}, dead={.next=&tail};
    struct proxy_logical d={&s,&dead};
    assert(submit_find(&d,(uintptr_t)&tail,PROXY_MEMORY)==&tail);
    assert(!submit_find(&d,(uintptr_t)&dead,PROXY_MEMORY));
    assert(s.memory_perf.lookups==2 && s.memory_perf.visited==4 && s.memory_perf.max_visit==2);
    s.perf_enabled=0; assert(submit_find(&d,(uintptr_t)&tail,PROXY_MEMORY)==&tail);
    assert(s.memory_perf.lookups==2 && s.memory_perf.visited==4);
    pthread_mutex_destroy(&s.lock);
}
''')
            binary = root / 'test'
            subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
                '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
                str(root / 'test.c'), '-pthread', '-o', str(binary)], check=True, timeout=10)
            subprocess.run([str(binary)], check=True, timeout=2)

    def test_patch_parity_and_no_rendering_or_rpc_operations_added(self):
        self.assertEqual(added_file(DETAIL_PATCH, 'normal_detail_perf.hpp'), (ROOT / 'normal_detail_perf.hpp').read_text())
        patch = DETAIL_PATCH.read_text()
        self.assertFalse([line for line in patch.splitlines() if line.startswith('-') and not line.startswith('---')])
        for line in patch.splitlines():
            if not line.startswith('+') or line.startswith('+++'):
                continue
            for forbidden in ('.vk.', 'vk.QueueSubmit', 'NormalRPC(', 'interop_rpc(', 'renderer_rpc('):
                self.assertNotIn(forbidden, line)
        self.assertIn('MaliCpuScope compositeScope(DD_CPU_SCENE, DD_CPU_COMPOSITE)', patch)
        self.assertIn('maliCpuDetail.uniformBytes += sizeof(data)', patch)
        self.assertIn('maliCpuDetail.shmBytes += uint64_t(stride) * height', patch)
        normal = added_file(PATCH, 'mali_normal_perf.inc')
        self.assertIn('maliCpuDetail.report(p.attempts, p.compositeNs / attempts / 1e6)', normal)


if __name__ == '__main__':
    unittest.main()
