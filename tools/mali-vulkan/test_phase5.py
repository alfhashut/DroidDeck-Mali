"""Phase 5 isolation, packaging and unchanged bridge/performance contracts."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

import panvk_raw_artifact as raw

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'app/src/main/java/com/droiddeck/launcher'
FROZEN = {'patches': {'files': ['tools/gamescope/patches/0002-steamcompmgr-fallback-appid-focus.patch',
                       'tools/gamescope/patches/0009-fix-arm64-steam-night-mode.patch',
                       'tools/gamescope/patches/0019-steamcompmgr-arm64-virtual-white.patch',
                       'tools/gamescope/patches/0020-color-p3-red-is-wide-gamut.patch',
                       'tools/gamescope/patches/0100-realtime-queue-and-gamepad-cursor.patch',
                       'tools/gamescope/patches/0101-nested-wayland-cursor-routing.patch',
                       'tools/gamescope/patches/0110-wayland-backend-touch.patch',
                       'tools/gamescope/patches/0111-wayland-pointer-warps-in-passthrough.patch',
                       'tools/gamescope/patches/0112-restore-iconified-game-on-resume.patch',
                       'tools/gamescope/patches/0114-take-override-redirect-from-mapnotify.patch',
                       'tools/gamescope/patches/0115-limiter-paces-per-window.patch',
                       'tools/gamescope/patches/0200-vulkan-enumerate-only.patch',
                       'tools/gamescope/patches/0201-vulkan-capabilities.patch',
                       'tools/gamescope/patches/0202-vulkan-create-device-test.patch',
                       'tools/gamescope/patches/0203-vulkan-submit-test.patch',
                       'tools/gamescope/patches/0204-vulkan-memory-ahb-tests.patch',
                       'tools/gamescope/patches/0205-vulkan-gamescope-renderer-tests.patch',
                       'tools/gamescope/patches/0206-vulkan-blit-input-proof.patch',
                       'tools/gamescope/patches/0210-mali-persistent-session.patch',
                       'tools/gamescope/patches/0211-mali-normal-wayland-session.patch',
                       'tools/gamescope/patches/0212-mali-optimized-normal-resources.patch',
                       'tools/gamescope/patches/0213-mali-frame-cost-attribution.patch',
                       'tools/gamescope/patches/0214-mali-upload-ring-ranges.patch',
                       'tools/gamescope/patches/0215-mali-steam-xwayland-client.patch',
                       'tools/gamescope/patches/0216-mali-steam-x11-presentation-diagnostics.patch',
                       'tools/gamescope/patches/0217-mali-frame-trace.patch',
                       'tools/gamescope/patches/0218-mali-staging-bulk-writes.patch'],
             'sha256': '873b259676b88fb5d4acc3611e4776cd1bd72158036a5c3b5f21bf6cb5d49b1b'},
 'bridge': {'files': ['app/src/main/cpp/malivulkan/capability_queries.h',
                      'app/src/main/cpp/malivulkan/device_commands.h',
                      'app/src/main/cpp/malivulkan/interop_commands.h',
                      'app/src/main/cpp/malivulkan/interop_consumer.c',
                      'app/src/main/cpp/malivulkan/interop_consumer.h',
                      'app/src/main/cpp/malivulkan/interop_objects.h',
                      'app/src/main/cpp/malivulkan/normal_commands.h',
                      'app/src/main/cpp/malivulkan/normal_ownership.c',
                      'app/src/main/cpp/malivulkan/normal_ownership.h',
                      'app/src/main/cpp/malivulkan/normal_wait_native.h',
                      'app/src/main/cpp/malivulkan/renderer_commands.h',
                      'app/src/main/cpp/malivulkan/renderer_objects.h',
                      'app/src/main/cpp/malivulkan/renderer_upload_dependency.h',
                      'app/src/main/cpp/malivulkan/session_commands.h',
                      'app/src/main/cpp/malivulkan/shared_staging_commands.h',
                      'app/src/main/cpp/malivulkan/submit_commands.h',
                      'app/src/main/cpp/malivulkan/submit_objects.h',
                      'app/src/main/cpp/malivulkan/system_broker.c',
                      'tools/mali-vulkan/broker_probe.c',
                      'tools/mali-vulkan/capabilities.h',
                      'tools/mali-vulkan/capability_icd.h',
                      'tools/mali-vulkan/capability_inventory.c',
                      'tools/mali-vulkan/capability_transport.h',
                      'tools/mali-vulkan/device_icd.h',
                      'tools/mali-vulkan/device_protocol.h',
                      'tools/mali-vulkan/frame_trace.c',
                      'tools/mali-vulkan/frame_trace.h',
                      'tools/mali-vulkan/frame_trace_proxy.h',
                      'tools/mali-vulkan/icd_proxy.c',
                      'tools/mali-vulkan/interop_icd.h',
                      'tools/mali-vulkan/interop_protocol.h',
                      'tools/mali-vulkan/interop_test_api.h',
                      'tools/mali-vulkan/loader_test.c',
                      'tools/mali-vulkan/mali_staging.inc',
                      'tools/mali-vulkan/mali_staging_pool.hpp',
                      'tools/mali-vulkan/mali_wait_profile.hpp',
                      'tools/mali-vulkan/normal_api.h',
                      'tools/mali-vulkan/normal_detail_perf.hpp',
                      'tools/mali-vulkan/normal_memory_perf.h',
                      'tools/mali-vulkan/normal_opcode_perf.h',
                      'tools/mali-vulkan/normal_perf.h',
                      'tools/mali-vulkan/normal_protocol.h',
                      'tools/mali-vulkan/normal_wait_perf.h',
                      'tools/mali-vulkan/properties.h',
                      'tools/mali-vulkan/protocol.h',
                      'tools/mali-vulkan/renderer_icd.h',
                      'tools/mali-vulkan/renderer_icd_fwd.h',
                      'tools/mali-vulkan/renderer_protocol.h',
                      'tools/mali-vulkan/resource_registry.h',
                      'tools/mali-vulkan/shared_staging.h',
                      'tools/mali-vulkan/shared_staging_types.h',
                      'tools/mali-vulkan/staging_api.h',
                      'tools/mali-vulkan/submit_icd.h',
                      'tools/mali-vulkan/submit_protocol.h',
                      'tools/mali-vulkan/upload_ring.h',
                      'tools/mali-vulkan/upload_ring_api.h',
                      'app/src/main/cpp/waylandcomp/src/ahb_swapchain.c',
                      'app/src/main/cpp/waylandcomp/src/sc_layer.c',
                      'app/src/main/java/com/droiddeck/launcher/gpu/SystemVulkanBroker.kt',
                      'app/src/main/java/com/droiddeck/launcher/gpu/MaliSessionSelection.kt',
                      'app/src/main/java/com/droiddeck/launcher/session/SessionService.kt'],
            'sha256': '580479121e80518ebc1a385139ac32adbdd19d4053644de38570379f54e5a8b7'}}


class Phase5Tests(unittest.TestCase):
    def test_phase4_bridge_sync_staging_and_all_27_patches_remain_byte_identical(self):
        for group in FROZEN.values():
            digest = hashlib.sha256()
            for name in group['files']:
                digest.update(name.encode() + b'\0' + (ROOT / name).read_bytes())
            self.assertEqual(digest.hexdigest(), group['sha256'])

    def test_receiver_uses_current_runtime_logs_lifecycle_and_shell_permission(self):
        android = '{http://schemas.android.com/apk/res/android}'
        manifest = ET.parse(ROOT / 'app/src/main/AndroidManifest.xml')
        receiver = next(row for row in manifest.findall('./application/receiver')
                        if row.get(android + 'name') == '.gpu.PanvkClientProbeReceiver')
        self.assertEqual(receiver.get(android + 'permission'), 'android.permission.DUMP')
        self.assertEqual(receiver.get(android + 'exported'), 'true')
        self.assertEqual(receiver.findall('intent-filter'), [])
        code = (JAVA / 'gpu/PanvkClientProbeReceiver.kt').read_text()
        self.assertIn('LinuxRuntime.logDir(context)', code)
        self.assertNotIn('debugLogDir', code)
        self.assertIn('MaliSessionSelection.supported(VulkanInfo.cachedOrNull())', code)
        self.assertIn('SessionState.sessionId == session', code)
        self.assertIn('SessionService.registerTerminalProcess', code)
        self.assertIn('SessionService.stopTerminalProcess', code)
        terminal = (JAVA / 'session/SessionTerminal.kt').read_text()
        self.assertIn('SessionState.running && SessionState.maliBackend && SessionState.mode == SessionService.MODE_STEAM', terminal)
        self.assertIn('current.hostEnvironment.forEach', terminal)
        self.assertNotIn('LinuxRuntime.command', terminal)

    def test_packaged_helpers_include_no_panvk_driver_and_source_artifacts_stay_manual(self):
        spec = importlib.util.spec_from_file_location('stage_phase5', ROOT / 'tools/mali-vulkan/stage-diagnostics.py')
        stage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory() as directory:
            target = stage.stage(Path(directory))
            self.assertTrue((target / 'steam_cef_evidence.py').is_file())
            self.assertFalse(list(target.glob('*.so*')))
            for name in ('steam_cef_evidence.py', 'probe_steam_angle.py'):
                result = subprocess.run(['python3', '-B', str(target / name), '--help'], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
        import yaml
        workflow = yaml.safe_load((ROOT / '.github/workflows/build-panvk-glibc.yml').read_text())
        self.assertEqual(set(workflow['on']), {'workflow_dispatch'})
        self.assertEqual(workflow['on']['workflow_dispatch']['inputs']['artifact_mode']['default'], 'raw-test')
        self.assertIn('run_phase5_tests.py --artifact-only', (ROOT / '.github/workflows/build-panvk-glibc.yml').read_text())
        self.assertNotIn('app/src/main/assets', (ROOT / '.github/workflows/build-panvk-glibc.yml').read_text())

    def test_client_selection_does_not_replace_native_shutdown_or_outer_environment(self):
        script = (ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-session').read_text()
        outer = script.split('case $mode in', 1)[0]
        self.assertNotIn('PANVK_ATOM_STRIDE', outer)
        self.assertNotIn('droiddeck-steam-client-panvk', outer)
        self.assertIn('MALI_VULKAN_SHARED_STAGING:-0', outer)
        self.assertIn('_bl_panvk_client=(python3', script)
        self.assertNotIn('steam_client=(python3', script)
        broker = (JAVA / 'gpu/MaliNormalBroker.kt').read_text()
        self.assertIn('sharedStagingEnvironment', broker)
        self.assertNotIn('PANVK_ATOM_STRIDE', broker)
        control = (JAVA / 'gpu/MaliSessionControls.kt').read_text()
        self.assertNotIn('PANVK_ATOM_STRIDE', control)

    def test_adb_cef_proof_requires_its_own_marker_and_success_exit(self):
        import sys
        before = sys.path[:]
        try:
            sys.path.insert(0, str(ROOT / 'tools/panvk'))
            import stage_phone_test as phone
        finally:
            sys.path[:] = before
        for marker, code, expected in [('DROIDDECK_CEF_PANVK_PASS', 0, 0),
                                       ('DROIDDECK_CEF_DIAGNOSTICS_DONE', 0, 1),
                                       ('C8B.1 PanVK CLIENT-ONLY PROBE PASS', 0, 1),
                                       ('DROIDDECK_CEF_PANVK_PASS', 1, 1)]:
            output = marker + f'\nDROIDDECK_PANVK_PROBE_DONE exit={code}\n'
            with patch.object(phone.subprocess, 'run', side_effect=[
                    subprocess.CompletedProcess([], 0, 'Broadcast completed: result=1', ''),
                    subprocess.CompletedProcess([], 0, output, '')]), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(phone.run_phone(['adb'], '/storage/emulated/0', cef_proof=True), expected)


if __name__ == '__main__':
    unittest.main()
