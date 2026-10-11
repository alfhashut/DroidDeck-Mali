"""Read-only swap scopes and scheduler observations. No API interception/ptrace.

CPU bounds include sampling uncertainty. Off-CPU time includes scheduling and
all waits; it is never automatically classified as GPU or X11 waiting.
"""
from collections import defaultdict
import ctypes as C
import json
import mmap
import os
from pathlib import Path
import re
import socket
import struct
import time
import uuid


def emit(label, value):
    print(label + '=' + json.dumps(value, sort_keys=True), flush=True)


def union_us(intervals, start, end):
    clipped = sorted((max(start, a), min(end, b)) for a, b in intervals if a < end and b > start)
    total, right = 0, start
    for a, b in clipped:
        if b > max(a, right):
            total += b - max(a, right)
        right = max(right, b)
    return total


def cpu_bounds(samples, start_us, end_us, clock_error_us=0):
    """Conservative ns counter bounds, accounting for each read's time bracket."""
    before_a = [s for s in samples if s['end_ns'] <= (start_us-clock_error_us) * 1000]
    after_a = [s for s in samples if s['begin_ns'] >= (start_us+clock_error_us) * 1000]
    before_b = [s for s in samples if s['end_ns'] <= (end_us-clock_error_us) * 1000]
    after_b = [s for s in samples if s['begin_ns'] >= (end_us+clock_error_us) * 1000]
    if not (before_a and after_a and before_b and after_b):
        return None
    wall = (end_us - start_us) * 1000
    low = max(0, before_b[-1]['cpu_ns'] - after_a[0]['cpu_ns'])
    high = min(wall, max(0, after_b[0]['cpu_ns'] - before_a[-1]['cpu_ns']))
    # A recycled TID or a nonmonotonic scheduler counter invalidates attribution.
    if high < low or after_b[0]['cpu_ns'] < before_a[-1]['cpu_ns']:
        return None
    queue_low = max(0, before_b[-1]['runqueue_ns'] - after_a[0]['runqueue_ns'])
    queue_high = min(wall, max(0, after_b[0]['runqueue_ns'] - before_a[-1]['runqueue_ns']))
    # Runqueue delay is charged when a wait completes, not continuously while
    # runnable. Account for a wait crossing either boundary. A known sleeping
    # observation bounds its earliest possible start; absent one, keep the
    # entire wall interval possible rather than pretending the counter is live.
    def last_not_queued(rows):
        anchors = [s['begin_ns'] for s in rows if s.get('state') in ('S', 'D')]
        # Increased runtime proves execution since the preceding counter read;
        # use that read's beginning, never the later sample's time, as the
        # conservative earliest execution anchor.
        anchors += [x['begin_ns'] for x, y in zip(rows, rows[1:]) if y['cpu_ns'] > x['cpu_ns']]
        return max(anchors) if anchors else None
    anchor_a, anchor_b = last_not_queued(before_a), last_not_queued(before_b)
    carry_a = after_a[0]['end_ns'] - anchor_a if anchor_a is not None else wall
    pending_b = max(0, (end_us+clock_error_us)*1000-anchor_b) if anchor_b is not None else wall
    queue_low = max(0, queue_low-carry_a)
    queue_high = min(wall, queue_high+pending_b)
    return dict(cpu_ms_lower=low / 1e6, cpu_ms_upper=high / 1e6,
                off_cpu_ms_lower=(wall-high) / 1e6, off_cpu_ms_upper=(wall-low) / 1e6,
                runqueue_ms_lower=queue_low / 1e6, runqueue_ms_upper=queue_high / 1e6)


def complete_scopes(events):
    """Reconstruct synchronous B/E scopes per thread; exclude unmatched pairs."""
    stacks, result = defaultdict(list), []
    for event in sorted(events, key=lambda e: (e.get('ts', 0), e.get('_order', 0))):
        key = (event.get('pid'), event.get('tid'))
        phase = event.get('ph')
        if phase == 'X' and isinstance(event.get('dur'), (int, float)):
            result.append(event)
        elif phase == 'B':
            stacks[key].append(event)
        elif phase == 'E' and stacks[key]:
            begin = stacks[key].pop()
            row = dict(begin, ph='X', dur=event['ts'] - begin['ts'])
            if isinstance(begin.get('tts'), (int, float)) and isinstance(event.get('tts'), (int, float)):
                row['tdur'] = event['tts'] - begin['tts']
            if row['dur'] >= 0:
                result.append(row)
    return result


