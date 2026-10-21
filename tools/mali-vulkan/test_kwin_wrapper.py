"""Exercise the managed wrapper, native-style socket allocation and real child logging."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DesktopComparisonContracts(unittest.TestCase):
    def test_all_twenty_nine_previous_patches_are_byte_identical(self):
        paths = sorted(path for path in (ROOT / 'tools/gamescope/patches').glob('*.patch') if path.name[:4] < '0221')
        self.assertEqual(len(paths), 29)
        digest = hashlib.sha256()
        for path in paths:
            digest.update(path.name.encode() + b'\0' + path.read_bytes())
        self.assertEqual(digest.hexdigest(), '74d6e65ea1f05cb9e895a62adbf6e2b694d81ba2a1248874e64a626acd4d3a43')

    def test_overlay_preserves_output_renderer_presentation_input_and_watchdog_implementations(self):
        if not os.environ.get('GAMESCOPE_SOURCE'):
            self.skipTest('Supply pristine Gamescope through --gamescope-source')
        from gamescope_source import source
        current, previous = source(), source('0220-mali-kwin-host-protocols.patch')
        for name in ('Backends/WaylandBackend.cpp', 'rendervulkan.cpp', 'mali_normal.inc', 'mali_normal_watchdog.hpp',
                     'mali_interactive.cpp', 'mali_x11.inc'):
            self.assertEqual((current / 'src' / name).read_bytes(), (previous / 'src' / name).read_bytes(), name)
        before, after = [(tree / 'src/mali_desktop.c').read_text() for tree in (previous, current)]
        for name in ('static void configure(', 'static void new_toplevel(', 'static void new_popup(',
                     'struct wlr_surface *mali_desktop_pointer(', 'static void render_buffer(',
                     'struct wlr_buffer *mali_desktop_render(', 'void mali_desktop_frame_done(',
                     'static void presented(', 'static void discarded(', 'static void sample(',
                     'bool mali_desktop_queue_presentation('):
            self.assertEqual(before.split(name, 1)[1].split('\n}\n', 1)[0],
                             after.split(name, 1)[1].split('\n}\n', 1)[0], name)
        normal = (current / 'src/mali_normal.cpp').read_text()
        self.assertIn('if (server.desktopServer) mali_desktop_report(server.desktopServer, server.child, "watchdog-timeout")', normal)
        trace = (current / 'src/mali_desktop_trace.inc').read_text()
        self.assertIn('wl_client_get_credentials', trace)
        self.assertIn('wl_display_add_protocol_logger', trace)
        self.assertIn('process.started == client->started', trace)
        self.assertIn('trace_descendant((pid_t)number, child)', trace)
        self.assertIn('dropped-records=%u untracked-clients=%u', trace)


class KWinWrapperTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='mali-kwin-log-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.private = self.root / 'mali-desktop'
        self.private.mkdir(mode=0o700)
        self.debug = self.root / 'debug'
        self.debug.mkdir()
        self.target = self.root / 'wayland-9'
        self.parent = socket.socket(socket.AF_UNIX)
        self.addCleanup(self.parent.close)
        self.parent.bind(str(self.target))
        self.parent.listen()
        self.parent.settimeout(2)
        (self.private / 'wayland-0').symlink_to(self.target)
        self.wrapper = self.root / 'kwin_wayland_wrapper'
        source = (ROOT / 'tools/linuxfs/desktop/kwin_wayland_wrapper').read_text()
        source = source.replace('/usr/local/bin/kwin_wayland_wrapper', str(self.wrapper)).replace(
            '/usr/bin/kwin_wayland_wrapper', str(self.root / 'native-wrapper')).replace(
            '"/usr/bin/kwin_wayland"', json.dumps(str(self.root / 'real-kwin')))
        self.wrapper.write_text(source)
        self.wrapper.chmod(0o755)
        self.stub('native-wrapper', '''import ctypes,os,signal,subprocess,sys
from pathlib import Path
library=ctypes.CDLL("libwayland-server.so.0")
library.wl_display_create.restype=ctypes.c_void_p
library.wl_display_add_socket_auto.argtypes=[ctypes.c_void_p]
library.wl_display_add_socket_auto.restype=ctypes.c_char_p
library.wl_display_destroy.argtypes=[ctypes.c_void_p]
display=library.wl_display_create()
name=library.wl_display_add_socket_auto(display).decode()
assert name == "wayland-1", name
assert (Path(os.environ["XDG_RUNTIME_DIR"])/"wayland-0").is_symlink()
if os.environ.get("DROP_ALIAS") == "1":
    (Path(os.environ["XDG_RUNTIME_DIR"])/"wayland-0").unlink()
read,write=os.pipe()
os.write(write,b"native-listening-descriptor");os.close(write)
process=subprocess.Popen(["kwin_wayland","--wayland-fd",str(read),*sys.argv[1:]],pass_fds=(read,))
os.close(read)
status=process.wait()
library.wl_display_destroy(display)
if status < 0:
    if -status not in (signal.SIGKILL,signal.SIGSTOP): signal.signal(-status,signal.SIG_DFL)
    os.kill(os.getpid(),-status)
raise SystemExit(status)
''')
        self.stub('real-kwin', '''import json,os,signal,socket,sys,time
from pathlib import Path
argv=sys.argv[1:]
runtime=Path(os.environ["XDG_RUNTIME_DIR"])
parent=runtime/argv[argv.index("--wayland-display")+1]
own=runtime/argv[argv.index("--socket")+1]
assert parent.is_symlink() and parent.is_socket() and own.is_socket()
fd=int(argv[argv.index("--wayland-fd")+1])
assert os.read(fd,128) == b"native-listening-descriptor"
with socket.socket(socket.AF_UNIX) as connection:
    connection.connect(str(parent));connection.sendall(b"actual-kwin-child")
print("KWin child stdout",flush=True)
print("KWin child stderr",file=sys.stderr,flush=True)
print("KWINCHILD="+json.dumps(dict(pid=os.getpid(),parent=str(parent.readlink()),own=own.name)),flush=True)
mode=os.environ.get("KWIN_TEST_MODE","success")
if mode == "wait": time.sleep(60)
if mode == "signal": os.kill(os.getpid(),int(os.environ.get("KWIN_TEST_SIGNAL",str(signal.SIGTERM))))
raise SystemExit(7 if mode == "exit7" else 0)
''')
        self.env = dict(os.environ, PATH='/usr/bin:/bin', XDG_RUNTIME_DIR=str(self.private),
                        WAYLAND_DISPLAY='wayland-0', BL_DESKTOP='1', BL_MALI_DESKTOP='1',
                        BL_DEBUG_DIR=str(self.debug), BL_WIDTH='1560', BL_HEIGHT='720',
                        KWIN_COMPOSE='Q', QT_QUICK_BACKEND='software')

    def stub(self, name, code):
        path = self.root / name
        path.write_text('#!/usr/bin/env python3\n' + code)
        path.chmod(0o755)

    def records(self):
        return [json.loads(line) for line in (self.debug / 'kwin-wayland.log').read_text().splitlines()
                if line.startswith('{')]

    def launch(self, **env):
        process = subprocess.Popen([str(self.wrapper)], env=dict(self.env, **env),
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
        def cleanup():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
        self.addCleanup(cleanup)
        return process

    def test_lock_survives_exec_child_fds_and_actual_stdout_stderr_are_logged(self):
        inode = self.target.stat().st_ino
        process = self.launch()
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, output + (self.debug / 'kwin-wayland.log').read_text())
        connection, _ = self.parent.accept()
        with connection:
            self.assertEqual(connection.recv(64), b'actual-kwin-child')
        records = self.records()
        launch = next(record for record in records if record['event'] == 'kwin-launch')
        started = next(record for record in records if record['event'] == 'kwin-started')
        exited = next(record for record in records if record['event'] == 'kwin-exited')
        text = (self.debug / 'kwin-wayland.log').read_text()
        child = json.loads(next(line[10:] for line in text.splitlines() if line.startswith('KWINCHILD=')))
        self.assertEqual(started['pid'], child['pid'])
        self.assertEqual(exited['pid'], child['pid'])
        self.assertEqual((exited['exit_status'], exited['signal']), (0, None))
        self.assertEqual(launch['parent_target'], str(self.target))
        self.assertEqual(launch['own_socket'], str(self.private / 'wayland-1'))
        self.assertEqual(launch['environment']['KWIN_COMPOSE'], 'Q')
        self.assertIn('--wayland-fd', launch['argv'])
        self.assertIn('KWin child stdout', text)
        self.assertIn('KWin child stderr', text)
        self.assertEqual(self.target.stat().st_ino, inode)
        self.assertTrue((self.private / 'wayland-0').is_symlink())
        with (self.private / 'wayland-0.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_nonzero_exit_is_preserved(self):
        process = self.launch(KWIN_TEST_MODE='exit7')
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 7, output)
        exited = self.records()[-1]
        self.assertEqual((exited['exit_status'], exited['signal']), (7, None))

    def test_child_signal_is_reported_and_preserved(self):
        for number in (signal.SIGTERM, signal.SIGKILL):
            with self.subTest(signal=number):
                process = self.launch(KWIN_TEST_MODE='signal', KWIN_TEST_SIGNAL=str(number))
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, -number, output)
                self.assertEqual(self.records()[-1]['signal'], number)

    def test_stop_signal_reaches_actual_child_and_releases_alias_lock(self):
        process = self.launch(KWIN_TEST_MODE='wait')
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            path = self.debug / 'kwin-wayland.log'
            if path.exists() and 'KWINCHILD=' in path.read_text():
                break
            time.sleep(.02)
        launch = next(record for record in self.records() if record['event'] == 'kwin-launch')
        os.kill(launch['supervisor_pid'], signal.SIGTERM)
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, -signal.SIGTERM, output)
        self.assertEqual(self.records()[-1]['signal'], signal.SIGTERM)
        with (self.private / 'wayland-0.lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_existing_lock_owner_fails_without_replacing_any_parent_socket(self):
        with (self.private / 'wayland-0.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            process = self.launch()
            output, _ = process.communicate(timeout=3)
            self.assertEqual(process.returncode, 65, output)
            self.assertIn('parent lock is already owned', output)
            self.assertTrue((self.private / 'wayland-0').is_symlink())
            self.assertTrue(self.target.is_socket())

    def test_hard_kill_of_logging_supervisor_does_not_orphan_actual_kwin(self):
        process = self.launch(KWIN_TEST_MODE='wait')
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            path = self.debug / 'kwin-wayland.log'
            if path.exists() and 'KWINCHILD=' in path.read_text():
                break
            time.sleep(.02)
        records = self.records()
        launch = next(record for record in records if record['event'] == 'kwin-launch')
        child = next(record for record in records if record['event'] == 'kwin-started')
        os.kill(launch['supervisor_pid'], signal.SIGKILL)
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, -signal.SIGKILL, output)
        stat_file = Path('/proc') / str(child['pid']) / 'stat'
        def state():
            try:
                return stat_file.read_text().rsplit(')', 1)[1].split()[0]
            except FileNotFoundError:
                return None
        deadline = time.monotonic() + 2
        observed = state()
        while observed not in (None, 'Z') and time.monotonic() < deadline:
            time.sleep(.02)
            observed = state()
        self.assertIn(observed, (None, 'Z'))

    def test_alias_loss_after_native_allocation_fails_before_actual_kwin(self):
        process = self.launch(DROP_ALIAS='1')
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 65, output)
        self.assertIn('kwin-parent-lost', (self.debug / 'kwin-wayland.log').read_text())
        self.assertNotIn('kwin-started', (self.debug / 'kwin-wayland.log').read_text())
        self.assertTrue(self.target.is_socket())


if __name__ == '__main__':
    unittest.main()
