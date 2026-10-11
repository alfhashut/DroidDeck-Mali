#!/usr/bin/env python3
"""Opt-in, bounded live Steam sampler; no renderer or session configuration changes.

CPU percentages use one core = 100%. XDamage batches and Chromium trace events
are observations, not a claim that every event becomes a displayed GPU frame.
"""
import argparse
from collections import Counter
import ctypes as C
import json
import os
from pathlib import Path
import re
import runpy
import select
import stat
import time
from urllib.parse import urlsplit


def emit(label, value):
    print(label + '=' + json.dumps(value, sort_keys=True), flush=True)


def stat_fields(text):
    # comm can contain spaces and parentheses. The fields after it start at 3.
    fields = text[text.rindex(')') + 2:].split()
    return dict(ppid=int(fields[1]), ticks=int(fields[11]) + int(fields[12]),
                start=int(fields[19]), name=text[text.index('(') + 1:text.rindex(')')])


def cpu_delta(previous, current, seconds, hz):
    if not previous or current['start'] != previous['start'] or seconds <= 0:
        return None
    ticks = current['ticks'] - previous['ticks']
    return 100 * ticks / hz / seconds if ticks >= 0 else None


def processes(extra_pids=()):
    result = {}
    for directory in Path('/proc').iterdir():
        if not directory.name.isdigit():
            continue
        try:
            argv = (directory / 'cmdline').read_bytes().decode(errors='replace').rstrip('\0').split('\0')
            names = [Path(arg).name for arg in argv[:3]]
            role = next((name for name in ('steamwebhelper', 'steam', 'Xwayland', 'gamescope', 'proot')
                         if name in names), None)
            if 'libproot.so' in names:
                role = 'proot'
            if int(directory.name) == os.getpid():
                role = 'perf-sampler'
            if int(directory.name) in extra_pids:
                role = 'affinity-watchdog'
            if argv and argv[0] == 'com.droiddeck.launcher':
                role = 'android-app'
            if not role:
                continue
            row = stat_fields((directory / 'stat').read_text())
            row.update(pid=int(directory.name), role=role, argv=argv)
            if role == 'gamescope':
                row['output_fds'] = {str(fd): os.readlink(directory / 'fd' / str(fd)) for fd in (1, 2)}
            result[directory.name] = row
            if role == 'android-app':
                # Broker is a subset of app CPU; retain all thread names so its
                # logcat TID can be correlated without guessing a thread name.
                for task in (directory / 'task').iterdir():
                    try:
                        thread = stat_fields((task / 'stat').read_text())
                        thread.update(pid=int(directory.name), tid=int(task.name), role='android-thread')
                        result[directory.name + '/' + task.name] = thread
                    except OSError:
                        pass
        except (OSError, ValueError, IndexError):
            continue
    return result


