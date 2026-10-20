"""Validate the pinned raw test artifact without loading any library."""
import hashlib
import json
from pathlib import Path
import re
import struct

FILES = ('libvulkan_panfrost.so', 'panfrost_icd.aarch64.json', 'DEPENDENCIES.txt',
         'BUILD_INFO.txt', 'SOURCE_DIFF.patch', 'Mesa-license.rst', 'SHA256SUMS')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def profile():
    transferred = Path(__file__).with_name('panvk-source.json')
    if transferred.is_file():
        return json.loads(transferred.read_text())
    tools = Path(__file__).resolve().parents[1] / 'panvk'
    return dict(json.loads((tools / 'source.json').read_text()),
                configure_arguments=(tools / 'meson-options.txt').read_text().splitlines(),
                patches=[dict(file=path.name, sha256=digest(path)) for path in sorted((tools / 'patches').glob('*.patch'))])


def verify(directory, pin=None):
    for name in FILES:
        path = directory / name
        if not path.is_file() or path.is_symlink():
            raise RuntimeError('missing/nonregular raw artifact file: ' + name)
    if pin is None:
        pin = profile()
    checksums = {}
    for line in (directory / 'SHA256SUMS').read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
        if not match or match[2] in checksums:
            raise RuntimeError('invalid/duplicate raw artifact checksum')
        checksums[match[2]] = match[1]
    if set(checksums) != set(FILES) - {'SHA256SUMS'}:
        raise RuntimeError('raw artifact checksum inventory differs')
    for name, expected in checksums.items():
        if digest(directory / name) != expected:
            raise RuntimeError('raw artifact checksum mismatch: ' + name)
    info = json.loads((directory / 'BUILD_INFO.txt').read_text())
    if (not isinstance(info, dict) or info.get('artifact') != 'droiddeck-panvk-raw-test-arm64' or
            info.get('source_commit') != pin['commit'] or
            info.get('source_repository') != 'https://github.com/' + pin['repository'] or
            info.get('source_diff_sha256') != checksums['SOURCE_DIFF.patch']):
        raise RuntimeError('raw artifact provenance differs from the pinned source')
    for key in ('patches', 'configure_arguments'):
        if key in pin and info.get(key) != pin[key]:
            raise RuntimeError('raw artifact provenance differs: ' + key)
    with (directory / FILES[0]).open('rb') as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:6] != b'\x7fELF\x02\x01' or
            struct.unpack_from('<HH', header, 16) != (3, 183)):
        raise RuntimeError('raw driver is not an AArch64 ELF shared object')
    manifest = json.loads((directory / FILES[1]).read_text())
    if not isinstance(manifest, dict) or not isinstance(manifest.get('ICD'), dict):
        raise RuntimeError('invalid generated raw ICD')
    entry = manifest.get('ICD', {})
    if (manifest.get('file_format_version') not in ('1.0.0', '1.0.1') or
            not isinstance(entry.get('library_path'), str) or
            '\0' in entry['library_path'] or
            Path(entry['library_path']).name != FILES[0] or
            not isinstance(entry.get('api_version'), str) or
            not re.fullmatch(r'\d+\.\d+\.\d+', entry['api_version']) or
            entry.get('library_arch', '64') != '64'):
        raise RuntimeError('invalid generated raw ICD')
    dependencies = (directory / 'DEPENDENCIES.txt').read_text()
    if not all(marker in dependencies for marker in ('readelf -dW:', 'readelf --version-info --wide:')):
        raise RuntimeError('raw artifact dependency evidence missing')
    if re.search(r'\[lib(?:GLES_mali|cutils|hardware|log)\.so(?:\]|\.)', dependencies):
        raise RuntimeError('raw artifact depends on Android/vendor libraries')
    return info