def sleep_queue_bounds(off_cpu_ms, counters):
    """Same-swap bounds, not poll syscall or per-wakeup timing.

    schedstat runqueue includes all runnable scheduling delay in this interval.
    Residual is non-runnable time of any cause; a Mali snapshot alone cannot
    attribute the residual entirely to a single poll.
    """
    if counters is None:
        return None
    low = counters['runqueue_ms_lower']
    high = min(off_cpu_ms, counters['runqueue_ms_upper'])
    if high < low:
        return None
    return dict(runnable_queue_ms_lower=low, runnable_queue_ms_upper=high,
                nonrunnable_ms_lower=max(0, off_cpu_ms-high),
                nonrunnable_ms_upper=max(0, off_cpu_ms-low),
                exact_poll_sleep_ms=None, exact_poll_wakeup_to_run_ms=None)


def scope_kind(name):
    # These are named CPU/API scopes, not hardware GPU execution durations.
    if re.search(r'vkWait|WaitForFences|WaitSemaphores|waitFor.*(?:Serial|Fence)|ClientWaitSync|WaitForGPU', name, re.I):
        return 'gpu-completion-wait-scope'
    if re.search(r'XSync|XFlush|XPutImage|XShmPutImage|XGetImage|XCopyArea|xcb_.*(?:image|flush)|(?:poll|recv|send).*socket', name, re.I):
        return 'x11-or-socket-api-scope'
    if re.search(r'readback|ReadPixels|copy.*(?:cpu|memory)|memcpy|PackPixels', name, re.I):
        return 'readback-or-cpu-copy-scope'
    if re.search(r'GLComposer|CCompositorGL|AcquirePixmap', name, re.I):
        return 'steam-glcomposer-scope'
    if re.search(r'vkQueue(?:Submit|Present)|QueueSubmit|QueuePresent', name, re.I):
        return 'vulkan-submit-present-api-scope'
    return None