class TraceCounts:
    frame_names = {'DrawFrame', 'MainFrame.SubmitCompositorFrame',
                   'SkiaOutputSurfaceImplOnGpu::SwapBuffers', 'DirectRenderer::DrawFrame'}

    def __init__(self):
        self.rows = {}
        self.total = 0
        self.frames = {}
        self.details = None

    def add(self, events):
        if self.details:
            self.details.add(events)
        for event in events:
            self.total += 1
            name = event.get('name', '')
            if not re.search(r'frame|commit|swap|present|submit|wait|draw|gpu|read|copy|flush|fence|raster', name, re.I):
                continue
            phase = event.get('ph')
            # End events are not another frame. Keep phases distinct for async
            # flows and nested events; never add different event names as FPS.
            if phase not in ('B', 'X', 'I', 'i', 'b', 'R'):
                continue
            key = (event.get('pid'), event.get('tid'), name, phase)
            if len(self.rows) >= 2048 and key not in self.rows:
                continue
            row = self.rows.setdefault(key, dict(count=0, first_us=None, last_us=None, duration_us=0))
            row['count'] += 1
            ts = event.get('ts')
            if isinstance(ts, (int, float)):
                row['first_us'] = ts if row['first_us'] is None else min(row['first_us'], ts)
                row['last_us'] = ts if row['last_us'] is None else max(row['last_us'], ts)
            if phase == 'X':
                row['duration_us'] += event.get('dur', 0)
            if name in self.frame_names and isinstance(ts, (int, float)):
                sequence = self.frames.setdefault(key, [])
                if len(sequence) < 1024:
                    args = event.get('args', {})
                    candidates = dict(args)
                    if isinstance(args.get('data'), dict):
                        candidates.update(args['data'])
                    ids = {k: v for k, v in candidates.items()
                           if re.search(r'^(?:frame_?id|frame_token|source_frame_number|trace_id)$', k, re.I)
                           and isinstance(v, (int, str))}
                    sequence.append(dict(ts_us=ts, duration_us=event.get('dur'), ids=ids))

    def report(self, seconds):
        for (pid, tid, name, phase), row in sorted(self.rows.items(), key=lambda item: str(item[0])):
            span = ((row['last_us'] - row['first_us']) / 1e6
                    if row['last_us'] is not None else 0)
            emit('cef-trace-event', dict(pid=pid, tid=tid, name=name, phase=phase,
                 retained_span_s=span, retained_span_event_hz=(row['count'] - 1) / span if span > 0 else None,
                 count_div_sample_s_lower_bound=row['count'] / seconds, **row))
        for (pid, tid, name, phase), sequence in self.frames.items():
            times = sorted(item['ts_us'] for item in sequence)
            intervals = sorted((b - a) / 1000 for a, b in zip(times, times[1:]))
            emit('cef-frame-timestamps', dict(pid=pid, tid=tid, name=name, phase=phase,
                 interval_p50_ms=intervals[len(intervals)//2] if intervals else None,
                 interval_p95_ms=intervals[int((len(intervals)-1)*0.95)] if intervals else None,
                 samples=sequence, capped=len(sequence) == 1024,
                 meaning='trace event timestamps/available IDs; duplicate IDs are not proof of repeated scanout'))


class Trace:
    def __init__(self, port, swap_details=False, frame_details=False):
        self.ws = None
        self.started = False
        self.done = False
        self.ident = 0
        self.counts = TraceCounts()
        self.frame_details = frame_details
        if swap_details:
            from steam_swap_details import SwapDetails
            self.counts.details = SwapDetails()
        api = runpy.run_path('/usr/local/bin/droiddeck-steam-compat', run_name='perf_cdp_transport')
        version = api['http_get'](port, '/json/version')
        url = urlsplit(version['webSocketDebuggerUrl'])
        if url.hostname not in ('127.0.0.1', 'localhost') or url.port != port:
            raise RuntimeError('refusing nonlocal CEF debugger')
        self.ws = api['WebSocket'](port, url.path)
        self.ws.sock.settimeout(2)

    def receive(self, timeout=0):
        if not self.ws.pending() and not select.select([self.ws], [], [], timeout)[0]:
            return None
        message = json.loads(self.ws.recv())
        if message.get('method') == 'Tracing.dataCollected':
            self.counts.add(message['params'].get('value', []))
        elif message.get('method') == 'Tracing.tracingComplete':
            self.done = True
            if self.counts.details:
                self.counts.details.complete = True
                self.counts.details.loss = message.get('params', {}).get('dataLossOccurred')
            emit('cef-trace-complete', message.get('params', {}))
        return message

    def call(self, method, params=None):
        self.ident += 1
        mine = self.ident
        self.ws.send(json.dumps(dict(id=mine, method=method, params=params or {})))
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            message = self.receive(0.1)
            if message and message.get('id') == mine:
                if 'error' in message:
                    raise RuntimeError(str(message['error']))
                return message.get('result', {})
        raise TimeoutError(method)

    def start(self):
        categories = ['cc', 'viz', 'benchmark', 'disabled-by-default-devtools.timeline.frame']
        details = self.counts.details
        if details:
            available = self.call('Tracing.getCategories').get('categories', [])
            emit('swap-available-trace-categories', [c for c in available if re.search(r'gpu|angle|vulkan|viz|skia', c, re.I)])
            requested = ['cc', 'disabled-by-default-devtools.timeline.frame',
                         'viz', 'gpu', 'gpu.angle', 'gpu.vulkan', 'disabled-by-default-gpu.angle',
                         'disabled-by-default-gpu.service', 'disabled-by-default-gpu.device',
                         'disabled-by-default-viz.gpu_composite_time']
            categories = [c for c in requested if c in available]
            if not categories:
                raise RuntimeError('installed CEF exposes no requested GPU/viz trace categories')
            try:
                info = self.call('SystemInfo.getProcessInfo').get('processInfo', [])
                emit('swap-cef-process-roles', info)
                try:
                    emit('swap-cef-gpu-info', self.call('SystemInfo.getInfo').get('gpu', {}))
                except (OSError, RuntimeError, TimeoutError) as error:
                    emit('swap-cef-gpu-info-unavailable', str(error))
                pids = [int(row['id']) for row in info if row.get('type', '').lower() in ('gpu', 'browser')]
                pids += [row['pid'] for row in processes().values()
                         if row['role'] == 'steam' and row['name'] == 'steam']
                if self.frame_details:
                    pids += [row['pid'] for row in processes().values() if row['role'] in ('gamescope', 'Xwayland')]
                from steam_swap_details import ThreadSampler, inspect_libraries
                details.thread_sampler = ThreadSampler(pids, interval_ms=100 if self.frame_details else 5)
                for row in info:
                    if row.get('type', '').lower() == 'gpu':
                        inspect_libraries(int(row['id']))
            except (OSError, ValueError, RuntimeError, TimeoutError) as error:
                emit('swap-thread-discovery-unavailable', str(error))
        self.call('Tracing.start', dict(transferMode='ReportEvents', traceConfig=dict(
            recordMode='recordContinuously', traceBufferSizeInKb=16384,
            includedCategories=categories)))
        self.started = True
        if details:
            # Keep the least-latency of three markers; every marker remains a
            # bracket, never an assumed equality between separate clocks.
            best = None
            for attempt in range(3):
                sync_id = details.clock_id + '-' + str(attempt)
                a = time.monotonic_ns()
                try:
                    self.call('Tracing.recordClockSyncMarker', dict(syncId=sync_id))
                    b = time.monotonic_ns()
                    if best is None or b-a < best[1]-best[0]:
                        best = (a, b, sync_id)
                except (OSError, RuntimeError, TimeoutError) as error:
                    emit('swap-clock-anchor-unavailable', str(error))
                    break
            if best:
                a, b, details.clock_id = best
                details.anchor = (a, b)
                emit('swap-clock-anchor', dict(sync_id=details.clock_id, begin_ns=a, end_ns=b))
        emit('cef-trace-start', dict(buffer_kib=16384, categories=categories,
                                    durations='CPU trace scopes, not PanVK GPU timestamps'))

    def close(self):
        try:
            # Do not stop somebody else's trace when Tracing.start was refused.
            if self.started:
                self.call('Tracing.end')
                deadline = time.monotonic() + 18
                while not self.done and time.monotonic() < deadline:
                    self.receive(0.1)
                if not self.done:
                    emit('cef-trace-incomplete', 'timeout; counts may be partial')
        finally:
            if self.ws:
                self.ws.close()
            details = getattr(getattr(self, 'counts', None), 'details', None)
            if details and details.thread_sampler:
                details.thread_sampler.close()


class DamageEvent(C.Structure):
    _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                ('display', C.c_void_p), ('drawable', C.c_ulong), ('damage', C.c_ulong),
                ('level', C.c_int), ('more', C.c_int), ('timestamp', C.c_ulong),
                ('area', C.c_short * 4), ('geometry', C.c_short * 4)]


