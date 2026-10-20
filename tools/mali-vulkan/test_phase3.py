"""Phase 3 source boundaries plus executable protocol and output ownership checks."""
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'app/src/main/java/com/droiddeck/launcher'
CPP = ROOT / 'app/src/main/cpp'


class SessionBoundaryTests(unittest.TestCase):
    def test_exclusive_normal_lease_blocks_every_diagnostic_and_retains_failed_stop(self):
        source = (JAVA / 'gpu/SystemVulkanBroker.kt').read_text()
        for name in ('start', 'runProbe', 'runIcdTest', 'runGamescopeEnumeration',
                     'runGamescopeDeviceTest', 'runGamescopeSubmitTest',
                     'runCapabilities', 'runInteropTest', 'stop'):
            body = source.split('fun ' + name + '(', 1)[1].split('\n    }', 1)[0]
            self.assertIn('if (normalOwned) throw IOException', body, name)
        start = source.split('fun startNormal(', 1)[1].split('\n    }', 1)[0]
        self.assertLess(start.index('socket != null'), start.index('start(context)'))
        self.assertLess(start.index('nativeQuarantinedSessions()'), start.index('start(context)'))
        stop = source.split('fun stopNormal()', 1)[1].split('\n    }', 1)[0]
        self.assertLess(stop.index('nativeStop()'), stop.index('socket = null'))
        self.assertLess(stop.index('nativeStop()'), stop.index('normalOwned = false'))
        self.assertIn('nativeQuarantinedSessions() == 0', stop)

    def test_stop_drains_guest_then_broker_before_idle_and_clears_restart_state(self):
        source = (JAVA / 'session/SessionService.kt').read_text()
        stop = source.split('private fun stopSession(', 1)[1].split('private fun importLauncherGames', 1)[0]
        normal = stop.split('if (SessionState.maliBackend)', 1)[1].split('val finishAfterTeardown', 1)[0]
        self.assertLess(normal.index('stopMaliGuest('), normal.index('MaliNormalBroker.stop()'))
        self.assertLess(normal.index('MaliNormalBroker.stop()'), normal.index('finishSessionStop('))
        self.assertIn('finally {\n                        synchronized(maliStartupLock)', normal)
        self.assertIn('MALI_RELEASE_TIMEOUT', normal)
        self.assertIn('maliGuestStartedAt = null', stop)
        finish = source.split('private fun finishSessionStop(', 1)[1].split('private fun raiseTracer', 1)[0]
        self.assertIn('SessionState.maliBackend = false', finish)
        self.assertIn('stoppedGen != sessionGen || SessionState.running', finish)
        drain = source.split('private fun stopMaliGuest(', 1)[1].split('private fun openSessionFolder', 1)[0]
        self.assertIn('readStat(prootPid)?.second != startedAt', drain)
        self.assertIn('readStat(pid)?.second == started', drain)
        self.assertLess(drain.index('sendSignal(pid, 15)'), drain.index('waitForExit(prootPid, 7000)'))
        self.assertLess(drain.index('waitForExit(prootPid, 7000)'), drain.index('teardown(prootPid, startedAt)'))

    def test_route_does_not_replace_steam_games_or_non_mali_sessions(self):
        source = (JAVA / 'MainActivity.kt').read_text()
        route = source.split('private fun startSession(intent:', 1)[1].split('private fun refreshPhantomStatus', 1)[0]
        self.assertIn('!steamSession && intent.getStringExtra(SessionService.EXTRA_MODE) == SessionService.MODE_DESKTOP', route)
        self.assertIn('!intent.hasExtra(SessionService.EXTRA_STEAM_UI)', route)
        self.assertIn('MaliSessionSelection.supported(gpu)', route)
        self.assertLess(route.index('StorageState.cleaning'), route.index('MaliSessionSelection.supported(gpu)'))
        service = (JAVA / 'session/SessionService.kt').read_text()
        self.assertIn('SessionState.maliBackend = SessionState.mode == com.droiddeck.launcher.gpu.MaliSessionSelection.MODE', service)
        normal = service.split('private fun runMaliSession', 1)[1].split('private fun stopMaliGuest', 1)[0]
        self.assertIn('LinuxRuntime.prootTempDir(this).path', normal)
        self.assertIn('SessionFiles.stage(this, root)', normal)
        self.assertIn('openSessionFolder()', normal)
        self.assertIn('SessionState.runtimeDriver = null', normal)
        self.assertIn('MaliSessionControls.environment(extraEnv())', normal)
        self.assertNotIn('debugLogDir', normal)

    def test_shared_staging_and_panvk_are_not_normal_session_options(self):
        broker = (JAVA / 'gpu/MaliNormalBroker.kt').read_text()
        self.assertIn('"MALI_VULKAN_SHARED_STAGING=0"', broker)
        self.assertNotIn('shared-staging.enabled', broker)
        self.assertNotIn('Panvk', broker)
        shell = (ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-session').read_text()
        self.assertIn('if [ "$mode" = mali-wayland ] ||', shell)
        self.assertIn('[ "${MALI_VULKAN_SHARED_STAGING:-0}" = 0 ]', shell)
        outer = shell.split('case $mode in', 1)[0]
        self.assertNotIn('-cef-use-vulkan', outer)
        self.assertNotIn('droiddeck-steam-client-panvk', outer)

    def test_ahb_only_buffer_has_no_dmabuf_planes_or_export(self):
        source = (CPP / 'waylandcomp/src/compositor.c').read_text()
        buffer = source.split('struct wl_resource *droiddeck_create_ahb_buffer(', 1)[1].split('static void surface_size', 1)[0]
        self.assertIn('calloc(1, sizeof(*buffer))', buffer)
        self.assertIn('buffer->fd[i] = -1', buffer)
        self.assertNotIn('n_planes = 1', buffer)
        self.assertIn('b->n_planes == 0 && ahb_swapchain_has_ahb(b)', source)
        self.assertIn('wl_single_pixel', (CPP / 'waylandcomp/CMakeLists.txt').read_text())
        self.assertIn('vkp_quest_layer_presentation()', source)
        registry = (CPP / 'waylandcomp/src/ahb_swapchain.c').read_text()
        self.assertIn('mb_normal_import(key, width, height)', registry)
        self.assertIn('mb_normal_submit(ab->broker_key)', registry)
        self.assertIn('mb_normal_not_submitted(ab->broker_key)', registry)
        self.assertIn('POLLERR | POLLNVAL', registry)
        self.assertIn('!state.owned', registry)


class PatchContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get('GAMESCOPE_SOURCE')
        if not source:
            raise unittest.SkipTest('Supply pristine Gamescope 3.16.29 through --gamescope-source')
        cls.temporary = tempfile.TemporaryDirectory(prefix='mali-phase3-patches-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.source = Path(cls.temporary.name) / 'source'
        shutil.copytree(source, cls.source, ignore=shutil.ignore_patterns('.git', 'build', 'builddir'))
        cls.patches = sorted((ROOT / 'tools/gamescope/patches').glob('*.patch'))
        cls.output = ''
        for patch in cls.patches:
            if patch.name.startswith('02'):
                subprocess.run(['git', 'apply', '--check', '--whitespace=error', str(patch)],
                               cwd=cls.source, check=True, capture_output=True)
            result = subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '--no-backup-if-mismatch', '-i', str(patch)],
                                    cwd=cls.source, check=True, text=True, capture_output=True)
            if patch.name.startswith('02'):
                cls.output += result.stdout

    def test_explicit_patch_order_preserves_diagnostics_and_has_no_fuzz_or_offset(self):
        prefixes = [p.name[:4] for p in self.patches]
        self.assertEqual(len(prefixes), len(set(prefixes)))
        self.assertEqual([p for p in prefixes if p.startswith('02')],
                         ['0200', '0201', '0202', '0203', '0204', '0205', '0206', '0210', '0211',
                          '0212', '0213', '0214', '0215', '0216', '0217', '0218'])
        self.assertNotIn('offset', self.output)
        self.assertNotIn('fuzz', self.output)

    def test_normal_protocol_and_headers_match_existing_broker_contract(self):
        for name in ('normal_api.h', 'normal_protocol.h', 'normal_perf.h', 'normal_wait_perf.h', 'interop_test_api.h'):
            self.assertEqual((self.source / 'src' / name).read_bytes(), (ROOT / 'tools/mali-vulkan' / name).read_bytes(), name)
        xml = (CPP / 'waylandcomp/protocols/banner-ahb-v1.xml').read_bytes()
        self.assertEqual((self.source / 'protocol/banner-ahb-v1.xml').read_bytes(), xml)
        interface = ET.fromstring(xml).find('interface')
        self.assertEqual(interface.get('version'), '3')
        request = interface.find("request[@name='create_broker_buffer']")
        self.assertEqual(request.get('since'), '3')
        self.assertEqual([(arg.get('name'), arg.get('type')) for arg in request.findall('arg')],
                         [('buffer', 'new_id'), ('key', 'uint'), ('width', 'uint'), ('height', 'uint')])

    def test_actual_output_pool_never_reuses_android_owned_slot(self):
        binary = Path(self.temporary.name) / 'output-pool'
        subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) +
                       ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(self.source / 'src'),
                        str(ROOT / 'tools/mali-vulkan/tests/session_pool.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=3)

    def test_real_sync_precedes_publish_and_real_drain_precedes_destruction(self):
        source = (self.source / 'src/mali_normal.inc').read_text()
        self.assertLess(source.index('NormalInterop(DD_SYNC_WAIT'), source.index('NormalRPC(MB_NORMAL_PUBLISH'))
        stop = source.split('int vulkan_mali_normal_stop()', 1)[1]
        self.assertLess(stop.index('NormalRPC(MB_NORMAL_END)'), stop.index('mali_normal_destroy_buffers()'))
        self.assertLess(stop.index('FailMaliDiagnostic("normal Android release drain"'), stop.index('DestroyImage('))
        self.assertLess(stop.index('maliStaging.shutdown()'), stop.index('mali_normal_destroy_buffers()'))
        self.assertIn('mali_normal_perf.inc', source)
        self.assertIn('MB_NORMAL_END', source)
        normal = (self.source / 'src/mali_normal.cpp').read_text()
        self.assertIn('unsetenv("MALI_VULKAN_BROKER_SOCKET")', normal)
        self.assertIn('mali-xwayland', normal)
        touch = (self.source / 'src/Backends/WaylandBackend.cpp').read_text()
        self.assertIn('m_TouchSurfaces[ nId ] = pSurface', touch)
        self.assertIn('wlserver_touchdown( flX, flY, nId + 1', touch)
