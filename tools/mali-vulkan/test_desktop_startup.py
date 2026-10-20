"""Desktop first-frame deadlines and real launcher socket handoff; host mocks only."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from gamescope_source import source
import test_steam_launch as launch

ROOT = Path(__file__).resolve().parents[2]


class DesktopWatchdogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get('GAMESCOPE_SOURCE'):
            raise unittest.SkipTest('Supply pristine Gamescope through --gamescope-source')
        cls.temporary = tempfile.TemporaryDirectory(prefix='mali-watchdog-')
        cls.addClassCleanup(cls.temporary.cleanup)
        build = Path(cls.temporary.name)
        fixture = build / 'watchdog.cpp'
        fixture.write_text('''#include "mali_normal_watchdog.hpp"
#include <cassert>
#include <string>
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int main(int argc, char **argv) {
    assert(argc == 2);
    std::string test = argv[1];
    Clock::time_point start{};
    if (test == "scope") {
        assert(mali_normal_desktop_child("1", "/usr/local/bin/droiddeck-session", "lxqt"));
        assert(mali_normal_desktop_child("1", "droiddeck-session", "lxqt"));
        assert(!mali_normal_desktop_child(nullptr, "droiddeck-session", "lxqt"));
        assert(!mali_normal_desktop_child("0", "droiddeck-session", "lxqt"));
        assert(!mali_normal_desktop_child("true", "droiddeck-session", "lxqt"));
        assert(!mali_normal_desktop_child("1", nullptr, "lxqt"));
        assert(!mali_normal_desktop_child("1", "droiddeck-session", nullptr));
        assert(!mali_normal_desktop_child("1", "droiddeck-session", "steam"));
        assert(!mali_normal_desktop_child("1", "gamescope", "--mali-interactive-client"));
        assert(!mali_normal_desktop_child("1", "other-session", "lxqt"));
    } else if (test == "cold") {
        MaliNormalWatchdog watchdog(start, true);
        assert(watchdog.awaiting_desktop_frame() && watchdog.timeout_seconds() == 90);
        assert(!watchdog.expired(start + 15s));
        assert(!watchdog.expired(start + 89s));
        assert(!watchdog.expired(start + 90s));
        assert(watchdog.expired(start + 90s + 1ns));
    } else if (test == "interactive") {
        MaliNormalWatchdog watchdog(start, false);
        assert(!watchdog.awaiting_desktop_frame() && watchdog.timeout_seconds() == 15);
        assert(!watchdog.expired(start + 15s));
        assert(watchdog.expired(start + 15s + 1ns));
        watchdog.presented(start + 16s);
        assert(!watchdog.expired(start + 31s));
        assert(watchdog.expired(start + 31s + 1ns));
    } else if (test == "first") {
        MaliNormalWatchdog watchdog(start, true);
        watchdog.presented(start + 2s);
        assert(!watchdog.awaiting_desktop_frame() && watchdog.timeout_seconds() == 15);
        assert(!watchdog.expired(start + 17s));
        assert(watchdog.expired(start + 17s + 1ns));
        watchdog.presented(start + 18s);
        assert(!watchdog.expired(start + 33s));
        assert(watchdog.expired(start + 33s + 1ns));
    } else if (test == "late") {
        MaliNormalWatchdog watchdog(start, true);
        watchdog.presented(start + 89s);
        assert(!watchdog.expired(start + 100s));
        assert(!watchdog.expired(start + 104s));
        assert(watchdog.expired(start + 104s + 1ns));
    } else return 2;
}
''')
        cls.binary = build / 'watchdog'
        subprocess.run(shlex.split(os.environ.get('HOST_CXX', 'c++')) +
                       ['-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-I' + str(source() / 'src'),
                        str(fixture), '-o', str(cls.binary)], check=True, timeout=15)

    def test_desktop_grace_is_explicit_and_excludes_steam_and_interactive(self):
        subprocess.run([str(self.binary), 'scope'], check=True)

    def test_cold_desktop_is_bounded_at_ninety_seconds(self):
        subprocess.run([str(self.binary), 'cold'], check=True)

    def test_interactive_keeps_original_fifteen_second_deadlines(self):
        subprocess.run([str(self.binary), 'interactive'], check=True)

    def test_first_present_restores_normal_deadline_even_inside_startup_grace(self):
        subprocess.run([str(self.binary), 'first'], check=True)

    def test_late_first_frame_gets_fifteen_seconds_from_actual_progress(self):
        subprocess.run([str(self.binary), 'late'], check=True)

    def test_actual_loop_arms_after_fork_and_updates_only_after_successful_present(self):
        current = (source() / 'src/mali_normal.cpp').read_text()
        previous = (source('0218-mali-staging-bulk-writes.patch') / 'src/mali_normal.cpp').read_text()
        self.assertLess(current.index('StartServer(command)'), current.index('MaliNormalWatchdog watchdog'))
        self.assertIn('!server.x11 && mali_normal_desktop_child(getenv("BL_MALI_DESKTOP"), command[0], command[1])', current)
        self.assertIn('if (!server.x11 && watchdog.expired(now))', current)
        self.assertEqual(current.count('watchdog.presented(now)'), 1)
        success = current.split('if (result == 0) {', 1)[1].split('} else if (result != EAGAIN)', 1)[0]
        self.assertIn('watchdog.presented(now)', success)
        for start, end in [('static bool StartServer(', 'static void StopServer('),
                           ('vulkan_mali_normal_frame_begin();', 'if (result == 0) {'),
                           ('    ok &= !failed;', '    return ok ? 0 : 1;')]:
            self.assertEqual(previous.split(start, 1)[1].split(end, 1)[0],
                             current.split(start, 1)[1].split(end, 1)[0])


class DesktopRuntimeTests(unittest.TestCase):
    setUp = launch.SteamLaunchTests.setUp
    stub = launch.SteamLaunchTests.stub
    run_session = launch.SteamLaunchTests.run_session

    def prepare_desktop(self):
        import re
        self.script.write_text(self.script.read_text().replace('/usr/local/bin/', str(self.bin) + '/').replace(
            '/usr/local/share/fonts/android', str(self.root / 'no-device-fonts')))
        self.env.update(BL_PIN_CORES='0', BL_LAUNCH_DIR='', BL_DEBUG_DIR=str(self.root / 'debug'))
        self.stub('localedef', 'raise SystemExit(1)')
        self.stub('pgrep', "print('steamwebhelper --type= droiddeck-appimage-run dbus-run-session kwin')")
        for name in ('wayland-info', 'droiddeck-fonts', 'droiddeck-desktop-bookmarks',
                     'droiddeck-desktop-games', 'droiddeck-pad-defaults', 'droiddeck-open',
                     'droiddeck-desktop-window-rules', 'update-mime-database',
                     'gtk-update-icon-cache', 'glib-compile-schemas', 'droiddeck-steam-launch'):
            self.stub(name, 'pass')
        self.stub('wayland-info', "print('mock parent globals')")
        self.stub('droiddeck-desktop-gpu', '''import os
from pathlib import Path
if os.environ.get('REMOVE_PARENT') == '1':
    (Path(os.environ['XDG_RUNTIME_DIR'])/'wayland-0').unlink()
''')
        self.stub('sleep', 'raise SystemExit(1)')
        self.stub('xdg-user-dir', 'print(' + repr(str(self.root / 'home/Desktop')) + ')')
        self.stub('dbus-run-session', 'import os,sys\nos.execvp(sys.argv[2],sys.argv[2:])')
        self.stub('startplasma-wayland', '''import os
from pathlib import Path
if os.environ.get('REMOVE_PARENT_AT_KWIN') == '1':
    (Path(os.environ['XDG_RUNTIME_DIR'])/'wayland-0').unlink()
os.execvp('kwin_wayland_wrapper',['kwin_wayland_wrapper'])
''')
        prefixes = {'/usr/local/bin/': str(self.bin) + '/', '/usr/share/': str(self.root / 'share') + '/',
                    '/etc/': str(self.root / 'etc') + '/', '/tmp/': str(self.root / 'tmp') + '/'}
        pattern = re.compile('|'.join(re.escape(path) for path in prefixes))
        desktop = pattern.sub(lambda match: prefixes[match[0]], (ROOT / 'tools/linuxfs/desktop/droiddeck-desktop').read_text())
        (self.root / 'share/applications').mkdir(parents=True)
        target = self.bin / 'droiddeck-desktop'
        target.write_text(desktop)
        target.chmod(0o755)
        wrapper = (ROOT / 'tools/linuxfs/desktop/kwin_wayland_wrapper').read_text()
        target = self.bin / 'kwin_wayland_wrapper'
        target.write_text(wrapper.replace('/usr/bin/kwin_wayland_wrapper', str(self.bin / 'kwin-native')))
        target.chmod(0o755)
        self.stub('kwin-native', '''import json,os,socket,sys
from pathlib import Path
runtime=Path(os.environ['XDG_RUNTIME_DIR'])
parent=runtime/sys.argv[sys.argv.index('--wayland-display')+1]
with socket.socket(socket.AF_UNIX) as native, socket.socket(socket.AF_UNIX) as own:
    native.connect(str(parent))
    native.sendall(b'kwin-parent-alias')
    assert native.recv(16) == b'gamescope'
    own.bind(str(runtime/sys.argv[sys.argv.index('--socket')+1]))
    print('KWIN='+json.dumps(dict(runtime=runtime.name,parent=str(parent.readlink()),
        parent_socket=parent.is_socket(),outer_socket=(runtime.parent/'wayland-0').is_socket(),
        broker=(runtime.parent/'broker').is_socket())))
''')

    def gamescope(self, name, absolute=False):
        self.stub('gamescope', '''import os,socket,subprocess,sys
from pathlib import Path
parent=Path(os.environ['XDG_RUNTIME_DIR'])/''' + repr(name) + '''
with socket.socket(socket.AF_UNIX) as server:
    server.bind(str(parent));server.listen();server.settimeout(.05)
    env=dict(os.environ,WAYLAND_DISPLAY=''' + ('str(parent)' if absolute else repr(name)) + ''')
    for key in ('VK_DRIVER_FILES','VK_ICD_FILENAMES','MALI_VULKAN_BROKER_SOCKET','MALI_VULKAN_NORMAL_SESSION',
                'MALI_VULKAN_RENDERER_TEST','MALI_VULKAN_AHB_TEST','MALI_VULKAN_SHARED_STAGING'):
        env.pop(key,None)
    with subprocess.Popen(sys.argv[sys.argv.index('--')+1:],env=env) as child:
        try:
            connected=False
            while child.poll() is None:
                try: connection,_=server.accept()
                except socket.timeout: continue
                with connection:
                    assert connection.recv(32) == b'kwin-parent-alias'
                    connection.sendall(b'gamescope');connected=True
                break
            code=child.wait(timeout=3)
            assert parent.is_socket()
            if code == 0: assert connected
        finally:
            if child.poll() is None: child.kill();child.wait()
parent.unlink()
raise SystemExit(code)
''')

    def test_alias_survives_real_desktop_cleanup_plasma_entry_and_kwin_wrapper(self):
        self.prepare_desktop()
        outer = (self.root / 'wayland-0').stat().st_ino
        broker = (self.root / 'broker').stat().st_ino
        for name, absolute in (('wayland-7', False), ('wayland-11', True)):
            with self.subTest(name=name, absolute=absolute):
                self.gamescope(name, absolute)
                report = self.root / 'debug/desktop.log'
                if report.exists(): report.unlink()
                result = self.run_session('mali-wayland')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr + report.read_text())
                output = report.read_text()
                for stage in ('Mali desktop parent (after-cleanup)', 'Mali desktop parent (before-Plasma)', 'Mali KWin parent:'):
                    self.assertIn(stage, output)
                actual = json.loads(next(line[5:] for line in output.splitlines() if line.startswith('KWIN=')))
                self.assertEqual(actual, dict(runtime='mali-desktop', parent=str(self.root / name),
                    parent_socket=True, outer_socket=True, broker=True))
                self.assertEqual((self.root / 'wayland-0').stat().st_ino, outer)
                self.assertEqual((self.root / 'broker').stat().st_ino, broker)
                self.assertIn('mock parent globals', (self.root / 'debug/mali-desktop-wayland.txt').read_text())

    def test_parent_loss_before_plasma_fails_without_using_android_display(self):
        self.prepare_desktop()
        self.gamescope('wayland-9')
        result = self.run_session('mali-wayland', env={'REMOVE_PARENT': '1'})
        self.assertEqual(result.returncode, 65, result.stdout + result.stderr)
        output = (self.root / 'debug/desktop.log').read_text()
        self.assertIn('Mali desktop parent unavailable (before-Plasma)', output)
        self.assertNotIn('KWIN=', output)
        self.assertTrue((self.root / 'wayland-0').is_socket())
        self.assertTrue((self.root / 'broker').is_socket())

    def test_parent_loss_at_kwin_entry_fails_without_fallback(self):
        self.prepare_desktop()
        self.gamescope('wayland-12')
        result = self.run_session('mali-wayland', env={'REMOVE_PARENT_AT_KWIN': '1'})
        self.assertEqual(result.returncode, 65, result.stdout + result.stderr)
        output = (self.root / 'debug/desktop.log').read_text()
        self.assertIn('Mali desktop parent (before-Plasma)', output)
        self.assertIn('Mali KWin parent unavailable:', output)
        self.assertNotIn('KWIN=', output)
        self.assertTrue((self.root / 'wayland-0').is_socket())

    def test_ordinary_desktop_wrapper_keeps_its_arguments_without_a_mali_alias(self):
        wrapper = ROOT / 'tools/linuxfs/desktop/kwin_wayland_wrapper'
        script = self.root / 'kwin-wrapper'
        script.write_text(wrapper.read_text().replace('/usr/bin/kwin_wayland_wrapper', str(self.bin / 'kwin-native')))
        self.stub('kwin-native', 'import json,sys\nprint(json.dumps(sys.argv[1:]))')
        env = dict(self.env, BL_DESKTOP='1', BL_MALI_DESKTOP='0')
        result = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), ['--wayland-display', 'wayland-0', '--socket', 'wayland-1',
            '--width', '1280', '--height', '720', '--no-lockscreen'])
        self.assertFalse((self.root / 'wayland-0').is_symlink())


if __name__ == '__main__':
    unittest.main()
