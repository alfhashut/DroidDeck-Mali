"""Cheap probe failure/isolation tests; no CDLL, Vulkan, X11, build, or download."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import probe_steam_angle as probe
import run_panvk_phone_test as phone


class SteamAngleProbeTests(unittest.TestCase):
    def test_client_configuration_stages_raw_inputs_and_adb_can_disable_the_guest_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.fixture(directory)
            steam = Path(args.steam_root)
            pair = Path(args.egl).parent
            (pair / 'libvulkan.so').write_bytes(Path(args.egl).read_bytes())
            artifact = root / 'artifact'
            artifact.mkdir()
            (artifact / phone.DRIVER).write_bytes(Path(args.panvk_library).read_bytes())
            (artifact / phone.MANIFEST).write_bytes((Path(__file__).resolve().parents[1] /
                'panvk/tests/fixtures/panfrost_icd.aarch64.json').read_bytes())
            shared = root / 'shared'
            shared.mkdir()
            original_stage = phone.stage
            original_is_file = Path.is_file
            client = {'DISPLAY': ':42', 'PANVK_ATOM_STRIDE': '64', 'LIBGL_ALWAYS_SOFTWARE': '1'}
            with patch.object(phone, 'client_environment', return_value=client), \
                 patch.object(phone, 'stage', side_effect=lambda path: original_stage(path, root / 'panvk-test')), \
                 patch.object(phone, '__file__', str(shared / 'run_panvk_phone_test.py')), \
                 patch.object(Path, 'home', return_value=root), \
                 patch.object(Path, 'is_file', lambda path: True if str(path) == '/usr/local/bin/droiddeck-steam-client-panvk' else original_is_file(path)), \
                 patch.object(phone.subprocess, 'run') as invoke, contextlib.redirect_stdout(io.StringIO()) as output:
                phone.configure_steam_client(artifact, steam, pair)
            self.assertFalse(invoke.called)
            pointer = root / '.config/droiddeck/steam-client-panvk.json'
            self.assertTrue(pointer.is_symlink())
            config = json.loads(pointer.read_text())
            self.assertEqual(config['atom_stride'], '56')
            self.assertEqual(config['driver_sha256'], phone.digest(Path(config['target']) / phone.DRIVER))
            self.assertIn('real Steam UI acceleration UNPROVEN', output.getvalue())
            self.assertNotIn('CLIENT-ONLY PROBE PASS', output.getvalue())
            (shared / pointer.name).unlink()
            self.assertFalse(pointer.is_file())
            self.assertEqual(client['PANVK_ATOM_STRIDE'], '64')

    def test_atom_stride_override_is_explicit_and_only_in_prepared_child_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            parent = {'DISPLAY': ':7', 'PANVK_ATOM_STRIDE': '64'}
            for stride in ('auto', '56', '64'):
                args.atom_stride = stride
                with patch.dict(os.environ, parent, clear=True):
                    child = probe.prepare(args)
                    self.assertEqual(dict(os.environ), parent)
                self.assertEqual(child.get('PANVK_ATOM_STRIDE'), None if stride == 'auto' else stride)
            args.atom_stride = '60'
            with self.assertRaisesRegex(RuntimeError, 'invalid atom stride'):
                probe.prepare(args, environment=parent)

    def test_phone_stride_request_preserves_faults_warnings_and_timeout_output(self):
        warning = 'MESA: warning: kbase: KBASE_IOCTL_MEM_JIT_INIT failed: Cannot allocate memory\n'
        fault = '[KFAULT] atom=0 code=0x40 core_req=0x0\n'
        for stride, log in (('56', warning + fault), ('64', 'kbase: atom stride forced to 64\n' + warning + fault)):
            for timeout in (False, True):
                output = io.StringIO()
                result = (subprocess.TimeoutExpired('probe', 40, output=log.encode()) if timeout else
                          subprocess.CompletedProcess([], 1, log))
                with patch.object(phone, 'client_environment', return_value={'DISPLAY': ':42'}), \
                     patch.object(phone, 'stage', return_value=Path('/test')), \
                     patch.object(phone, 'angle_pair', return_value=Path('/Steam')), \
                     patch.object(phone.probe, 'prepare', return_value={'DISPLAY': ':42'}), \
                     patch.object(phone.subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 0), result]) as invoke, \
                     contextlib.redirect_stdout(output):
                    with self.assertRaises((RuntimeError, subprocess.TimeoutExpired)):
                        phone.run(Path('/artifact'), Path('/Steam'), atom_stride=stride)
                self.assertEqual(invoke.call_args.args[0][-2:], ['--atom-stride', stride])
                for call in invoke.call_args_list:
                    self.assertNotIn('PANVK_ATOM_STRIDE', call.kwargs['env'])
                logged = output.getvalue()
                self.assertIn(warning, logged)
                self.assertIn(fault, logged)
                self.assertIn('code=0x40 (JOB_CONFIG_FAULT) core_req=0x0', logged)
                self.assertIn('not exposed in probe output' if stride == '56' else 'atom stride forced to 64', logged)
                self.assertNotIn('CLIENT-ONLY PROBE PASS', logged)

    def maps_line(self, path, shown=None, identity=None):
        if identity is None:
            try:
                identity = probe.file_identity(Path(path))
            except OSError:
                identity = (0, 0, 0)
        major, minor, inode = identity
        return f'1000-2000 r-xp 0000 {major:x}:{minor:x} {inode} {shown or path}'

    def test_mapping_identity_accepts_steam_symlinks_and_proot_host_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            egl = Path(args.egl)
            target = egl.with_name('libANGLE_egl_target.so')
            egl.rename(target)
            egl.symlink_to(target.name)
            for host_paths in (False, True):
                rows = [self.maps_line(path, '/unreachable/android/linuxfs' + str(Path(path).resolve())
                                       if host_paths else str(Path(path).resolve()))
                        for path in (args.egl, args.gles, args.panvk_library)]
                rows.append(self.maps_line('/usr/lib/libvulkan.so.1', identity=(8, 1, 999)))
                # The same loader through a proot host alias is not a second loader.
                rows.append(self.maps_line('/android/linuxfs/usr/lib/libvulkan.so.1', identity=(8, 1, 999)))
                output = io.StringIO()
                with patch.object(Path, 'read_text', return_value='\n'.join(rows)), contextlib.redirect_stdout(output):
                    probe.verify_loaded_libraries(args)
                self.assertIn('match=device/inode', output.getvalue())
                self.assertIn('expected=' + str(target), output.getvalue())
                self.assertIn('PanVK library=' + args.panvk_library, output.getvalue())

    def test_mapping_mismatched_inode_or_device_never_falls_back_to_same_path(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            major, minor, inode = probe.file_identity(Path(args.egl))
            for wrong in ((major, minor, inode + 1), (major + 1, minor, inode)):
                rows = [self.maps_line(args.egl, identity=wrong), self.maps_line(args.gles),
                        self.maps_line(args.panvk_library), self.maps_line('/usr/lib/libvulkan.so.1')]
                output = io.StringIO()
                with patch.object(Path, 'read_text', return_value='\n'.join(rows)), contextlib.redirect_stdout(output):
                    with self.assertRaisesRegex(RuntimeError, 'required library not mapped'):
                        probe.verify_loaded_libraries(args)
                self.assertIn('relevant /proc/self/maps lines:', output.getvalue())
                self.assertIn(rows[0], output.getvalue())
                self.assertIn(rows[1], output.getvalue())
                self.assertIn('realpath=' + args.egl, output.getvalue())
                self.assertIn('device/inode=', output.getvalue())

    def test_mapping_realpath_fallback_only_when_kernel_identity_is_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            rows = [self.maps_line(path, identity=(0, 0, 0))
                    for path in (args.egl, args.gles, args.panvk_library, '/usr/lib/libvulkan.so.1')]
            output = io.StringIO()
            with patch.object(Path, 'read_text', return_value='\n'.join(rows)), contextlib.redirect_stdout(output):
                probe.verify_loaded_libraries(args)
            self.assertIn('match=realpath', output.getvalue())

    def test_mapping_rejects_unrelated_system_or_software_egl_even_with_expected_pair_loaded(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            rows = [self.maps_line(path) for path in
                    (args.egl, args.gles, args.panvk_library, '/usr/lib/libvulkan.so.1')]
            for other in ('/system/lib64/libEGL.so', '/usr/lib/libEGL_mesa.so',
                          '/root/Steam/swiftshader/libEGL.so', '/usr/lib/swrast_dri.so'):
                output = io.StringIO()
                with patch.object(Path, 'read_text', return_value='\n'.join(rows + [self.maps_line(other)])), \
                     contextlib.redirect_stdout(output):
                    with self.assertRaisesRegex(RuntimeError, 'unrelated EGL/GLES|software graphics'):
                        probe.verify_loaded_libraries(args)
                self.assertIn(other, output.getvalue())

    def test_phone_angle_pair_discovery_fails_on_ambiguity_instead_of_guessing(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            steam = Path(args.steam_root)
            pair = Path(args.egl).parent
            self.assertEqual(phone.angle_pair(steam), pair)
            other = pair / 'other-angle'
            other.mkdir()
            (other / 'libEGL.so').write_bytes(Path(args.egl).read_bytes())
            (other / 'libGLESv2.so').write_bytes(Path(args.gles).read_bytes())
            with self.assertRaisesRegex(RuntimeError, '--angle-dir'):
                phone.angle_pair(steam)
            self.assertEqual(phone.angle_pair(steam, other), other)

    def test_phone_staging_keeps_raw_manifest_and_never_overwrites_a_live_library(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = self.fixture(directory)
            artifact = root / 'raw'
            artifact.mkdir()
            raw_manifest = Path(__file__).resolve().parents[1] / 'panvk/tests/fixtures/panfrost_icd.aarch64.json'
            (artifact / phone.MANIFEST).write_bytes(raw_manifest.read_bytes())
            (artifact / phone.DRIVER).write_bytes(Path(args.panvk_library).read_bytes())
            target = phone.stage(artifact, root / 'guest')
            self.assertEqual((target / phone.MANIFEST).read_bytes(), raw_manifest.read_bytes())
            expected = json.loads(raw_manifest.read_text())
            expected['ICD']['library_path'] = './' + phone.DRIVER
            self.assertEqual(json.loads((target / 'panvk-local.json').read_text()), expected)
            self.assertEqual(phone.stage(artifact, root / 'guest'), target)
            old_driver = (target / phone.DRIVER).read_bytes()
            (artifact / phone.DRIVER).write_bytes(old_driver + b'new build')
            replacement = phone.stage(artifact, root / 'guest')
            self.assertNotEqual(target, replacement)
            self.assertEqual((target / phone.DRIVER).read_bytes(), old_driver)
            expected['ICD']['library_path'] = 'libGLES_mali.so'
            (artifact / phone.MANIFEST).write_text(json.dumps(expected))
            with self.assertRaisesRegex(RuntimeError, 'does not identify'):
                phone.stage(artifact, root / 'guest')

    def test_phone_discovers_real_client_and_rejects_compositor_or_ambiguous_displays(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = Path(directory)
            def process(pid, env):
                path = proc / str(pid)
                path.mkdir()
                (path / 'cmdline').write_bytes(b'/bin/bash\0/usr/local/bin/droiddeck-session\0steam\0')
                (path / 'environ').write_bytes('\0'.join(f'{k}={v}' for k, v in env.items()).encode())
            client = dict(BL_INSIDE='1', BL_MALI_STEAM_UI='1', DISPLAY=':42', XDG_RUNTIME_DIR='/run/droiddeck')
            process(100, dict(client, MALI_VULKAN_NORMAL_SESSION='1', MALI_VULKAN_BROKER_SOCKET='/broker.sock'))
            with self.assertRaisesRegex(RuntimeError, 'no guessed DISPLAY'):
                phone.client_environment(proc)
            process(101, client)
            self.assertEqual(phone.client_environment(proc), client)
            process(102, dict(client, DISPLAY=':43'))
            with self.assertRaisesRegex(RuntimeError, 'readable client displays=2'):
                phone.client_environment(proc)

    def test_phone_pass_requires_probe_success_and_every_evidence_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            target = Path(directory)
            target_icd = target / 'panvk-local.json'
            target_icd.write_bytes(Path(args.icd).read_bytes())
            Path(args.panvk_library).rename(target / phone.DRIVER)
            manifest = json.loads(target_icd.read_text())
            manifest['ICD']['library_path'] = './' + phone.DRIVER
            target_icd.write_text(json.dumps(manifest))
            pair = Path(args.egl).parent
            client = {'DISPLAY': ':42', 'LIBGL_ALWAYS_SOFTWARE': '1'}
            success = (f'ICD={target_icd}\nPanVK library={target / phone.DRIVER}\nVulkan loader=/usr/lib/libvulkan.so.1\n'
                       'Vulkan device=Mali-G52 (PanVK) vendor=0x13b5\n'
                       'ANGLE backend=Vulkan; software renderer=none (validated)\n'
                       'GPU triangle readback: verified\nX11 CPU-copy pixels: verified\n'
                       'probe render/copy/cleanup completed\nprobe exit=0\n')
            for code, stdout in ((0, success), (1, success), (0, success.replace('GPU triangle readback: verified', 'readback failed'))):
                output = io.StringIO()
                with patch.object(phone, 'client_environment', return_value=client), \
                     patch.object(phone, 'stage', return_value=target), \
                     patch.object(phone, 'angle_pair', return_value=pair), \
                     patch.object(phone.subprocess, 'run', side_effect=[subprocess.CompletedProcess([], 0),
                         subprocess.CompletedProcess([], code, stdout)]) as invoke, contextlib.redirect_stdout(output):
                    if code or 'readback failed' in stdout:
                        with self.assertRaises(RuntimeError):
                            phone.run(target, Path(args.steam_root))
                        self.assertNotIn('CLIENT-ONLY PROBE PASS', output.getvalue())
                    else:
                        phone.run(target, Path(args.steam_root))
                        self.assertIn('CLIENT-ONLY PROBE PASS', output.getvalue())
                    for call in invoke.call_args_list:
                        env = call.kwargs['env']
                        self.assertEqual(env['DISPLAY'], ':42')
                        self.assertEqual(env['VK_DRIVER_FILES'], str(target_icd))
                        self.assertNotIn('LIBGL_ALWAYS_SOFTWARE', env)
                self.assertEqual(client, {'DISPLAY': ':42', 'LIBGL_ALWAYS_SOFTWARE': '1'})

    def test_phone_dependency_failure_never_launches_probe_or_reports_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            target = Path(directory)
            output = io.StringIO()
            with patch.object(phone, 'client_environment', return_value={}), \
                 patch.object(phone, 'stage', return_value=target), \
                 patch.object(phone, 'angle_pair', return_value=Path(args.egl).parent), \
                 patch.object(phone.probe, 'prepare', return_value={'DISPLAY': ':42'}), \
                 patch.object(phone.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'preflight')) as invoke, \
                 contextlib.redirect_stdout(output):
                with self.assertRaises(subprocess.CalledProcessError):
                    phone.run(target, Path(args.steam_root))
                self.assertEqual(invoke.call_count, 1)
                self.assertNotIn('CLIENT-ONLY PROBE PASS', output.getvalue())

    def test_phone_main_reports_exact_missing_dependency_and_nonzero_failure(self):
        output = io.StringIO()
        with patch('sys.argv', ['run_panvk_phone_test.py']), \
             patch.object(phone, 'run', side_effect=OSError('libdrm.so.2: cannot open shared object file')), \
             contextlib.redirect_stderr(output):
            self.assertEqual(phone.main(), 1)
        self.assertIn('CLIENT-ONLY PROBE FAIL', output.getvalue())
        self.assertIn('libdrm.so.2', output.getvalue())
        self.assertNotIn('CLIENT-ONLY PROBE PASS', output.getvalue())

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
            maps = '\n'.join(self.maps_line(path) for path in (args.egl, args.gles))
            with patch.object(Path, 'read_text', return_value=maps), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, 'required library not mapped'):
                    probe.verify_loaded_libraries(args)

    def test_raw_mesa_101_manifest_works_after_library_path_only_relocation(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            fixture = Path(__file__).resolve().parents[1] / 'panvk/tests/fixtures/panfrost_icd.aarch64.json'
            manifest = json.loads(fixture.read_text())
            driver = Path(args.panvk_library).with_name('libvulkan_panfrost.so')
            Path(args.panvk_library).rename(driver)
            args.panvk_library = str(driver)
            manifest['ICD']['library_path'] = './' + driver.name
            icd = Path(args.icd)
            icd.write_text(json.dumps(manifest))
            with patch.dict(os.environ, {'DISPLAY': ':7'}, clear=True):
                child = probe.prepare(args)
                self.assertEqual(child['VK_DRIVER_FILES'], str(icd))
                self.assertEqual(child['LD_LIBRARY_PATH'].split(':')[0], str(driver.parent))
                for arch in ('32', 64, None):
                    manifest['ICD']['library_arch'] = arch
                    icd.write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(RuntimeError, 'architecture'):
                        probe.prepare(args)
                manifest['ICD']['library_arch'] = '64'
                manifest['file_format_version'] = '1.0.2'
                icd.write_text(json.dumps(manifest))
                with self.assertRaisesRegex(RuntimeError, 'ICD format'):
                    probe.prepare(args)

    def test_loader_path_is_reported_and_ambiguous_loaders_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            args = self.fixture(directory)
            libraries = [args.egl, args.gles, args.panvk_library, '/usr/lib/libvulkan.so.1.4.357']
            maps = '\n'.join(self.maps_line(p) for p in libraries)
            output = io.StringIO()
            with patch.object(Path, 'read_text', return_value=maps), contextlib.redirect_stdout(output):
                probe.verify_loaded_libraries(args)
            self.assertIn('Vulkan loader=/usr/lib/libvulkan.so.1.4.357', output.getvalue())
            self.assertIn('PanVK library=' + args.panvk_library, output.getvalue())
            maps += '\n' + self.maps_line('/another/libvulkan.so.1')
            with patch.object(Path, 'read_text', return_value=maps), contextlib.redirect_stdout(io.StringIO()):
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
