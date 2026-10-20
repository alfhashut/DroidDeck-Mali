"""Final optimized path contracts without Android/Gamescope project compilation."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from gamescope_source import PATCHES, source
import frame_journal_control as journal
import steam_client_environment as client

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'app/src/main/java/com/droiddeck/launcher'

FROZEN_PATCHES = {
    '0002-steamcompmgr-fallback-appid-focus.patch': '611989049383b9d3a5d30f908a5dbe164c7ee5a4d451bebb9c108d6ba9a2d02b',
    '0009-fix-arm64-steam-night-mode.patch': '201ca8388f5376f03e5d001b3dbb8e5c404b38b81d443f456ae4fac17365866e',
    '0019-steamcompmgr-arm64-virtual-white.patch': 'eeb5a5ea9b2a876ebbf04168f95b6520973ed05c2cf587bcf762cfc5460ecfe9',
    '0020-color-p3-red-is-wide-gamut.patch': '4a2532480acabce47dde01674fd0258b47141d2ab1233e0f8d0498ec0537da98',
    '0100-realtime-queue-and-gamepad-cursor.patch': 'd7ad2ab66d7d0ecc73f20f412194fac74b764610fb651d23abb8fcec4e06ba04',
    '0101-nested-wayland-cursor-routing.patch': '4502958ed9a481e4c028fbee35340bc02637e484e6e5dc4e006e9c208e985440',
    '0110-wayland-backend-touch.patch': '2fab269c671829e75f8a45b3ebd2d8432969a13de8ac4421d7067444c51af312',
    '0111-wayland-pointer-warps-in-passthrough.patch': '8aeffc39fe778b73d9117badfb318cbdb3b7931bcf2626c6e6c8ace9b85776ac',
    '0112-restore-iconified-game-on-resume.patch': '475398b3435d2619932eaff9eacb4108a2efc85441cc48fde43d13da4e869d4d',
    '0114-take-override-redirect-from-mapnotify.patch': '56cf10c38f48569872263e63fc46964133e238c9421b3eba7a55fa78c24cc8a7',
    '0115-limiter-paces-per-window.patch': '6ee6df85d9d8093ee8b86fc146151dbb19c3c73f475849648751f553be4629ad',
    '0200-vulkan-enumerate-only.patch': '37abe4a0cee96c8bf91ace08d60976b0a39dbd31893da80658410168378a0155',
    '0201-vulkan-capabilities.patch': '140211bf530b731d8c09b7ba6146fa5d9b34476388987ad82ab392cd44d2557d',
    '0202-vulkan-create-device-test.patch': '1ced362d3190458040b7625f2556ecd876d25bb0118ff91be063d35426cc15cc',
    '0203-vulkan-submit-test.patch': 'ace555a23b94f03c1a828a60b0a4b696677cb29abfde04f375ad009c26e937b0',
    '0204-vulkan-memory-ahb-tests.patch': '4c94acf5ea755bd5e9af2def8b47b7bd91cd55a2583d4c182b4c2cba8bacfaa2',
    '0205-vulkan-gamescope-renderer-tests.patch': '14985a3a6ae3f1d7f7a2790700d8d24a9f5bc29a200cd0bbffb8a979eef99aa9',
    '0206-vulkan-blit-input-proof.patch': '216d080643d2e99a8d49049c66732cf4be142f52e4dfea0f3cbe552940d932f4',
    '0210-mali-persistent-session.patch': '230a77a66195bfa461a5d7eef8fd74954aa062115126c2bc9d418902eeeec722',
    '0211-mali-normal-wayland-session.patch': 'e5052f91d8ddedd57e31278e1f0abf692159fda85de3f654aaef9b376b5d3f09',
}


class FinalPathTests(unittest.TestCase):
    def test_previous_twenty_patches_remain_byte_identical(self):
        for name, expected in FROZEN_PATCHES.items():
            self.assertEqual(hashlib.sha256((PATCHES / name).read_bytes()).hexdigest(), expected, name)

    def test_final_contracts_and_bulk_limits_are_coordinated(self):
        for name in ('interop_protocol.h', 'renderer_protocol.h', 'normal_api.h', 'normal_perf.h',
                     'normal_wait_perf.h', 'normal_opcode_perf.h', 'mali_staging_pool.hpp',
                     'mali_wait_profile.hpp', 'staging_api.h', 'upload_ring.h', 'upload_ring_api.h',
                     'normal_detail_perf.hpp', 'frame_trace.h'):
            self.assertEqual((source() / 'src' / name).read_bytes(), (ROOT / 'tools/mali-vulkan' / name).read_bytes(), name)
        self.assertEqual((source() / 'src/frame_trace.inc').read_bytes(), (ROOT / 'tools/mali-vulkan/frame_trace.c').read_bytes())
        protocol = (source() / 'src/interop_protocol.h').read_text()
        self.assertIn('#define MB_INTEROP_STAGING_WRITE_MAX (2u * 1024u * 1024u)', protocol)
        self.assertIn('MB_RENDERER_MAX_SHADER', (source() / 'src/renderer_protocol.h').read_text())
        normal = (source() / 'src/mali_normal.inc').read_text()
        self.assertLess(normal.index('maliStaging.backend.resolve()'), normal.index('MB_NORMAL_BEGIN'))
        self.assertIn('maliCompletedTimeline.reset()', normal)
        self.assertLess(normal.index('NormalInterop(DD_SYNC_WAIT'), normal.index('NormalRPC(MB_NORMAL_PUBLISH'))
        stop = normal.split('int vulkan_mali_normal_stop()', 1)[1]
        self.assertLess(stop.index('maliStaging.shutdown()'), stop.index('mali_normal_destroy_buffers()'))

    def test_steam_keeps_services_and_stop_waits_for_the_broker(self):
        service = (JAVA / 'session/SessionService.kt').read_text()
        self.assertIn('synchronized(maliStartupLock) { runStandardSession(gen) }', service)
        self.assertIn('if (maliSteam) synchronized(stopLock) { finishGuestStart() }', service)
        self.assertIn('if (!maliSteam) sessionPid = pid', service)
        self.assertIn('if (maliSteam) "arm64" else if (steamHere) SessionPrefs.steamClientArch(this)', service)
        for invariant in ('LinuxRuntimeInstaller.isMaintaining()', 'networkLink.publish()',
                          'SessionArtifacts.awaitCollections()', 'sessionBinds(',
                          'AutoDriverTrial.sessionStarting(', 'FexCatalog.environment(it)',
                          'SessionTerminal.prepare(', 'OfflineMode.apply(this, root)'):
            self.assertIn(invariant, service)
        stop = service.split('val finishMali:', 1)[1].split('val finishAfterTeardown:', 1)[0]
        self.assertLess(stop.index('askSteamToExit(prootPid)'), stop.index('stopMaliGuest('))
        self.assertLess(stop.index('stopMaliGuest('), stop.index('MaliNormalBroker.stop()'))
        self.assertIn('MALI_RELEASE_TIMEOUT', stop)
        self.assertIn('SessionActivity.EXTRA_STEAMVR', (JAVA / 'MainActivity.kt').read_text())

    def test_controls_use_app_specific_paths_and_never_activate_shared_or_panvk(self):
        broker = (JAVA / 'gpu/MaliNormalBroker.kt').read_text()
        self.assertIn('File(LinuxRuntime.logDir(context), "mali-frame-trace")', broker)
        self.assertIn('MALI_VULKAN_SHARED_STAGING=0', broker)
        self.assertNotIn('debugLogDir', broker)
        controls = (JAVA / 'gpu/MaliSessionControls.kt').read_text()
        for forbidden in ('VK_DRIVER_FILES', 'MALI_VULKAN_SHARED_STAGING', 'PANVK_ATOM_STRIDE', 'FRAME_TRACE_DIR'):
            self.assertNotIn(forbidden, controls)
        native = (ROOT / 'tools/mali-vulkan/frame_trace.c').read_text()
        self.assertNotIn('/Download/', native)
        self.assertIn('#define DD_FT_ROOT NULL', native)
        self.assertIn('DROIDDECK_FRAME_TRACE_DIR', native)
        script = (ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-session').read_text()
        self.assertNotIn('-cef-use-vulkan', script)
        self.assertNotIn('droiddeck-steam-client-panvk', script)
        self.assertIn('-cef-use-angle=gl', script)
        self.assertIn('"${BL_MALI_STEAM_UI:-0}" = 1 ] || reap_orphans', script)

    def test_stage_only_observers_and_analyzers(self):
        spec = importlib.util.spec_from_file_location('stage_mali_diagnostics', ROOT / 'tools/mali-vulkan/stage-diagnostics.py')
        stage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory(prefix='mali-observers-') as directory:
            target = stage.stage(Path(directory))
            self.assertEqual({p.name for p in target.iterdir()}, set(stage.MALI + stage.ANALYZERS))
            self.assertFalse(list(target.glob('*.so')))
            for path in target.iterdir():
                self.assertNotIn('run_panvk_phone_test_impl', path.read_text())
            for script in ('diagnose_steam_perf.py', 'capture_frame_journal.py', 'analyze_frame_journal.py', 'analyze_steam_frames.py'):
                result = subprocess.run(['python3', '-B', str(target / script), '--help'], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_journal_control_targets_only_the_selected_app(self):
        import xml.etree.ElementTree as ET
        android = '{http://schemas.android.com/apk/res/android}'
        manifest = ET.parse(ROOT / 'app/src/main/AndroidManifest.xml')
        receiver = next(r for r in manifest.findall('./application/receiver') if r.get(android + 'name') == '.gpu.MaliFrameJournalReceiver')
        self.assertEqual(receiver.get(android + 'permission'), 'android.permission.DUMP')
        self.assertEqual(receiver.findall('intent-filter'), [])
        for action in ('arm', 'disarm', 'status', 'pull'):
            words = shlex.split(journal.command(action, 'com.droiddeck.launcher.test'))
            self.assertEqual(words[:3], ['am', 'broadcast', '-n'])
            self.assertEqual(words[3], 'com.droiddeck.launcher.test/com.droiddeck.launcher.gpu.MaliFrameJournalReceiver')
            self.assertEqual(words[-1], 'export' if action == 'pull' else action)
        with self.assertRaises(ValueError):
            journal.command('arm', 'com.example; touch /tmp/unsafe')
        code = (JAVA / 'gpu/MaliFrameJournalReceiver.kt').read_text()
        self.assertIn('File(LinuxRuntime.logDir(app), "mali-frame-trace")', code)
        self.assertIn('!SessionState.running', code)
        self.assertIn('SessionPhase.IDLE, SessionPhase.FAILED', code)
        self.assertNotIn('SystemVulkanBroker', code)
        self.assertNotIn('MaliNormalBroker', code)
        self.assertNotIn('/Download/', code)

    def test_client_environment_is_resolved_without_guessing_or_driver_setup(self):
        with tempfile.TemporaryDirectory(prefix='mali-client-proc-') as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                client.client_environment(root)
            pid = root / '123'
            pid.mkdir()
            (pid / 'cmdline').write_bytes(b'/bin/bash\0/usr/local/bin/droiddeck-session\0steam\0')
            env = {'BL_MALI_STEAM_UI': '1', 'BL_INSIDE': '1', 'DISPLAY': ':42',
                   'DROIDDECK_FRAME_TRACE_DIR': '/app/files/logs/mali-frame-trace'}
            (pid / 'environ').write_bytes('\0'.join(f'{key}={value}' for key, value in env.items()).encode())
            self.assertEqual(client.client_environment(root), env)
            env['MALI_VULKAN_BROKER_SOCKET'] = '/app/broker.sock'
            (pid / 'environ').write_bytes('\0'.join(f'{key}={value}' for key, value in env.items()).encode())
            with self.assertRaises(RuntimeError):
                client.client_environment(root)