class ThreadSampler:
    def __init__(self, pids, interval_ms=5):
        self.paths, self.rows = {}, defaultdict(list)
        self.next = 0
        self.interval_ns = interval_ms * 1000000
        self.handles = {}
        self.mem = {}
        self.unix = unix_peers()
        for pid in pids:
            directory = Path('/proc') / str(pid) / 'task'
            try:
                candidates = list(directory.iterdir())
            except OSError:
                continue
            discovered = []
            for task in candidates:
                try:
                    name = (task / 'comm').read_text().strip()
                    discovered.append(dict(tid=int(task.name), name=name))
                    if task.name != str(pid) and not re.search(r'VizCompositor|GLComposer|CCompositorGL', name):
                        continue
                    info = dict(pid=pid, tid=int(task.name), name=name, path=task, start=None,
                                syscall=True, wchan=True, active=True)
                    status = (task / 'stat').read_text()
                    info['start'] = int(status[status.rindex(')')+2:].split()[19])
                    self.paths[(pid, int(task.name))] = info
                except (OSError, ValueError, IndexError):
                    continue
            emit('swap-process-thread-names', dict(pid=pid, threads=discovered))
        emit('swap-thread-targets', [{k: v for k, v in info.items() if k != 'path'} for info in self.paths.values()])
        # Probe once and preserve permission errors; never retry an inaccessible
        # syscall state at sampling frequency or attach to bypass permissions.
        for info in self.paths.values():
            for name in ('syscall', 'wchan'):
                try:
                    value = (info['path'] / name).read_text().strip()
                    emit('swap-thread-probe', dict(pid=info['pid'], tid=info['tid'], field=name, value=value,
                         available=info[name], note='zero wchan is inconclusive, including while running'))
                except OSError as error:
                    info[name] = False
                    emit('swap-thread-probe', dict(pid=info['pid'], tid=info['tid'], field=name, error=str(error), available=False))
            for name in ('schedstat', 'stat', 'syscall', 'wchan'):
                if name in ('syscall', 'wchan') and not info[name]:
                    continue
                try:
                    self.handles[(info['pid'], info['tid'], name)] = os.open(info['path'] / name, os.O_RDONLY)
                except OSError:
                    pass
            pid = info['pid']
            if pid not in self.mem:
                try:
                    self.mem[pid] = os.open(Path('/proc') / str(pid) / 'mem', os.O_RDONLY)
                    emit('swap-pollfd-reader', dict(pid=pid, available=True,
                         scope='read-only, at most eight pollfd structs at the observed syscall argument; no attach or memory writes'))
                except OSError as error:
                    self.mem[pid] = None
                    emit('swap-pollfd-reader', dict(pid=pid, available=False, error=str(error)))

    def read(self, info, name):
        handle = self.handles[(info['pid'], info['tid'], name)]
        return os.pread(handle, 4096, 0).decode(errors='replace').strip()

    def syscall_detail(self, pid, tid, text):
        fields = text.split()
        if not fields or fields[0] == 'running':
            return None
        try:
            number = int(fields[0])
            args = [int(value, 0) for value in fields[1:7]]
            # AArch64 syscall numbers from Linux asm-generic/unistd.h.
            names = {29: 'ioctl', 63: 'read', 64: 'write', 65: 'readv', 66: 'writev',
                     73: 'ppoll', 98: 'futex', 206: 'sendto', 207: 'recvfrom',
                     211: 'sendmsg', 212: 'recvmsg'}
            result = dict(number=number, name=names.get(number, 'other'), fds=[])
            fds = []
            poll_events = {}
            if number == 73 and self.mem.get(pid) is not None and len(args) >= 2:
                count = min(args[1], 8)
                payload = os.pread(self.mem[pid], count*8, args[0])
                if len(payload) == count*8:
                    poll_records = [struct.unpack_from('=ihh', payload, i*8) for i in range(count)]
                    fds = [record[0] for record in poll_records]
                    poll_events = {fd: events for fd, events, _ in poll_records}
                    result['pollfd_count'] = args[1]
                    result['pollfd_count_capped'] = args[1] > 8
            elif number in (29, 63, 64, 65, 66, 206, 207, 211, 212) and args:
                fds = [args[0]]
            for fd in fds:
                if fd < 0:
                    continue
                try:
                    target = os.readlink(Path('/proc') / str(pid) / 'fd' / str(fd))
                    peer = None
                    kind = 'other'
                    if 'sync_file' in target:
                        kind = 'gpu-sync-file-candidate'
                    elif target == '/dev/mali0':
                        kind = 'mali-device'
                    elif target.startswith('socket:['):
                        inode = int(target[len('socket:['):-1])
                        info = self.unix.get(inode, {})
                        peer = self.unix.get(info.get('peer'), {}).get('name')
                        kind = 'x11-socket' if peer and '.X11-unix/' in peer else 'unclassified-socket'
                    result['fds'].append(dict(fd=fd, target=target, peer=peer, kind=kind,
                                             poll_events=poll_events.get(fd)))
                except OSError:
                    result['fds'].append(dict(fd=fd, unavailable=True))
            # Syscall arguments point into a running process. Discard attribution
            # if the task left/re-entered a different syscall during the read.
            try:
                again = self.read(self.paths[(pid, tid)], 'syscall')
                result['stable_observation'] = again == text
                if again != text:
                    result['fds'] = []
            except (OSError, KeyError):
                result['stable_observation'] = False
                result['fds'] = []
            return result
        except (OSError, ValueError, IndexError, OverflowError) as error:
            return dict(error=str(error))

    def poll(self, force=False):
        now = time.monotonic_ns()
        if not force and now < self.next:
            return
        self.next = now + self.interval_ns
        for key, info in self.paths.items():
            if not info['active'] or len(self.rows[key]) >= 8000:
                continue
            begin = time.monotonic_ns()
            try:
                fields = self.read(info, 'schedstat').split()
                status = self.read(info, 'stat')
                remaining = status[status.rindex(')')+2:].split()
                if int(remaining[19]) != info['start']:
                    continue
                row = dict(begin_ns=begin, cpu_ns=int(fields[0]), runqueue_ns=int(fields[1]),
                           state=remaining[0], last_cpu=int(remaining[36]), affinity=sorted(os.sched_getaffinity(info['tid'])))
                for name in ('syscall', 'wchan'):
                    if info[name]:
                        try:
                            row[name] = self.read(info, name)
                            if name == 'syscall':
                                row['syscall_detail'] = self.syscall_detail(info['pid'], info['tid'], row[name])
                        except (OSError, KeyError):
                            info[name] = False
                row['end_ns'] = time.monotonic_ns()
                self.rows[key].append(row)
            except (OSError, ValueError, IndexError, KeyError) as error:
                if not self.rows[key]:
                    emit('swap-scheduler-unavailable', dict(pid=key[0], tid=key[1], error=str(error)))
                info['syscall'] = info['wchan'] = False
                info['active'] = False

    def close(self):
        for handle in list(self.handles.values()) + [value for value in self.mem.values() if value is not None]:
            os.close(handle)
        self.handles.clear()
        self.mem.clear()

    def report(self):
        for (pid, tid), rows in self.rows.items():
            gaps = [b['begin_ns']-a['end_ns'] for a, b in zip(rows, rows[1:])]
            emit('swap-thread-samples', dict(pid=pid, tid=tid, samples=rows,
                 max_sample_gap_ms=max(gaps)/1e6 if gaps else None,
                 meaning='schedstat CPU/runqueue counters and instantaneous states; not syscall durations'))


