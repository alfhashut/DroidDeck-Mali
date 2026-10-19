#!/usr/bin/env python3
"""TEMPORARY development-only output staging; no portable dependency packaging."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

TOOLS = Path(__file__).resolve().parent
DRIVER = 'libvulkan_panfrost.so'
MANIFEST = 'panfrost_icd.aarch64.json'


def command(*argv):
    return subprocess.check_output(argv, text=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stage(source, build, output):
    pin = json.loads((TOOLS / 'source.json').read_text())
    commit = command('git', '-C', str(source), 'rev-parse', 'HEAD').strip()
    if commit != pin['commit']:
        raise RuntimeError('raw test source differs from pinned commit')
    driver = build / 'src/panfrost/vulkan' / DRIVER
    # Record dependency names/version requirements without executing any DSO.
    dependencies = 'readelf -dW:\n' + command('readelf', '-dW', str(driver))
    dependencies += '\nreadelf --version-info --wide:\n' + command(
        'readelf', '--version-info', '--wide', str(driver))
    source_diff = command('git', '-C', str(source), 'diff', '--binary')
    # An explicit file list ensures no vendor, glibc, loader or dependency DSOs
    # can accidentally enter this raw artifact, even if present in the build.
    output.mkdir(parents=True, exist_ok=False)
    for name in (DRIVER, MANIFEST):
        shutil.copyfile(build / 'src/panfrost/vulkan' / name, output / name)
    shutil.copyfile(source / 'docs/license.rst', output / 'Mesa-license.rst')
    (output / 'DEPENDENCIES.txt').write_text(dependencies)
    (output / 'SOURCE_DIFF.patch').write_text(source_diff)
    info = {
        'status': 'TEMPORARY / DEVELOPMENT-ONLY; NOT PRODUCTION-READY; NOT PHONE-VALIDATED',
        'artifact': 'droiddeck-panvk-raw-test-arm64',
        'source_repository': 'https://github.com/' + pin['repository'],
        'source_commit': commit, 'mesa_version': (source / 'VERSION').read_text().strip(),
        'configure_arguments': (TOOLS / 'meson-options.txt').read_text().splitlines(),
        'source_diff_sha256': digest(output / 'SOURCE_DIFF.patch'),
        'patches': [{'file': path.name, 'sha256': digest(path)}
                    for path in sorted((TOOLS / 'patches').glob('*.patch'))],
        'dependencies': 'NONE BUNDLED; supplied by the phone Linux runtime; see DEPENDENCIES.txt',
        'manifest': 'Exact generated JSON, unmodified; create a separate local ICD for relocation',
    }
    (output / 'BUILD_INFO.txt').write_text(json.dumps(info, indent=2) + '\n')
    (output / 'SHA256SUMS').write_text(''.join(
        f'{digest(path)}  {path.name}\n' for path in sorted(output.iterdir()) if path.is_file()))
    print(info['status'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        stage(args.source.resolve(), args.build.resolve(), args.output.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'PanVK raw-test staging FAILED: {error}\n')


if __name__ == '__main__':
    main()
