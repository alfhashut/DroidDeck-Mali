"""B0 host checks: synthetic ELF/metadata only; no compiler, loader, GPU or network."""
import importlib.util
import contextlib
import json
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import tarfile
import unittest
import zipfile
import xml.etree.ElementTree as ET
from unittest.mock import patch

import package_glibc as artifact
import stage_raw_test as raw
import stage_phone_test as phone

ROOT = Path(__file__).resolve().parents[2]


class ArtifactTests(unittest.TestCase):
    def test_phone_adb_trigger_is_shell_protected_and_not_implicitly_started(self):
        namespace = '{http://schemas.android.com/apk/res/android}'
        manifest = ET.parse(ROOT / 'app/src/main/AndroidManifest.xml')
        receiver = next(item for item in manifest.findall('./application/receiver')
                        if item.get(namespace + 'name') == '.gpu.PanvkClientProbeReceiver')
        self.assertEqual(receiver.get(namespace + 'exported'), 'true')
        self.assertEqual(receiver.get(namespace + 'permission'), 'android.permission.DUMP')
        self.assertFalse(receiver.findall('intent-filter'))

    def test_phone_adb_run_requires_matching_request_completion_and_probe_pass(self):
        for exit_code, proof in ((0, 'C8B.1 PanVK CLIENT-ONLY PROBE PASS'),
                                 (1, 'C8B.1 PanVK CLIENT-ONLY PROBE PASS'),
                                 (0, 'probe failed')):
            output = f'{proof}\nDROIDDECK_PANVK_PROBE_DONE exit={exit_code}\n'
            with patch.object(phone.subprocess, 'run', side_effect=[
                    subprocess.CompletedProcess([], 0, 'Broadcast completed: result=1', ''),
                    subprocess.CompletedProcess([], 0, output, '')]) as invoke, contextlib.redirect_stdout(io.StringIO()) as logged:
                self.assertEqual(phone.run_phone(['adb', '-s', 'test-device'], '/storage/emulated/0'),
                                 0 if exit_code == 0 and proof.endswith('PASS') else 1)
                self.assertIn('DroidDeck PanVK phone test ' + ('PASS' if exit_code == 0 and proof.endswith('PASS') else 'FAIL'),
                              logged.getvalue())
            request_command = invoke.call_args_list[0].args[0][-1]
            request = request_command.split()[-1]
            self.assertEqual(len(request), 32)
            self.assertIn('panvk-client-probe-' + request + '.log', invoke.call_args_list[1].args[0][-1])

    def test_phone_adb_run_rejects_old_apk_and_times_out_without_stale_pass(self):
        with patch.object(phone.subprocess, 'run', return_value=
                          subprocess.CompletedProcess([], 0, 'Broadcast completed: result=0', '')):
            with self.assertRaisesRegex(RuntimeError, 'CI APK'):
                phone.run_phone(['adb'], '/storage/emulated/0')
        with patch.object(phone.subprocess, 'run', side_effect=[
                subprocess.CompletedProcess([], 0, 'Broadcast completed: result=1', ''),
                subprocess.CompletedProcess([], 0, 'unfinished output', '')]), \
             patch.object(phone.time, 'monotonic', side_effect=[0, 0, 100]), \
             patch.object(phone.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'timed out waiting'):
                phone.run_phone(['adb'], '/storage/emulated/0')

    def test_phone_transfer_accepts_actions_zip_tar_and_directory_without_extra_dsos(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw_files = root / 'raw'
            raw_files.mkdir()
            for name in phone.FILES[:4]:
                (raw_files / name).write_text(name + '\n')
            (raw_files / 'libc.so.6').write_text('not copied')
            archive = root / 'raw.tar.gz'
            with tarfile.open(archive, 'w:gz') as tar:
                tar.add(raw_files, arcname='droiddeck-panvk-raw-test-arm64')
            download = root / 'download.zip'
            with zipfile.ZipFile(download, 'w') as zipped:
                zipped.write(archive, archive.name)
            for index, source in enumerate((raw_files, archive, download)):
                target = root / f'extracted{index}'
                phone.unpack(source, target)
                self.assertEqual({p.name for p in target.iterdir()}, set(phone.FILES[:4]))
                for name in phone.FILES[:4]:
                    self.assertEqual((target / name).read_bytes(), (raw_files / name).read_bytes())

    def test_phone_transfer_rejects_duplicate_link_and_missing_members(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, kind in enumerate(('duplicate', 'link', 'missing')):
                archive = root / f'{kind}.tar.gz'
                with tarfile.open(archive, 'w:gz') as tar:
                    if kind == 'missing':
                        continue
                    for repeat in range(2 if kind == 'duplicate' else 1):
                        entry = tarfile.TarInfo(f'../../{repeat}/{phone.FILES[0]}')
                        if kind == 'link':
                            entry.type = tarfile.SYMTYPE
                            entry.linkname = '/some/other/library'
                            tar.addfile(entry)
                        else:
                            entry.size = 3
                            tar.addfile(entry, io.BytesIO(b'ELF'))
                with self.assertRaisesRegex(RuntimeError, 'duplicate|nonregular|missing'):
                    phone.unpack(archive, root / f'out{index}')

    def test_raw_test_copies_only_explicit_outputs_and_keeps_exact_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, build, output = root / 'source', root / 'build', root / 'raw-test'
            (source / 'docs').mkdir(parents=True)
            (source / 'docs/license.rst').write_text('Mesa license fixture')
            (source / 'VERSION').write_text('26.3.0-devel\n')
            generated = build / 'src/panfrost/vulkan'
            generated.mkdir(parents=True)
            self.elf(generated / raw.DRIVER)
            manifest = (artifact.TOOLS / 'tests/fixtures/panfrost_icd.aarch64.json').read_bytes()
            (generated / raw.MANIFEST).write_bytes(manifest)
            for name in ('libGLES_mali.so', 'libc.so.6', 'libvulkan.so.1', 'libdrm.so.2'):
                (generated / name).write_bytes(b'must not be copied')
            pin = json.loads((artifact.TOOLS / 'source.json').read_text())
            def command(*argv):
                if argv[-2:] == ('rev-parse', 'HEAD'):
                    return pin['commit'] + '\n'
                if argv[-2:] == ('diff', '--binary'):
                    return 'source patch fixture\n'
                if argv[:2] == ('readelf', '-dW'):
                    return '(NEEDED) Shared library: [libdrm.so.2]\n'
                if argv[:3] == ('readelf', '--version-info', '--wide'):
                    return 'Name: GLIBC_2.38\n'
                self.fail(f'unexpected command: {argv}')
            with patch.object(raw, 'command', side_effect=command):
                raw.stage(source, build, output)
                with self.assertRaises(FileExistsError):
                    raw.stage(source, build, output)
            self.assertEqual({p.name for p in output.iterdir()}, {
                raw.DRIVER, raw.MANIFEST, 'DEPENDENCIES.txt', 'SOURCE_DIFF.patch',
                'BUILD_INFO.txt', 'SHA256SUMS', 'Mesa-license.rst'})
            self.assertEqual((output / raw.MANIFEST).read_bytes(), manifest)
            self.assertEqual((output / raw.DRIVER).read_bytes(), (generated / raw.DRIVER).read_bytes())
            dependencies = (output / 'DEPENDENCIES.txt').read_text()
            self.assertIn('libdrm.so.2', dependencies)
            self.assertIn('GLIBC_2.38', dependencies)
            info = json.loads((output / 'BUILD_INFO.txt').read_text())
            self.assertIn('NOT PRODUCTION-READY', info['status'])
            self.assertEqual(info['source_commit'], pin['commit'])
            for line in (output / 'SHA256SUMS').read_text().splitlines():
                checksum, name = line.split('  ')
                self.assertEqual(checksum, raw.digest(output / name))

    def test_raw_test_rejects_wrong_source_before_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(raw, 'command', return_value='wrong commit\n'):
                with self.assertRaisesRegex(RuntimeError, 'pinned commit'):
                    raw.stage(root / 'source', root / 'build', root / 'out')
            self.assertFalse((root / 'out').exists())

    def elf(self, path, machine=183):
        data = bytearray(20)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<HH', data, 16, 3, machine)
        path.write_bytes(data)
        return path

    def test_elf_machine_soname_search_paths_and_required_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.elf(Path(directory) / artifact.DRIVER)
            dynamic = f'(SONAME) Library soname: [{artifact.DRIVER}]\n(NEEDED) Shared library: [libc.so.6]'
            versions = ('Version definition section\n Name: GLIBC_9.99\n'
                        'Version needs section\n Name: GLIBC_2.34\n')
            with patch.object(artifact, 'command', side_effect=[dynamic, versions]):
                info = artifact.inspect_elf(path)
            self.assertEqual(info['needed'], ['libc.so.6'])
            self.assertEqual(info['required_symbol_versions'], ['GLIBC_2.34'])
            with patch.object(artifact, 'command', return_value=dynamic + '\n(RUNPATH) [/tmp/build]'):
                with self.assertRaisesRegex(RuntimeError, 'nonportable'):
                    artifact.inspect_elf(path)
            self.elf(path, machine=62)
            with self.assertRaisesRegex(RuntimeError, 'AArch64'):
                artifact.inspect_elf(path)

    def test_dependency_closure_keeps_glibc_external_and_handles_cycles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in (artifact.DRIVER, 'libdrm.so.2', 'libz.so.1'):
                self.elf(root / name)
            graph = {artifact.DRIVER: ['libdrm.so.2', 'libc.so.6'],
                     'libdrm.so.2': ['libz.so.1'], 'libz.so.1': ['libdrm.so.2', 'libm.so.6']}
            def inspect(path):
                return {'soname': path.name, 'needed': graph[path.name], 'required_symbol_versions': []}
            with patch.object(artifact, 'inspect_elf', side_effect=inspect):
                files, system = artifact.runtime_closure(root / artifact.DRIVER, [root])
            self.assertEqual(set(files), set(graph))
            self.assertEqual(system, ['libc.so.6', 'libm.so.6'])
            self.assertNotIn('libc.so.6', files)

    def test_unreviewed_vendor_loader_or_software_dependencies_fail_closed(self):
        for name in ('libGLES_mali.so', 'liblog.so', 'libvulkan.so.1',
                     'libLLVM.so.19.1', 'libgallium.so', '../libdrm.so.2'):
            info = {'soname': artifact.DRIVER, 'needed': [name], 'required_symbol_versions': []}
            with self.subTest(name=name), patch.object(artifact, 'inspect_elf', return_value=info):
                with self.assertRaisesRegex(RuntimeError, 'unreviewed runtime dependency'):
                    artifact.runtime_closure(Path('/synthetic/driver'), [])

    def test_missing_escaping_wrong_soname_and_private_glibc_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'trusted'
            root.mkdir()
            with self.assertRaisesRegex(RuntimeError, 'unresolved'):
                artifact.resolve_dependency('libdrm.so.2', [root])
            outside = self.elf(Path(directory) / 'outside')
            (root / 'libdrm.so.2').symlink_to(outside)
            with self.assertRaisesRegex(RuntimeError, 'escapes'):
                artifact.resolve_dependency('libdrm.so.2', [root])
            for info, reason in (({'soname': 'wrong.so', 'needed': [], 'required_symbol_versions': []}, 'SONAME'),
                                 ({'soname': artifact.DRIVER, 'needed': [],
                                   'required_symbol_versions': ['GLIBC_PRIVATE']}, 'private glibc')):
                with patch.object(artifact, 'inspect_elf', return_value=info):
                    with self.assertRaisesRegex(RuntimeError, reason):
                        artifact.runtime_closure(outside, [root])

    def test_pinned_mesa_manifest_relocates_only_library_path(self):
        # Exact pinned vk_icd_gen.py output with the B0 Meson target arguments:
        # --api-version 1.4 --sizeof-pointer 8, pinned vk.xml, installed libdir.
        fixture = artifact.TOOLS / 'tests/fixtures/panfrost_icd.aarch64.json'
        original = json.loads(fixture.read_text())
        relocated = artifact.read_manifest(fixture)
        expected = json.loads(fixture.read_text())
        expected['ICD']['library_path'] = '../../../lib/' + artifact.DRIVER
        self.assertEqual(relocated, expected)
        self.assertEqual(json.loads(fixture.read_text()), original)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'relocated artifact with spaces'
            manifest = root / artifact.ICD
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps(relocated))
            (root / 'lib').mkdir()
            driver = self.elf(root / 'lib' / artifact.DRIVER)
            self.assertEqual((manifest.parent / relocated['ICD']['library_path']).resolve(), driver)

    def test_manifest_rejections_report_path_and_received_json(self):
        original = json.loads((artifact.TOOLS / 'tests/fixtures/panfrost_icd.aarch64.json').read_text())
        invalid = [None, [], {}, dict(original, file_format_version='1.0.2'),
                   dict(original, ICD=[])]
        for key, value in (('api_version', None), ('api_version', '1.4'),
                           ('library_path', None), ('library_path', 'libGLES_mali.so'),
                           ('library_path', 'libvulkan.so.1'), ('library_path', 'libc.so.6'),
                           ('library_path', 'bad\0/' + artifact.DRIVER),
                           ('library_arch', '32'), ('library_arch', 64)):
            invalid.append(dict(original, ICD=dict(original['ICD'], **{key: value})))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'panfrost_icd.aarch64.json'
            for data in invalid:
                with self.subTest(data=data):
                    path.write_text(json.dumps(data))
                    with self.assertRaises(RuntimeError) as caught:
                        artifact.read_manifest(path)
                    self.assertIn(str(path), str(caught.exception))
                    self.assertIn('received JSON=' + json.dumps(data, sort_keys=True), str(caught.exception))
            path.write_text('{broken JSON')
            with self.assertRaises(RuntimeError) as caught:
                artifact.read_manifest(path)
            self.assertIn(str(path), str(caught.exception))
            self.assertIn('{broken JSON', str(caught.exception))

    def test_legacy_relocated_manifest_preserves_semantics_and_works_with_existing_probe(self):
        original = {'file_format_version': '1.0.0', 'ICD': {
            'library_path': '/opt/droiddeck-client-panvk/lib/' + artifact.DRIVER,
            'api_version': '1.4.357', 'is_portability_driver': False}}
        relocated = artifact.relocate_manifest(original)
        self.assertEqual(relocated['ICD']['api_version'], original['ICD']['api_version'])
        self.assertFalse(relocated['ICD']['is_portability_driver'])
        self.assertTrue(original['ICD']['library_path'].startswith('/opt/'))
        spec = importlib.util.spec_from_file_location('probe', ROOT / 'tools/mali-vulkan/probe_steam_angle.py')
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        import argparse
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'unpacked artifact'
            (root / 'lib').mkdir(parents=True)
            manifest = root / artifact.ICD
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps(relocated))
            driver = self.elf(root / 'lib' / artifact.DRIVER)
            steam = Path(directory) / 'Steam/angle'
            steam.mkdir(parents=True)
            egl, gles = (self.elf(steam / name) for name in ('libEGL.so', 'libGLESv2.so'))
            args = argparse.Namespace(icd=str(manifest), panvk_library=str(driver),
                steam_root=str(steam.parent), egl=str(egl), gles=str(gles))
            with patch.dict(os.environ, {'DISPLAY': ':7'}, clear=True):
                env = probe.prepare(args)
            self.assertEqual(env['VK_DRIVER_FILES'], str(manifest))
            self.assertEqual(env['VK_ICD_FILENAMES'], str(manifest))
            self.assertEqual(env['LD_LIBRARY_PATH'].split(':')[0], str(driver.parent))
            self.assertEqual(env['PANVK_DXVK_COMPAT'], '0')
        for invalid in ({}, {'file_format_version': '9', 'ICD': original['ICD']},
                        {'file_format_version': '1.0.0', 'ICD': {'library_path': 'libGLES_mali.so', 'api_version': '1.4.0'}}):
            with self.assertRaises(RuntimeError):
                artifact.relocate_manifest(invalid)

    def test_manual_workflow_source_pin_and_minimal_configuration(self):
        import yaml
        workflow = yaml.safe_load((ROOT / '.github/workflows/build-panvk-glibc.yml').read_text())
        self.assertEqual(set(workflow['on']), {'workflow_dispatch'})
        mode = workflow['on']['workflow_dispatch']['inputs']['artifact_mode']
        self.assertEqual(mode['default'], 'raw-test')
        self.assertEqual(mode['options'], ['raw-test', 'audited-package'])
        self.assertEqual(workflow['permissions'], {'contents': 'read'})
        job = workflow['jobs']['panvk']
        self.assertIn('droiddeck-panvk-raw-test-arm64', job['env']['PANVK_ARTIFACT'])
        self.assertEqual(job['runs-on'], 'ubuntu-24.04-arm')
        self.assertEqual(job['container'], 'debian:13-slim')
        pin = json.loads((artifact.TOOLS / 'source.json').read_text())
        self.assertEqual(pin['commit'], 'efd07bab401b263a9ac114560ecdb9a2934f5d43')
        args = (artifact.TOOLS / 'meson-options.txt').read_text().splitlines()
        for required in ('-Dpanfrost-kmds=kbase', '-Dplatforms=x11', '-Dgallium-drivers=',
                         '-Dpanfrost-rust=false', '-Dopengl=false', '-Degl=disabled', '-Dglx=disabled',
                         '-Dauto_features=disabled', '-Dmesa-clc=enabled',
                         '-Dprecomp-compiler=enabled', '-Dspirv-tools=enabled'):
            self.assertIn(required, args)
        install = next(step['run'] for step in job['steps']
                       if step.get('name') == 'Install native build dependencies')
        self.assertIn('spirv-tools', install.split())
        self.assertNotIn('build-panvk-glibc', (ROOT / '.github/workflows/build.yml').read_text())


if __name__ == '__main__':
    unittest.main()
