"""Pinned raw artifact integrity, provenance and dependency evidence; no DSO execution."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import panvk_raw_artifact as raw


def seal(directory):
    (directory / 'SHA256SUMS').write_text(''.join(
        f'{raw.digest(directory / name)}  {name}\n' for name in raw.FILES if name != 'SHA256SUMS'))


def fixture(directory, driver=None):
    directory.mkdir(exist_ok=True)
    if driver is None:
        driver = bytearray(20)
        driver[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<HH', driver, 16, 3, 183)
    (directory / raw.FILES[0]).write_bytes(driver)
    original = Path(__file__).resolve().parents[1] / 'panvk/tests/fixtures/panfrost_icd.aarch64.json'
    (directory / raw.FILES[1]).write_bytes(original.read_bytes())
    (directory / 'DEPENDENCIES.txt').write_text('readelf -dW:\n(NEEDED) [libdrm.so.2]\nreadelf --version-info --wide:\nGLIBC_2.38\n')
    (directory / 'SOURCE_DIFF.patch').write_text('pinned build patch fixture\n')
    (directory / 'Mesa-license.rst').write_text('Mesa license fixture\n')
    pin = raw.profile()
    info = dict(artifact='droiddeck-panvk-raw-test-arm64', source_commit=pin['commit'],
                source_repository='https://github.com/' + pin['repository'],
                source_diff_sha256=raw.digest(directory / 'SOURCE_DIFF.patch'),
                patches=pin['patches'], configure_arguments=pin['configure_arguments'])
    (directory / 'BUILD_INFO.txt').write_text(json.dumps(info) + '\n')
    seal(directory)


class RawArtifactTests(unittest.TestCase):
    def test_exact_generated_icd_and_provenance_are_accepted_without_rewriting(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture(root)
            before = {name: (root / name).read_bytes() for name in raw.FILES}
            info = raw.verify(root)
            self.assertEqual(info['source_commit'], raw.profile()['commit'])
            self.assertEqual(before, {name: (root / name).read_bytes() for name in raw.FILES})

    def test_corruption_missing_checksums_and_duplicate_inventory_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in raw.FILES:
                fixture(root)
                (root / name).write_bytes((root / name).read_bytes() + b'corrupt')
                with self.subTest(name=name), self.assertRaises((RuntimeError, ValueError)):
                    raw.verify(root)
            fixture(root)
            checksum = root / 'SHA256SUMS'
            checksum.write_text(checksum.read_text() * 2)
            with self.assertRaisesRegex(RuntimeError, 'duplicate'):
                raw.verify(root)
            checksum.unlink()
            with self.assertRaisesRegex(RuntimeError, 'missing'):
                raw.verify(root)

    def test_resealed_wrong_pin_patch_set_vendor_dependency_or_architecture_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for key, value in [('source_commit', '0' * 40), ('patches', []), ('configure_arguments', [])]:
                fixture(root)
                path = root / 'BUILD_INFO.txt'
                info = json.loads(path.read_text())
                info[key] = value
                path.write_text(json.dumps(info))
                seal(root)
                with self.assertRaisesRegex(RuntimeError, 'provenance'):
                    raw.verify(root)
            fixture(root)
            with (root / 'DEPENDENCIES.txt').open('a') as stream:
                stream.write('(NEEDED) [libGLES_mali.so]\n')
            seal(root)
            with self.assertRaisesRegex(RuntimeError, 'vendor'):
                raw.verify(root)
            fixture(root, b'not a driver')
            with self.assertRaisesRegex(RuntimeError, 'AArch64'):
                raw.verify(root)

    def test_source_profile_is_pinned_and_client_only(self):
        pin = raw.profile()
        self.assertEqual(pin['commit'], 'efd07bab401b263a9ac114560ecdb9a2934f5d43')
        self.assertIn('-Dpanfrost-kmds=kbase', pin['configure_arguments'])
        self.assertIn('-Dgallium-drivers=', pin['configure_arguments'])
        self.assertIn('-Degl=disabled', pin['configure_arguments'])


if __name__ == '__main__':
    unittest.main()
