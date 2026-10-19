"""Host-side native log/CPU capture for the opt-in live Steam sampler."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

sys.path.append(str(Path(__file__).resolve().parents[1] / 'mali-vulkan'))
from diagnose_steam_perf import cpu_delta, stat_fields


class PerformanceCapture:
    def __init__(self, adb, directory=None, frame_seconds=None, frame_scheduler=False):
        self.adb = adb
        self.directory = directory or Path(tempfile.mkdtemp(prefix='droiddeck-steam-perf-'))
        self.stop = threading.Event()
        self.thread = None
        self.logcat = None
        self.log = None
        self.frame_seconds = frame_seconds
        self.frame_scheduler = frame_scheduler
        self.system = None

    def __enter__(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        # Prevent accidental overwrite when a named capture is repeated.
        for name in ('native-logcat.txt', 'native-cpu.jsonl', 'guest-report.txt',
                     'system-trace-session.json', 'system-trace.pftrace', 'perfetto-config.pbtxt',
                     'surfaceflinger-latency-samples.jsonl'):
            if (self.directory / name).exists():
                raise FileExistsError('capture already exists: ' + str(self.directory / name))
        self.log = (self.directory / 'native-logcat.txt').open('x')
        print('Performance capture directory=' + str(self.directory.resolve()), flush=True)
        try:
            pid = subprocess.check_output(self.adb + ['shell', 'pidof com.droiddeck.launcher'],
                                          text=True, timeout=10).strip()
            if not pid.isdigit():
                raise RuntimeError('need exactly one running DroidDeck app')
            hz = int(subprocess.check_output(self.adb + ['shell', 'getconf CLK_TCK'], text=True, timeout=10))
            self.logcat = subprocess.Popen(self.adb + ['logcat', '-v', 'epoch', '-T', '1', '--pid=' + pid,
                  'MaliVulkanBroker:I', '*:S'], stdout=self.log, stderr=subprocess.STDOUT)
            self.thread = threading.Thread(target=self.sample, args=(pid, hz), daemon=True)
            self.thread.start()
            if self.frame_seconds:
                from system_frame_capture import SystemFrameCapture
                self.system = SystemFrameCapture(self.adb, self.directory, self.frame_seconds, self.frame_scheduler)
                self.system.start()
        except Exception:
            self.__exit__(None, None, None)
            raise
        return self

    def sample(self, pid, hz):
        before, previous = {}, None
        with (self.directory / 'native-cpu.jsonl').open('x') as stream:
            while not self.stop.is_set():
                try:
                    result = subprocess.run(self.adb + ['shell', 'cat /proc/' + pid + '/stat /proc/' +
                                            pid + '/task/*/stat'], text=True, capture_output=True, timeout=5)
                    now = time.monotonic()
                    current, rows = {}, []
                    for line in result.stdout.splitlines():
                        key = line.split(' ', 1)[0]
                        if not key.isdigit() or '(' not in line:
                            continue
                        # The app stat is also present as its main thread stat.
                        # Use separate keys; the process total includes all threads.
                        scope = 'process' if not current else 'thread'
                        row = stat_fields(line)
                        identity = scope + '/' + key
                        current[identity] = row
                        cpu = cpu_delta(before.get(identity), row, now - previous, hz) if previous else None
                        rows.append(dict(key=identity, name=row['name'], cpu_percent=cpu))
                    stream.write(json.dumps(dict(host_monotonic_ns=time.monotonic_ns(),
                        host_realtime_ns=time.time_ns(), hz=hz, rows=rows, error=result.stderr.strip())) + '\n')
                    stream.flush()
                    before, previous = current, now
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    stream.write(json.dumps(dict(error=str(error))) + '\n')
                    stream.flush()
                self.stop.wait(1)

    def pull_journal(self):
        report = self.directory / 'guest-report.txt'
        if not report.is_file():
            return
        controls = [json.loads(line.split('=', 1)[1]) for line in report.read_text().splitlines()
                    if line.startswith('frame-journal-control=')]
        if not controls:
            return
        from steam_frame_journal import ROOT
        target = self.directory / 'frame-journal'
        target.mkdir(exist_ok=False)
        for config in controls:
            run = config['run']
            names = [f'capture-{run}.json'] + [f"trace-{run}-{row['pid']}-{row['role']}.jsonl" for row in config['ready']]
            for name in names:
                result = subprocess.run(self.adb + ['pull', str(ROOT / name), str(target / name)],
                                        capture_output=True, text=True, timeout=15)
                if result.returncode:
                    (target / (name + '.pull-error.txt')).write_text(result.stderr)
        print('Internal frame journals=' + str(target), flush=True)

    def __exit__(self, *args):
        try:
            if self.system:
                self.system.close()
        finally:
            self.stop.set()
            if self.thread:
                self.thread.join(timeout=6)
            if self.logcat:
                self.logcat.terminate()
                try:
                    self.logcat.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.logcat.kill()
                    self.logcat.wait(timeout=3)
            if self.log:
                self.log.close()
            if self.frame_seconds:
                self.pull_journal()
