"""Cheap probe failure/isolation tests; no CDLL, Vulkan, X11, build, or download."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import probe_steam_angle as probe


class SteamAngleProbeTests(unittest.TestCase):
    def test_software_wrong_backend_and_wrong_bundle_never_pass(self):
        hardware = 'ANGLE (Mesa, Vulkan 1.3 (Mali-G52 (PanVK)), panvk)'
        version = 'OpenGL ES 2.0 (ANGLE git hash: 5d4df51d1d7d)'
        probe.validate_renderer('Google Inc. (Mesa)', hardware, version)
        for renderer, actual_version in (
                ('ANGLE (Mesa, llvmpipe)', version),
                (hardware + ' LLVMpipe', version),
                (hardware + ' softpipe', version),
                (hardware + ' swrast', version),
                ('ANGLE (SwiftShader, Vulkan)', version),
                ('ANGLE (Mesa, OpenGL Mali-G52 PanVK)', version),
                ('ANGLE (Mesa, Vulkan Mali-G57 PanVK)', version),
                (hardware, 'ANGLE git hash: other')):
            with self.subTest(renderer=renderer, version=actual_version):
                with self.assertRaises(RuntimeError):
                    probe.validate_renderer('Google Inc.', renderer, actual_version)

    def test_stale_truncated_or_wrong_gpu_readback_never_passes(self):
        data = bytearray([0, 0, 0, 255] * probe.SIZE * probe.SIZE)
        center = (probe.SIZE // 2 * probe.SIZE + probe.SIZE // 2) * 4
        data[center:center + 4] = bytes((255, 0, 0, 255))
        probe.validate_pixels(data)
        for failed in (data[:-1], bytes(len(data)),
                       data[:center] + bytes((0, 255, 0, 255)) + data[center + 4:]):
            with self.assertRaises(RuntimeError):
                probe.validate_pixels(failed)

    def fixture(self, directory):
        root = Path(directory)
        pair = root / 'steam/steamrtarm64'
        pair.mkdir(parents=True)
        native = bytearray(20)
        native[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', native, 18, 183)
        for name in ('libEGL.so', 'libGLESv2.so'):
            (pair / name).write_bytes(native)
        driver = root / 'libvulkan_panfrost_kbase.so'
        driver.write_bytes(native)
        icd = root / 'panvk.json'
        icd.write_text(json.dumps({'file_format_version': '1.0.0', 'ICD': {
            'api_version': '1.3.0', 'library_path': driver.name}}))
        return argparse.Namespace(steam_root=str(root / 'steam'), egl=str(pair / 'libEGL.so'),
            gles=str(pair / 'libGLESv2.so'), icd=str(icd), panvk_library=str(driver))

    def test_child_environment_isolated_and_no_compatibility_or_version_spoof(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            parent = {'DISPLAY': ':7', 'VK_DRIVER_FILES': '/compositor/proxy.json',
                      'VK_ICD_FILENAMES': '/compositor/proxy.json', 'PANVK_DXVK_COMPAT': '1',
                      'MESA_VK_VERSION_OVERRIDE': '1.4', 'LD_PRELOAD': '/interposer.so',
                      'ANGLE_FEATURE_OVERRIDES_ENABLED': 'exposeNonConformantExtensionsAndVersions',
                      'VK_INSTANCE_LAYERS': 'VK_LAYER_FAKE', 'VK_LOADER_DRIVERS_SELECT': '*swift*',
                      'pan_force_enable_shader_atomics': 'true', 'force_vk_devicename': 'fake GPU',
                      'LIBGL_ALWAYS_SOFTWARE': '1', 'GALLIUM_DRIVER': 'llvmpipe',
                      'MESA_LOADER_DRIVER_OVERRIDE': 'swrast', 'BL_VK_DRIVER': '/turnip.json'}
            with patch.dict(os.environ, parent, clear=True):
                before = dict(os.environ)
                child = probe.prepare(args)
                self.assertEqual(dict(os.environ), before)
            self.assertEqual(child['VK_DRIVER_FILES'], args.icd)
            self.assertEqual(child['VK_ICD_FILENAMES'], args.icd)
            self.assertEqual(child['PANVK_DXVK_COMPAT'], '0')
            self.assertEqual(child['PANVK_KBASE_DRI3'], '0')
            self.assertEqual(child['pan_force_enable_shader_atomics'], 'false')
            self.assertEqual(child['pan_enable_vertex_pipeline_stores_atomics'], 'false')
            self.assertEqual(child['DISPLAY'], ':7')
            self.assertEqual(child['LD_LIBRARY_PATH'].split(':')[0], str(Path(args.panvk_library).parent))
            for key in ('MESA_VK_VERSION_OVERRIDE', 'LIBGL_ALWAYS_SOFTWARE', 'GALLIUM_DRIVER',
                        'MESA_LOADER_DRIVER_OVERRIDE', 'LD_PRELOAD', 'BL_VK_DRIVER',
                        'ANGLE_FEATURE_OVERRIDES_ENABLED', 'VK_INSTANCE_LAYERS',
                        'VK_LOADER_DRIVERS_SELECT', 'force_vk_devicename'):
                self.assertNotIn(key, child)

    def test_unloaded_explicit_driver_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            maps = '\n'.join(f'1-2 r-xp 0000 00:00 1 {path}' for path in (args.egl, args.gles))
            with patch.object(Path, 'read_text', return_value=maps):
                with self.assertRaisesRegex(RuntimeError, 'required library not mapped'):
                    probe.verify_loaded_libraries(args)

    def test_loader_path_is_reported_and_ambiguous_loaders_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            libraries = [args.egl, args.gles, args.panvk_library, '/usr/lib/libvulkan.so.1.4.357']
            maps = '\n'.join(f'1-2 r-xp 0000 00:00 1 {p}' for p in libraries)
            output = io.StringIO()
            with patch.object(Path, 'read_text', return_value=maps), contextlib.redirect_stdout(output):
                probe.verify_loaded_libraries(args)
            self.assertIn('Vulkan loader=/usr/lib/libvulkan.so.1.4.357', output.getvalue())
            self.assertIn('PanVK library=' + args.panvk_library, output.getvalue())
            maps += '\n1-2 r-xp 0000 00:00 1 /another/libvulkan.so.1'
            with patch.object(Path, 'read_text', return_value=maps):
                with self.assertRaisesRegex(RuntimeError, 'exactly one mapped Vulkan loader'):
                    probe.verify_loaded_libraries(args)

    def test_compositor_environment_is_rejected(self):
        for key in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET'):
            with patch.dict(os.environ, {key: '1'}, clear=True):
                with self.assertRaisesRegex(RuntimeError, 'CLIENT environment'):
                    probe.prepare(argparse.Namespace())

    def test_manifest_identity_wrong_arch_and_system_egl_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            with patch.dict(os.environ, {'DISPLAY': ':7'}, clear=True):
                icd = Path(args.icd)
                manifest = json.loads(icd.read_text())
                manifest['ICD']['library_path'] = args.egl
                icd.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(RuntimeError, 'explicit PanVK'):
                    probe.prepare(args)
                manifest['ICD']['library_path'] = args.panvk_library
                icd.write_text(json.dumps(manifest))
                Path(args.panvk_library).write_bytes(b'not an AArch64 driver')
                with self.assertRaisesRegex(RuntimeError, 'AArch64 ELF'):
                    probe.prepare(args)
                args.egl = args.panvk_library  # Outside the installed Steam tree.
                with self.assertRaisesRegex(RuntimeError, 'paired libraries'):
                    probe.prepare(args)


if __name__ == '__main__':
    unittest.main()
