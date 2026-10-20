"""Cheap C8A command/environment contracts. No project compilation or Steam download."""
from pathlib import Path
import json
import os
import socket
import signal
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-session'
PATCH = ROOT / 'tools/gamescope/patches/0215-mali-steam-xwayland-client.patch'


def added_source(patch, name):
    section = patch.read_text().split('+++ b/' + name + '\n', 1)[1].split('\n--- ', 1)[0]
    return '\n'.join(line[1:] for line in section.splitlines() if line.startswith('+')) + '\n'


class SteamLaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='c8a-launch-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.script = self.root / 'session'
        # Substitute only filesystem locations, leaving production branching intact.
        text = SCRIPT.read_text().replace('/usr/lib/dri/swrast_dri.so', str(self.root / 'swrast.so'))
        text = text.replace('/run/dbus', str(self.root / 'bus'))
        text = text.replace('/usr/local/share/droiddeck', str(self.root / 'share'))
        text = text.replace('/usr/local/bin/droiddeck-steam-client-panvk', str(self.bin / 'droiddeck-steam-client-panvk'))
        self.script.write_text(text)
        self.script.chmod(0o755)
        self.env = dict(os.environ, HOME=str(self.root / 'home'), PATH=str(self.bin) + ':' + os.environ['PATH'],
            BL_LOG='', BL_INSIDE='', BL_WIDTH='1280', BL_HEIGHT='720', BL_DEBUG_DIR='',
            BL_MALI_STEAM_UI='1', XDG_RUNTIME_DIR=str(self.root), WAYLAND_DISPLAY='wayland-0',
            MALI_VULKAN_NORMAL_SESSION='1', MALI_VULKAN_BROKER_SOCKET=str(self.root / 'broker'),
            VK_ICD_FILENAMES=str(self.root / 'mali.json'), VK_DRIVER_FILES=str(self.root / 'mali.json'))
        self.env.pop('MALI_VULKAN_SHARED_STAGING', None)
        (self.root / 'mali.json').write_text('{}')
        (self.root / 'swrast.so').touch()
        for name in ('broker', 'wayland-0'):
            sock = socket.socket(socket.AF_UNIX)
            sock.bind(str(self.root / name))
            self.addCleanup(sock.close)
        self.stub('gamescope', "import json,os,sys\nprint('ARGV='+json.dumps(sys.argv[1:]))\nprint('ICD='+os.environ['VK_DRIVER_FILES'])\nprint('INSIDE='+os.environ.get('BL_INSIDE',''))\nprint('SHARED='+os.environ.get('MALI_VULKAN_SHARED_STAGING','0'))")
        self.stub('Xwayland', 'pass')

    def stub(self, name, body):
        p = self.bin / name
        p.write_text('#!/usr/bin/env python3\n' + body + '\n')
        p.chmod(0o755)

    def run_session(self, mode='steam', env=None):
        command = ['bash', str(self.script), mode]
        with subprocess.Popen(command, env=self.env | (env or {}), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, start_new_session=True) as child:
            try:
                stdout, stderr = child.communicate(timeout=5)
                return subprocess.CompletedProcess(command, child.returncode, stdout, stderr)
            finally:
                # A failed fixture must not leave background fake services behind.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def test_mali_steam_outer_uses_existing_session_script_as_child(self):
        result = self.run_session()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(next(x[5:] for x in result.stdout.splitlines() if x.startswith('ARGV=')))
        self.assertEqual(args, ['--backend', 'wayland', '--mali-wayland-session', '--mali-xwayland',
            '--expose-wayland', '-f', '-W', '1280', '-H', '720', '--', str(self.script), 'steam'])
        self.assertIn('INSIDE=1', result.stdout)
        self.assertIn('SHARED=0', result.stdout)
        self.assertIn('ICD=' + self.env['VK_DRIVER_FILES'], result.stdout)
        self.assertNotIn('--mali-interactive-client', result.stdout)

    def test_shared_transport_reaches_only_outer_proxy_with_same_steam_command(self):
        baseline = self.run_session()
        enabled = self.run_session(env={'MALI_VULKAN_SHARED_STAGING': '1'})
        self.assertEqual(enabled.returncode, 0, enabled.stdout + enabled.stderr)
        self.assertIn('SHARED=1', enabled.stdout)
        self.assertIn('ICD=' + self.env['VK_DRIVER_FILES'], enabled.stdout)
        argv = lambda result: next(line for line in result.stdout.splitlines() if line.startswith('ARGV='))
        self.assertEqual(argv(baseline), argv(enabled))

    def test_outer_gamescope_ignores_client_config_and_retains_its_proxy_icd(self):
        config = Path(self.env['HOME']) / '.config/droiddeck/steam-client-panvk.json'
        config.parent.mkdir(parents=True)
        config.write_text('invalid development client config; must not be read by the compositor')
        result = self.run_session()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ICD=' + self.env['VK_DRIVER_FILES'], result.stdout)
        self.assertNotIn('PANVK_ATOM_STRIDE', result.stdout)
        self.assertNotIn('configuration/launch FAIL', result.stdout)

    def test_compatibility_dependencies_fail_before_gamescope_or_download(self):
        for missing, message in [('Xwayland', 'Xwayland missing'), ('software', 'software GL')]:
            with self.subTest(missing=missing):
                path = self.bin / 'Xwayland' if missing == 'Xwayland' else self.root / 'swrast.so'
                saved = path.read_bytes()
                path.unlink()
                # Keep host Xwayland outside the fixture search path in this case.
                self.stub('which-unused', 'pass')
                if missing == 'Xwayland':
                    self.script.write_text(self.script.read_text().replace('command -v Xwayland', 'command -v c8a-missing-Xwayland'))
                result = self.run_session()
                self.assertEqual(result.returncode, 66, result.stdout + result.stderr)
                self.assertIn(message, result.stdout)
                self.assertNotIn('ARGV=', result.stdout)
                path.write_bytes(saved)
                path.chmod(0o755)
                self.script.write_text(self.script.read_text().replace('command -v c8a-missing-Xwayland', 'command -v Xwayland'))

    def test_normal_gpu_preconditions_remain_required_for_steam(self):
        for env, expected in [({'MALI_VULKAN_NORMAL_SESSION': '0'}, 'environment missing'),
                              ({'WAYLAND_DISPLAY': 'wayland-9'}, 'outer Wayland'),
                              ({'VK_DRIVER_FILES': '/missing'}, 'proxy manifest missing'),
                              ({'MALI_VULKAN_BROKER_SOCKET': '/missing'}, 'broker socket disappeared')]:
            with self.subTest(env=env):
                result = self.run_session(env=env)
                self.assertEqual(result.returncode, 65)
                self.assertIn(expected, result.stdout)
                self.assertNotIn('ARGV=', result.stdout)

    def test_desktop_workload_reuses_lxqt_without_steam_xwayland(self):
        result = self.run_session('mali-wayland')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        args = json.loads(next(x[5:] for x in result.stdout.splitlines() if x.startswith('ARGV=')))
        self.assertEqual(args, ['--backend', 'wayland', '--mali-wayland-session', '--expose-wayland',
                               '-f', '-W', '1280', '-H', '720', '--', str(self.script), 'lxqt'])
        self.assertNotIn('--mali-interactive-client', args)
        self.assertNotIn('--mali-xwayland', args)
        self.assertIn('INSIDE=\n', result.stdout)

    def test_desktop_child_reuses_kde_launcher_and_preserves_parent_sockets(self):
        self.script.write_text(self.script.read_text().replace('/usr/local/bin/', str(self.bin) + '/').replace(
            '/usr/local/share/fonts/android', str(self.root / 'no-device-fonts')))
        self.env.update(BL_PIN_CORES='0', BL_LAUNCH_DIR='')
        self.stub('localedef', 'raise SystemExit(1)')
        self.stub('pgrep', "print('steamwebhelper --type= droiddeck-appimage-run dbus-run-session kwin')")
        self.stub('wayland-info', 'pass')
        self.stub('droiddeck-fonts', 'pass')
        self.stub('droiddeck-desktop-bookmarks', 'pass')
        desktop = (ROOT / 'tools/linuxfs/desktop/droiddeck-desktop').read_text()
        cleanup = next(line for line in desktop.splitlines() if line.startswith('find "$XDG_RUNTIME_DIR"'))
        self.stub('droiddeck-desktop', '''import json,os,socket,stat,subprocess
from pathlib import Path
subprocess.run(['bash','-c', ''' + repr(cleanup) + '''],check=True)
runtime=Path(os.environ['XDG_RUNTIME_DIR'])
parent=runtime/'wayland-0'
with socket.socket(socket.AF_UNIX) as kwin:
    kwin.bind(str(runtime/'wayland-1'))
    print('DESKTOP='+json.dumps(dict(display=os.environ['WAYLAND_DISPLAY'],runtime=runtime.name,
        parent=str(parent.readlink()),parent_socket=parent.is_socket(),
        outer_socket=(runtime.parent/'wayland-0').is_socket(),broker=(runtime.parent/'broker').is_socket(),
        mode=stat.S_IMODE(runtime.stat().st_mode),icd=os.environ.get('VK_DRIVER_FILES'))))
''')
        for absolute in (False, True):
            with self.subTest(absolute=absolute):
                self.stub('gamescope', '''import os,socket,subprocess,sys
from pathlib import Path
parent=Path(os.environ['XDG_RUNTIME_DIR'])/'wayland-1'
with socket.socket(socket.AF_UNIX) as server:
    server.bind(str(parent))
    env=dict(os.environ,WAYLAND_DISPLAY=''' + ('str(parent)' if absolute else "'wayland-1'") + ''')
    for key in ('VK_DRIVER_FILES','VK_ICD_FILENAMES','MALI_VULKAN_BROKER_SOCKET','MALI_VULKAN_NORMAL_SESSION',
                'MALI_VULKAN_RENDERER_TEST','MALI_VULKAN_AHB_TEST','MALI_VULKAN_SHARED_STAGING'):
        env.pop(key,None)
    child=subprocess.run(sys.argv[sys.argv.index('--')+1:],env=env)
parent.unlink()
raise SystemExit(child.returncode)
''')
                result = self.run_session('mali-wayland')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                actual = json.loads(next(line[8:] for line in result.stdout.splitlines() if line.startswith('DESKTOP=')))
                self.assertEqual(actual, dict(display='wayland-0', runtime='mali-desktop',
                    parent=str(self.root / 'wayland-1'), parent_socket=True,
                    outer_socket=True, broker=True, mode=0o700, icd=None))
        self.assertIn('dbus-run-session -- startplasma-wayland', desktop)

    def inner(self, mali=True, exits=(0,), graceful=False, experimental=False):
        # Exercise the actual shared Steam branch with cheap subprocess fixtures.
        home = self.root / 'home'
        steam = home / '.local/share/Steam'
        (steam / 'steamrtarm64').mkdir(parents=True)
        (steam / 'package').mkdir()
        (steam / 'steamapps').mkdir()
        (steam / 'steamapps/appmanifest_4427310.acf').touch()
        (home / '.bl-autolaunch').write_text('12345')
        (home / '.bl-proton-extra').write_text('ge\n')
        (home / '.bl-steam-urls').write_text('steam://install/12345\n')
        launch = self.root / 'launch'
        launch.mkdir()
        self.stub('dbus-daemon', "import os,socket,time\ns=socket.socket(socket.AF_UNIX)\ns.bind(" + repr(str(self.root / 'bus/system_bus_socket')) + ")\ntime.sleep(4)")
        self.stub('droiddeck-login1', "import os\nfrom pathlib import Path\nPath(os.environ['BL_LAUNCH_DIR'],'steam-sleep-ready').touch()")
        for name in ('droiddeck-steam-install', 'droiddeck-steam-library', 'droiddeck-steam-ui-scale',
                     'droiddeck-netmanager', 'droiddeck-steam-language', 'xprop', 'droiddeck-steam-compat'):
            self.stub(name, 'pass')
        for name in ('steam-compatibility', 'droiddeck-seed-redists', 'droiddeck-proton-extra', 'droiddeck-fex'):
            self.stub(name, "from pathlib import Path\nPath(" + repr(str(self.root / 'forbidden')) + ").touch()")
        binary = steam / 'steamrtarm64/steam'
        binary.write_text('''#!/usr/bin/env python3
import json,os,sys,time
from pathlib import Path
root=Path(''' + repr(str(self.root)) + ''')
p=root/'calls.json'
calls=json.loads(p.read_text()) if p.exists() else []
calls.append(sys.argv[1:]);p.write_text(json.dumps(calls))
e=root/'client-envs.json'
envs=json.loads(e.read_text()) if e.exists() else []
envs.append({k:os.environ.get(k) for k in ('PANVK_ATOM_STRIDE','VK_DRIVER_FILES','LIBGL_ALWAYS_SOFTWARE','ANGLE_DEFAULT_PLATFORM','MALI_VULKAN_SHARED_STAGING')})
e.write_text(json.dumps(envs))
if '-shutdown' in sys.argv:
    (root/'shutdown-ack').touch();sys.exit(0)
if ''' + repr(graceful) + ''' and not (root/'graceful-once').exists():
    (root/'graceful-once').touch()
    (root/'launch/steam-stop').touch()
    deadline=time.monotonic()+2
    while not (root/'shutdown-ack').exists() and time.monotonic()<deadline: time.sleep(.01)
    sys.exit(0 if (root/'shutdown-ack').exists() else 9)
exits=''' + repr(exits) + '''
sys.exit(exits[min(len(calls)-1,len(exits)-1)])
''')
        binary.chmod(0o755)
        self.inner_env = dict(BL_INSIDE='1', BL_MALI_STEAM_UI='1' if mali else '0',
            DISPLAY=':42', BL_LAUNCH_DIR=str(launch), BL_STEAM_CHANNEL='publicbeta', BL_STEAM_LANGUAGE='english',
            BL_DESKTOP='1', BL_LOSSLESS_OWNED='1', BL_STEAMDECK='0', BL_ADDED_GAMES='',
            XDG_DATA_HOME=str(home / '.local/share'), XDG_CONFIG_HOME=str(home / '.config'),
            XDG_CACHE_HOME=str(home / '.cache'), BL_CLIENT_CPUS='')

        if experimental:
            config = home / '.config/droiddeck/steam-client-panvk.json'
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text('{"version":1}')
            self.stub('droiddeck-steam-client-panvk', '''import json,os,sys
mode=sys.argv[1]
if mode=='offsets': print('{}')
if mode=='launch':
    command=sys.argv[sys.argv.index('--')+1:]
    env=dict(os.environ, PANVK_ATOM_STRIDE='56', ANGLE_DEFAULT_PLATFORM='vulkan', VK_DRIVER_FILES='/client/panvk.json')
    for key in ('LIBGL_ALWAYS_SOFTWARE','MESA_LOADER_DRIVER_OVERRIDE','GALLIUM_DRIVER'): env.pop(key,None)
    os.execve(command[0],command,env)
''')

        result = self.run_session(env=self.inner_env)
        calls = self.root / 'calls.json'
        return result, json.loads(calls.read_text()) if calls.exists() else []

    def test_shared_steam_bootstrap_channel_and_update_restart_without_game_provisioning(self):
        result, calls = self.inner(exits=(42, 0))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 2)
        for args in calls:
            self.assertIn('-gamepadui', args)
            self.assertIn('-cef-use-angle=gl', args)
            self.assertEqual(args[args.index('-clientbeta') + 1], 'steamdeck_publicbeta')
            self.assertNotIn('-cef-use-angle=vulkan', args)
            self.assertFalse(any(x.startswith('steam://') for x in args))
        self.assertFalse((self.root / 'forbidden').exists())
        self.assertIn('Steam client ready', result.stdout)
        self.assertEqual(result.stdout.count('starting the Steam client'), 2)
        self.assertIn('steam exited rc=42', result.stdout)
        self.assertTrue((self.root / 'home/.bl-proton-extra').exists())

    def test_non_mali_client_keeps_vulkan_angle_flags(self):
        # Isolate the real argument selection; ordinary game-helper lifetimes
        # are unrelated to C8A and are deliberately not started by this test.
        text = SCRIPT.read_text()
        selection = '    _bl_cef=' + text.split('    _bl_cef=', 1)[1].split('    if [ "${BL_STEAM_UI:-}" != desktop ]; then', 1)[0]
        result = subprocess.run(['bash', '-uc', selection + '\nprintf "%s\\n" "${_bl_cef[@]}"'],
            env=self.env | {'BL_MALI_STEAM_UI': '0'}, capture_output=True, text=True, timeout=2)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['-cef-force-gpu', '-cef-use-gl=angle', '-cef-use-angle=vulkan'])

    def test_invalid_optional_configuration_fails_before_client_exec(self):
        config = self.root / 'home/.config/droiddeck/steam-client-panvk.json'
        config.parent.mkdir(parents=True)
        config.write_text('invalid optional PanVK configuration')
        helper = ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-client-panvk'
        (self.bin / helper.name).write_bytes(helper.read_bytes())
        result, calls = self.inner()
        self.assertEqual(result.returncode, 66, result.stdout + result.stderr)
        self.assertEqual(calls, [])
        self.assertIn('configuration/launch FAIL', result.stdout)

    def test_software_fallback_without_configuration_clears_client_acceleration_overrides(self):
        self.env.update(PANVK_ATOM_STRIDE='64', ANGLE_DEFAULT_PLATFORM='vulkan', MALI_VULKAN_SHARED_STAGING='1')
        result, calls = self.inner()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('-cef-use-angle=gl', calls[0])
        environments = json.loads((self.root / 'client-envs.json').read_text())
        self.assertEqual(len(environments), 1)
        self.assertEqual(environments[0]['LIBGL_ALWAYS_SOFTWARE'], '1')
        self.assertIsNone(environments[0]['VK_DRIVER_FILES'])
        self.assertIsNone(environments[0]['PANVK_ATOM_STRIDE'])
        self.assertIsNone(environments[0]['ANGLE_DEFAULT_PLATFORM'])
        self.assertIsNone(environments[0]['MALI_VULKAN_SHARED_STAGING'])

    def test_optional_panvk_uses_supported_cef_option_across_update_restart(self):
        self.env['MALI_VULKAN_SHARED_STAGING'] = '1'
        result, calls = self.inner(exits=(42, 0), experimental=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 2)
        for args in calls:
            self.assertIn('-cef-use-vulkan', args)
            self.assertFalse(any(arg.startswith(('-cef-use-gl', '-cef-use-angle')) for arg in args))
        environments = json.loads((self.root / 'client-envs.json').read_text())
        self.assertTrue(all(row['PANVK_ATOM_STRIDE'] == '56' and row['ANGLE_DEFAULT_PLATFORM'] == 'vulkan'
                            and row['LIBGL_ALWAYS_SOFTWARE'] is None
                            and row['MALI_VULKAN_SHARED_STAGING'] is None for row in environments))

    def test_accelerated_client_stop_uses_native_shutdown_and_software_recovery(self):
        result, calls = self.inner(graceful=True, experimental=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(calls[1], ['-shutdown'])
        (self.root / 'home/.config/droiddeck/steam-client-panvk.json').unlink()
        result = self.run_session(env=self.inner_env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = json.loads((self.root / 'calls.json').read_text())
        self.assertIn('-cef-use-angle=gl', calls[-1])
        self.assertNotIn('-cef-use-vulkan', calls[-1])

    def test_mali_does_not_silently_start_the_x86_client(self):
        result = self.run_session(env={'BL_STEAM_CLIENT_ARCH': 'x86_64'})
        self.assertEqual(result.returncode, 66, result.stdout + result.stderr)
        self.assertIn('requires the ARM64 Steam client', result.stdout)
        self.assertNotIn('ARGV=', result.stdout)

    def test_real_stop_mailbox_and_second_launch_reuse_existing_client(self):
        result, calls = self.inner(graceful=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('the app asked for a clean exit', result.stdout)
        self.assertEqual(calls[1], ['-shutdown'])
        binary = self.root / 'home/.local/share/Steam/steamrtarm64/steam'
        saved = binary.read_bytes()
        second = self.run_session(env=self.inner_env)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(binary.read_bytes(), saved)
        self.assertIn('starting the Steam client', second.stdout)
        self.assertNotIn('PASS', second.stdout)

    def test_steam_failure_propagates_no_success_claim(self):
        result, calls = self.inner(exits=(7,))
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertEqual(len(calls), 1)
        self.assertIn('steam exited rc=7', result.stdout)
        self.assertNotIn('PASS', result.stdout)
        self.assertNotIn('UI reached', result.stdout)

    def test_xwm_patch_and_renderer_boundary(self):
        # Apply to the exact existing added source; no download, compiler or project build.
        from gamescope_source import source as patched
        before = (patched('0214-mali-upload-ring-ranges.patch') / 'src/mali_normal.cpp').read_text()
        source = patched('0215-mali-steam-xwayland-client.patch') / 'src'
        after = (source / 'mali_normal.cpp').read_text()
        self.assertLess(after.index('!StartX11(compositor)'), after.index('server.child = fork()'))
        self.assertIn('"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "BL_VK_DRIVER"', after)
        self.assertIn('setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1)', after)
        self.assertIn('setenv("GALLIUM_DRIVER", "llvmpipe", 1)', after)
        self.assertIn('if (!server.x11 && now - progress', after)
        self.assertIn('server.x11 && server.childGroup > 1 ? -server.childGroup : server.child', after)
        self.assertIn('kill(target, SIGTERM)', after)
        self.assertIn('kill(target, SIGKILL)', after)
        self.assertEqual(before.split('vulkan_mali_normal_frame_begin();',1)[1].split('timespec time{};',1)[0],
                         after.split('vulkan_mali_normal_frame_begin();',1)[1].split('timespec time{};',1)[0])
        self.assertNotIn('rendervulkan', '\n'.join(x for x in PATCH.read_text().splitlines() if x.startswith('+++')))
        xwm = (source / 'mali_x11.inc').read_text()
        self.assertIn('XWAYLAND_NO_GLAMOR', xwm)
        self.assertLess(xwm.index('wlr_xwayland_destroy(server.xwayland)'), xwm.index('wlr_backend_destroy'))
        self.assertIn('UI/sign-in needs visual confirmation', xwm)

    def test_app_reuses_steam_services_with_separate_backend_and_safe_stop(self):
        base = ROOT / 'app/src/main/java/com/droiddeck/launcher'
        service = (base / 'session/SessionService.kt').read_text()
        activity = (base / 'SessionActivity.kt').read_text()
        main = (base / 'MainActivity.kt').read_text()
        self.assertIn('intent.putExtra(SessionService.EXTRA_MODE, SessionService.MODE_STEAM)', main)
        self.assertIn('maliBackend = loadingMali()', activity)
        self.assertIn('val needProton = !loadingMali()', activity)
        normal = service.split('private fun runSession',1)[1].split('private fun runMaliSession',1)[0]
        self.assertLess(normal.index('MaliNormalBroker.start'), normal.index('HostProcess.start'))
        self.assertLess(normal.index('networkLink.publish()'), normal.index('HostProcess.start'))
        self.assertIn('addControllerEnvironment(guest, fakeInputDir, sessionDir)', normal)
        self.assertIn('val binds = sessionBinds(', normal)
        stop = service.split('val finishMali:',1)[1].split('val finishAfterTeardown:',1)[0]
        self.assertLess(stop.index('askSteamToExit'), stop.index('stopMaliGuest'))
        self.assertLess(stop.index('stopMaliGuest'), stop.index('MaliNormalBroker.stop'))
        self.assertIn('SessionState.maliBackend || SessionState.stopRequested', service)


if __name__ == '__main__':
    unittest.main()
