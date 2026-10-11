"""Bounded webhelper-only affinity override. No Steam-owned files are written.

A separate, session-detached worker owns both mutation and restoration. EOF on
the parent's control pipe or a hard deadline restores masks while the worker
remains alive. PRoot teardown can kill both processes; normal DroidDeck Stop
also kills the target session itself. No SIGKILL-survival guarantee is made.
"""
import json
import fcntl
import atexit
import os
from pathlib import Path
import select
import signal
import time


def emit(label, value):
    print(label + '=' + json.dumps(value, sort_keys=True), flush=True)


def identity(pid, tid):
    text = (Path('/proc') / str(pid) / 'task' / str(tid) / 'stat').read_text()
    fields = text[text.rindex(')')+2:].split()
    return int(fields[19])


def targets(client):
    """Exact binary and isolated session environment; never match shell argv."""
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            argv = proc.joinpath('cmdline').read_bytes().split(b'\0')
            if not argv or Path(os.fsdecode(argv[0])).name != 'steamwebhelper':
                continue
            env = dict(item.split('=', 1) for item in proc.joinpath('environ').read_text().split('\0') if '=' in item)
            if not all(env.get(k) == client.get(k) for k in ('BL_LOG', 'DISPLAY', 'XAUTHORITY')):
                continue
            if env.get('BL_INSIDE') != '1' or env.get('BL_MALI_STEAM_UI') != '1':
                continue
            if env.get('MALI_VULKAN_NORMAL_SESSION') or env.get('MALI_VULKAN_BROKER_SOCKET'):
                raise RuntimeError('webhelper unexpectedly has compositor/broker environment')
            if env.get('PANVK_ATOM_STRIDE') != '56' or not env.get('VK_DRIVER_FILES') or env.get('ANGLE_DEFAULT_PLATFORM') != 'vulkan':
                raise RuntimeError('webhelper is not the proven stride-56 Vulkan client')
            found.append(int(proc.name))
        except (OSError, ValueError):
            continue
    return found


