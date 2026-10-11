"""Development Steam launch isolation/evidence checks; no GPU, compiler or download."""
import argparse
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / 'tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-client-panvk'
loader = importlib.machinery.SourceFileLoader('steam_client_panvk', str(LAUNCHER))
spec = importlib.util.spec_from_loader(loader.name, loader)
client = importlib.util.module_from_spec(spec)
loader.exec_module(client)


class SteamClientPanvkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def fixture(self):
        target = self.root / 'panvk-test' / ('a' * 64)
        target.mkdir(parents=True)
        steam = self.root / 'Steam'
        pair = steam / 'steamrtarm64'
        pair.mkdir(parents=True)
        native = bytearray(20)
        native[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', native, 18, 183)
        for path in (pair / 'libEGL.so', pair / 'libGLESv2.so', pair / 'libvulkan.so', target / 'libvulkan_panfrost.so'):
            path.write_bytes(native)
        (target / 'panvk-local.json').write_text(json.dumps({'file_format_version': '1.0.1', 'ICD': {
            'library_path': './libvulkan_panfrost.so', 'api_version': '1.3.354', 'library_arch': '64'}}))
        (target / 'probe_steam_angle.py').write_bytes((ROOT / 'tools/mali-vulkan/probe_steam_angle.py').read_bytes())
        config = self.root / 'config.json'
        config.write_text(json.dumps({'version': 1, 'atom_stride': '56', 'target': str(target),
            'steam_root': str(steam), 'angle_dir': str(pair),
            'probe_sha256': client.digest(target / 'probe_steam_angle.py'),
            'driver_sha256': client.digest(target / 'libvulkan_panfrost.so')}))
        env = dict(BL_INSIDE='1', BL_MALI_STEAM_UI='1', DISPLAY=':42',
            LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe', MESA_LOADER_DRIVER_OVERRIDE='swrast',
            PANVK_ATOM_STRIDE='64', VK_DRIVER_FILES='/old/proxy.json', LD_PRELOAD='/old/inject.so')
        return config, steam, target, env

    def test_reuses_proven_environment_and_only_changes_the_client_copy(self):
        config, steam, target, parent = self.fixture()
        before = dict(parent)
        child, args, loader = client.inputs(config, steam, parent, target.parent)
        self.assertEqual(parent, before)
        self.assertEqual(child['PANVK_ATOM_STRIDE'], '56')
        self.assertEqual(child['ANGLE_DEFAULT_PLATFORM'], 'vulkan')
        self.assertEqual(child['VK_DRIVER_FILES'], str(target / 'panvk-local.json'))
        self.assertEqual(child['VK_ICD_FILENAMES'], child['VK_DRIVER_FILES'])
        self.assertEqual(child['LD_LIBRARY_PATH'].split(':')[:2], [str(target), str(steam / 'steamrtarm64')])
        self.assertEqual(loader, steam / 'steamrtarm64/libvulkan.so')
        for key in ('LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER', 'MESA_LOADER_DRIVER_OVERRIDE', 'LD_PRELOAD'):
            self.assertNotIn(key, child)
        self.assertEqual(child['PANVK_DXVK_COMPAT'], '0')
        self.assertEqual(child['PANVK_KBASE_DRI3'], '0')

    def test_compositor_wrong_mode_changed_driver_and_wrong_stride_are_rejected(self):
        config, steam, target, env = self.fixture()
        for changes in ({'BL_INSIDE': '0'}, {'BL_MALI_STEAM_UI': '0'},
                        {'MALI_VULKAN_NORMAL_SESSION': '1'}, {'MALI_VULKAN_BROKER_SOCKET': '/broker'}):
            with self.assertRaises(RuntimeError):
                client.inputs(config, steam, env | changes, target.parent)
        data = json.loads(config.read_text())
        config.write_text(json.dumps(data | {'atom_stride': '64'}))
        with self.assertRaisesRegex(RuntimeError, 'unsupported'):
            client.inputs(config, steam, env, target.parent)
        config.write_text(json.dumps(data))
        (target / 'libvulkan_panfrost.so').write_bytes(b'changed driver')
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            client.inputs(config, steam, env, target.parent)

    def test_cli_passes_vulkan_flags_and_environment_to_exec_without_changing_parent(self):
        config, steam, target, parent = self.fixture()
        binary = steam / 'steamrtarm64/steam'
        binary.touch()
        prepared = client.inputs(config, steam, parent, target.parent)
        command = [str(binary), '-gamepadui', '-cef-use-gl=angle', '-cef-use-angle=vulkan']
        with patch('sys.argv', [str(LAUNCHER), 'launch', '--config', str(config), '--steam-root', str(steam), '--', *command]), \
             patch.object(client, 'inputs', return_value=prepared), patch.object(client.os, 'execve') as execute, \
             patch.dict(os.environ, parent, clear=True), contextlib.redirect_stdout(io.StringIO()):
            before = dict(os.environ)
            client.main()
            self.assertEqual(dict(os.environ), before)
        self.assertEqual(execute.call_args.args[:2], (str(binary), command))
        self.assertEqual(execute.call_args.args[2]['PANVK_ATOM_STRIDE'], '56')

    def process(self, proc, pid, argv, parent, env=None):
        path = proc / str(pid)
        path.mkdir(parents=True)
        (path / 'cmdline').write_bytes('\0'.join(argv).encode())
        (path / 'status').write_text(f'PPid:\t{parent}\n')
        (path / 'environ').write_bytes('\0'.join(f'{key}={value}' for key, value in (env or {}).items()).encode())
        (path / 'maps').write_text('1000-2000 r-xp 0000 08:01 7 /root/panvk-test/libvulkan_panfrost.so\n')

    def test_observation_reports_real_helper_even_if_steam_drops_overrides_and_ignores_old_logs(self):
        proc = self.root / 'proc'
        self.process(proc, 100, ['/Steam/steam'], 1)
        self.process(proc, 101, ['/Steam/steamwebhelper', '--type=gpu-process', '--use-angle=vulkan'], 100)
        self.process(proc, 102, ['/Steam/steamwebhelper'], 1)
        self.assertIsNone(client.process_evidence(102, 100, proc))
        evidence = client.process_evidence(101, 100, proc)
        self.assertEqual(evidence[1]['PANVK_ATOM_STRIDE'], '<unset>')
        steam = self.root / 'Steam'
        logs = steam / 'logs'
        logs.mkdir(parents=True)
        renderer = logs / 'webhelper_gpu.txt'
        renderer.write_text('old GL_RENDERER=llvmpipe\n')
        offsets = client.log_offsets(steam)
        with renderer.open('a') as stream:
            stream.write('GL_RENDERER=ANGLE (ARM, Vulkan Mali-G52, panvk)\n')
        output = io.StringIO()
        with patch.object(client.time, 'monotonic', side_effect=[0, 0, 2]), \
             patch.object(client.time, 'sleep'), contextlib.redirect_stdout(output):
            client.observe(100, steam, offsets, proc, duration=1)
        text = output.getvalue()
        self.assertIn('actually started: pid=101', text)
        self.assertIn('--use-angle=vulkan', text)
        self.assertIn('new renderer/backend log evidence (webhelper_gpu.txt): GL_RENDERER=ANGLE', text)
        self.assertNotIn('old GL_RENDERER', text)
        self.assertNotIn('pid=102', text)
        self.assertNotIn('PASS', text)

    def test_no_helper_no_renderer_does_not_claim_ui_acceleration(self):
        proc = self.root / 'proc'
        self.process(proc, 100, ['/Steam/steam'], 1)
        output = io.StringIO()
        with patch.object(client.time, 'monotonic', side_effect=[0, 0, 2]), \
             patch.object(client.time, 'sleep'), contextlib.redirect_stdout(output):
            client.observe(100, self.root / 'Steam', {}, proc, duration=1)
        self.assertIn('not observed', output.getvalue())
        self.assertIn('UI acceleration UNPROVEN', output.getvalue())


if __name__ == '__main__':
    unittest.main()
