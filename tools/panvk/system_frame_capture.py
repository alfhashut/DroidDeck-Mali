"""Opt-in system trace alongside one guest sample. No renderer configuration.

Only this recorder's verified perfetto process is stopped. Existing tracing is
never cleared/stopped. The remote duration/file-size limits also bound capture
if the host disappears. Physical presentation and buffer release are distinct.
"""
import json
from pathlib import Path
import re
import shlex
import subprocess
import threading
import time
import uuid


def trace_config(seconds, scheduler=False):
    config = '''buffers { size_kb: 16384 fill_policy: RING_BUFFER }
duration_ms: %d
write_into_file: true
file_write_period_ms: 1000
flush_period_ms: 1000
max_file_size_bytes: 134217728
data_sources { config { name: "linux.ftrace" ftrace_config {
  ftrace_events: "power/cpu_frequency"
  ftrace_events: "dma_fence/dma_fence_signaled"
  atrace_categories: "gfx"
  atrace_apps: "com.droiddeck.launcher"
  compact_sched { enabled: true }
  disable_generic_events: true
  buffer_size_kb: 2048
  drain_period_ms: 250
} } }
data_sources { config { name: "linux.process_stats" process_stats_config {
  scan_all_processes_on_start: true
  proc_stats_poll_ms: 1000
} } }
data_sources { config { name: "android.surfaceflinger.frametimeline" } }
''' % ((seconds+30)*1000)
    if scheduler:
        config = config.replace('  compact_sched', '  ftrace_events: "sched/sched_switch"\n'
                                '  ftrace_events: "sched/sched_waking"\n  compact_sched')
    return config


def layer_names(text):
    return [line.strip() for line in text.splitlines()
            if line.startswith('droiddeck_wayland_game') or
            ('SurfaceView[' in line and 'com.droiddeck.launcher' in line)]


class SystemFrameCapture:
    def __init__(self, adb, directory, seconds, scheduler=False):
        self.adb, self.directory, self.seconds = adb, directory, seconds
        self.remote = '/data/local/tmp/droiddeck-frames-'+uuid.uuid4().hex
        self.trace_remote = '/data/misc/perfetto-traces/'+Path(self.remote).name+'.pftrace'
        self.pid = None
        self.layers = []
        self.stop = threading.Event()
        self.thread = None
        self.metadata = dict(available=False, clock='Perfetto clock snapshots; guest BOOTTIME/MONOTONIC anchors',
                             device_trace=self.trace_remote, scheduler_recording=scheduler)

    def command(self, argv, **kwargs):
        return subprocess.run(self.adb+['shell', shlex.join(argv)], text=True,
                              capture_output=True, timeout=15, **kwargs)

    def latency(self, phase):
        listing = self.command(['dumpsys', 'SurfaceFlinger', '--list'])
        self.layers = list(dict.fromkeys(self.layers+layer_names(listing.stdout)))
        rows = []
        for layer in self.layers:
            a = time.time_ns()
            result = self.command(['dumpsys', 'SurfaceFlinger', '--latency', layer])
            rows.append(dict(layer=layer, host_realtime_before_ns=a, host_realtime_after_ns=time.time_ns(),
                             returncode=result.returncode, output=result.stdout, error=result.stderr))
        (self.directory / ('surfaceflinger-latency-'+phase+'.json')).write_text(json.dumps(rows, indent=2))

    def start(self):
        config = self.directory / 'perfetto-config.pbtxt'
        config.write_text(trace_config(self.seconds, self.metadata['scheduler_recording']))
        try:
            capability = self.command(['perfetto', '--query'])
            (self.directory / 'system-capabilities.txt').write_text(capability.stdout+capability.stderr)
            if capability.returncode or 'linux.ftrace' not in capability.stdout:
                raise RuntimeError('system Perfetto ftrace source unavailable')
            self.latency('before')
            subprocess.run(self.adb+['push', str(config), self.remote+'.pbtxt'], check=True,
                           capture_output=True, timeout=15)
            # Android's perfetto SELinux domain cannot open shell_data_file
            # configs in /data/local/tmp. Feed stdin and use its trace directory.
            command = shlex.join(['cat', self.remote+'.pbtxt'])+' | '+shlex.join([
                'perfetto', '--background', '--txt', '-c', '-', '-o', self.trace_remote])
            result = self.command(['sh', '-c', command])
            self.metadata.update(start_stdout=result.stdout, start_stderr=result.stderr)
            match = re.fullmatch(r'\s*(\d+)\s*', result.stdout)
            if result.returncode or not match:
                raise RuntimeError('Perfetto background start did not return a recorder PID')
            self.pid = int(match[1])
            self.metadata.update(available=True, pid=self.pid)
            self.thread = threading.Thread(target=self.sample_latency, daemon=True)
            self.thread.start()
            print('System frame trace started; recorder PID='+str(self.pid), flush=True)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            self.metadata['error'] = str(error)
            print('System frame trace unavailable: '+str(error), flush=True)
        self.save()

    def save(self):
        (self.directory / 'system-trace-session.json').write_text(json.dumps(self.metadata, indent=2))

    def sample_latency(self):
        # The 128-entry native-layer history can expire while the guest drains
        # its CEF report. Snapshot during sampling, without clearing SF history.
        with (self.directory/'surfaceflinger-latency-samples.jsonl').open('x') as stream:
            while not self.stop.is_set():
                for layer in self.layers:
                    if not layer.startswith('droiddeck_wayland_game'):
                        continue
                    try:
                        before = time.time_ns()
                        result = self.command(['dumpsys', 'SurfaceFlinger', '--latency', layer])
                        row = dict(layer=layer, host_realtime_before_ns=before,
                                   host_realtime_after_ns=time.time_ns(), returncode=result.returncode,
                                   output=result.stdout, error=result.stderr)
                    except (OSError, subprocess.SubprocessError) as error:
                        row = dict(layer=layer, error=str(error))
                    stream.write(json.dumps(row)+'\n')
                    stream.flush()
                self.stop.wait(2)

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=16)
        try:
            if self.pid:
                # A reused PID must not receive SIGINT. Inspect the exact UUID
                # output argument; do not use killall/perfetto --stop.
                cmdline = self.command(['cat', '/proc/'+str(self.pid)+'/cmdline'])
                arguments = cmdline.stdout.rstrip('\0').split('\0')
                if self.trace_remote in arguments and any(Path(arg).name == 'perfetto' for arg in arguments[:1]):
                    result = self.command(['kill', '-INT', str(self.pid)])
                    self.metadata['stop_returncode'] = result.returncode
                    deadline = time.monotonic()+8
                    while time.monotonic() < deadline:
                        check = self.command(['test', '-e', '/proc/'+str(self.pid)])
                        if check.returncode:
                            break
                        time.sleep(0.2)
                    else:
                        raise RuntimeError('recorder did not finish; remote duration limit remains active')
                elif cmdline.returncode == 0:
                    raise RuntimeError('recorder identity mismatch; refusing to signal PID')
                result = subprocess.run(self.adb+['pull', self.trace_remote, str(self.directory/'system-trace.pftrace')],
                                        text=True, capture_output=True, timeout=20)
                self.metadata.update(pull_returncode=result.returncode, pull_output=result.stdout+result.stderr)
                if result.returncode:
                    raise RuntimeError('system trace pull failed')
            self.latency('after')
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            self.metadata['close_error'] = str(error)
            print('System frame trace collection incomplete: '+str(error), flush=True)
        finally:
            self.save()
