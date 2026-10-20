"""Live renderer evidence must correlate with this launch, GPU child and selected DSOs."""
import argparse
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest

import steam_cef_evidence as cef


class CefEvidenceTests(unittest.TestCase):
    def fixture(self):
        return dict(steam=dict(pid=100, argv=['/Steam/steamrtarm64/steam', '-cef-use-vulkan']),
                    gpu=dict(auxAttributes=dict(glRenderer='ANGLE (ARM, Vulkan 1.3 (Mali-G52 (PanVK)), panvk)',
                        glVendor='Google Inc. (ARM)', glImplementationParts='(gl=egl-angle,angle=vulkan)',
                        displayType='ANGLE_VULKAN'), featureStatus=dict(vulkan='enabled_on')),
                    gpu_process_ids=[101], expected_icd='/root/panvk-test/identity/panvk-local.json',
                    expected_libraries=dict(panvk=[8, 1, 1], egl=[8, 1, 2], gles=[8, 1, 3]),
                    helpers=[dict(pid=101, argv=['/Steam/steamwebhelper', '--type=gpu-process', '--use-angle=vulkan'],
                        environment=dict(PANVK_ATOM_STRIDE='56', ANGLE_DEFAULT_PLATFORM='vulkan',
                            VK_DRIVER_FILES='/root/panvk-test/identity/panvk-local.json'),
                        mappings=[dict(path='/android/alias/' + name, identity=[8, 1, inode])
                            for name, inode in [('libvulkan_panfrost.so', 1), ('libEGL.so', 2), ('libGLESv2.so', 3)]])])

    def test_matching_live_hardware_evidence_passes_and_returns_individual_checks(self):
        result = cef.evaluate(self.fixture())
        self.assertTrue(result['proven'])
        self.assertTrue(all(result['checks'].values()))
        self.assertIn('physical display timing remain separate', result['meaning'])

    def test_configuration_logs_flags_and_library_presence_cannot_replace_renderer_evidence(self):
        evidence = self.fixture()
        for attributes in ({}, dict(glRenderer='llvmpipe', glVendor='Mesa'),
                           dict(glRenderer='ANGLE (Mali-G52 PanVK OpenGL)')):
            changed = copy.deepcopy(evidence)
            changed['gpu']['auxAttributes'] = attributes
            self.assertFalse(cef.evaluate(changed)['proven'])
        for key, value in [('glImplementationParts', '(gl=egl-angle,angle=opengl)'),
                           ('displayType', 'ANGLE_OPENGL'),
                           ('glRenderer', 'ANGLE (ARM, Vulkan Mali-G57 PanVK)'),
                           ('glRenderer', 'ANGLE (Mesa, Vulkan llvmpipe)')]:
            changed = copy.deepcopy(evidence)
            changed['gpu']['auxAttributes'][key] = value
            self.assertFalse(cef.evaluate(changed)['proven'])
        evidence['gpu']['featureStatus']['vulkan'] = 'disabled_off'
        self.assertFalse(cef.evaluate(evidence)['proven'])

    def test_wrong_flags_missing_gpu_pid_stride_or_inode_never_pass(self):
        for flags in ([], ['-cef-use-gl=angle', '-cef-use-angle=vulkan'],
                      ['-cef-use-vulkan', '-cef-use-angle=vulkan'], ['-cef-use-vulkan', '-cef-disable-gpu']):
            evidence = self.fixture()
            evidence['steam']['argv'] = ['/Steam/steamrtarm64/steam', *flags]
            self.assertFalse(cef.evaluate(evidence)['proven'])
        for mutate in (lambda e: e.update(gpu_process_ids=[999]),
                       lambda e: e['helpers'][0]['environment'].update(PANVK_ATOM_STRIDE='64'),
                       lambda e: e['helpers'][0]['environment'].update(MALI_VULKAN_BROKER_SOCKET='/broker'),
                       lambda e: e['helpers'][0]['mappings'][0].update(identity=[8, 1, 99]),
                       lambda e: e['helpers'][0]['mappings'][0].update(path='/driver.so (deleted)')):
            evidence = self.fixture()
            mutate(evidence)
            self.assertFalse(cef.evaluate(evidence)['proven'])

    def test_different_gpu_children_cannot_supply_halves_of_one_proof(self):
        evidence = self.fixture()
        second = copy.deepcopy(evidence['helpers'][0])
        second['pid'] = 102
        second['environment'] = {}
        evidence['helpers'][0]['mappings'] = []
        evidence['helpers'].append(second)
        evidence['gpu_process_ids'].append(102)
        self.assertFalse(cef.evaluate(evidence)['proven'])

    def test_live_collection_uses_same_uid_ancestry_and_kernel_identity_across_proot_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            proc = root / 'proc'
            proc.mkdir()
            paths = [root / name for name in ('libvulkan_panfrost.so', 'libEGL.so', 'libGLESv2.so')]
            for path in paths:
                path.touch()
            args = argparse.Namespace(panvk_library=str(paths[0]), egl=str(paths[1]), gles=str(paths[2]), icd='/client/panvk.json')
            def process(pid, ppid, argv, env):
                path = proc / str(pid)
                path.mkdir()
                (path / 'cmdline').write_bytes('\0'.join(argv).encode())
                (path / 'status').write_text(f'PPid:\t{ppid}\n')
                (path / 'environ').write_bytes('\0'.join(f'{key}={value}' for key, value in env.items()).encode())
                rows = []
                for library in paths:
                    stat = library.stat()
                    rows.append(f'1000-2000 r-xp 0000 {os.major(stat.st_dev):x}:{os.minor(stat.st_dev):x} {stat.st_ino} /android/host/{library.name}')
                (path / 'maps').write_text('\n'.join(rows))
            process(100, 1, ['/Steam/steamrtarm64/steam', '-cef-use-vulkan'],
                    dict(DISPLAY=':42', DROIDDECK_STEAM_CLIENT_PANVK_RUN='100', BL_CDP_PORT='32100'))
            process(101, 100, ['/Steam/steamwebhelper', '--use-angle=vulkan'], {})
            process(102, 1, ['/Other/steamwebhelper'], {})
            evidence = cef.collect(dict(DISPLAY=':42'), args, {}, [dict(type='GPU', id=101)], proc)
            self.assertEqual([row['pid'] for row in evidence['helpers']], [101])
            self.assertEqual(evidence['steam']['environment']['BL_CDP_PORT'], '32100')
            self.assertEqual(evidence['expected_libraries']['panvk'], evidence['helpers'][0]['mappings'][0]['identity'])
            (proc / '100/environ').write_bytes(b'DISPLAY=:43\0DROIDDECK_STEAM_CLIENT_PANVK_RUN=100')
            with self.assertRaisesRegex(RuntimeError, 'exactly one'):
                cef.collect(dict(DISPLAY=':42'), args, {}, [], proc)


if __name__ == '__main__':
    unittest.main()
