"""Tiny real registry/lifecycle helpers with RPC stubs; no ICD/project build."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


def between(source, first, following):
    return first + source.split(first, 1)[1].split(following, 1)[0]


class RegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mali-registry-')
        cls.addClassCleanup(cls.temp.cleanup)
        build = Path(cls.temp.name)
        submit = (ROOT / 'submit_icd.h').read_text()
        interop = (ROOT / 'interop_icd.h').read_text()
        renderer = (ROOT / 'renderer_icd.h').read_text()
        code = submit.split('static VkResult submit_rpc', 1)[0]
        code += between(submit, 'static VkResult submit_new', 'static void submit_void_error')
        code += between(submit, 'static VKAPI_ATTR void VKAPI_CALL proxy_DestroyCommandPool', 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_AllocateCommandBuffers')
        code += between(submit, 'static VKAPI_ATTR void VKAPI_CALL proxy_FreeCommandBuffers', 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_BeginCommandBuffer')
        code += between(submit, '#define SIMPLE_OBJECT', 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_WaitForFences')
        code += between(interop, 'static void interop_publish', 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_CreateBuffer')
        code += between(interop, '#define INTEROP_DESTROY', 'static void interop_requirements')
        code += between(interop, 'static VKAPI_ATTR void VKAPI_CALL proxy_FreeMemory', 'static VkResult interop_bind')
        code += between(interop, 'static struct proxy_resource *interop_command', 'static VKAPI_ATTR void VKAPI_CALL proxy_CmdFillBuffer')
        code += between(interop, 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckInteropTEST', 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_DroidDeckSessionTEST')
        code += between(renderer, 'static VkResult renderer_new', '#define R_DESTROY')
        code += between(renderer, '#define R_DESTROY', 'R_DESTROY(Semaphore')
        code += 'R_DESTROY(ImageView, PROXY_VIEW, MB_R_VIEW)\nR_DESTROY(DescriptorPool, PROXY_DESCRIPTOR_POOL, MB_R_POOL)\n#undef R_DESTROY\n'
        code += between(renderer, 'static VKAPI_ATTR VkResult VKAPI_CALL proxy_AllocateDescriptorSets', 'static VKAPI_ATTR void VKAPI_CALL proxy_UpdateDescriptorSets')
        (build / 'lifecycle.inc').write_text(code)
        cls.binary = build / 'registry'
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) + [
            '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror',
            '-I' + str(build), '-I' + str(ROOT), str(ROOT / 'tests/resource_registry.c'),
            '-pthread', '-o', str(cls.binary)], check=True, timeout=15)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ' PASS', result.stdout)

    def test_10000_frames_lookup_bound_stale_handles_and_id_reuse(self):
        self.run_case('stress')

    def test_collision_chain_removal_and_retained_reference_storage(self):
        self.run_case('collisions')

    def test_native_ack_pool_children_and_failed_destroy_keep_live(self):
        self.run_case('lifecycle')

    def test_creation_oom_rpc_failure_and_partial_descriptor_batch(self):
        self.run_case('errors')

    def test_sync_and_ahb_token_retirement_and_partial_creation(self):
        self.run_case('tokens')

    def test_all_production_hot_walks_use_active_registry(self):
        for name in ('device_icd.h', 'submit_icd.h', 'interop_icd.h', 'renderer_icd.h'):
            source = (ROOT / name).read_text()
            self.assertNotIn('d->resources', source)
            self.assertNotIn('->live = 0', source)
        renderer = (ROOT / 'renderer_icd.h').read_text()
        submit = between(renderer, 'static VkResult renderer_QueueSubmit', '/* Local capability probe')
        self.assertEqual(submit.count('d->registry.active;'), 2)
        self.assertIn('interop_copy_upload_locked', submit)
        self.assertNotIn('registry.owned', submit)
        # Registry helpers contain no Vulkan/RPC operations or record recycling.
        registry = (ROOT / 'resource_registry.h').read_text()
        retire = between(registry, 'static inline void proxy_registry_retire(', 'static inline void proxy_registry_retire_children')
        self.assertNotIn('free(', retire)
        self.assertNotIn('rpc(', registry)


if __name__ == '__main__':
    unittest.main()
