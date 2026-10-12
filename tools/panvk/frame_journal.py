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
        shell('mkdir', '-p', ROOT)
        # Stale control can never start a new session by accident.
        shell('rm', '-f', ROOT + '/control')
        shell('touch', ROOT + '/armed')
        print('Armed. Use normal Stop then Mali Steam Launch to start both new CI workers.')
    elif args.action == 'disarm':
        shell('rm', '-f', ROOT + '/armed', ROOT + '/control')
        print('Disarmed; workers flush incomplete captures and exit. Existing trace files retained.')
    else:
        shell('ls', '-l', ROOT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
