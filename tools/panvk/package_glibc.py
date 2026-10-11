#!/usr/bin/env python3
"""Stage only the native ICD and its audited ELF dependency closure. No DSO execution."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

TOOLS = Path(__file__).resolve().parent
DRIVER = 'libvulkan_panfrost.so'
ICD = 'share/vulkan/icd.d/panvk_droiddeck.json'
# glibc and its dynamic loader MUST come from the guest, never this artifact.
SYSTEM = frozenset(('libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
                    'librt.so.1', 'libresolv.so.2', 'libutil.so.1', 'ld-linux-aarch64.so.1'))
# Fail closed if the pinned minimal build unexpectedly links another driver,
# Android/vendor library, Vulkan loader, LLVM, or another unreviewed dependency.
PRIVATE = frozenset(('libdrm.so.2', 'libelf.so.1', 'libz.so.1', 'libzstd.so.1',
    'liblzma.so.5', 'libexpat.so.1', 'libX11.so.6', 'libX11-xcb.so.1', 'libXau.so.6',
    'libXdmcp.so.6', 'libxcb.so.1', 'libxcb-dri3.so.0', 'libxcb-present.so.0',
    'libxcb-randr.so.0', 'libxcb-shm.so.0', 'libxcb-sync.so.1', 'libxcb-xfixes.so.0',
    'libxcb-keysyms.so.1', 'libxshmfence.so.1', 'libXrandr.so.2', 'libXext.so.6',
    'libXrender.so.1', 'libbsd.so.0', 'libmd.so.0', 'libstdc++.so.6', 'libgcc_s.so.1',
    'libudev.so.1'))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def command(*argv):
    return subprocess.check_output(argv, text=True).strip()


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_elf(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    require(len(header) == 20 and header[:6] == b'\x7fELF\x02\x01'
            and struct.unpack_from('<HH', header, 16) == (3, 183),
            f'not an AArch64 ELF64 shared object: {path}')
    dynamic = command('readelf', '-dW', str(path))
    needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
    sonames = re.findall(r'\(SONAME\).*?\[(.*?)\]', dynamic)
    require(len(sonames) == 1, f'missing/ambiguous ELF SONAME: {path}')
    for entry in re.findall(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', dynamic):
        require(all(part in ('', '$ORIGIN') for part in entry.split(':')),
                f'nonportable ELF search path in {path}: {entry}')
    versions, in_needs = set(), False
    for line in command('readelf', '--version-info', '--wide', str(path)).splitlines():
        if line.startswith('Version '):
            in_needs = line.startswith('Version needs section')
        if in_needs:
            versions.update(re.findall(r'Name: (\S+)', line))
    return {'soname': sonames[0], 'needed': sorted(set(needed)),
            'required_symbol_versions': sorted(versions)}


def resolve_dependency(name, roots):
    require(name in PRIVATE, f'unreviewed runtime dependency: {name}')
    for root in roots:
        candidate = root / name
        if candidate.exists():
            resolved = candidate.resolve(strict=True)
            require(any(resolved.is_relative_to(r.resolve()) for r in roots),
                    f'dependency escapes trusted system library directories: {candidate}')
            return resolved
    raise RuntimeError(f'unresolved runtime dependency: {name}')


def runtime_closure(driver, roots):
    pending, files, external = [(DRIVER, driver)], {}, set()
    while pending:
        name, path = pending.pop()
        if name in files:
            continue
        info = inspect_elf(path)
        require(info['soname'] == name, f'ELF SONAME mismatch: {path} / {name}')
        require('GLIBC_PRIVATE' not in info['required_symbol_versions'],
                f'private glibc ABI dependency: {path}')
        files[name] = (path, info)
        for dependency in info['needed']:
            if dependency in SYSTEM:
                external.add(dependency)
            elif dependency not in files:
                pending.append((dependency, resolve_dependency(dependency, roots)))
    return files, sorted(external)


def relocate_manifest(original):
    # Preserve generated format/API/other semantic fields; relocate only the library.
    manifest = json.loads(json.dumps(original))
    require(manifest.get('file_format_version') == '1.0.0', 'unexpected generated ICD format')
    require(isinstance(manifest.get('ICD'), dict), 'missing generated ICD fields')
    entry = manifest['ICD']
    require(re.fullmatch(r'\d+\.\d+\.\d+', entry.get('api_version', '')),
            'invalid generated ICD API version')
    require(Path(entry.get('library_path', '')).name == DRIVER, 'wrong generated ICD library')
    entry['library_path'] = '../../../lib/' + DRIVER
    return manifest


def package_owner(path):
    # Debian's /usr merge may differ between dpkg's pathname and realpath.
    candidates = [str(path)]
    if str(path).startswith('/usr/lib/'):
        candidates.append(str(path)[4:])
    for candidate in candidates:
        result = subprocess.run(['dpkg-query', '-S', candidate], capture_output=True, text=True)
        if result.returncode == 0:
            owner = result.stdout.splitlines()[0].split(': ', 1)[0]
            require(re.fullmatch(r'[a-z0-9+.-]+(?::arm64)?', owner), 'ambiguous package owner')
            return owner
    raise RuntimeError(f'runtime dependency has no Debian package owner: {path}')


def package(source, build, output):
    pin = json.loads((TOOLS / 'source.json').read_text())
    require(command('git', '-C', str(source), 'rev-parse', 'HEAD') == pin['commit'],
            'source differs from audited commit')
    require(not output.exists(), 'output already exists; refusing to overwrite')
    driver = build / 'src/panfrost/vulkan' / DRIVER
    original = json.loads((build / 'src/panfrost/vulkan/panfrost_icd.aarch64.json').read_text())
    manifest = relocate_manifest(original)
    roots = [Path('/usr/lib/aarch64-linux-gnu'), Path('/lib/aarch64-linux-gnu')]
    files, external = runtime_closure(driver, roots)
    require(command('uname', '-m') == 'aarch64', 'packaging requires native AArch64')
    require(command('getconf', 'GNU_LIBC_VERSION').startswith('glibc '), 'not a glibc build')
    (output / 'lib').mkdir(parents=True)
    (output / ICD).parent.mkdir(parents=True)
    licenses = output / 'share/licenses'
    licenses.mkdir(parents=True)
    shutil.copyfile(source / 'docs/license.rst', licenses / 'Mesa-license.rst')
    records, packages = {}, {}
    for name, (path, info) in sorted(files.items()):
        target = output / 'lib' / name
        # Dereference trusted system symlinks, so the archive has no escaping links.
        shutil.copyfile(path, target)
        target.chmod(0o755)
        records[name] = dict(info, sha256=digest(target))
        if name != DRIVER:
            owner = package_owner(path)
            records[name]['debian_package'] = owner
            if owner not in packages:
                packages[owner] = command('dpkg-query', '-W', '-f=${Version}', owner)
                doc = Path('/usr/share/doc') / owner.split(':')[0] / 'copyright'
                require(doc.is_file(), f'missing runtime license: {doc}')
                shutil.copyfile(doc, licenses / (owner.replace(':', '_') + '-copyright'))
    (output / ICD).write_text(json.dumps(manifest, indent=2) + '\n')
    selected = {}
    args = (TOOLS / 'meson-options.txt').read_text().splitlines()
    options = json.loads(command('meson', 'introspect', '--buildoptions', str(build)))
    for option in options:
        if any(arg.split('=', 1)[0] == '-D' + option['name'] for arg in args) or option['name'] in (
                'buildtype', 'prefix', 'libdir', 'wrap_mode', 'c_args', 'cpp_args',
                'c_link_args', 'cpp_link_args'):
            selected[option['name']] = option['value']
    info = {
        'source_repository': 'https://github.com/' + pin['repository'],
        'source_commit': pin['commit'], 'mesa_version': (source / 'VERSION').read_text().strip(),
        'architecture': 'AArch64 / Linux / glibc / native Debian 13 on ubuntu-24.04-arm',
        'patch': {'file': '0001-android-only-liblog.patch',
                  'sha256': digest(TOOLS / 'patches/0001-android-only-liblog.patch')},
        'source_diff_sha256': hashlib.sha256(command('git', '-C', str(source), 'diff').encode()).hexdigest(),
        'configure_arguments': args, 'meson_build_options': selected,
        'toolchain': {name: command(*argv) for name, argv in {
            'C': ('gcc', '--version'), 'C++': ('g++', '--version'),
            'LLVM': ('/usr/bin/llvm-config-19', '--version'), 'Meson': ('meson', '--version'),
            'Ninja': ('ninja', '--version'), 'binutils': ('readelf', '--version'),
            'glibc': ('getconf', 'GNU_LIBC_VERSION')}.items()},
        'python': sys.version, 'icd_json': ICD,
        'bundled_libraries': records, 'bundled_dependency_packages': packages,
        'system_glibc_dependencies': external,
        'required_glibc_symbol_versions': sorted({v for _, elf in files.values()
            for v in elf['required_symbol_versions'] if v.startswith('GLIBC_')}),
        'build_package_versions': command('dpkg-query', '-W', '-f=${binary:Package}=${Version}\n'),
        'status': 'CI build only; PanVK/ANGLE/Steam hardware acceleration is NOT phone-validated',
    }
    (output / 'BUILD_INFO.txt').write_text(json.dumps(info, indent=2, sort_keys=True) + '\n')
    # Stable ordering, explicit content identities; no source tree or vendor blobs.
    lines = [f'{digest(p)}  {p.relative_to(output).as_posix()}'
             for p in sorted(output.rglob('*')) if p.is_file()]
    (output / 'SHA256SUMS').write_text('\n'.join(lines) + '\n')
    print(f'packaged {len(files)} audited runtime libraries; ICD={ICD}; no phone proof')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        package(args.source.resolve(), args.build.resolve(), args.output.resolve())
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'PanVK packaging FAILED: {error}\n')


if __name__ == '__main__':
    main()