class Windows:
    def __init__(self):
        from diagnose_steam_x11 import X11, bind, I, P, U
        self.x = X11()
        self.damage = C.CDLL('libXdamage.so.1')
        bind(self.damage, 'XDamageQueryExtension', I, P, C.POINTER(I), C.POINTER(I))
        bind(self.damage, 'XDamageCreate', U, P, U, I)
        bind(self.damage, 'XDamageDestroy', None, P, U)
        bind(self.x.x, 'XPending', I, P)
        bind(self.x.x, 'XNextEvent', I, P, P)
        base, error = I(), I()
        if not self.damage.XDamageQueryExtension(self.x.d, C.byref(base), C.byref(error)):
            self.x.close()
            raise RuntimeError('XDamage unavailable')
        self.base = base.value
        self.objects = {}
        self.counts = Counter()
        self.batches = Counter()
        self.last = {}
        self.timestamps = {}

    def snapshot(self):
        rows, pending, seen = [], [self.x.root], set()
        while pending and len(seen) < 256:
            window = pending.pop()
            if window in seen:
                continue
            seen.add(window)
            pending.extend(self.x.children(window))
            pid = self.x.property(window, '_NET_WM_PID')
            if not pid:
                continue
            a, errors = self.x.attributes(window)
            if not a:
                continue
            rows.append(dict(window=hex(window), pid=pid, map_state=a.map_state,
                             width=a.width, height=a.height, depth=a.depth,
                             visual=hex(a.visual.contents.visualid) if a.visual else None))
            if a.class_ == 1 and window not in self.objects:
                # RawRectangles observer only: no redirect, pixmap, GLX context,
                # subtract or changes to Gamescope's damage bookkeeping.
                damage, errors = self.x.checked(self.damage.XDamageCreate, self.x.d, window, 0)
                if damage and not errors:
                    self.objects[window] = damage
        emit('x11-window-snapshot', dict(received_ns=time.monotonic_ns(), windows=rows))

    def drain(self):
        event = (C.c_long * 24)()
        for _ in range(10000):
            if not self.x.x.XPending(self.x.d):
                break
            self.x.x.XNextEvent(self.x.d, C.byref(event))
            value = C.cast(C.byref(event), C.POINTER(DamageEvent)).contents
            if value.type == self.base:
                window = hex(value.drawable)
                self.counts[window] += 1
                if self.last.get(window) != value.timestamp:
                    self.batches[window] += 1
                    self.last[window] = value.timestamp
                    sequence = self.timestamps.setdefault(window, [])
                    if len(sequence) < 2048:
                        sequence.append(int(value.timestamp))

    def close(self):
        for value in self.objects.values():
            self.x.checked(self.damage.XDamageDestroy, self.x.d, value)
        self.x.close()


