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
import panvk_raw_artifact as raw

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
    raw.verify(artifact)
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
    identity = hashlib.sha256((digest(driver) + digest(script)).encode() + original +
                              (artifact / 'SHA256SUMS').read_bytes()).hexdigest()
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
            for name in raw.FILES[2:]:
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
    raw.verify(target)
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


def submission_diagnostics(output):
    # Report driver evidence verbatim; the requested override is not proof that
    # this driver selected it. Do not infer an effective stride from the request.
    strides = [line for line in output.splitlines()
               if re.search(r'atom[ _-]*stride|stride.*atom|JOB_SUBMIT.*stride', line, re.IGNORECASE)
               and not line.startswith(('Requested atom stride=', 'probe child PANVK_ATOM_STRIDE='))
               and 'Command ' not in line]
    print('PanVK detected/effective atom stride (driver log): ' +
          ('\n'.join(strides) if strides else 'not exposed in probe output'), flush=True)
    for fault in re.finditer(r'\[KFAULT\]\s+atom=(\S+)\s+code=(\S+)\s+core_req=(\S+)', output):
        atom, code, core_req = fault.groups()
        meaning = ' (JOB_CONFIG_FAULT)' if code.lower() == '0x40' else ''
        print(f'KFAULT summary: atom={atom} code={code}{meaning} core_req={core_req}', flush=True)


def configure_steam_client(artifact, steam, explicit=None):
    client = client_environment()
    probe.require(Path('/usr/local/bin/droiddeck-steam-client-panvk').is_file(),
                  'install the APK containing the development Steam client launcher first')
    target = stage(artifact)
    pair = angle_pair(steam, explicit).resolve(strict=True)
    args = argparse.Namespace(icd=str(target / 'panvk-local.json'), panvk_library=str(target / DRIVER),
                              steam_root=str(steam), egl=str(pair / 'libEGL.so'),
                              gles=str(pair / 'libGLESv2.so'), atom_stride='56')
    env = probe.prepare(args, environment=client)
    loader = pair / 'libvulkan.so'
    probe.require(loader.resolve(strict=True).is_relative_to(steam.resolve(strict=True)),
                  'Steam ARM64 Vulkan loader must be inside the selected Steam tree')
    probe.native_elf(loader)
    config = {'version': 1, 'target': str(target.resolve()), 'steam_root': str(steam.resolve()),
              'angle_dir': str(pair), 'atom_stride': '56',
              'driver_sha256': digest(target / DRIVER), 'probe_sha256': digest(target / 'probe_steam_angle.py')}
    config['icd_sha256'] = digest(target / 'panvk-local.json')
    shared = Path(__file__).resolve().parent / 'steam-client-panvk.json'
    pointer = Path.home() / '.config/droiddeck/steam-client-panvk.json'
    pointer.parent.mkdir(parents=True, exist_ok=True)
    # The guest owns only a link to the development config. ADB can remove the
    # shared file to recover C8A even when the experimental client cannot start.
    with tempfile.TemporaryDirectory(prefix='.panvk-config-', dir=shared.parent) as directory:
        pending = Path(directory) / shared.name
        pending.write_text(json.dumps(config, indent=2) + '\n')
        pending.replace(shared)
    with tempfile.TemporaryDirectory(prefix='.panvk-link-', dir=pointer.parent) as directory:
        pending = Path(directory) / pointer.name
        pending.symlink_to(shared)
        pending.replace(pointer)
    print(f'Steam client PanVK acceleration configured: enabled (DEVELOPMENT ONLY); ICD={env["VK_DRIVER_FILES"]}', flush=True)
    print('PANVK_ATOM_STRIDE=56; Steam CEF request=-cef-use-vulkan -> --use-angle=vulkan; ANGLE_DEFAULT_PLATFORM=vulkan', flush=True)
    print(f'Steam ARM64 ANGLE directory={pair}; Vulkan loader={loader}', flush=True)
    print(f'Guest config={pointer}; shared recovery config={shared}', flush=True)
    print('Stop normally, then Launch again to apply. Current Steam/Gamescope environments were not changed.', flush=True)
    print('DROIDDECK_STEAM_CLIENT_PANVK_CONFIGURED enabled; real Steam UI acceleration UNPROVEN', flush=True)


def run(artifact, steam, explicit=None, atom_stride='56'):
    client = client_environment()
    target = stage(artifact)
    pair = angle_pair(steam, explicit)
    args = argparse.Namespace(icd=str(target / 'panvk-local.json'), panvk_library=str(target / DRIVER),
                              steam_root=str(steam), egl=str(pair / 'libEGL.so'), gles=str(pair / 'libGLESv2.so'))
    env = probe.prepare(args, environment=client)
    print(f'DEVELOPMENT CLIENT-ONLY PanVK test: guest={target}; client DISPLAY={env["DISPLAY"]}', flush=True)
    print('Gamescope stays on its existing Mali proxy/broker; normal Steam startup is unchanged.', flush=True)
    print(f'Requested atom stride={atom_stride}', flush=True)
    # Separate process: report missing dependencies without preloading the probe's ICD.
    subprocess.run([sys.executable, '-B', '-c', 'import ctypes,sys; ctypes.CDLL(sys.argv[1])',
                    args.panvk_library], env=env, check=True, timeout=15)
    command = [sys.executable, '-B', str(target / 'probe_steam_angle.py')]
    for option in ('icd', 'panvk_library', 'steam_root', 'egl', 'gles'):
        command += ['--' + option.replace('_', '-'), getattr(args, option)]
    command += ['--atom-stride', atom_stride]
    try:
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ''
        if isinstance(output, bytes):
            output = output.decode(errors='replace')
        print(output, end='', flush=True)
        submission_diagnostics(output)
        raise
    print(result.stdout, end='', flush=True)
    submission_diagnostics(result.stdout)
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
    parser.add_argument('--steam-client', choices=('enable',),
                        help='Configure the next development Steam client launch with stride 56; does not run the standalone probe')
    parser.add_argument('--atom-stride', choices=('auto', '56', '64'), default='56',
                        help='Development GPU probe child only; auto leaves PANVK_ATOM_STRIDE unset')
    args = parser.parse_args()
    try:
        if args.steam_client:
            configure_steam_client(args.artifact, args.steam_root, args.angle_dir)
            return 0
        run(args.artifact, args.steam_root, args.angle_dir, args.atom_stride)
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        label = 'Steam client PanVK configuration FAIL' if args.steam_client else 'C8B.1 PanVK CLIENT-ONLY PROBE FAIL'
        print(f'{label}: {error}', file=sys.stderr)
        print('No PASS. Keep dependency/load/native errors above; no dependency libraries were installed.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