class Masks:
    def __init__(self, client, cpus):
        self.client, self.cpus = client, set(cpus)
        self.saved = {}
        self.initial = True
        self.baseline = {2, 3, 4, 5, 6}
        self.process_original = {}
        self.name_original = {}
        self.ticks = {}
        self.hz = os.sysconf('SC_CLK_TCK')

    def refresh(self, apply=True):
        initial = self.initial
        pids = targets(self.client)
        if self.initial and not pids:
            raise RuntimeError('no validated experimental webhelper processes')
        pending = []
        for pid in pids:
            if self.initial:
                proc = Path('/proc') / str(pid)
                try:
                    env = dict(item.split('=', 1) for item in proc.joinpath('environ').read_text().split('\0') if '=' in item)
                    argv = proc.joinpath('cmdline').read_text().rstrip('\0').split('\0')
                    emit('affinity-client-identity', dict(pid=pid,
                        angle_flags=[arg for arg in argv if 'use-angle' in arg or 'use-gl=' in arg],
                        environment={key: env.get(key) for key in ('ANGLE_DEFAULT_PLATFORM', 'PANVK_ATOM_STRIDE', 'VK_DRIVER_FILES')}))
                except OSError:
                    continue
            try:
                tasks = list((Path('/proc') / str(pid) / 'task').iterdir())
            except OSError:
                continue
            for task in tasks:
                tid = int(task.name)
                try:
                    start = identity(pid, tid)
                    key = (pid, tid, start)
                    if key in self.saved:
                        if apply and self.cpus != self.baseline and set(os.sched_getaffinity(tid)) != self.cpus:
                            os.sched_setaffinity(tid, self.cpus)
                        continue
                    original = set(os.sched_getaffinity(tid))
                    name = (task / 'comm').read_text().strip()
                    if self.initial:
                        self.name_original.setdefault((pid, name), set()).add(tuple(sorted(original)))
                        if tid == pid:
                            self.process_original[pid] = original
                    restore = original
                    if not self.initial and original == self.cpus and self.cpus != self.baseline:
                        # A new task has no pre-experiment mask to restore.
                        # Prefer its pre-existing same-name policy, otherwise
                        # use this process's original leader/launcher policy.
                        policies = self.name_original.get((pid, name), set())
                        restore = set(next(iter(policies))) if len(policies) == 1 else self.process_original.get(pid, self.baseline)
                        emit('affinity-new-thread-policy', dict(pid=pid, tid=tid, name=name,
                             restore=sorted(restore), exact_original_available=False))
                    pending.append((key, set(restore)))
                except (FileNotFoundError, ProcessLookupError):
                    continue
        # Validate the whole initial population before changing any thread.
        self.initial = False
        for key, original in pending:
            pid, tid, start = key
            try:
                if identity(pid, tid) != start:
                    continue
            except (FileNotFoundError, ProcessLookupError):
                continue
            self.saved[key] = original  # journal in the restoring worker first
            if not apply:
                continue
            try:
                # A means the actual current wrapper policy, including any
                # individual masks subsequently chosen by CEF itself.
                desired = original if self.cpus == self.baseline else self.cpus
                os.sched_setaffinity(tid, desired)
                effective = set(os.sched_getaffinity(tid))
            except ProcessLookupError:
                continue
            emit('affinity-thread-applied', dict(pid=pid, tid=tid, start=start,
                 restore=sorted(original), requested=sorted(desired), effective=sorted(effective),
                 inherited_restore=not initial))
            if effective != desired:
                raise RuntimeError('cpuset/online constraints changed the requested affinity')
        if not self.saved:
            raise RuntimeError('no live webhelper threads to sample')
        return len(pending)

    def snapshot(self, phase):
        now = time.monotonic_ns()
        rows = []
        for (pid, tid, start), original in self.saved.items():
            try:
                if identity(pid, tid) != start:
                    continue
                task = Path('/proc') / str(pid) / 'task' / str(tid)
                text = (task / 'stat').read_text()
                fields = text[text.rindex(')')+2:].split()
                ticks = int(fields[11])+int(fields[12])
                previous = self.ticks.get((pid, tid, start))
                cpu = (ticks-previous[0])/self.hz if previous else None
                elapsed = (now-previous[1])/1e9 if previous else None
                self.ticks[(pid, tid, start)] = ticks, now
                sched = [int(v) for v in (task / 'schedstat').read_text().split()]
                rows.append(dict(pid=pid, tid=tid, start=start,
                    name=text[text.index('(')+1:text.rindex(')')], affinity=sorted(os.sched_getaffinity(tid)),
                    last_cpu=int(fields[36]), cpu_s=cpu, interval_s=elapsed,
                    cpu_percent=100*cpu/elapsed if elapsed and cpu is not None else None,
                    sched_runtime_ns=sched[0], sched_runqueue_ns=sched[1]))
            except (OSError, ValueError, IndexError):
                continue
        emit('affinity-thread-snapshot', dict(phase=phase, monotonic_ns=now, rows=rows))

    def restore(self):
        errors = []
        if self.saved:
            self.initial = False
        for attempt in range(3):
            for (pid, tid, start), original in list(self.saved.items()):
                try:
                    if identity(pid, tid) != start:
                        continue  # recycled TIDs must never be changed
                    os.sched_setaffinity(tid, original)
                    if set(os.sched_getaffinity(tid)) != original:
                        raise RuntimeError('restored mask verification failed')
                except (FileNotFoundError, ProcessLookupError):
                    continue
                except (OSError, RuntimeError) as error:
                    errors.append(dict(pid=pid, tid=tid, error=str(error)))
            if not self.saved:
                break
            try:
                # Parents now have their baseline masks, so subsequent clones
                # inherit the restored policy. Catch tasks created during the
                # first restore pass without applying the experiment again.
                if not self.refresh(apply=False):
                    break
                if attempt == 2:
                    errors.append(dict(error='thread churn exceeded restoration passes'))
            except (OSError, RuntimeError) as error:
                errors.append(dict(error='restore discovery: '+str(error)))
                break
        emit('affinity-restored', dict(ok=not errors, recorded_threads=len(self.saved), errors=errors))
        return not errors


