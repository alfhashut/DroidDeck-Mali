"""Tiny normal staging/watermark/dependency harnesses; no project build."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest
from test_perf import PATCH, added_file

ROOT = Path(__file__).resolve().parent
NATIVE = ROOT.parents[1] / 'app/src/main/cpp/malivulkan'


class StagingTests(unittest.TestCase):
    def test_completed_watermark_and_actual_staging_backend(self):
        with tempfile.TemporaryDirectory(prefix='mali-staging-') as directory:
            root = Path(directory)
            (root / 'vulkan').mkdir()
            (root / 'vulkan/vulkan.h').write_text((ROOT / 'tests/staging_vulkan_stub.h').read_text())
            binary = root / 'staging'
            subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) + [
                '-std=c++17', '-D_POSIX_C_SOURCE=200809L', '-O0', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root), '-I' + str(ROOT), str(ROOT / 'tests/watermark_staging.cpp'),
                '-o', str(binary)], check=True, timeout=15)
            for args in ([], ['old-proxy-fallback']):
                result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=2)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('completed watermark and real staging backend PASS', result.stdout)

    def test_actual_broker_queued_upload_guards_and_planned_layout(self):
        with tempfile.TemporaryDirectory(prefix='mali-upload-dependency-') as directory:
            root = Path(directory)
            commands = (NATIVE / 'renderer_commands.h').read_text()
            find = commands.split('static struct native_renderer_object *renderer_find', 1)[1].split('#include "renderer_upload_dependency.h"', 1)[0]
            (root / 'renderer_find.inc').write_text('static struct native_renderer_object *renderer_find' + find)
            state = commands.split('static int renderer_image_state', 1)[1].split('static int renderer_descriptors_live', 1)[0]
            (root / 'renderer_state.inc').write_text('static int renderer_image_state' + state)
            submit = commands.split('static int native_renderer_can_submit_ordered', 1)[1].split('static void renderer_destroy_object', 1)[0]
            (root / 'renderer_guard.inc').write_text('static int native_renderer_can_submit_ordered' + submit)
            objects = (NATIVE / 'submit_objects.h').read_text().split('    struct native_command {', 1)[1].split('    } commands[', 1)[0]
            (root / 'command.inc').write_text('struct native_command {' + objects + '};\n')
            interop = (NATIVE / 'interop_commands.h').read_text()
            refs = interop.split('static int interop_referenced', 1)[1].split('static struct native_command *interop_recording', 1)[0]
            (root / 'refs.inc').write_text('static int interop_referenced' + refs)
            binary = root / 'dependency'
            subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
                '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror', '-I' + str(root),
                '-I' + str(NATIVE), str(ROOT / 'tests/staging_dependency.c'),
                '-o', str(binary)], check=True, timeout=15)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('native ordered upload guard PASS', result.stdout)

    def test_normal_integration_and_unchanged_output_sync(self):
        patch = PATCH.read_text()
        self.assertIn('if (maliWaitProfilingActive)', patch)
        self.assertIn('maliStaging.acquire(uint64_t(stride) * height, staging.slot)', patch)
        self.assertIn('staging.submitted(sequence);', patch)
        self.assertIn('stride > UINT64_MAX / height', patch)
        self.assertIn('maliStagingUploadRecording && flush && !image->externalImage() ? VK_ACCESS_SHADER_READ_BIT', patch)
        normal = added_file(PATCH, 'mali_normal.inc')
        stop = normal.split('int vulkan_mali_normal_stop()', 1)[1]
        self.assertLess(stop.index('s.stopping = true'), stop.index('maliStaging.shutdown()'))
        self.assertLess(stop.index('maliStaging.shutdown()'), stop.index('maliWaitProfilingActive = false'))
        self.assertIn('normal staging drain FAILED: retained slots=', stop)
        present = normal.split('int vulkan_mali_normal_present', 1)[1].split('int vulkan_mali_normal_stop', 1)[0]
        for text in ('g_device.wait(*sequence, false)', 'g_device.wait(*sequence, true)', 'g_device.completedSeqNo() >= *sequence',
                     'DD_SYNC_EXPORT', 'DD_SYNC_WAIT', 'ownership.ready(index)', 'MB_NORMAL_PUBLISH', 'ownership.presented(index)'):
            self.assertIn(text, present)
        self.assertLess(present.index('DD_SYNC_WAIT'), present.index('MB_NORMAL_PUBLISH'))
        perf = added_file(PATCH, 'mali_normal_perf.inc')
        self.assertIn('MaliPerf local-wait:', perf)
        self.assertIn('MaliPerf staging:', perf)
        self.assertIn('MaliPerf staging-map:', perf)
        self.assertIn('NormalStagingMapReport("teardown")', stop)
        self.assertIn('void *dst = staging.slot ? staging.slot->resource.mapped : nullptr', patch)
        self.assertIn('uint64_t(stride) * height > staging.slot->resource.mappedBytes', patch)
        self.assertIn('if (!staging.slot) g_device.vk.UnmapMemory', patch)
        self.assertIn('maliStaging.backend.prepare(staging.slot->resource, cmdBuffer->rawBuffer())', patch)
        backend = (ROOT / 'mali_staging.inc').read_text()
        self.assertEqual(backend.count('g_device.vk.MapMemory('), 1)
        self.assertEqual(backend.count('g_device.vk.UnmapMemory('), 1)
        self.assertLess(backend.index('g_device.resetCmdBuffers(completedSequence)'), backend.index('g_device.vk.UnmapMemory'))
        self.assertIn('m->staging_managed = m->staging_command = 0', (ROOT / 'interop_icd.h').read_text())
        self.assertIn('"vkDroidDeckStagingMALI"', (ROOT / 'icd_proxy.c').read_text())
        self.assertNotIn('maliCompletedTimeline.reset', perf)  # report reset must retain completion knowledge
        self.assertIn('maliCompletedTimeline.reset()', normal.split('bool vulkan_mali_normal_init', 1)[1])
        scope = (ROOT / 'mali_wait_profile.hpp').read_text()
        self.assertNotIn('QueueSubmit', scope)
        self.assertIn('reason == DD_WAIT_OUTPUT_RETIRE && maliCompletedTimeline.producerCovers', scope)
        native = (NATIVE / 'renderer_commands.h').read_text()
        update = native.split('case MB_RENDERER_UPDATE:', 1)[1].split('case MB_RENDERER_BIND_PIPELINE:', 1)[0]
        self.assertIn('renderer_descriptor_update_allowed(d, set->id)', update)
        self.assertLess(update.index('renderer_descriptor_update_allowed'), update.index('v->UpdateDescriptorSets'))


if __name__ == '__main__':
    unittest.main()
