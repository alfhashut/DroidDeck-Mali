"""Guest controller for the explicitly armed native journal; never changes rendering."""
from pathlib import Path
import json
import os
import time

ROOT = Path('/storage/emulated/0/Download/DroidDeckFrameTrace')


def schedule(seconds, emit, root=ROOT):
    if not (root / 'armed').is_file():
        emit('frame-journal-unavailable', 'not armed before Launch; external tracing continues')
        return None
    # Both workers must belong to LIVE processes, not stale ready files.
    ready = []
    for path in root.glob('ready-*-*'):
        try:
            _, pid, role = path.name.split('-', 2)
            if Path('/proc', pid).is_dir():
                ready.append(dict(pid=int(pid), role=role))
        except (OSError, ValueError):
            continue
    if sorted(row['role'] for row in ready) != ['android', 'gamescope'] or len({row['pid'] for row in ready}) != 2:
        emit('frame-journal-unavailable', dict(reason='need exactly one live worker per role; arm then Stop/Launch the new CI build', ready=ready))
        return None
    if seconds > 27:
        emit('frame-journal-unavailable', 'internal capture limited to 27 seconds plus boundary guards')
        return None
    # One clock/control shared by both processes. Arm early; hot events still
    # filter strictly to the requested interval. Guard capture edges explicitly.
    run = time.monotonic_ns()
    start = run + 1_000_000_000
    end = start + (seconds + 2) * 1_000_000_000
    config = dict(run=run, start_ns=start, end_ns=end, cpu_stride=8, ready=ready)
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
    emit('frame-journal-files', dict(run=config['run'], files=files, expected=2,
                                   complete_pair_available=len(files) == 2))
