#!/usr/bin/env python3
"""Arm/disarm development frame journals before Launch. Does not change session policy."""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adb_device import resolve_serial
ROOT = '/storage/emulated/0/Download/DroidDeckFrameTrace'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('arm', 'disarm', 'status'))
    parser.add_argument('--serial')
    parser.add_argument('--adb', default='adb')
    args = parser.parse_args()
    adb = [args.adb, '-s', resolve_serial(args.adb, args.serial, log=True)]
    def shell(*words):
        subprocess.run(adb + ['shell', shlex.join(words)], check=True)
    if args.action == 'arm':
        # Expand only the two worker-marker patterns in the Android shell.
        # Quoting them as individual shell() arguments would disable globbing.
        # Retain trace-*.jsonl and capture-*.json for previous-run analysis.
        script = '\n'.join([
            'set -e',
            'trace_root=' + shlex.quote(ROOT),
            'mkdir -p "$trace_root"',
            'rm -f "$trace_root/armed" "$trace_root/control"',
            'rm -f "$trace_root"/ready-*-gamescope "$trace_root"/ready-*-android',
            'touch "$trace_root/armed"',
        ])
        subprocess.run(adb + ['shell', script], check=True)
        print('Armed. Use normal Stop then Mali Steam Launch to create both current-cycle workers.')
    elif args.action == 'disarm':
        shell('rm', '-f', ROOT + '/armed', ROOT + '/control')
        print('Disarmed; workers flush incomplete captures and exit. Existing trace files retained.')
    else:
        shell('ls', '-l', ROOT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
