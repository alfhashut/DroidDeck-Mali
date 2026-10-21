"""KWin host protocol contracts and a small real wlroots/Wayland client fixture."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import unittest

from gamescope_source import source

ROOT = Path(__file__).resolve().parents[2]
PATCHES = ROOT / 'tools/gamescope/patches'
OVERLAY = '0220-mali-kwin-host-protocols.patch'


class DesktopProtocolContracts(unittest.TestCase):
    def test_all_twenty_eight_previous_patches_are_byte_identical(self):
        patches = sorted(path for path in PATCHES.glob('*.patch') if path.name < OVERLAY)
        self.assertEqual(len(patches), 28)
        digest = hashlib.sha256()
        for path in patches:
            digest.update(path.name.encode() + b'\0' + path.read_bytes())
        self.assertEqual(digest.hexdigest(), '3b8e6e6f9a965dc628b1721639eeceaabcd0fa576e6aefcaa3bde94ddafcd584')

    def test_overlay_changes_only_nested_server_and_test_build_wiring(self):
        targets = {line[6:] for line in (PATCHES / OVERLAY).read_text().splitlines() if line.startswith('+++ b/')}
        self.assertEqual(targets, {'src/mali_desktop.c', 'src/mali_desktop.h', 'src/mali_normal.cpp',
                                  'src/mali_normal.hpp', 'src/Backends/WaylandBackend.cpp',
                                  'src/meson.build', 'tests/mali_desktop_protocols.c'})

    def test_ci_runs_real_protocol_requests_and_unconfigured_buffer_rejection(self):
        builder = (ROOT / 'tools/gamescope/build-in-arch.sh').read_text()
        self.assertIn('cursor_routing mali_desktop_protocols mali_desktop_unconfigured', builder)
        if not os.environ.get('GAMESCOPE_SOURCE'):
            self.skipTest('Supply pristine Gamescope through --gamescope-source')
        meson = (source() / 'src/meson.build').read_text()
        self.assertIn("test('mali_desktop_protocols', malidesktop_test", meson)
        self.assertIn("test('mali_desktop_unconfigured', malidesktop_test", meson)
        self.assertIn("['mali_desktop.c', '../tests/mali_desktop_protocols.c']", meson)

    def test_desktop_is_scoped_and_legacy_paths_and_gpu_ownership_are_preserved(self):
        if not os.environ.get('GAMESCOPE_SOURCE'):
            self.skipTest('Supply pristine Gamescope through --gamescope-source')
        current, previous = source(), source('0219-mali-desktop-first-frame-grace.patch')
        normal = (current / 'src/mali_normal.cpp').read_text()
        old = (previous / 'src/mali_normal.cpp').read_text()
        self.assertIn('server.desktop = !server.x11 && mali_normal_desktop_child(', normal)
        self.assertIn('server.desktop ? wlr_pixman_renderer_create() : vulkan_renderer_create()', normal)
        self.assertIn('const char *socket = wl_display_add_socket_auto(server.display);', normal)
        for name in ('mali_interactive.cpp', 'mali_x11.inc',
                     'mali_normal.inc', 'mali_normal_watchdog.hpp', 'rendervulkan.cpp'):
            self.assertEqual((current / 'src' / name).read_bytes(), (previous / 'src' / name).read_bytes(), name)
        for start, end in [('    if (!server.child) {', 'static void StopServer()'),
                           ('static void NewSurface(', 'static void DesktopDirty(')]:
            if start.startswith('static'):
                self.assertEqual(normal.split(start)[1].split(end)[0], old.split(start)[1].split('static bool Dispatch(')[0])
            else:
                self.assertEqual(normal.split(start)[1].split(end)[0], old.split(start)[1].split(end)[0])
        desktop = (current / 'src/mali_desktop.c').read_text()
        for factory in ('subcompositor', 'xdg_shell', 'single_pixel_buffer_manager_v1', 'viewporter',
                        'pointer_constraints_v1', 'presentation'):
            self.assertIn('wlr_' + factory + '_create(', desktop)
        self.assertNotIn('wl_global_create(', desktop)
        self.assertIn('wlr_scene_xdg_surface_create(', desktop)
        self.assertIn('wlr_scene_subsurface_tree_create(', desktop)
        self.assertIn('wlr_presentation_surface_sampled(', desktop)
        self.assertIn('flags & ~WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY', desktop)
        self.assertNotIn('wlr_scene_output_commit(', desktop)
        self.assertNotIn('wlr_output_send_present(', desktop)


class DesktopProtocolExecution(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not all(os.environ.get(key) for key in ('GAMESCOPE_SOURCE', 'WLR_BUILD', 'WLR_HEADERS', 'WAYLAND_PROTOCOLS_DIR')):
            raise unittest.SkipTest('Real protocol fixture needs public wlroots 0.20.2 headers/library and Wayland XML; CI runs it via Meson')
        cls.temporary = tempfile.TemporaryDirectory(prefix='mali-desktop-protocols-')
        cls.addClassCleanup(cls.temporary.cleanup)
        build = Path(cls.temporary.name)
        generated = build / 'wayland-protocols'
        generated.mkdir()
        protocols = Path(os.environ['WAYLAND_PROTOCOLS_DIR'])
        for xml in protocols.rglob('*.xml'):
            subprocess.run(['wayland-scanner', 'enum-header', str(xml), str(generated / (xml.stem + '-enum.h'))],
                           check=True, capture_output=True)
        objects = []
        for name in ('xdg-shell', 'viewporter', 'single-pixel-buffer-v1', 'pointer-constraints-unstable-v1', 'presentation-time'):
            xml = next(protocols.rglob(name + '.xml'))
            for mode, suffix in (('client-header', '-client-protocol.h'), ('server-header', '-protocol.h'), ('private-code', '-protocol.c')):
                subprocess.run(['wayland-scanner', mode, str(xml), str(build / (name + suffix))], check=True, capture_output=True)
            objects.append(str(build / (name + '-protocol.c')))
        pkg = ['wayland-server', 'wayland-client', 'pixman-1', 'libdrm']
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', *pkg], text=True))
        tree = source()
        cls.binary = build / 'protocol-test'
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) +
                       ['-std=gnu11', '-O0', '-Wall', '-Wextra', '-Werror', '-DWLR_USE_UNSTABLE',
                        '-I' + str(tree / 'src'), '-I' + str(build), '-I' + os.environ['WLR_HEADERS'],
                        str(tree / 'src/mali_desktop.c'), str(tree / 'tests/mali_desktop_protocols.c'), *objects,
                        '-L' + os.environ['WLR_BUILD'], '-Wl,-rpath,' + os.environ['WLR_BUILD'], '-lwlroots-0.20',
                        *flags, '-o', str(cls.binary)], check=True, timeout=30)

    def run_fixture(self, *args):
        process = subprocess.Popen([str(self.binary), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, start_new_session=True)
        try:
            output, _ = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, output)
            self.assertIn('KWin host protocol bind/use and lifecycle: PASS', output)
            return output
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()

    def test_bind_configure_ack_popup_subsurfaces_viewports_constraints_feedback_and_cleanup(self):
        output = self.run_fixture()
        for name in ('wl_compositor', 'wl_subcompositor', 'xdg_wm_base', 'wp_single_pixel_buffer_manager_v1',
                     'wp_viewporter', 'wl_seat', 'zwp_pointer_constraints_v1', 'wp_presentation'):
            self.assertIn('nested registry: ' + name + ' version=', output)
        self.assertIn('comm=wayland-info', output)
        self.assertIn('comm=kwin_wayland', output)
        summary = next(line for line in output.splitlines() if line.startswith('KWin handshake: client='))
        for field in ('globals=8/8', 'configure-sent=2', 'configure-acked=2', 'shm-pools=2', 'buffers=2',
                      'frame-requests=2', 'frame-fired=2', 'feedback=4', 'presented=2', 'discarded=1',
                      'protocol-error=none-observed', 'lost-state=0'):
            self.assertIn(field, summary)
        self.assertIn('dropped-records=0', output)
        self.assertIn('bound xdg_wm_base@', output)
        self.assertIn('format=1', output)
        self.assertIn('format=0', output)
        self.assertIn('surface-frame=no', output)
        self.assertIn('surface-frame=yes', output)
        self.assertEqual(len({line for line in output.splitlines() if line.startswith('KWin handshake: client=')}), 1)

    def test_unconfigured_xdg_buffer_is_a_real_protocol_error(self):
        output = self.run_fixture('unconfigured')
        self.assertIn('xdg_surface has never been configured', output)
        self.assertIn('accepted-commits=0 buffer-commits=0', output)
        self.assertIn('connected=no', output)
        self.assertIn('protocol-error=code=3', output)

    def test_wrong_configure_serial_is_reported_without_counting_an_ack(self):
        output = self.run_fixture('bad-ack')
        self.assertIn('wrong configure serial:', output)
        self.assertIn('configure-sent=1 configure-acked=0', output)
        self.assertIn('protocol-error=code=4', output)


if __name__ == '__main__':
    unittest.main()
