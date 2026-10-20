"""Check shell access and the Android/native diagnostic boundary without an APK build."""
from pathlib import Path
import re
import os
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
GPU = ROOT / 'app/src/main/java/com/droiddeck/launcher/gpu'
ANDROID = '{http://schemas.android.com/apk/res/android}'


class DiagnosticBoundaryTests(unittest.TestCase):
    def test_component_smoke_rejects_wrong_status_and_missing_help_option(self):
        options = ('vk-enumerate-only', 'vk-capabilities', 'vk-create-device-test', 'vk-submit-test',
                   'vk-buffer-memory-test', 'vk-image-memory-test', 'vk-ahb-test', 'vk-ahb-present-test',
                   'vk-gamescope-renderer-init-test', 'vk-gamescope-first-frame-test')
        checker = ROOT / 'tools/gamescope/check-diagnostics.sh'
        with tempfile.TemporaryDirectory(prefix='mali-diagnostic-smoke-') as directory:
            binary = Path(directory) / 'gamescope'
            binary.write_text('''#!/usr/bin/env python3
import os, sys
options = ''' + repr(tuple('--' + option for option in options)) + '''
if sys.argv[1:] == ['--help']:
    print('\\n'.join(option for option in options if option[2:] != os.environ.get('HIDE_OPTION')))
    sys.exit(0)
assert os.environ['VK_DRIVER_FILES'] == os.environ['VK_ICD_FILENAMES']
assert not os.path.exists(os.environ['VK_DRIVER_FILES'])
sys.exit(int(os.environ.get('DIAGNOSTIC_STATUS', '1')))
''')
            binary.chmod(0o700)
            for changes, success in (({}, True), ({'DIAGNOSTIC_STATUS': '0'}, False),
                                     ({'HIDE_OPTION': 'vk-submit-test'}, False)):
                with self.subTest(changes=changes):
                    result = subprocess.run(['bash', str(checker), str(binary)],
                                            env=dict(os.environ, **changes), capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

    def test_activity_is_explicit_and_shell_protected(self):
        manifest = ET.parse(ROOT / 'app/src/main/AndroidManifest.xml')
        matches = [activity for activity in manifest.findall('./application/activity')
                   if activity.get(ANDROID + 'name') == '.gpu.SystemVulkanBrokerActivity']
        self.assertEqual(len(matches), 1)
        activity = matches[0]
        self.assertEqual(activity.get(ANDROID + 'permission'), 'android.permission.DUMP')
        self.assertEqual(activity.get(ANDROID + 'exported'), 'true')
        self.assertEqual(activity.get(ANDROID + 'excludeFromRecents'), 'true')
        self.assertEqual(activity.findall('intent-filter'), [])

    def test_kotlin_native_declarations_match_broker_exports(self):
        wrapper = (GPU / 'SystemVulkanBroker.kt').read_text()
        declarations = set(re.findall(r'@JvmStatic private external fun (native\w+)\(', wrapper))
        self.assertEqual(declarations, {'nativeStart', 'nativeStop', 'nativeSetSurface', 'nativeQuarantinedSessions'})
        native = ROOT / 'app/src/main/cpp/malivulkan'
        code = '\n'.join(path.read_text() for path in native.glob('*.c'))
        exports = set(re.findall(r'Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_(native\w+)\(', code))
        self.assertEqual(declarations, exports)

    def test_only_diagnostic_activity_calls_the_wrapper(self):
        java = ROOT / 'app/src/main/java'
        callers = {path.relative_to(java).as_posix() for path in java.rglob('*.kt')
                   if 'SystemVulkanBroker.' in path.read_text()}
        self.assertEqual(callers, {'com/droiddeck/launcher/gpu/SystemVulkanBrokerActivity.kt'})

    def test_preview_diagnostics_cannot_reach_persistent_or_shared_modes(self):
        wrapper = (GPU / 'SystemVulkanBroker.kt').read_text()
        activity = (GPU / 'SystemVulkanBrokerActivity.kt').read_text()
        for forbidden in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_SESSION_TEST',
                          'MALI_VULKAN_SHARED_STAGING', '--mali-session-test',
                          '--mali-wayland-client-test', 'startNormal', 'stopNormal', 'Panvk'):
            self.assertNotIn(forbidden, wrapper + activity)

    def test_diagnostics_use_current_runtime_logging_api(self):
        wrapper = (GPU / 'SystemVulkanBroker.kt').read_text()
        guest = (ROOT / 'app/src/main/java/com/droiddeck/launcher/runtime/GuestCommand.kt').read_text()
        self.assertNotIn('debugLogDir', wrapper)
        calls = [line for line in wrapper.splitlines() if 'GuestCommand.run(' in line]
        self.assertEqual(len(calls), 7)
        self.assertTrue(all('logName = ' in line for line in calls))
        self.assertIn('LinuxRuntime.logDir(context)', guest)
        self.assertIn('"-i"', guest)


if __name__ == '__main__':
    unittest.main()
