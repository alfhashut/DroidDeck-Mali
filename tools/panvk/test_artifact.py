"""B0 host checks: synthetic ELF/metadata only; no compiler, loader, GPU or network."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import package_glibc as artifact

ROOT = Path(__file__).resolve().parents[2]


class ArtifactTests(unittest.TestCase):
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

    def test_relocated_manifest_preserves_semantics_and_works_with_existing_probe(self):
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
        self.assertEqual(workflow['permissions'], {'contents': 'read'})
        job = workflow['jobs']['panvk']
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