def unix_peers():
    """Read Linux UNIX_DIAG peer/name metadata; no socket connection changes."""
    result = {}
    try:
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 4) as sock:
            sock.settimeout(0.3)
            sock.bind((0, 0))
            request = struct.pack('=BBHIIIII', socket.AF_UNIX, 0, 0, 0xffffffff, 0, 5, 0xffffffff, 0xffffffff)
            sock.sendto(struct.pack('=IHHII', 16+len(request), 20, 0x301, 1, 0) + request, (0, 0))
            for _ in range(64):
                data = sock.recv(65536)
                pos = 0
                while pos+16 <= len(data):
                    length, kind, _, _, _ = struct.unpack_from('=IHHII', data, pos)
                    if length < 16 or pos+length > len(data):
                        raise ValueError('invalid UNIX_DIAG message length')
                    if kind == 3:
                        return result
                    if kind == 2:
                        raise OSError('UNIX_DIAG error ' + str(struct.unpack_from('=i', data, pos+16)[0]))
                    if kind == 20 and length >= 32:
                        inode = struct.unpack_from('=I', data, pos+20)[0]
                        row = {}
                        at = pos+32
                        while at+4 <= pos+length:
                            size, attr = struct.unpack_from('=HH', data, at)
                            if size < 4 or at+size > pos+length:
                                break
                            value = data[at+4:at+size]
                            if attr == 0:
                                row['name'] = value.rstrip(b'\0').replace(b'\0', b'@').decode(errors='replace')
                            elif attr == 2 and len(value) == 4:
                                row['peer'] = struct.unpack('=I', value)[0]
                            at += (size+3) & ~3
                        result[inode] = row
                    pos += (length+3) & ~3
        return result
    except (OSError, ValueError) as error:
        emit('swap-unix-peer-discovery-unavailable', str(error))
        return {}


