#!/usr/bin/env python3
"""Read-only installed Steam ARM64 argv/CEF option audit. No graphics API calls."""
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import time

TOKENS = (b'cef-use-angle', b'cef-use-gl', b'cef-use-vulkan', b'no-linux-angle', b'use-angle', b'use-gl',
          b'-cef-', b'no-cef-', b'STEAMWEBHELPER', b'CEF_ARGS', b'cef-command',
          b'cef-extra', b'angle=opengl', b'--use-angle=gl', b'--use-angle=opengl',
          b'ANGLE_DEFAULT_PLATFORM')


def emit(label, value):
    print(label + '=' + json.dumps(value, sort_keys=True), flush=True)


def cstring(data, offset):
    lower = max(0, offset - 512)
    previous = data.rfind(b'\0', lower, offset)
    start = previous + 1 if previous >= 0 else lower
    end = data.find(b'\0', offset, min(len(data), offset + 1024))
    if end < 0:
        end = min(len(data), offset + 1024)
    return data[start:end].decode(errors='backslashreplace')


def scan(path):
    """Report exact byte offsets/strings; presence alone does not prove support."""
    with path.open('rb') as stream:
        stat = os.fstat(stream.fileno())
        if stat.st_size == 0:
            return
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            emit('installed-file', dict(path=str(path), size=len(data), mtime_ns=stat.st_mtime_ns,
                                        sha256=hashlib.sha256(data).hexdigest(), elf=data[:4] == b'\x7fELF'))
            for token in TOKENS:
                position, hits = 0, []
                while len(hits) < 80:
                    offset = data.find(token, position)
                    if offset < 0:
                        break
                    hits.append(dict(offset=hex(offset), text=cstring(data, offset)))
                    position = offset + len(token)
                emit('option-strings', dict(path=str(path), token=token.decode(), hits=hits,
                                            truncated=len(hits) == 80))


def processes(client):
    installed = set()
    gpu_pids = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            if p.stat().st_uid != os.getuid():
                continue
            raw = (p / 'cmdline').read_bytes()
            argv = raw.rstrip(b'\0').decode(errors='backslashreplace').split('\0')
            if not any(Path(arg).name in ('steam', 'steamwebhelper', 'steamwebhelper.sh', 'droiddeck-session')
                       or 'steamwebhelper --' in arg for arg in argv):
                continue
            env = dict(item.split('=', 1) for item in (p / 'environ').read_bytes()
                       .decode(errors='replace').split('\0') if '=' in item)
            if env.get('DISPLAY') != client['DISPLAY']:
                continue
            status = (p / 'status').read_text()
            is_gpu = any(arg == '--type=gpu-process' or ' --type=gpu-process' in arg for arg in argv)
            if is_gpu:
                gpu_pids.append(int(p.name))
            emit('live-process-exact-cmdline', dict(pid=int(p.name), argv=argv,
                trailing_nul=raw.endswith(b'\0'), exe=os.readlink(p / 'exe'),
                name=re.search(r'^Name:\s*(.*)', status, re.M)[1],
                ppid=int(re.search(r'^PPid:\s*(\d+)', status, re.M)[1]), explicit_gpu_type=is_gpu,
                environment={k: env.get(k) for k in ('PANVK_ATOM_STRIDE', 'VK_DRIVER_FILES',
                              'ANGLE_DEFAULT_PLATFORM', 'LD_LIBRARY_PATH', 'STEAMWEBHELPER_EXTRA_ARGS',
                              'STEAMWEBHELPER_ARGS', 'CEF_ARGS')}))
            for row in (p / 'maps').read_text().splitlines():
                parts = row.split(maxsplit=5)
                if len(parts) == 6 and '/Steam/' in parts[5]:
                    # /proc maps are Android-host absolute paths under proot.
                    guest = Path('/root/.local/share/Steam') / parts[5].split('/Steam/', 1)[1]
                    if guest.is_file():
                        installed.add(guest)
        except (OSError, ValueError, TypeError):
            continue
    emit('explicit-gpu-process-cmdline-pids', gpu_pids)
    if not gpu_pids:
        emit('gpu-process-attribution', 'No live cmdline contains --type=gpu-process. Compare PID-labelled GPU logs with the exact cmdline; inherited zygote argv alone does not identify the child role.')
    return installed


def run():
    from run_panvk_phone_test_impl import client_environment
    client = client_environment()
    emit('CEF-argv-audit', dict(time=time.strftime('%Y-%m-%d %H:%M:%S %z'), display=client['DISPLAY'],
                              graphics_calls=0, session_changes=False))
    installed = processes(client)
    root = Path(client.get('HOME', '/root')) / '.local/share/Steam'
    # Restrict to this installed Steam tree; never search desktop/other builds.
    for name in ('steamrtarm64', 'ubuntu12_64'):
        directory = root / name
        if directory.is_dir():
            for filename in ('steam', 'steamclient.so', 'steamui.so', 'steamwebhelper',
                             'steamwebhelper.sh', 'libcef.so', 'libsteamwebrtc.so'):
                installed.update(directory.rglob(filename))
    for path in sorted(installed):
        if path.name in ('steam', 'steamclient.so', 'steamui.so', 'steamwebhelper',
                         'steamwebhelper.sh', 'libcef.so', 'libsteamwebrtc.so'):
            try:
                scan(path)
                if path.suffix == '.sh' and path.stat().st_size < 65536:
                    emit('installed-webhelper-script', dict(path=str(path), text=path.read_text()))
            except OSError as error:
                emit('installed-file-error', dict(path=str(path), error=str(error)))
    for path in sorted((root / 'package').glob('*.version')):
        if path.stat().st_size < 65536:
            emit('installed-version', dict(path=str(path), text=path.read_text(errors='replace')))
    for name in ('webhelper.txt', 'webhelper_gpu.txt', 'steamwebhelper.log', 'cef_log.txt', 'bootstrap_log.txt'):
        path = root / 'logs' / name
        try:
            with path.open('rb') as stream:
                stream.seek(max(0, path.stat().st_size - 131072))
                rows = stream.read(131072).decode(errors='replace').splitlines()
            selected = [row[:8192] for row in rows if re.search(
                r'argv|command.line|Running|Launching|startup|Starting|Steam Client|build|'
                r'use-angle|use-gl|linux-angle|ANGLE_OPENGL|ANGLE_VULKAN|GL implementation parts|'
                r'GL_RENDERER|GPU process|gpu-process|Vulkan|vulkan|cef-.*(?:gl|angle)|failed to create drawable', row)]
            emit('installed-log-tail-may-include-prior-launches', dict(file=name, rows=selected[-100:]))
        except OSError as error:
            emit('log-unavailable', dict(file=name, error=str(error)))
    print('DROIDDECK_CEF_DIAGNOSTICS_DONE (read-only; no GPU/UI success claim)', flush=True)


if __name__ == '__main__':
    run()
