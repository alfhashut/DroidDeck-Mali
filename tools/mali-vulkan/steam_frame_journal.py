"""Guest controller for the explicitly armed native journal; never changes rendering."""
from pathlib import Path
import json
import os
import re
import time

ROOT = Path(os.environ['DROIDDECK_FRAME_TRACE_DIR']) if os.environ.get('DROIDDECK_FRAME_TRACE_DIR') else None


def schedule(seconds, emit, root=ROOT, *, proc_root=Path('/proc')):
    if root is None:
        emit('frame-journal-unavailable', 'DROIDDECK_FRAME_TRACE_DIR is not set for this app session')
        return None
    armed = root / 'armed'
    if not armed.is_file():
        emit('frame-journal-unavailable', 'not armed before Launch; external tracing continues')
        return None
    arm_mtime_ns = armed.stat().st_mtime_ns
    # The app's private log directory is bound into PRoot. Android's PID need
    # not exist in guest /proc; only Gamescope has guest /proc liveness.
    ready, rejected = [], []
    for path in sorted(root.glob('ready-*')):
        try:
            match = re.fullmatch(r'ready-([1-9][0-9]*)-(gamescope|android)', path.name)
            if not match or not path.is_file():
                raise ValueError('malformed ready marker name/type')
            pid, role = match.groups()
            if path.stat().st_mtime_ns < arm_mtime_ns:
                raise ValueError('ready marker predates current armed marker')
            if path.read_text().strip() != f'version=1 pid={pid} role={role}':
                raise ValueError('malformed ready marker contents')
            if role == 'gamescope' and not (proc_root / pid).is_dir():
                raise ValueError('Gamescope PID absent from guest /proc')
            ready.append(dict(pid=int(pid), role=role))
        except (OSError, ValueError) as error:
            rejected.append(dict(marker=path.name, reason=str(error)))
    duplicates = [role for role in ('gamescope', 'android')
                  if sum(row['role'] == role for row in ready) > 1]
    if rejected or duplicates or sorted(row['role'] for row in ready) != ['android', 'gamescope'] or len({row['pid'] for row in ready}) != 2:
        emit('frame-journal-unavailable', dict(reason='need exactly one fresh worker per role for this arm cycle; arm then Stop/Launch',
                                             ready=ready, rejected=rejected, duplicate_roles=duplicates))
        return None
    if not armed.is_file() or armed.stat().st_mtime_ns != arm_mtime_ns:
        emit('frame-journal-unavailable', 'arm cycle changed during worker selection; retry after Stop/Launch')
        return None
    if seconds > 27:
        emit('frame-journal-unavailable', 'internal capture limited to 27 seconds plus boundary guards')
        return None
    # One clock/control shared by both processes. Arm early; hot events still
    # filter strictly to the requested interval. Guard capture edges explicitly.
    run = time.monotonic_ns()
    start = run + 1_000_000_000
    end = start + (seconds + 2) * 1_000_000_000
    config = dict(run=run, start_ns=start, end_ns=end, cpu_stride=8,
                  arm_mtime_ns=arm_mtime_ns, ready=ready)
    temporary = root / ('control-' + str(os.getpid()))
    temporary.write_text(f'{run} {start} {end} 8\n')
    temporary.replace(root / 'control')
    (root / f'capture-{run}.json').write_text(json.dumps(config) + '\n')
    emit('frame-journal-control', config)
    return config


def finish(config, emit, root=ROOT):
    if config is None:
        return
    # Called only AFTER restoring affinity and draining the external trace.
    while time.monotonic_ns() < config['end_ns'] + 300_000_000:
        time.sleep(0.05)
    files = sorted(p.name for p in root.glob(f"trace-{config['run']}-*.jsonl"))
    expected_files = sorted(f"trace-{config['run']}-{row['pid']}-{row['role']}.jsonl"
                            for row in config['ready'])
    missing = sorted(set(expected_files) - set(files))
    unexpected = sorted(set(files) - set(expected_files))
    emit('frame-journal-files', dict(run=config['run'], files=files, expected=2,
                                   expected_files=expected_files, missing_files=missing,
                                   unexpected_files=unexpected,
                                   complete_pair_available=len(expected_files) == 2 and not missing and not unexpected))
