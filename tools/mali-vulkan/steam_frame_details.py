"""Read-only downstream capability audit. Never consume another process's pipe.

Missing private symbols are a diagnostic gap, never guessed addresses. This
audit does not read process memory or inject code into the renderer.
"""
import json
import mmap
import os
from pathlib import Path
import stat
import struct
import time


def emit(label, value):
    print(label+'='+json.dumps(value, sort_keys=True), flush=True)


def elf_symbols(path):
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('requires little-endian ELF64')
        offset = struct.unpack_from('<Q', data, 40)[0]
        size, count = struct.unpack_from('<HH', data, 58)
        if size != 64 or not 0 < count <= 8192 or offset+size*count > len(data):
            raise ValueError('invalid/extended ELF section table')
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset+i*size) for i in range(count)]
        wanted = ('normalPerf', 'normalOutput', 'maliCpuDetail', 'normal_sequence', 'mb_normal_')
        symbols, tables = [], []
        for section in sections:
            if section[1] not in (2, 11):
                continue
            tables.append('symtab' if section[1] == 2 else 'dynsym')
            if section[9] != 24 or section[5] % 24 or section[6] >= len(sections):
                raise ValueError('invalid symbol table')
            names = sections[section[6]]
            if names[4]+names[5] > len(data) or section[4]+section[5] > len(data):
                raise ValueError('symbol table outside ELF')
            for at in range(section[4], section[4]+section[5], 24):
                name, info, other, shndx, value, length = struct.unpack_from('<IBBHQQ', data, at)
                if name >= names[5]:
                    continue
                begin = names[4]+name
                end = data.find(b'\0', begin, min(begin+512, names[4]+names[5]))
                if end < 0:
                    continue
                text = data[begin:end].decode(errors='replace')
                if text == 'records' or any(part in text for part in wanted):
                    symbols.append(dict(name=text, value=value, size=length, defined=shndx != 0,
                                        type=info & 15, table=tables[-1]))
        return dict(elf_type=struct.unpack_from('<H', data, 16)[0], tables=tables, symbols=symbols[:128])


def clock_anchor(label):
    a = time.monotonic_ns()
    boot = time.clock_gettime_ns(getattr(time, 'CLOCK_BOOTTIME', 7))
    b = time.monotonic_ns()
    emit(label, dict(monotonic_before_ns=a, boottime_ns=boot, monotonic_after_ns=b))


class FrameObserver:
    def __init__(self, inventory):
        self.additional_logs = []
        pipes = {target for row in inventory.values() if row['role'] == 'gamescope'
                 for target in row.get('output_fds', {}).values() if target.startswith('pipe:[')}
        # Observe the logger's regular output file, never its input pipe. This
        # also detects a discarded stream or a rotated/deleted-but-open log.
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                name = proc.joinpath('comm').read_text().strip()
                if name not in ('dd', 'cat', 'tee') or os.readlink(proc/'fd/0') not in pipes:
                    continue
                outputs = []
                for fd in (proc/'fd').iterdir():
                    if fd.name == '0':
                        continue
                    target = os.readlink(fd)
                    if 'session.log' in target and stat.S_ISREG(fd.stat().st_mode):
                        self.additional_logs.append(str(fd))
                        outputs.append(dict(path=str(fd), target=target, inode=fd.stat().st_ino))
                emit('frame-session-log-consumer', dict(pid=int(proc.name), name=name, regular_outputs=outputs,
                     discarded=name == 'cat' and not outputs))
            except OSError:
                continue
        for row in inventory.values():
            if row['role'] not in ('gamescope', 'android-app'):
                continue
            pid = row['pid']
            candidates = set()
            try:
                if row['role'] == 'gamescope':
                    candidates.add(Path('/proc') / str(pid) / 'exe')
                for line in (Path('/proc') / str(pid) / 'maps').read_text().splitlines():
                    parts = line.split(maxsplit=5)
                    if len(parts) == 6 and parts[5].endswith('/usr/local/bin/gamescope'):
                        path = Path('/usr/local/bin/gamescope')
                        if path.stat().st_ino == int(parts[4]):
                            candidates.add(path)
                    if len(parts) == 6 and any(token in parts[5] for token in ('malivulkan', 'mali_broker', 'waylandcomp')):
                        candidates.add(Path(parts[5]))
                for path in candidates:
                    try:
                        info = elf_symbols(path)
                        emit('frame-installed-elf', dict(pid=pid, role=row['role'], path=str(path), **info))
                    except (OSError, ValueError, struct.error) as error:
                        emit('frame-elf-unavailable', dict(pid=pid, path=str(path), error=str(error)))
            except OSError as error:
                emit('frame-process-inspection-unavailable', dict(pid=pid, error=str(error)))
        emit('frame-stage-coverage', dict(existing_cpu_detail_object=False,
             gamescope_commit_frame_ids=False, lossless_shm_scopes=False, output_ahb_acquire_ids=False,
             native_submit_completion_ids=False, android_handoff_release_ids=False,
             missing='private/static or absent trace points cannot be recovered from aggregate logs; no address guessing, ptrace or pipe reads'))

    def poll(self):
        pass  # No private process-memory reads or guessed binary layouts.

    def close(self):
        pass