class SwapDetails:
    def __init__(self):
        self.events = []
        self.capped = False
        self.clock_id = uuid.uuid4().hex
        self.anchor = None
        self.thread_sampler = None
        self.complete = False
        self.loss = None
        self.xclock = None
        self.sample_window = None

    def add(self, events):
        for event in events:
            # Keep all synchronous begin/end events so nesting can be reconstructed
            # correctly, even when an intermediate scope has an unrelated name.
            if event.get('ph') not in ('X', 'B', 'E', 'M', 'c', 'I', 'i'):
                continue
            if len(self.events) >= 160000:
                self.capped = True
                continue
            row = {key: event[key] for key in ('pid', 'tid', 'name', 'cat', 'ph', 'ts', 'dur', 'tts', 'tdur') if key in event}
            row['_order'] = len(self.events)
            args = event.get('args', {})
            # Avoid retaining page contents/URLs. Clock/thread/frame IDs only.
            allowed = ('name', 'sync_id', 'syncId', 'issue_ts', 'frame_token', 'frame_id', 'source_frame_number')
            row['args'] = {key: args[key] for key in allowed if isinstance(args.get(key), (int, float, str))}
            self.events.append(row)

    def report(self, windows):
        scopes = complete_scopes(self.events)
        anchor = None
        for event in self.events:
            args = event.get('args', {})
            if self.clock_id in (args.get('sync_id'), args.get('syncId')) and isinstance(event.get('ts'), (int, float)):
                anchor = event['ts']
                break
        offset = None
        error_us = None
        if anchor is not None and self.anchor:
            a, b = self.anchor
            offset = anchor - (a+b)/2000
            error_us = (b-a)/2000
        emit('swap-trace-integrity', dict(complete=self.complete, data_loss=self.loss, event_cap_hit=self.capped,
             clock_marker_ts_us=anchor, trace_minus_monotonic_us=offset, clock_error_us=error_us,
             clocks_aligned=offset is not None, retained_events=len(self.events)))
        names = sorted({scope['name'] for scope in scopes if scope_kind(scope.get('name', '')) or
                        re.search(r'eglSwap|SwapBuffers|glReadPixels', scope.get('name', ''), re.I)})
        emit('swap-observed-api-scopes', names)
        # Preserve exact scope timestamps/CPU durations for offline inspection.
        relevant = [scope for scope in scopes if scope_kind(scope.get('name', '')) or
                    re.search(r'eglSwap|SwapBuffers|glReadPixels', scope.get('name', ''), re.I)]
        emit('swap-api-timestamps', dict(scopes=relevant, meaning='built-in trace scopes only; missing APIs are uninstrumented, not zero-cost'))
        swaps = [scope for scope in scopes if scope.get('name') == 'SkiaOutputSurfaceImplOnGpu::SwapBuffers']
        for swap in swaps[:1024]:
            a, b = swap['ts'], swap['ts']+swap['dur']
            if offset is not None and self.sample_window:
                if a-offset-error_us < self.sample_window[0]/1000 or b-offset+error_us > self.sample_window[1]/1000:
                    continue
            key = (swap['pid'], swap['tid'])
            nested, intervals = [], defaultdict(list)
            for scope in relevant:
                if (scope['pid'], scope['tid']) != key or scope is swap:
                    continue
                x, y = scope['ts'], scope['ts']+scope['dur']
                if x >= a and y <= b:
                    nested.append(scope)
                    if scope_kind(scope['name']):
                        intervals[scope_kind(scope['name'])].append((x, y))
            cpu = None
            if isinstance(swap.get('tdur'), (int, float)):
                cpu = dict(trace_thread_cpu_ms=swap['tdur']/1000,
                           trace_off_cpu_ms=max(0, swap['dur']-swap['tdur'])/1000)
            approximate = None
            arrivals = None
            observations = None
            if offset is not None and self.thread_sampler:
                # Widen counter brackets, not the swap's wall duration.
                approximate = cpu_bounds(self.thread_sampler.rows.get(key, []), a-offset, b-offset, error_us)
                observations = [row for row in self.thread_sampler.rows.get(key, [])
                                if row['begin_ns']/1000 >= a-offset+error_us
                                and row['end_ns']/1000 <= b-offset-error_us]
                if windows and self.xclock:
                    xa, xb, server_ms = self.xclock
                    mono_us = (xa+xb)/2000
                    uncertainty = (xb-xa)/2000 + error_us + 1000  # integer-ms X server time
                    arrivals = {}
                    for window, values in windows.timestamps.items():
                        matching = []
                        for ts in values:
                            delta_ms = ((ts-server_ms + 2**31) % 2**32) - 2**31
                            mapped_us = mono_us + delta_ms*1000
                            if a-offset-uncertainty <= mapped_us <= b-offset+uncertainty:
                                matching.append(ts)
                        arrivals[window] = matching
            emit('swap-sample', dict(pid=key[0], tid=key[1], trace_begin_us=a, trace_end_us=b,
                 slow=swap['dur'] >= 20000,
                 wall_ms=swap['dur']/1000, exact_trace_cpu=cpu, sampled_cpu_bounds=approximate,
                 sleep_vs_queue_bounds=sleep_queue_bounds(cpu['trace_off_cpu_ms'], approximate) if cpu else None,
                 named_scope_union_ms={name: union_us(values, a, b)/1000 for name, values in intervals.items()},
                 nested_api_scopes=nested, x11_update_server_ms_candidates=arrivals,
                 thread_state_observations=observations,
                 frame_correlation='timestamp overlap only, with clock uncertainty; no common frame ID',
                 gamescope_import='per-frame timestamp unavailable in current session instrumentation',
                 android_release='current native log gives cumulative two-second windows, not per-frame release timestamps'))
        if self.thread_sampler:
            self.thread_sampler.report()
        emit('swap-diagnostic-limits', dict(
             gpu_execution='no hardware GPU timestamp queries injected; API wait duration does not equal GPU execution time',
             x11_socket='syscall/pollfd snapshots and UNIX_DIAG peer metadata where permitted; observations are not exact syscall durations',
             steam_glcomposer='reported only if an existing named trace scope is emitted; missing scope does not exclude this path',
             missing_api='no LD_PRELOAD/ptrace injection or binary changes; EGL/Vulkan/Xlib hooks absent from the build cannot be timed exactly'))


