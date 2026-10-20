"""Final reference transport and Phase 5 architecture isolation contracts."""
import hashlib
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
FROZEN = {'final_reference': {'files': ['app/src/main/cpp/malivulkan/shared_staging_commands.h',
                               'app/src/main/cpp/malivulkan/system_broker.c',
                               'app/src/main/cpp/malivulkan/interop_commands.h',
                               'app/src/main/cpp/malivulkan/interop_objects.h',
                               'app/src/main/cpp/malivulkan/renderer_objects.h',
                               'app/src/main/cpp/malivulkan/renderer_commands.h',
                               'tools/mali-vulkan/shared_staging.h',
                               'tools/mali-vulkan/shared_staging_types.h',
                               'tools/mali-vulkan/icd_proxy.c',
                               'tools/mali-vulkan/interop_icd.h',
                               'tools/mali-vulkan/resource_registry.h',
                               'tools/mali-vulkan/renderer_icd.h',
                               'tools/mali-vulkan/submit_icd.h',
                               'tools/mali-vulkan/interop_protocol.h',
                               'tools/mali-vulkan/frame_trace.h',
                               'tools/mali-vulkan/frame_trace_proxy.h'],
                     'sha256': '89cf5995ee14526feaff3ffd57f540a8fb74c7612d3cd73161393b5b7f39acf5'},
 'phase5_isolation': {'files': ['app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionSelection.kt',
                                'app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionControls.kt',
                                'app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt',
                                'app/src/main/java/com/droiddeck/launcher/gpu/PanvkClientProbeReceiver.kt',
                                'app/src/main/java/com/droiddeck/launcher/MainActivity.kt',
                                'app/src/main/java/com/droiddeck/launcher/session/SessionService.kt',
                                'app/src/main/java/com/droiddeck/launcher/session/SessionFiles.kt',
                                'app/src/main/cpp/waylandcomp/src/ahb_swapchain.c',
                                'app/src/main/cpp/waylandcomp/src/compositor.c',
                                'app/src/main/cpp/malivulkan/normal_ownership.c',
                                'app/src/main/cpp/malivulkan/normal_commands.h',
                                'tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-client-panvk',
                                'tools/mali-vulkan/steam_client_environment.py',
                                'tools/mali-vulkan/probe_steam_angle.py',
                                'tools/mali-vulkan/steam_cef_evidence.py',
                                '.github/workflows/build-panvk-glibc.yml'],
                      'sha256': 'aa2dd625a1995d6b28079563cc4a616debc77d47b229184736e73d4a85924c06'}}


class Phase6Tests(unittest.TestCase):
    def test_final_reference_transport_and_phase5_client_presentation_are_unchanged(self):
        for name, group in FROZEN.items():
            with self.subTest(group=name):
                digest = hashlib.sha256()
                for path in group['files']:
                    digest.update(path.encode() + b'\0' + (ROOT / path).read_bytes())
                self.assertEqual(digest.hexdigest(), group['sha256'])

    def test_app_specific_opt_in_is_read_only_at_normal_startup(self):
        broker = (ROOT / 'app/src/main/java/com/droiddeck/launcher/gpu/MaliNormalBroker.kt').read_text()
        self.assertIn('runCatching { context.getExternalFilesDir("mali-vulkan") }.getOrNull()', broker)
        self.assertIn('directory?.let { File(it, "shared-staging.enabled").isFile } == true', broker)
        self.assertIn('MALI_VULKAN_SHARED_STAGING=${if (enabled) "1" else "0"}', broker)
        self.assertLess(broker.index('SystemVulkanBroker.startNormal'), broker.index('val sharedStaging ='))
        self.assertLess(broker.index('val sharedStaging ='), broker.index('return MaliSessionSelection.environment'))
        self.assertNotIn('debugLogDir', broker)
        self.assertNotIn('/Download/', broker)
        controls = (ROOT / 'app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionControls.kt').read_text()
        self.assertNotIn('MALI_VULKAN_SHARED_STAGING', controls)


if __name__ == '__main__':
    unittest.main()
