#!/usr/bin/env python3
"""Development-only test beside the running Mali Steam client; never reconfigure Gamescope."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import probe_steam_angle as probe

DRIVER = 'libvulkan_panfrost.so'
MANIFEST = 'panfrost_icd.aarch64.json'
GUEST = Path('/root/panvk-test')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def client_environment(proc=Path('/proc')):
    clients = {}
    for pid in proc.iterdir():
        if not pid.name.isdigit():
            continue
        try:
            if pid.stat().st_uid != os.getuid():
                continue
            argv = (pid / 'cmdline').read_bytes().decode(errors='replace').split('\0')
            if not any(Path(arg).name == 'droiddeck-session' for arg in argv) or 'steam' not in argv:
                continue
            env = dict(item.split('=', 1) for item in
                       (pid / 'environ').read_bytes().decode(errors='strict').split('\0') if '=' in item)
            if (env.get('BL_MALI_STEAM_UI') != '1' or env.get('BL_INSIDE') != '1'
                    or not re.fullmatch(r':\d+(?:\.\d+)?', env.get('DISPLAY', ''))
                    or any(env.get(key) for key in ('MALI_VULKAN_NORMAL_SESSION', 'MALI_VULKAN_BROKER_SOCKET'))):
                continue
            key = tuple(env.get(name, '') for name in ('DISPLAY', 'XDG_RUNTIME_DIR', 'XAUTHORITY'))
            clients[key] = env
        except (OSError, UnicodeError):
            continue
    probe.require(len(clients) == 1,
                  'need one running Mali Steam Xwayland client; start normal Launch and use its session Terminal '
                  f'(readable client displays={len(clients)}); no guessed DISPLAY or compositor fallback')
    return next(iter(clients.values()))


def stage(artifact, base=GUEST):
    driver = artifact / DRIVER
    probe.require((artifact / MANIFEST).stat().st_size <= 1024 * 1024, 'oversized raw ICD manifest')
    original = (artifact / MANIFEST).read_bytes()
    manifest = json.loads(original)
    probe.require(isinstance(manifest, dict) and manifest.get('file_format_version') in ('1.0.0', '1.0.1'),
                  'unsupported raw PanVK ICD format')
    entry = manifest.get('ICD')
    probe.require(isinstance(entry, dict) and isinstance(entry.get('library_path'), str)
                  and Path(entry['library_path']).name == DRIVER, 'raw ICD does not identify libvulkan_panfrost.so')
    probe.require(isinstance(entry.get('api_version'), str)
                  and re.fullmatch(r'\d+\.\d+\.\d+', entry['api_version']), 'invalid raw ICD API version')
    probe.require('library_arch' not in entry or entry['library_arch'] == '64', 'raw ICD is not AArch64/64-bit')
    probe.native_elf(driver)
    script = Path(probe.__file__).resolve()
    # Immutable per-content directories: another invocation never overwrites a loaded DSO.
    identity = hashlib.sha256((digest(driver) + digest(script)).encode() + original).hexdigest()
    base.mkdir(parents=True, exist_ok=True)
    target = base / identity
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix='.stage-', dir=base) as directory:
            pending = Path(directory)
            shutil.copyfile(driver, pending / DRIVER)
            (pending / MANIFEST).write_bytes(original)
            entry['library_path'] = './' + DRIVER
            (pending / 'panvk-local.json').write_text(json.dumps(manifest, indent=2) + '\n')
            shutil.copyfile(script, pending / 'probe_steam_angle.py')
            for name in ('DEPENDENCIES.txt', 'BUILD_INFO.txt'):
                if (artifact / name).is_file():
                    shutil.copyfile(artifact / name, pending / name)
            # Concurrent identical staging may already have published this directory.
            try:
                pending.rename(target)
            except OSError as error:
                if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                    raise
    probe.require(digest(target / DRIVER) == digest(driver)
                  and (target / MANIFEST).read_bytes() == original
                  and digest(target / 'probe_steam_angle.py') == digest(script), 'staged raw artifact identity mismatch')
    return target


def angle_pair(steam, explicit=None):
    if explicit:
        candidates = [explicit]
    else:
        candidates = sorted({path.parent.resolve() for path in
                             (steam / 'steamrtarm64').rglob('libEGL.so')
                             if (path.parent / 'libGLESv2.so').is_file()})
    probe.require(len(candidates) == 1,
                  'cannot select one Steam ARM64 ANGLE pair; use --angle-dir. Candidates: '
                  + ', '.join(str(path) for path in candidates))
    return candidates[0]


def run(artifact, steam, explicit=None):
    client = client_environment()
    target = stage(artifact)
    pair = angle_pair(steam, explicit)
    args = argparse.Namespace(icd=str(target / 'panvk-local.json'), panvk_library=str(target / DRIVER),
                              steam_root=str(steam), egl=str(pair / 'libEGL.so'), gles=str(pair / 'libGLESv2.so'))
    env = probe.prepare(args, environment=client)
    print(f'DEVELOPMENT CLIENT-ONLY PanVK test: guest={target}; client DISPLAY={env["DISPLAY"]}', flush=True)
    print('Gamescope stays on its existing Mali proxy/broker; normal Steam startup is unchanged.', flush=True)
    # Separate process: report missing dependencies without preloading the probe's ICD.
    subprocess.run([sys.executable, '-B', '-c', 'import ctypes,sys; ctypes.CDLL(sys.argv[1])',
                    args.panvk_library], env=env, check=True, timeout=15)
    command = [sys.executable, '-B', str(target / 'probe_steam_angle.py')]
    for option in ('icd', 'panvk_library', 'steam_root', 'egl', 'gles'):
        command += ['--' + option.replace('_', '-'), getattr(args, option)]
    result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
    print(result.stdout, end='', flush=True)
    probe.require(result.returncode == 0, f'ANGLE probe exited {result.returncode}; see exact failure above')
    evidence = {
        'PanVK ICD/library loaded': ('ICD=' + args.icd, 'PanVK library=' + args.panvk_library, 'Vulkan loader='),
        'hardware Mali-G52 / PanVK, no llvmpipe': ('Vulkan device=', 'vendor=0x13b5', 'software renderer=none (validated)'),
        'ANGLE Vulkan backend': ('ANGLE backend=Vulkan',),
        'GPU draw/readback': ('GPU triangle readback: verified',),
        'X11 presentation and cleanup': ('X11 CPU-copy pixels: verified', 'probe render/copy/cleanup completed', 'probe exit=0'),
    }
    for name, markers in evidence.items():
        probe.require(all(marker in result.stdout for marker in markers), 'missing probe evidence: ' + name)
    for name in evidence:
        print('PASS: ' + name)
    print('C8B.1 PanVK CLIENT-ONLY PROBE PASS; Steam/CEF acceleration remains unproven')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, default=Path(__file__).resolve().parent / 'artifact')
    parser.add_argument('--steam-root', type=Path, default=Path('/root/.local/share/Steam'))
    parser.add_argument('--angle-dir', type=Path)
    args = parser.parse_args()
    try:
        run(args.artifact, args.steam_root, args.angle_dir)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f'C8B.1 PanVK CLIENT-ONLY PROBE FAIL: {error}', file=sys.stderr)
        print('No PASS. Keep dependency/load/native errors above; no dependency libraries were installed.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