def xserver_clock(windows):
    """Read XSync SERVERTIME; no helper window/property or rendering changes."""
    class Value(C.Structure):
        _fields_ = [('hi', C.c_int), ('lo', C.c_uint)]

    class Counter(C.Structure):
        _fields_ = [('name', C.c_char_p), ('counter', C.c_ulong), ('resolution', Value)]

    from diagnose_steam_x11 import bind, I, P, U
    lib = C.CDLL('libXext.so.6')
    bind(lib, 'XSyncInitialize', I, P, C.POINTER(I), C.POINTER(I))
    bind(lib, 'XSyncListSystemCounters', C.POINTER(Counter), P, C.POINTER(I))
    bind(lib, 'XSyncFreeSystemCounterList', None, C.POINTER(Counter))
    bind(lib, 'XSyncQueryCounter', I, P, U, C.POINTER(Value))
    major, minor, count = I(), I(), I()
    if not lib.XSyncInitialize(windows.x.d, C.byref(major), C.byref(minor)):
        raise RuntimeError('XSync extension unavailable')
    counters = lib.XSyncListSystemCounters(windows.x.d, C.byref(count))
    try:
        for item in counters[:min(count.value, 64)]:
            if item.name == b'SERVERTIME':
                value = Value()
                a = time.monotonic_ns()
                ok = lib.XSyncQueryCounter(windows.x.d, item.counter, C.byref(value))
                b = time.monotonic_ns()
                if not ok:
                    raise RuntimeError('XSyncQueryCounter failed')
                server_ms = (value.hi << 32) | value.lo
                emit('swap-xserver-clock-anchor', dict(begin_ns=a, end_ns=b, server_ms=server_ms))
                return a, b, server_ms
    finally:
        if counters:
            lib.XSyncFreeSystemCounterList(counters)
    raise RuntimeError('XSync SERVERTIME counter unavailable')


def inspect_libraries(pid):
    tokens = ('gpu.angle', 'gpu.vulkan', 'eglSwapBuffers', 'vkQueueSubmit', 'vkWaitForFences',
              'vkWaitSemaphores', 'vkQueuePresentKHR', 'ReadPixels', 'XPutImage', 'XShmPutImage', 'GLComposer')
    try:
        paths = set()
        for line in (Path('/proc') / str(pid) / 'maps').read_text().splitlines():
            parts = line.split(maxsplit=5)
            if len(parts) != 6 or not re.search(r'lib(?:EGL|GLES|cef|vulkan)', parts[5]):
                continue
            path = Path(parts[5])
            if not path.is_file() and '/Steam/' in str(path):
                path = Path('/root/.local/share/Steam') / str(path).split('/Steam/', 1)[1]
            if path.is_file():
                paths.add(path)
        for path in sorted(paths):
            with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                found = [token for token in tokens if data.find(token.encode()) >= 0]
                emit('swap-installed-library-tokens', dict(pid=pid, path=str(path), size=len(data), tokens=found,
                     meaning='string presence is not proof an API tracing hook is active'))
    except OSError as error:
        emit('swap-library-inspection-unavailable', str(error))