class LogTail:
    """Read regular files only; never consume a live stdout pipe/socket."""
    pattern = re.compile(r'MaliPerf |Mali Steam X11 |KBASE_IOCTL_MEM_JIT_INIT|\[KFAULT\]|'
                         r'BadMatch|BadDrawable|GLXBadPixmap|AcquirePixmap|failed to create drawable|'
                         r'GL implementation parts|GL_RENDERER|Display type|vulkan:|ANGLE.*Mali')

    def __init__(self, path):
        self.path = str(path)
        self.stream = os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK), 'rb')
        info = os.fstat(self.stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            self.stream.close()
            raise ValueError('not a regular log file')
        self.offset = info.st_size
        self.stream.seek(max(0, self.offset - 65536))
        tail = self.stream.read().decode(errors='replace').splitlines()
        emit('log-baseline-tail-historical', dict(path=self.path, size=self.offset, mtime_ns=info.st_mtime_ns,
             rows=[line[:4096] for line in tail if self.pattern.search(line)][-20:]))
        self.pending = b''
        self.bytes = 0

    def poll(self):
        # Bound work even if a broken client is flooding its log.
        data = self.stream.read(262144)
        self.bytes += len(data)
        parts = (self.pending + data).split(b'\n')
        self.pending = parts.pop()[-8192:]
        selected = [line.decode(errors='replace')[:8192] for line in parts
                    if self.pattern.search(line.decode(errors='replace'))]
        if selected:
            emit('fresh-log-lines', dict(path=self.path, received_ns=time.monotonic_ns(), rows=selected))

    def close(self):
        self.poll()
        emit('log-sample-coverage', dict(path=self.path, start_offset=self.offset,
             bytes_read=self.bytes, end_size=os.fstat(self.stream.fileno()).st_size,
             unread_bytes=max(0, os.fstat(self.stream.fileno()).st_size - self.stream.tell())))
        self.stream.close()


def run(seconds, swap_details=False, client_affinity=None, frame_details=False):
    from run_panvk_phone_test_impl import client_environment
    client = client_environment()
    os.environ.update({key: client[key] for key in ('DISPLAY', 'XAUTHORITY') if key in client})
    initial = processes()
    emit('perf-metadata', dict(seconds=seconds, hz=os.sysconf('SC_CLK_TCK'),
         clock='monotonic_ns; trace ts is Chromium microseconds; XDamage timestamp is X server milliseconds',
         environment={key: client.get(key) for key in ('DISPLAY', 'BL_LOG', 'BL_DEBUG_DIR')},
         limits='XDamage update batches are not commits; Android presented is buffer handoff, not scanout; no direct PanVK timing'))
    emit('process-inventory', initial)
    frame_observer = None
    if frame_details:
        from steam_frame_details import FrameObserver
        frame_observer = FrameObserver(initial)
    logs = []
    log_paths = [client.get('BL_LOG', '')]
    if frame_observer:
        log_paths += frame_observer.additional_logs
    root = Path(client.get('HOME', '/root')) / '.local/share/Steam/logs'
    log_paths += [str(root / name) for name in ('webhelper.txt', 'steamwebhelper.log', 'cef_log.txt', 'webhelper_gpu.txt')]
    for row in initial.values():
        if row['role'] == 'gamescope':
            for fd, target in row.get('output_fds', {}).items():
                if target.startswith('/'):
                    log_paths.append('/proc/' + str(row['pid']) + '/fd/' + fd)
    for path in dict.fromkeys(log_paths):
        if not path:
            continue
        try:
            logs.append(LogTail(path))
        except (OSError, ValueError) as error:
            emit('log-unavailable', dict(path=path, error=str(error)))
    port = None
    for row in initial.values():
        argv = row.get('argv', [])
        if row['role'] == 'steam' and '-devtools-port' in argv:
            port = int(argv[argv.index('-devtools-port') + 1])
    trace, windows = None, None
    try:
        if port:
            trace = Trace(port, swap_details=swap_details, frame_details=frame_details)
            trace.start()
        else:
            emit('cef-trace-unavailable', 'no existing Steam devtools port; no restart/config change attempted')
    except Exception as error:
        emit('cef-trace-unavailable', str(error))
        if trace:
            trace.close()
        trace = None
    try:
        windows = Windows()
        windows.snapshot()
        windows.drain()  # Exclude observer-creation initial full-window damage.
        windows.counts.clear()
        windows.batches.clear()
        windows.last.clear()
        windows.timestamps.clear()
    except Exception as error:
        emit('x11-update-observer-unavailable', str(error))
        if windows:
            windows.close()
        windows = None
    before = processes()
    detail = trace.counts.details if trace else None
    sampler = detail.thread_sampler if detail else None
    if detail and windows:
        from steam_swap_details import xserver_clock
        try:
            detail.xclock = xserver_clock(windows)
        except (OSError, RuntimeError) as error:
            emit('swap-xserver-clock-unavailable', str(error))
    experiment = None
    if client_affinity:
        try:
            if not (detail and sampler and trace.started):
                raise RuntimeError('refusing affinity change without active swap trace and scheduler sampler')
            from steam_affinity import Experiment
            experiment = Experiment(client, client_affinity, seconds)
            before = processes(extra_pids=(experiment.pid,))
        except BaseException:
            if frame_observer:
                frame_observer.close()
            for log in logs:
                log.close()
            if windows:
                windows.close()
            if trace:
                trace.close()
            raise
    if sampler:
        sampler.poll(force=True)
    start = previous_time = time.monotonic()
    if detail:
        detail.sample_window = (int(start*1e9), None)
    emit('PERF_SAMPLING_START', dict(monotonic_ns=time.monotonic_ns(), realtime_ns=time.time_ns()))
    if frame_details:
        from steam_frame_details import clock_anchor
        clock_anchor('frame-clock-start')
    try:
        for index in range(seconds):
            deadline = start + index + 1
            while time.monotonic() < deadline:
                if windows:
                    windows.drain()
                if sampler:
                    sampler.poll()
                if frame_observer:
                    frame_observer.poll()
                time.sleep(min(0.005 if swap_details and not frame_details else 0.02, max(0, deadline - time.monotonic())))
            current = processes(extra_pids=(experiment.pid,) if experiment else ())
            now = time.monotonic()
            rows = []
            for key, row in current.items():
                cpu = cpu_delta(before.get(key), row, now - previous_time, os.sysconf('SC_CLK_TCK'))
                if cpu is not None and (row['role'] != 'android-thread' or cpu >= 0.5):
                    rows.append(dict(key=key, role=row['role'], name=row['name'], cpu_percent=cpu))
            emit('cpu-sample', dict(received_ns=time.monotonic_ns(), elapsed_s=now-start,
                                   interval_s=now-previous_time, rows=rows))
            before, previous_time = current, now
            for log in logs:
                log.poll()
            if windows and index % 5 == 4:
                windows.snapshot()
    finally:
        elapsed = time.monotonic() - start
        if detail:
            detail.sample_window = (detail.sample_window[0], time.monotonic_ns())
        if sampler:
            sampler.poll(force=True)
        emit('PERF_SAMPLING_END', dict(monotonic_ns=time.monotonic_ns(), realtime_ns=time.time_ns(), elapsed_s=elapsed))
        restore_error = None
        if experiment:
            try:
                experiment.close()  # Restore before trace draining/report generation.
            except RuntimeError as error:
                restore_error = error
        if frame_details:
            clock_anchor('frame-clock-end')
        if frame_observer:
            frame_observer.close()
        for log in logs:
            log.close()
        if windows:
            windows.drain()
            emit('x11-damage-observation', dict(elapsed_s=elapsed, rectangles=dict(windows.counts),
                 distinct_server_ms_batches=dict(windows.batches), last_server_ms=windows.last,
                 server_ms_timestamps=windows.timestamps,
                 meaning='update batches, not Xwayland commits, CEF GPU frames or displayed frames'))
            windows.close()
        if trace:
            try:
                trace.close()
            except Exception as error:
                emit('cef-trace-error', str(error))
            trace.counts.report(elapsed)
            emit('cef-trace-total-events', trace.counts.total)
            if detail:
                detail.report(windows)
        if restore_error:
            raise restore_error
    emit('diagnostic-gaps', dict(
         xwayland_commits='XDamage batches observed; exact wlroots buffer commit/coalescing counter unavailable in the dynamic helper',
         frame_drops_repeats='no shared end-to-end frame ID or physical scanout feedback; do not subtract unrelated counters or call MaliPerf missed dropped frames',
         panvk_gpu_timing='not exposed by current noninvasive counters; broker Vulkan timings belong to Gamescope/Android Vulkan',
         android_cpu='host native-cpu.jsonl includes app total and individual threads; thread CPU is a subset of app CPU'))
    print('DROIDDECK_PERF_DIAGNOSTICS_DONE (observations only; no GPU/UI success claim)', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, choices=range(5, 31), default=25)
    parser.add_argument('--swap-details', action='store_true')
    parser.add_argument('--client-affinity', choices=('0x7c', '0xff', '0xfc'))
    parser.add_argument('--frame-details', action='store_true')
    args = parser.parse_args()
    if args.client_affinity and not args.swap_details:
        parser.error('--client-affinity requires --swap-details')
    if args.frame_details and (not args.swap_details or args.client_affinity != '0xff'):
        parser.error('--frame-details requires --swap-details --client-affinity 0xff')
    run(args.seconds, swap_details=args.swap_details, client_affinity=args.client_affinity, frame_details=args.frame_details)