class Experiment:
    def __init__(self, client, mask, seconds):
        if not client.get('BL_LOG'):
            raise RuntimeError('session identity missing')
        cpus = {i for i in range(8) if int(mask, 16) & (1 << i)}
        read_control, self.control = os.pipe()
        ready, write_ready = os.pipe()
        self.pid = os.fork()
        self.closed = False
        if self.pid == 0:
            os.close(self.control)
            os.close(ready)
            masks = Masks(client, cpus)
            ok = False
            lock = None
            try:
                os.setsid()
                signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(InterruptedError('watchdog terminated')))
                signal.signal(signal.SIGINT, signal.SIG_IGN)
                lock = os.open('/dev/shm/droiddeck-steam-affinity.lock', os.O_CREAT | os.O_RDWR, 0o600)
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                platform = {}
                for path in (Path('/sys/devices/system/cpu/online'), *Path('/sys/devices/system/cpu').glob('cpu[0-9]/cpufreq/cpuinfo_max_freq')):
                    try:
                        platform[str(path)] = path.read_text().strip()
                    except OSError as error:
                        platform[str(path)] = str(error)
                emit('affinity-platform', platform)
                steam = Path(client.get('HOME', '/root')) / '.local/share/Steam'
                for wrapper in steam.glob('*/steamwebhelper.sh'):
                    try:
                        emit('affinity-installed-wrapper', dict(path=str(wrapper),
                            taskset_lines=[line for line in wrapper.read_text().splitlines() if 'taskset' in line]))
                    except OSError as error:
                        emit('affinity-wrapper-unavailable', str(error))
                masks.refresh()
                masks.snapshot('start')
                os.write(write_ready, b'1')
                os.close(write_ready)
                write_ready = None
                deadline = time.monotonic() + seconds + 5
                next_snapshot = time.monotonic() + 1
                while time.monotonic() < deadline:
                    if select.select([read_control], [], [], 1)[0]:
                        os.read(read_control, 1)  # stop byte or EOF after parent death
                        break
                    masks.refresh()
                    if time.monotonic() >= next_snapshot:
                        masks.snapshot('sample')
                        next_snapshot = time.monotonic() + 1
                if time.monotonic() >= deadline:
                    emit('affinity-deadline-reached', dict(monotonic_ns=time.monotonic_ns()))
                masks.refresh()  # include newly inherited threads before restore
                masks.snapshot('end')
                ok = True
            except BaseException as error:
                emit('affinity-experiment-error', str(error))
            finally:
                ok = masks.restore() and ok
                if write_ready is not None:
                    os.close(write_ready)
                os.close(read_control)
                if lock is not None:
                    os.close(lock)
            os._exit(0 if ok else 1)
        os.close(read_control)
        os.close(write_ready)
        try:
            if not select.select([ready], [], [], 8)[0] or os.read(ready, 1) != b'1':
                raise RuntimeError('affinity watchdog failed before sampling')
        except BaseException:
            self.close()
            raise
        finally:
            os.close(ready)
        emit('affinity-experiment-start', dict(mask=mask, watchdog_pid=self.pid,
             scope='validated experimental steamwebhelper threads only',
             hard_deadline_s=seconds+5, restore='exact original masks for existing tasks; same-name/leader baseline policy for newly inherited tasks',
             baseline='0x7c preserves actual per-thread masks, including CEF overrides'))
        self.old_term = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(SystemExit('affinity sampler terminated')))
        atexit.register(self.close)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.control is not None:
            os.close(self.control)
            self.control = None
        _, status = os.waitpid(self.pid, 0)
        if hasattr(self, 'old_term'):
            signal.signal(signal.SIGTERM, self.old_term)
        atexit.unregister(self.close)
        if status != 0:
            raise RuntimeError('affinity worker failed; inspect affinity-restored before retrying')
